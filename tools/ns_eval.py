#!/usr/bin/env python3
"""Offline noise-suppression comparison for the web chain.

AEC is not the question here: the engine is Off, so the mic reaches
the NS stage untouched. That isolates what each NS backend does to a
known noise bed. Hygiene stages are bypassed by default (``--full``
puts them back) for the same reason aec_eval isolates the engine.

Beds are deterministic and cover the three cases that rank the
backends differently:

  pink      steady broadband hiss (fan, air, preamp)
  keyboard  short wideband transients (typing, clicks, taps)
  babble    several overlapping talkers (office/cafe background)

Metrics:

  noise dB   noise-only output / input RMS (negative = suppression)
  STOI       clean near-end vs processed speech+noise, delay-aligned
  speech dB  speech-active output / input RMS (0 dB = level kept)
  gap dB     noise-only-frame output / input RMS inside speech+noise
  RTF        processing seconds per audio second (Python, relative)

Examples (from repo root):

  python3 tools/ns_eval.py --near-wav /tmp/asr_3.flac \
      --beds pink,keyboard,babble --ns off,dtln,hush,gtcrn \
      --out /tmp/ns_eval

  python3 tools/ns_eval.py --seconds 6 --beds pink --ns dtln,hush

The WAVs in <out>/out/ are the point of the exercise: listen before
trusting the table.
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
except ImportError:  # optional
    _stoi = None

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from web.chain import Chain  # noqa: E402
from tools.aec_eval import (  # noqa: E402
    EVAL_FRAME,
    SR,
    align_to,
    apply_gate,
    pink_noise,
    read_speech,
    synth_voice,
)

EPS = 1e-12


def rms(x):
    x = np.asarray(x, dtype=np.float64)
    return float(np.sqrt(np.mean(x * x) + EPS))


def db20(x):
    return 20.0 * math.log10(max(float(x), 1e-12))


def frame_rms(x, frame=EVAL_FRAME):
    x = np.asarray(x, dtype=np.float64)
    nf = len(x) // frame
    if nf == 0:
        return np.zeros(0, dtype=np.float64)
    return np.sqrt(np.mean(x[: nf * frame].reshape(nf, frame) ** 2, axis=1)
                   + EPS)


def make_keyboard(seconds, rms_target, seed):
    """Sparse decaying wideband clicks, ~7-12 events/s."""
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    x = np.zeros(n, dtype=np.float64)
    pos = int(0.2 * SR)
    while pos < n:
        ln = int(0.03 * SR)
        b = min(n, pos + ln)
        burst = rng.standard_normal(b - pos)
        burst *= np.exp(-np.arange(b - pos) / (0.004 * SR))
        burst = signal.sosfilt(
            signal.butter(2, [1200 / (SR / 2), 6000 / (SR / 2)],
                          "band", output="sos"), burst)
        x[pos:b] += burst * (0.02 + 0.10 * rng.random())
        pos += int((0.07 + 0.18 * rng.random()) * SR)
    return x / (rms(x) + EPS) * rms_target


def make_babble(seconds, rms_target, seed):
    """Three overlapping synthetic talkers with different pitch."""
    x = np.zeros(int(seconds * SR), dtype=np.float64)
    for k, (f0, gain) in enumerate(((105.0, 1.0), (150.0, 0.8),
                                     (205.0, 0.65))):
        v = synth_voice(seconds, f0, seed + k, rms=1.0)
        v = np.roll(v, int((0.7 + 0.9 * k) * SR))
        x += gain * v
    return x / (rms(x) + EPS) * rms_target


def make_chain(ns, full):
    ch = Chain(model_dir=str(ROOT / "models"), num_threads=2)
    if not ch.set_ns_backend(ns):
        raise RuntimeError(f"NS backend refused: {ch.last_error}")
    if not ch.set_engine("off"):
        raise RuntimeError(f"engine refused: {ch.last_error}")
    if not full:
        def identity(x):
            return np.asarray(x, dtype=np.float32)
        ch._hpf_run = identity
        ch._exp_run = identity
        ch._pres_run = identity
    if not ch.start():
        raise RuntimeError(f"chain start failed: {ch.last_error}")
    return ch


def run_chain(ch, mic):
    """One signal through the running chain; returns float output + RTF."""
    n = len(mic)
    mic16 = np.clip(np.asarray(mic) * 32767.0, -32768, 32767).astype(np.int16)
    zeros = np.zeros(n, dtype=np.int16)
    out = []
    t0 = time.perf_counter()
    for i in range(0, n, EVAL_FRAME):
        m = mic16[i:i + EVAL_FRAME]
        r = zeros[i:i + EVAL_FRAME]
        if len(m) < EVAL_FRAME:
            m = np.pad(m, (0, EVAL_FRAME - len(m)))
            r = np.pad(r, (0, EVAL_FRAME - len(r)))
        out.append(ch.process_frame(m, r))
    elapsed = time.perf_counter() - t0
    return np.concatenate(out)[:n].astype(np.float64) / 32768.0, elapsed / (n / SR)


def active_frames(clean, rel_db=-35.0):
    e = frame_rms(clean)
    if e.size == 0 or float(e.max()) <= 1e-12:
        return np.zeros(0, dtype=bool)
    return e > float(e.max()) * 10 ** (rel_db / 20.0)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--near-wav", default=None,
                    help="real near-end speech (16k or resampled); "
                         "default: deterministic synthetic voice")
    ap.add_argument("--beds", default="pink,keyboard,babble")
    ap.add_argument("--ns", default="off,dtln,hush,gtcrn")
    ap.add_argument("--snr-db", type=float, default=15.0,
                    help="bed RMS below speech-active RMS (default 15)")
    ap.add_argument("--out", default="/tmp/ns_eval")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--full", action="store_true",
                    help="keep HPF/expander/presence (default: raw NS only)")
    args = ap.parse_args()

    seconds = args.seconds
    n = int(seconds * SR)
    outdir = Path(args.out)
    (outdir / "signals").mkdir(parents=True, exist_ok=True)
    (outdir / "out").mkdir(parents=True, exist_ok=True)

    if args.near_wav:
        speech = read_speech(args.near_wav, seconds)
    else:
        speech = synth_voice(seconds, 205.0, args.seed + 2,
                             formants=((700, 90, 1.0), (1800, 110, 0.60),
                                       (2900, 150, 0.32)))
    speech = apply_gate(speech, [(0.30 * seconds, 0.70 * seconds)])
    act = active_frames(speech)
    if act.size == 0:
        raise SystemExit("no speech-active frames")
    sample_act = np.repeat(act, EVAL_FRAME)
    speech_rms = rms(speech[:len(sample_act)][sample_act])
    bed_rms = speech_rms * 10 ** (-args.snr_db / 20.0)
    print(f"speech active RMS {speech_rms:.5f}  "
          f"bed RMS {bed_rms:.5f}  (SNR {args.snr_db:g} dB)")

    beds = {}
    for name in [x.strip() for x in args.beds.split(",") if x.strip()]:
        if name == "pink":
            beds[name] = pink_noise(n, args.seed + 5) * bed_rms
        elif name == "keyboard":
            beds[name] = make_keyboard(seconds, bed_rms, args.seed + 11)
        elif name == "babble":
            beds[name] = make_babble(seconds, bed_rms, args.seed + 21)
        else:
            raise SystemExit(f"unknown bed: {name}")

    import soundfile as sf
    sf.write(outdir / "signals" / "speech.wav", np.clip(speech, -1, 1), SR,
             subtype="PCM_16")
    for name, bed in beds.items():
        sf.write(outdir / "signals" / f"{name}.wav", np.clip(bed, -1, 1),
                 SR, subtype="PCM_16")

    rows = []
    ns_list = [x.strip() for x in args.ns.split(",") if x.strip()]
    for ns in ns_list:
        try:
            ch = make_chain(ns, args.full)
        except Exception as e:
            print(f"ns {ns}: SKIP ({e})")
            continue
        for bed_name, bed in beds.items():
            mic = np.clip(speech + bed, -1, 1)
            # noise-only pass
            ch.stop()
            ch.start()
            noise_out, _ = run_chain(ch, bed)
            # speech + bed pass (reset every time, like a new call)
            ch.stop()
            ch.start()
            out, rtf = run_chain(ch, mic)
            ch.stop()

            ali, lag = align_to(speech * 32768.0, out * 32768.0)
            ali = np.asarray(ali, dtype=np.float64) / 32768.0
            ali = np.clip(ali, -1.0, 1.0)
            fi = frame_rms(mic)
            fo = frame_rms(ali)
            shift = int(round(lag / EVAL_FRAME))
            mask = np.roll(act, -shift)
            mask[:5] = False
            mask[-5:] = False
            gap = db20(rms(fo[~mask]) / rms(fi[~mask]))
            sp = db20(rms(fo[mask]) / rms(fi[mask]))
            noise_db = db20(rms(noise_out) / rms(bed))
            stoi = float("nan")
            if _stoi is not None and len(ali) > SR // 2:
                try:
                    stoi = float(_stoi(np.clip(speech, -1, 1), ali, SR,
                                       extended=False))
                except Exception:
                    pass
            rec = {
                "ns": ns, "bed": bed_name,
                "noise_db": round(noise_db, 2),
                "stoi": None if math.isnan(stoi) else round(stoi, 3),
                "speech_db": round(sp, 2),
                "gap_db": round(gap, 2),
                "rtf": round(rtf, 4),
            }
            rows.append(rec)
            print(f"  {bed_name:8} {ns:6} noise {noise_db:+7.2f} dB  "
                  f"stoi {stoi:5.3f}  speech {sp:+6.2f} dB  "
                  f"gap {gap:+7.2f} dB  rtf {rtf:5.3f}", flush=True)
            sf.write(outdir / "out" / f"{bed_name}_{ns}_noise.wav",
                     np.clip(noise_out, -1, 1), SR, subtype="PCM_16")
            sf.write(outdir / "out" / f"{bed_name}_{ns}_speech.wav",
                     ali, SR, subtype="PCM_16")
        ch.stop()

    (outdir / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(f"\nwrote {outdir / 'results.json'} and WAVs in {outdir / 'out'}")
    print("noise dB is noise-only attenuation; gap dB is noise during "
          "speech pauses.\nListen before choosing: a high suppression "
          "number can still pump, lisp or swallow consonants.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
