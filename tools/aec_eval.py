#!/usr/bin/env python3
"""Offline AEC evaluation harness — loudspeaker, earphone, double-talk.

The web chain (web/chain.py) is a Python mirror of the desktop pump:
same AEC3 / DTLN-AEC / NKF engines, same NS stage, same 16 kHz rate.
This script drives it with deterministic synthetic speech through
simulated echo paths, then reports the numbers that matter:

  ERLE          echo-only echo reduction (dB)
  near SNR      near-end transparency after delay/gain alignment (dB)
  near corr     waveform correlation with clean near-end
  energy ratio  output RMS / clean near-end RMS (gating detector)
  STOI          short-time intelligibility after alignment
  RTF           processing seconds per audio second (lower = cheaper)

Signals are synthetic but voiced (harmonic formants, syllable envelope),
so engine ranking is meaningful; the WAVs are written for listening.
Use real recordings later to confirm the ranking.

ERLE is only comparable in --modes raw. After NS, the gate suppresses
echo-only output to near-silence and inflates ERLE to meaningless
values; use STOI and the listening WAVs for the full chain.

Examples (from repo root):

  # AEC only, all engines, raw chain
  python3 tools/aec_eval.py --ns off --modes raw

  # End-to-end chain with DTLN-NS + HPF/expander/presence shelf
  python3 tools/aec_eval.py --engines off,aec3,dtln --ns dtln --modes full

  # Quick smoke
  python3 tools/aec_eval.py --seconds 6 --engines off,aec3 --ns off --modes raw
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

import numpy as np
from scipy import signal

try:
    from pystoi import stoi as _stoi
except ImportError:  # optional; metrics still work without it
    _stoi = None

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from web.chain import Chain  # noqa: E402

SR = 16000
# The desktop pump runs 10 ms frames (160 @ 16 kHz), which is also
# AEC3's native block. The web UI normally uses 128 (8 ms), but its
# 128->160 bridge is a separate defect; evaluating at 160 here matches
# the desktop and gives AEC3 an exact block every call.
EVAL_FRAME = 160
EPS = 1e-12


# ---------------------------------------------------------------------------
# signal generation
# ---------------------------------------------------------------------------

def synth_voice(seconds, f0_base, seed, formants=None, active=None, rms=0.08):
    """Voiced speech-like signal: formant-weighted harmonics, syllables.

    Not intelligible speech, but it has the pitch/formant/temporal
    structure AEC and NS react to. active=[(start_s, end_s), ...] gates
    the signal (near-end talk spurts); edges are raised-cosine.
    """
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    t = np.arange(n) / SR

    # Syllable envelope: 160-300 ms voiced chunks, short gaps, occasional
    # phrase pauses. This gives the AEC both adaptation and silence.
    env = np.zeros(n, dtype=np.float64)
    pos = 0.0
    while pos < seconds:
        dur = float(rng.uniform(0.16, 0.30))
        gap = float(rng.uniform(0.04, 0.10))
        if rng.random() < 0.10:
            gap += float(rng.uniform(0.25, 0.55))
        a = int(pos * SR)
        b = min(n, int((pos + dur) * SR))
        if b > a:
            ln = b - a
            w = 0.5 - 0.5 * np.cos(np.linspace(0, 2 * np.pi, ln))
            env[a:b] = np.maximum(env[a:b], w)
        pos += dur + gap

    # Pitch contour: slow phrase movement + vibrato + tiny jitter.
    f0 = f0_base * (
        1.0
        + 0.10 * np.sin(2 * np.pi * 0.55 * t + rng.uniform(0, 2 * np.pi))
        + 0.035 * np.sin(2 * np.pi * 3.1 * t + rng.uniform(0, 2 * np.pi))
    )
    f0 *= 1.0 + 0.01 * np.cumsum(rng.standard_normal(n)) / math.sqrt(n)
    phase = 2 * np.pi * np.cumsum(f0) / SR

    if formants is None:
        formants = ((500.0, 90.0, 1.0),
                    (1500.0, 110.0, 0.65),
                    (2500.0, 140.0, 0.35))

    sig = np.zeros(n, dtype=np.float64)
    for h in range(1, 56):
        fh = h * f0
        amp = np.zeros(n, dtype=np.float64)
        for fc, bw, gain in formants:
            amp += gain * np.exp(-0.5 * ((fh - fc) / bw) ** 2)
        amp *= 1.0 / (h ** 0.30)
        sig += amp * np.sin(h * phase + rng.uniform(0, 2 * np.pi))

    # A little fricative noise at a few syllable onsets: keeps the
    # signal from being purely harmonic (more realistic for NS).
    onsets = np.where(np.diff((env > 0.05).astype(np.int8)) == 1)[0]
    for idx in onsets[::3]:
        ln = int(0.025 * SR)
        a = int(idx)
        b = min(n, a + ln)
        if b <= a:
            continue
        burst = rng.standard_normal(b - a)
        burst = signal.sosfilt(
            signal.butter(2, 2500 / (SR / 2), "high", output="sos"), burst)
        w = np.linspace(0, np.pi, b - a)
        sig[a:b] += 0.12 * burst * np.sin(w) ** 2

    sig *= env
    sig /= np.sqrt(np.mean(sig ** 2) + EPS)
    sig *= rms

    if active:
        gate = np.zeros(n, dtype=np.float64)
        for a_s, b_s in active:
            a = max(0, int(a_s * SR))
            b = min(n, int(b_s * SR))
            if b > a:
                ln = b - a
                fade = min(int(0.010 * SR), ln // 2)
                gate[a:b] = 1.0
                if fade > 0:
                    ramp = 0.5 - 0.5 * np.cos(
                        np.linspace(0, np.pi, fade))
                    gate[a:a + fade] *= ramp
                    gate[b - fade:b] *= ramp[::-1]
        sig *= gate

    return sig.astype(np.float32)


def apply_gate(x, active):
    """Raised-cosine gate for near-end talk spurts."""
    x = np.asarray(x, dtype=np.float64).copy()
    n = len(x)
    gate = np.zeros(n, dtype=np.float64)
    for a_s, b_s in active:
        a = max(0, int(a_s * SR))
        b = min(n, int(b_s * SR))
        if b > a:
            ln = b - a
            fade = min(int(0.010 * SR), ln // 2)
            gate[a:b] = 1.0
            if fade > 0:
                ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, fade))
                gate[a:a + fade] *= ramp
                gate[b - fade:b] *= ramp[::-1]
    return (x * gate).astype(np.float32)


def read_speech(path, seconds, rms_target=0.08):
    """Load a mono speech file, resample to 16 kHz, tile/trim, normalize."""
    import soundfile as sf
    x, sr = sf.read(str(path), dtype="float64", always_2d=False)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if sr != SR:
        x = signal.resample_poly(x, SR, sr)
    n = int(seconds * SR)
    if len(x) < n:
        x = np.tile(x, int(math.ceil(n / max(len(x), 1))))
    x = x[:n]
    x = x - float(np.mean(x))
    x = x / (np.sqrt(np.mean(x ** 2)) + EPS) * rms_target
    return x.astype(np.float32)


def make_rir_loudspeaker(seed=1, rt60=0.35, pre_ms=8.0):
    """Small-room loudspeaker path: direct + early taps + diffuse tail."""
    rng = np.random.default_rng(seed)
    n = int(rt60 * SR)
    rir = np.zeros(n, dtype=np.float64)
    direct = int(pre_ms * SR / 1000.0)
    rir[direct] = 1.0
    for delay_ms, gain in ((4, 0.55), (9, -0.40), (15, 0.30),
                           (22, -0.22), (31, 0.16), (43, -0.12),
                           (57, 0.09)):
        idx = direct + int(delay_ms * SR / 1000.0)
        if idx < n:
            rir[idx] += gain
    tail_start = direct + int(0.045 * SR)
    m = n - tail_start
    if m > 0:
        tail = rng.standard_normal(m) * np.exp(
            -np.arange(m) / (SR * rt60 / 6.9))
        tail = signal.sosfilt(
            signal.butter(2, 4500 / (SR / 2), "low", output="sos"), tail)
        tail *= 10 ** (-18 / 20) / (np.sqrt(np.mean(tail ** 2)) + EPS)
        rir[tail_start:] += tail
    rir = signal.sosfilt(
        signal.butter(2, 100 / (SR / 2), "high", output="sos"), rir)
    rir /= np.max(np.abs(rir)) + EPS
    return rir


def make_rir_earphone(seed=2, delay_ms=1.5, atten_db=-24.0):
    """Short, weak leakage path: one direct tap + one tiny reflection."""
    n = int(0.010 * SR)
    rir = np.zeros(n, dtype=np.float64)
    d = int(delay_ms * SR / 1000.0)
    g = 10 ** (atten_db / 20.0)
    rir[d] = g
    idx = d + int(0.0004 * SR)
    if idx < n:
        rir[idx] = -0.35 * g
    return rir


def pink_noise(n, seed):
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(n)
    b = (0.049922035, -0.095993537, 0.050612699, -0.004408786)
    a = (1.0, -2.494956002, 2.017265875, -0.522189400)
    x = signal.lfilter(b, a, white)
    return x / (np.sqrt(np.mean(x ** 2)) + EPS)


def convolve(x, rir):
    return signal.fftconvolve(x, rir, mode="full")[: len(x)]


def set_rms(x, target):
    return x * (target / (np.sqrt(np.mean(x ** 2)) + EPS))


def clip16(x):
    return np.clip(x, -1.0, 1.0)


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------

def rms(x):
    x = np.asarray(x, dtype=np.float64)
    return float(np.sqrt(np.mean(x ** 2) + EPS))


def db20(x):
    return 20.0 * math.log10(max(float(x), 1e-12))


def active_mask(x, frame=160, rel_db=-35.0):
    """Sample mask for frames within rel_db of the signal's loudest frame."""
    x = np.asarray(x, dtype=np.float64)
    nf = len(x) // frame
    if nf == 0:
        return np.zeros(len(x), dtype=bool)
    e = np.sqrt(np.mean(x[: nf * frame].reshape(nf, frame) ** 2, axis=1))
    ref = float(np.max(e))
    if ref <= 1e-12:
        return np.zeros(len(x), dtype=bool)
    keep = (e / ref) > (10 ** (rel_db / 20.0))
    return np.repeat(keep, frame)


def align_to(ref, sig, max_lag=4000):
    """Return sig delayed/aligned so sig[n] ~= ref[n].

    Uses FFT cross-correlation; limits the search so a spurious peak in
    silence cannot pull the alignment by seconds.
    """
    ref = np.asarray(ref, dtype=np.float64)
    sig = np.asarray(sig, dtype=np.float64)
    c = signal.correlate(sig, ref, mode="full", method="fft")
    lags = signal.correlation_lags(len(sig), len(ref), mode="full")
    sel = (lags >= -max_lag) & (lags <= max_lag)
    if not np.any(sel):
        return sig[: len(ref)], 0
    k = int(np.argmax(np.abs(c[sel])))
    lag = int(lags[sel][k])
    if lag >= 0:
        out = sig[lag: lag + len(ref)]
    else:
        out = np.concatenate([np.zeros(-lag, dtype=np.float64),
                              sig[: len(ref) + lag]])
    if len(out) < len(ref):
        out = np.pad(out, (0, len(ref) - len(out)))
    return out[: len(ref)], lag


def near_metrics(out, near, mask):
    """Transparency: delay/gain-aligned output vs clean near-end."""
    a, lag = align_to(near, out)
    m = mask & (np.abs(near) > 1e-9)
    if np.count_nonzero(m) < 160:
        return {"snr_db": float("nan"), "corr": float("nan"),
                "energy_ratio": float("nan"), "lag_ms": lag * 1000.0 / SR}
    x = a[m]
    y = near[m]
    yy = float(np.dot(y, y))
    g = float(np.dot(x, y) / (yy + EPS))
    g = max(g, 1e-3)
    err = x - g * y
    snr = 10.0 * math.log10(
        (float(np.dot(g * y, g * y)) + EPS) /
        (float(np.dot(err, err)) + EPS))
    corr = float(np.corrcoef(x, y)[0, 1]) if np.std(x) > 0 and np.std(y) > 0 else 0.0
    er = rms(x) / (rms(y) + EPS)
    stoi = float("nan")
    if _stoi is not None and len(x) > SR // 2:
        try:
            stoi = float(_stoi(np.clip(y / 32768.0, -1, 1),
                               np.clip(x / 32768.0, -1, 1),
                               SR, extended=False))
        except Exception:
            stoi = float("nan")
    return {"snr_db": float(snr), "corr": float(corr),
            "energy_ratio": float(er), "lag_ms": lag * 1000.0 / SR,
            "stoi": stoi}


def erle(mic, out, mask):
    m = mask
    if np.count_nonzero(m) < 160:
        return float("nan")
    return 10.0 * math.log10(
        (float(np.dot(mic[m], mic[m])) + EPS) /
        (float(np.dot(out[m], out[m])) + EPS))


def erle_windows(mic, out, echo, seconds=0.5):
    """ERLE per window over the echo-active span, for convergence view."""
    n = len(mic)
    w = int(seconds * SR)
    vals = []
    for a in range(0, n - w + 1, w):
        b = a + w
        if rms(echo[a:b]) < 1e-5:
            continue
        vals.append(erle(mic[a:b], out[a:b], np.ones(w, dtype=bool)))
    return vals


def clip_count(out_i16):
    return int(np.count_nonzero(np.abs(out_i16.astype(np.int32)) >= 32767))


# ---------------------------------------------------------------------------
# chain driving
# ---------------------------------------------------------------------------

def make_chain(engine, ns, raw):
    ch = Chain(model_dir=str(ROOT / "models"), num_threads=2)
    if not ch.set_ns_backend(ns):
        raise RuntimeError(f"NS backend refused: {ch.last_error}")
    if not ch.set_engine(engine):
        raise RuntimeError(f"engine refused: {ch.last_error}")
    if raw:
        # Isolate the AEC/NS core. The desktop hygiene stages are taste
        # controls; they must not contaminate an engine comparison.
        def identity(x):
            return np.asarray(x, dtype=np.float32)
        ch._hpf_run = identity
        ch._exp_run = identity
        ch._pres_run = identity
    if not ch.start():
        raise RuntimeError(f"chain start failed: {ch.last_error}")
    return ch


def run_chain(ch, mic, ref):
    """Process a full signal; returns int16 output and processing seconds."""
    n = len(mic)
    out = []
    t0 = time.perf_counter()
    for i in range(0, n, EVAL_FRAME):
        m = mic[i: i + EVAL_FRAME]
        r = ref[i: i + EVAL_FRAME]
        if len(m) < EVAL_FRAME:
            m = np.pad(m, (0, EVAL_FRAME - len(m)))
            r = np.pad(r, (0, EVAL_FRAME - len(r)))
        out.append(ch.process_frame(m, r))
    elapsed = time.perf_counter() - t0
    out = np.concatenate(out)[:n]
    return out.astype(np.int16), elapsed


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_list(s):
    return [x.strip() for x in s.split(",") if x.strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--engines", default="off,aec3,dtln,nkf")
    ap.add_argument("--ns", default="off")
    ap.add_argument("--modes", default="raw",
                    help="raw (no hygiene DSP), full, or raw,full")
    ap.add_argument("--scenarios",
                    default="speaker_echo,speaker_dt,earphone_dt,"
                            "near_only,near_only_silent")
    ap.add_argument("--echo-db", type=float, default=-3.0,
                    help="loudspeaker echo level relative to near-end")
    ap.add_argument("--earphone-db", type=float, default=-20.0,
                    help="earphone leakage level relative to near-end")
    ap.add_argument("--noise-db", type=float, default=30.0,
                    help="noise SNR below the active signal")
    ap.add_argument("--out", default="/tmp/aec_eval")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--far-wav", default=None,
                    help="real far-end speech WAV/FLAC (16k or resampled)")
    ap.add_argument("--near-wav", default=None,
                    help="real near-end speech WAV/FLAC (16k or resampled)")
    args = ap.parse_args()

    seconds = args.seconds
    n = int(seconds * SR)
    outdir = Path(args.out)
    (outdir / "signals").mkdir(parents=True, exist_ok=True)
    (outdir / "out").mkdir(parents=True, exist_ok=True)

    # --- deterministic signals -------------------------------------------
    near_active = [(0.30 * seconds, 0.70 * seconds)]
    if args.far_wav or args.near_wav:
        if not (args.far_wav and args.near_wav):
            raise SystemExit("give both --far-wav and --near-wav")
        far = read_speech(args.far_wav, seconds)
        near = apply_gate(read_speech(args.near_wav, seconds), near_active)
    else:
        far = synth_voice(seconds, 115.0, args.seed + 1,
                          formants=((520, 90, 1.0), (1480, 110, 0.62),
                                    (2480, 140, 0.34)))
        near = synth_voice(seconds, 205.0, args.seed + 2,
                           formants=((700, 90, 1.0), (1800, 110, 0.60),
                                     (2900, 150, 0.32)),
                           active=near_active, rms=0.08)

    echo_spk = set_rms(convolve(far, make_rir_loudspeaker(args.seed + 3)),
                       rms(near) * 10 ** (args.echo_db / 20.0))
    echo_ear = set_rms(convolve(far, make_rir_earphone(args.seed + 4)),
                       rms(near) * 10 ** (args.earphone_db / 20.0))
    noise = pink_noise(n, args.seed + 5) * (
        rms(near) * 10 ** (-args.noise_db / 20.0))

    zeros = np.zeros(n, dtype=np.float32)
    scenarios = {
        "speaker_echo": dict(mic=echo_spk + noise, ref=far,
                             near=zeros, echo=echo_spk),
        "speaker_dt": dict(mic=echo_spk + near + noise, ref=far,
                           near=near, echo=echo_spk),
        "earphone_dt": dict(mic=echo_ear + near + noise, ref=far,
                            near=near, echo=echo_ear),
        # Sealed earphone / headset: far-end is playing, but nothing leaks
        # into the mic. The AEC must not treat the near-end as echo.
        "near_only": dict(mic=near + noise, ref=far,
                          near=near, echo=zeros),
        # Control: no reference at all. Any output damage here is the
        # engine's own filter, not mis-adaptation to an active reference.
        "near_only_silent": dict(mic=near + noise, ref=zeros,
                                 near=near, echo=zeros),
    }
    for name, sc in scenarios.items():
        sc["mic"] = clip16(sc["mic"]).astype(np.float32)
        sc["ref"] = clip16(sc["ref"]).astype(np.float32)
        # The chain consumes int16, exactly like the desktop pump. Keep
        # the float copies for metrics so echo/near stay separable.
        sc["mic_i16"] = np.clip(sc["mic"] * 32767.0, -32768, 32767).astype(np.int16)
        sc["ref_i16"] = np.clip(sc["ref"] * 32767.0, -32768, 32767).astype(np.int16)
        sc["echo"] = np.asarray(sc["echo"], dtype=np.float32)
        sc["near"] = np.asarray(sc["near"], dtype=np.float32)

    # write the reference signals once for listening / external analysis
    import soundfile as sf
    for nm, arr in (("far", far), ("near", near),
                    ("echo_spk", echo_spk), ("echo_ear", echo_ear),
                    ("noise", noise)):
        sf.write(outdir / "signals" / f"{nm}.wav", clip16(arr), SR,
                 subtype="PCM_16")
    for name, sc in scenarios.items():
        sf.write(outdir / "signals" / f"mic_{name}.wav",
                 sc["mic"], SR, subtype="PCM_16")

    wanted_scen = parse_list(args.scenarios)
    engines = parse_list(args.engines)
    ns_list = parse_list(args.ns)
    modes = parse_list(args.modes)

    near_mask = active_mask(near, rel_db=-35.0)
    echo_spk_mask = active_mask(echo_spk, rel_db=-35.0)
    echo_ear_mask = active_mask(echo_ear, rel_db=-35.0)

    rows = []
    for mode in modes:
        raw = (mode == "raw")
        if mode not in ("raw", "full"):
            raise SystemExit(f"unknown mode: {mode}")
        for engine in engines:
            for ns in ns_list:
                label = f"{engine}/{ns}/{mode}"
                print(f"\n=== {label} ===", flush=True)
                try:
                    ch = make_chain(engine, ns, raw)
                except Exception as e:
                    print(f"  SKIP: {e}")
                    continue
                for scen_name in wanted_scen:
                    if scen_name not in scenarios:
                        print(f"  SKIP unknown scenario {scen_name}")
                        continue
                    sc = scenarios[scen_name]
                    ch.stop()
                    ch.start()
                    outp, elapsed = run_chain(ch, sc["mic_i16"], sc["ref_i16"])
                    rtf = elapsed / seconds
                    outp_f = outp.astype(np.float64)
                    mic_f = sc["mic_i16"].astype(np.float64)
                    near_f = sc["near"].astype(np.float64) * 32768.0
                    echo_f = sc["echo"].astype(np.float64) * 32768.0
                    peak = int(np.max(np.abs(outp.astype(np.int32))))
                    clip = clip_count(outp)
                    sf.write(outdir / "out" / f"{scen_name}_{engine}_{ns}_{mode}.wav",
                             outp, SR, subtype="PCM_16")

                    base = {
                        "mode": mode, "engine": engine, "ns": ns,
                        "scenario": scen_name, "rtf": round(rtf, 4),
                        "peak": peak, "clipped": clip,
                        "out_rms_db": round(db20(rms(outp_f) / 32768.0), 2),
                    }
                    if scen_name in ("speaker_echo",):
                        e = erle(mic_f, outp_f, echo_spk_mask)
                        wins = erle_windows(mic_f, outp_f, echo_f)
                        base.update({
                            "erle_db": round(float(e), 2),
                            "erle_first": round(float(wins[0]), 2) if wins else None,
                            "erle_med": round(float(np.median(wins)), 2) if wins else None,
                            "erle_last": round(float(wins[-1]), 2) if wins else None,
                        })
                    if scen_name in ("speaker_dt", "earphone_dt",
                                     "near_only", "near_only_silent"):
                        nm = near_metrics(outp_f, near_f, near_mask)
                        base.update({
                            "near_snr_db": round(nm["snr_db"], 2),
                            "near_corr": round(nm["corr"], 4),
                            "near_energy": round(nm["energy_ratio"], 3),
                            "near_lag_ms": round(nm["lag_ms"], 2),
                            "stoi": (round(nm["stoi"], 3)
                                     if math.isfinite(nm["stoi"]) else None),
                        })
                    rows.append(base)
                    print("  " + json.dumps(base, sort_keys=True), flush=True)
                ch.stop()

    # --- report -----------------------------------------------------------
    cols = ["mode", "engine", "ns", "scenario", "erle_db", "erle_med",
            "near_snr_db", "stoi", "near_corr", "near_energy",
            "rtf", "peak", "clipped"]
    print("\n" + " | ".join(c.rjust(14) for c in cols))
    print("-" * (17 * len(cols)))
    for row in rows:
        vals = []
        for c in cols:
            v = row.get(c, "")
            if isinstance(v, float):
                vals.append(f"{v:.2f}".rjust(14))
            else:
                vals.append(str(v).rjust(14))
        print(" | ".join(vals))

    with open(outdir / "results.json", "w") as f:
        json.dump(rows, f, indent=2)
    print(f"\nwrote {outdir / 'results.json'}")
    print(f"WAVs: {outdir / 'out'}  (listen before trusting the numbers)")


if __name__ == "__main__":
    main()
