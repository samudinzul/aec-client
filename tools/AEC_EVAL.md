# Offline AEC evaluation

`tools/aec_eval.py` drives the web chain (`web/chain.py`) at 16 kHz with
160-sample frames, matching the desktop's 10 ms pump. It is a ranking
tool, not a substitute for a desktop build and a listening test.

The web chain mirrors the desktop engine + NS + hygiene order. The
harness isolates the engine in `--modes raw` (no NS, no HPF/expander/
presence shelf) and then checks the shipped chain in `--modes full`.

## Run

```bash
# Deterministic synthetic speech, engine-only ranking
python3 tools/aec_eval.py --seconds 10 --engines off,aec3,dtln,nkf \
  --ns off --modes raw --out /tmp/aec_raw

# End-to-end chain with the default DTLN-NS stage
python3 tools/aec_eval.py --seconds 10 --engines off,aec3,dtln \
  --ns dtln --modes full --out /tmp/aec_full

# Real speech (16 kHz mono; resampled if needed)
python3 tools/aec_eval.py --seconds 10 \
  --far-wav /tmp/asr_dummy.flac --near-wav /tmp/asr_3.flac \
  --engines off,aec3,dtln,nkf --ns off --modes raw --out /tmp/aec_real
```

The WAVs are written to `<out>/out/`; listen before trusting a metric.
`pystoi` is optional. Without it the STOI column is simply absent.

## Metrics

- **ERLE** — echo-only echo reduction (dB). Only comparable in
  `--modes raw`: after NS, the gate pushes echo-only output to near
  silence and produces meaningless 100+ dB values.
- **STOI** — intelligibility after delay/gain alignment. The main
  near-end transparency metric in double-talk.
- **near SNR / corr / energy** — waveform-level checks. Useful for
  spotting phase or level damage, but a nonlinear NS can lower them
  without hurting intelligibility.
- **RTF** — processing seconds per audio second. Python-only, so use it
  for relative cost, not desktop CPU predictions.

## Results

Real speech, 10 s, 16 kHz, loudspeaker echo at −3 dB relative to the
near-end, earphone leakage at −20 dB, noise 30 dB below the active
signal. Engine only (`--ns off --modes raw`):

| Engine | Loudspeaker echo ERLE | Speaker double-talk STOI | Earphone double-talk STOI | Sealed earphone STOI | RTF |
|---|---:|---:|---:|---:|---:|
| Off | 0.00 dB | 0.897 | 0.994 | 1.000 | 0.001 |
| WebRTC AEC3 | 22.19 dB | 0.708 | 0.888 | 0.411 | 0.015 |
| DTLN-AEC | 16.87 dB | 0.934 | 0.984 | 0.996 | 0.041 |
| NKF-AEC | 4.62 dB | 0.895 | 0.923 | 0.872 | 0.065 |

"Sealed earphone" uses an active far-end reference but no acoustic echo
path: the mic contains only near-end + noise. That is the case that
breaks AEC3 in this harness. With a silent reference AEC3 recovers
(STOI 0.997), so the damage is mis-adaptation to an active reference,
not the engine's own filter.

Full chain with DTLN-NS:

| Engine | Speaker DT STOI | Earphone DT STOI | Sealed earphone STOI | RTF |
|---|---:|---:|---:|---:|
| Off | 0.907 | 0.987 | 0.992 | 0.045 |
| WebRTC AEC3 | 0.714 | 0.891 | 0.359 | 0.062 |
| DTLN-AEC | 0.927 | 0.977 | 0.988 | 0.086 |

Hush was also checked end-to-end for Off and DTLN. It does not change
their ordering (STOI within 0.01–0.02 of DTLN-NS) and is heavier in the
Python harness. It remains a noise-type choice, not an AEC choice.

## Reading the numbers

- **AEC3 has the highest raw ERLE but the lowest near-end transparency
  in double-talk.** It is the low-CPU canceller, not the quality one.
- **AEC3 is unsafe for sealed earphones/headsets.** An active reference
  with no echo path makes it suppress the near-end (STOI 0.411 vs 1.000
  for Off). DTLN stays transparent (0.996). Use Off for sealed use.
- **DTLN is the loudspeaker and audible-leakage choice.** It gives up
  ~5 dB of echo-only ERLE to AEC3 but keeps far more of the near-end in
  double-talk (STOI 0.934 vs 0.708). It also does not collapse when the
  reference is active but acoustically isolated.
- **Leakage level matters.** On the same 10 s pair, earphone-double-talk
  STOI was: −20 dB Off 0.994 / DTLN 0.984; −12 dB Off 0.969 / DTLN
  0.973; −6 dB Off 0.922 / DTLN 0.948. Off wins for sealed/very low
  leakage; DTLN starts winning once the far end is audible (around
  −12 dB and louder).
- **NKF is dominated here.** Lower ERLE, lower double-talk STOI and the
  highest Python RTF. The 45 KB model may still be cheap in C++, but its
  quality is behind; keep it experimental.
- **NS dominates full-chain CPU.** DTLN+NS costs ~24–40% more than
  AEC3+NS in Python, but both are far below real time. Measure the
  desktop build for absolute cost.

## Recommendation

| Use | Engine | Why |
|---|---|---|
| Loudspeaker | DTLN-AEC | Best double-talk quality; enough ERLE for a small room |
| Sealed earphone / headset | Off | No echo path; zero CPU and fully transparent |
| Earphone with audible leakage | DTLN-AEC | Beats Off around −12 dB leakage and louder |
| Weak CPU + loudspeaker | WebRTC AEC3 | Cheapest canceller; accept the double-talk cost |
| NKF / LocalVQE | Experimental only | Not competitive / not yet measured |

This is guidance, not auto-detection. The app cannot reliably know
whether the output is a sealed earphone or a loudspeaker, so the exp
UI keeps the engine radios and shows the measured use-case line beneath
them.

## Caveats

- The harness runs the Python chain. Desktop C++ CPU and latency differ.
- One real-speech pair plus deterministic synthetic scenarios is enough
  to rank these engines, not to certify a room.
- The 48 kHz high-band splice is output fidelity; it is separate from
  the mic-cleanliness decision and is not measured here.
- AEC3 at 48 kHz is not in the web chain (16 kHz only). Re-measure on
  the desktop if the native 48 kHz path matters.
