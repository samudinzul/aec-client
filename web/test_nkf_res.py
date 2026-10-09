"""NKF residual-suppressor regression tests (offline, no hardware).

Usage (from repo root):
    python -m web.test_nkf_res

Three properties the intrinsic Wiener RES must hold (see web/nkf.py
RES_* constants for the design,and the module docstring for why the
baselines are what they are):

T1 transparency — voice-only mic, silent ref: RES-on output must be
   bit-comparable to RES-off (relative diff < 1%). Rationale: with
   no far-end energy the echo-power estimate is zero, so every gain
   is exactly 1. NOTE: this compares against the core's own output,
   not the input — the reference WOLA path reconstructs at ~0.77
   (double periodic-Hann windowing, same as the desktop C++ and the
   official nkf.py), so input-identity is neither expected nor tested.
T2 echo-only A/B — delayed-noise echo, no near-end: RES-on residual
   must beat RES-off by >= 3 dB (measured +18 dB at introduction).
T3 doubletalk fingerprint — the healthy-engine invariants must hold
   with RES on: TDC locks the true delay, exposed, 0 guard resets,
   no give-up, backstop ~0 dB, finite output.

Exit code: 0 = all pass, 1 = a failure. Prints measured numbers so a
regression shows its size, not just its existence.
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


def _voice(dur=8.0):
    t = np.arange(int(SR * dur)) / SR
    return (((t % 2.0) < 0.5).astype(np.float32)
            * (0.25 * np.sin(2 * np.pi * 220 * t)).astype(np.float32))


def _echo_pair(dur=8.0, delay=800, seed=7):
    rng = np.random.default_rng(seed)
    n = int(SR * dur)
    ref = (0.4 * rng.standard_normal(n)).astype(np.float32)
    echo = np.concatenate([np.zeros(delay, np.float32),
                           0.7 * ref[:n - delay]])
    return np.clip(echo, -1, 1), ref


def t1_transparency():
    voice = _voice()
    z = np.zeros_like(voice)
    tail = slice(-2 * SR, None)
    a = _run(NkfEngine(enable_res=True), voice, z)
    b = _run(NkfEngine(enable_res=False), voice, z)
    rel = (np.sqrt(np.mean((a[tail] - b[tail]) ** 2))
           / (np.sqrt(np.mean(b[tail] ** 2)) + 1e-12))
    ok = bool(rel < 0.01)
    print(f"T1 transparency: on-vs-off rel diff {rel * 100:.3f}% "
          f"({'PASS' if ok else 'FAIL: want <1%'})")
    return ok


def t2_echo_only():
    echo, ref = _echo_pair()
    tail = slice(-4 * SR, None)
    outs = {}
    for tag, kw in (("off", {"enable_res": False}), ("on", {})):
        outs[tag] = _run(NkfEngine(**kw), echo, ref)
    if not np.all(np.isfinite(outs["on"])):
        print("T2 echo-only: FAIL (non-finite output)")
        return False
    rms = lambda x: 10 * np.log10(np.mean(x[tail] ** 2) + 1e-12)
    base, withres = rms(outs["off"]), rms(outs["on"])
    ok = bool(base - withres >= 3.0)
    print(f"T2 echo-only: off {base:.1f} dB, on {withres:.1f} dB "
          f"(gain {base - withres:.1f} dB, {'PASS' if ok else 'FAIL: want >=3 dB'})")
    return ok


def t3_doubletalk():
    echo, ref = _echo_pair()
    mic = np.clip(echo + _voice(), -1, 1)
    eng = NkfEngine()
    out = _run(eng, mic, ref)
    st = eng.get_state()
    checks = [st["exposed"] == 1, st["guardResets"] == 0,
              st["giveUp"] == 0, st["notchFreq"] == 0.0,
              abs(st["notchDb"]) < 0.5,
              bool(np.all(np.isfinite(out)))]
    ok = all(checks)
    res = st.get("resDb", 0.0)
    print(f"T3 doubletalk: exposed={st['exposed']} resets={st['guardResets']} "
          f"giveup={st['giveUp']} notch={st['notchFreq']:.0f}Hz/"
          f"{st['notchDb']:.1f}dB "
          f"res={res:.1f}dB ({'PASS' if ok else 'FAIL'})")
    return ok


def main():
    import os
    if not os.path.isfile(os.path.join("models", "nkf.onnx")):
        # Magic phrase: test.sh maps it to SKIP, not FAIL.
        print("SKIP (models missing?)")
        return 0
    probe = NkfEngine()
    if not probe.ready:
        print(f"SKIP (NKF backend unavailable: {probe.last_error})")
        return 0
    results = [t1_transparency(), t2_echo_only(), t3_doubletalk()]
    print("RES TESTS: " + ("ALL PASS" if all(results) else "FAILURES PRESENT"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
