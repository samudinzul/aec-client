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

## Noise suppression

`tools/ns_eval.py` isolates the NS stage (engine Off, hygiene bypassed
unless `--full`) and drives one real near-end recording through three
deterministic beds at 15 dB below the speech-active RMS: steady pink
hiss, synthetic keyboard clicks, and three overlapping synthetic
talkers. STOI is clean near-end vs the delay-aligned output; "noise"
is noise-only output/input; "gap" is output/input during speech pauses.
Python RTF is relative, not desktop CPU.

Raw NS only, 10 s (`--near-wav /tmp/asr_3.flac --beds pink,keyboard,babble`):

| Bed | NS | noise dB | STOI | speech dB | gap dB | RTF |
|---|---|---:|---:|---:|---:|---:|
| pink | Off | −0.01 | 0.967 | 0.00 | −0.01 | 0.001 |
| pink | DTLN-NS | −7.47 | 0.963 | +1.35 | −3.91 | 0.030 |
| pink | Hush | −25.35 | 0.965 | −3.41 | −15.30 | 0.099 |
| pink | GTCRN | −11.28 | **0.969** | **−0.18** | −11.51 | 0.061 |
| keyboard | Off | −0.00 | 0.888 | 0.00 | −0.00 | 0.001 |
| keyboard | DTLN-NS | −0.02 | 0.883 | +1.31 | +0.25 | 0.031 |
| keyboard | Hush | −20.11 | 0.961 | −1.81 | −19.15 | 0.052 |
| keyboard | GTCRN | **−46.02** | **0.968** | **−0.27** | −16.99 | 0.060 |
| babble | Off | −0.01 | 0.973 | 0.00 | −0.01 | 0.001 |
| babble | DTLN-NS | +0.03 | 0.966 | +1.29 | −0.41 | 0.031 |
| babble | Hush | **−34.35** | 0.951 | −2.73 | **−22.22** | 0.097 |
| babble | GTCRN | −2.62 | 0.956 | −1.69 | −11.64 | 0.061 |

The shipped chain (`--full`, HPF/expander/presence on) moves the floors
much lower because the expander closes on the cleaner NS output: pink
noise −31 dB GTCRN / −82 dB Hush, keyboard −82 dB / −19 dB, babble
−3 dB / −82 dB. The −82 dB entries are the expander's digital black,
not the NS alone. Speech cost and STOI keep the same ordering.

The babble bed is intentionally adversarial and seed-sensitive. Across
four seeds, GTCRN STOI ranged **0.805–0.968**: when a background
talker overlaps the near-end, GTCRN can suppress part of the target
voice. Hush stayed **0.941–0.951** and DTLN-NS **0.963–0.968** across
the same seeds. Treat GTCRN's babble row as a warning, not a win.

GTCRN is not a black box here: the Python port was checked against the
upstream GTCRN `stream/test_wavs/enh.wav` on `mix.wav` (same 512/256
sqrt-Hann STFT, caches, model) — after delay alignment the waveform
correlation is 0.99999, and the frame-RMS distribution matches. The
desktop wrapper is the same algorithm. Hush numbers are the web ONNX
port, not the desktop DLL; verify character on the desktop build.

### NS recommendation

| Noise | Backend | Why |
|---|---|---|
| Quiet/treated room | Off | Zero CPU and no coloration; nothing to remove |
| Steady hiss/fan/air | **GTCRN (exp)** | Most transparent: −11 dB noise, −0.2 dB speech, best STOI (.969); Hush cuts more but ducks voice 3.4 dB |
| Keyboard/mouse clicks | **GTCRN (exp)** | −46 dB on clicks vs −20 dB Hush; best STOI (.968) and speech level |
| Background talkers | **Hush** | The only backend that consistently removes babble (−34 dB); GTCRN leaves it and can distort overlapping speech |
| Weakest CPU | DTLN-NS / Off | About half GTCRN's RTF, but DTLN-NS barely touches hiss, clicks or talkers |

For the confirmed natural-sounding chain (DTLN-AEC + 48 kHz splice at
100%), **GTCRN is the default NS backend in `aec_exp` when the noise is
hiss or keyboard; switch to Hush when background talkers dominate.**
DTLN-NS is the low-CPU fallback, not the quality choice. Do not switch
the AEC or the splice to chase an NS difference: all NS backends are
16 kHz, so they only act on the 0–8 kHz low band.

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
- The 48 kHz high-band splice is output fidelity; the engine ranking
  above still applies to its 0-8 kHz chain. The splice itself is not
  measured here. The exp build defaults to 48 kHz output on.
- AEC3 at 48 kHz is not in the web chain (16 kHz only). Re-measure on
  the desktop if the native 48 kHz path matters.
