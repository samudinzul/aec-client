"""NKF tracking-notch regression tests (offline, no hardware).

Usage (from repo root):
    python -m web.test_nkf_notch

The notch replaces the broadband backstop trim with identical
engagement conditions (tonal + stable-centre runs, 2 windows
unlooped / 1 looped, heal on proof) — so voice protection is
provably unchanged, and only the actuator differs. These tests
prove what the actuator adds:

N1 stable-tone kill — loud fixed 440 Hz interferer + voice, ref
   SILENT (Kalman blind by construction, so all suppression is the
   notch): engages at ~440 Hz, tone bin drops >= 10 dB vs ANF-off
   A/B, release after.
N2 sweeping-tone kill — slow 400 -> 640 Hz chirp (20 Hz/s: the
   trackable-sweep case the fixed trim could never hold), ref
   silent: notchFreq tracks within +-150 Hz, tone-following
   suppression >= 6 dB vs A/B, release. Fast slides never form
   stability runs (backstop included) — documented boundary.
N3 voice transparency — implicit, by construction: engagement uses
   byte-identical census/thresholds/windows as the retired trim,
   and T3 + smoke assert notchFreq == 0 on speech. No separate
   test (a duplicate would prove nothing new).

Exit code: 0 = all pass, 1 = a failure.
"""

import sys

import numpy as np

from .nkf import NkfEngine

SR = 16000


def _run(eng, mic, ref, frame=128):
    out = []
    for i in range(0, len(mic), frame):
        m = (np.clip(mic[i:i + frame], -1, 1) * 32767).astype(np.int16)
        r = (np.clip(ref[i:i + frame], -1, 1) * 32767).astype(np.int16)
        out.append(eng.process(m, r))
    return np.concatenate(out).astype(np.float32) / 32768.0


def _voice(dur=10.0):
    t = np.arange(int(SR * dur)) / SR
    return (((t % 2.0) < 0.5).astype(np.float32)
            * (0.2 * np.sin(2 * np.pi * 220 * t)).astype(np.float32))


def _band_energy(x, f0, bw=45.0):
    """Mean power within +-bw of f0 over the whole signal (dBFS)."""
    spec = np.abs(np.fft.rfft(x.astype(np.float64)
                               * np.hanning(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1.0 / SR)
    sel = (freqs >= f0 - bw) & (freqs <= f0 + bw)
    return 10 * np.log10(spec[sel].mean() + 1e-12)


def _hot_pair(dur=12.0, kind="tone", sweep_rate=20.0):
    """Actuator-isolation fixture: the ref is DIGITAL SILENCE, so the
    Kalman core silence-skips (passes everything) and the loop
    detector stays quiet — the ONLY suppression in the chain is the
    notch under test. (A usable ref would let the linear stage eat
    stable tones itself, hiding the actuator: verified, 60 dB gone
    with no notch involved.) The mic carries voice bursts plus a
    loud interferer at conversational (not clipped) levels.
    kind="tone": fixed 440 Hz. kind="sweep": slow linear chirp
    (sweep_rate Hz/s — trackable; fast slides never form the
    stability runs any tonal defense needs, backstop included).
    Returns (mic, ref).
    """
    n = int(SR * dur)
    t = np.arange(n) / SR
    if kind == "tone":
        interf = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    else:
        f_inst = 400.0 + sweep_rate * t
        ph = 2 * np.pi * np.cumsum(f_inst) / SR
        interf = (0.4 * np.sin(ph)).astype(np.float32)
    mic = np.clip(_voice(dur) + interf, -1, 1)
    return mic, np.zeros(n, np.float32)


def n1_stable_kill():
    mic, ref = _hot_pair()
    seg = slice(5 * SR, 8 * SR)
    outs, freqs = {}, {}
    for tag, kw in (("off", {"enable_anf": False}), ("on", {})):
        eng = NkfEngine(**kw)
        o = _run(eng, mic, ref)
        outs[tag] = o
        # Sample engagement mid-run on a fresh engine (stateful).
        e2 = NkfEngine(**kw)
        _run(e2, mic[:6 * SR], ref[:6 * SR])
        freqs[tag] = e2.get_state()["notchFreq"]
    assert np.all(np.isfinite(outs["on"]))
    cut = _band_energy(mic[seg], 440) - _band_energy(outs["on"][seg], 440)
    cut_off = _band_energy(mic[seg], 440) - _band_energy(outs["off"][seg], 440)
    f0 = freqs["on"]
    # Release: 2 s of silence after the tone stops (toneless windows
    # disarm; voice would too via gaps, silence is deterministic).
    eng = NkfEngine()
    _run(eng, mic, ref)
    # 3 s tail: a full 64-block window must close on toneless
    # input to disarm (2 s falls just short at 62 blocks).
    _run(eng, np.zeros(3 * SR, np.float32), np.zeros(3 * SR, np.float32))
    released = eng.get_state()["notchFreq"] == 0.0
    ok = (abs(f0 - 440) < 50 and cut - cut_off >= 10.0 and released
          and freqs["off"] == 0.0)
    print(f"N1 stable kill: f0={f0:.0f}Hz cut={cut:.1f}dB "
          f"(off: {cut_off:.1f}dB, marginal {cut - cut_off:.1f}dB, want >=10) "
          f"released={released} ({'PASS' if ok else 'FAIL'})")
    return bool(ok)


def n2_sweep_kill():
    mic, ref = _hot_pair(kind="sweep")
    seg = slice(5 * SR, 8 * SR)
    outs = {}
    for tag, kw in (("off", {"enable_anf": False}), ("on", {})):
        outs[tag] = _run(NkfEngine(**kw), mic, ref)
    assert np.all(np.isfinite(outs["on"]))
    # Tone-following energy: integrate bins along the slow sweep
    # (relative offsets — the input is pre-sliced here).
    def sweep_energy(x):
        e = 0.0
        m = 0
        for start in range(0, len(x) - 4096, 4096):
            blk = x[start:start + 4096]
            fabs = 5 * SR + start + 2048
            fmid = 400.0 + 20.0 * (fabs / SR)
            spec = np.abs(np.fft.rfft(
                blk.astype(np.float64) * np.hanning(len(blk)))) ** 2
            fr = np.fft.rfftfreq(len(blk), 1.0 / SR)
            sel = (fr >= fmid - 60) & (fr <= fmid + 60)
            e += spec[sel].mean()
            m += 1
        return 10 * np.log10(e / m + 1e-12)
    cut = sweep_energy(mic[seg]) - sweep_energy(outs["on"][seg])
    cut_off = sweep_energy(mic[seg]) - sweep_energy(outs["off"][seg])
    e2 = NkfEngine()
    _run(e2, mic[:7 * SR], ref[:7 * SR])
    f0 = e2.get_state()["notchFreq"]
    f_true = 400.0 + 20.0 * 7.0
    e3 = NkfEngine()
    _run(e3, mic, ref)
    # 3 s tail: a full 64-block window must close on toneless
    # input to disarm (2 s falls just short at 62 blocks).
    _run(e3, np.zeros(3 * SR, np.float32), np.zeros(3 * SR, np.float32))
    released = e3.get_state()["notchFreq"] == 0.0
    ok = (abs(f0 - f_true) < 150 and cut - cut_off >= 6.0 and released)
    print(f"N2 sweep kill: f0={f0:.0f}Hz (true {f_true:.0f}) "
          f"marginal {cut - cut_off:.1f}dB (want >=6) released={released} "
          f"({'PASS' if ok else 'FAIL'})")
    return bool(ok)


def main():
    if not NkfEngine().ready:
        print("SKIP (NKF backend unavailable)")
        return 0
    results = [n1_stable_kill(), n2_sweep_kill()]
    print("NOTCH TESTS: " + ("ALL PASS" if all(results) else "FAILURES PRESENT"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
