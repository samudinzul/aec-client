# exp/ — LocalVQE v1.4 experiment (desktop-only, never shipped)

Isolated playground for two candidate engines. Nothing here touches the
main app (`src/`, `web/`, `models/`, `scripts/make-release-*`): the release
scripts never see this folder, and the two smoke tools build
`EXCLUDE_FROM_ALL` (the default `cmake --build build` output is unchanged).

## Layout

| Path | What |
|------|------|
| `get_models.sh` | Downloads the three model files into `exp/models/` (gitignored) |
| `probe.py` | Dumps ONNX input/output names + shapes (run on the Windows box: `python exp/probe.py exp/models/<file>.onnx`) |
| `gtcrn/` | GTCRN NS wrapper (`Gtcrn*` C ABI, mirrors `src/hush_wrapper.h`) + `gtcrn_smoke` WAV harness |
| `localvqe/` | LocalVQE v1.4-AEC wrapper (`LocalVqe*` C ABI) + `localvqe_smoke` WAV harness |
| `resampler.h` | 193-tap linear-phase 48<->16 kHz polyphase FIR pair for the high-band splice |
| `resampler_test.cpp` | Offline DSP check for `resampler.h` (`resampler_test`, `EXCLUDE_FROM_ALL`) |
| `models/` | Downloaded weights (gitignored, never committed) |

## Models (all 16 kHz mono, all CPU-realtime)

| Engine | Role | File | Source | License |
|--------|------|------|--------|---------|
| GTCRN | NS (default; mic-only, no ref needed) | `gtcrn_simple.onnx` ~0.5 MB | [Xiaobin-Rong/gtcrn](https://github.com/Xiaobin-Rong/gtcrn) `stream/onnx_models/` | MIT |
| LocalVQE v1.4-AEC | AEC (mic + ref mandatory, echo-only) | `localvqe-v1.4-aec-200K-f32.gguf` ~3 MB | [LocalAI-io/LocalVQE](https://huggingface.co/LocalAI-io/LocalVQE) | Apache-2.0 |
| LocalVQE v1.4-AEC | same, portable ONNX mask | `LocalVQEAECResidualMask.onnx` ~1 MB + `LocalVQEAECFrontend.json` | [soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32](https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32) | Apache-2.0 |

Key contracts (from the model cards — read before touching the wrappers):

- **GTCRN** (`gtcrn_simple.onnx`): stateful streaming graph, one STFT frame
  per step: `mix[1,257,1,2]` + `conv_cache`/`tra_cache`/`inter_cache` in →
  `enh[1,257,1,2]` + updated caches out. ERB + subband features live inside
  the graph; only a sqrt-Hann STFT/ISTFT runs outside (NumPy/C++ side).
  FFT 512 / hop 256. RTF ~0.07 on i5-12400.
- **LocalVQE v1.4-AEC**: a *hybrid* canceller, not a waveform model. The ONNX
  file is only the residual mask (`[1,2,1,256]` residual + echo-estimate in →
  enhanced spectrum out, 7 state pairs); the host must run GCC-PHAT
  prealignment, a partitioned-block frequency-domain Kalman filter, the
  2,742-weight controller, sqrt-Hann STFT + overlap-add (see
  `LocalVQEAECFrontend.json`). Reference implementation:
  [soniqo/speech-core](https://github.com/soniqo/speech-core)
  (`OnnxLocalVQEEchoCanceller`). FFT 512 / hop 256 (16 ms).

## Status

- **GTCRN: default NS backend (V1 neural path live).** `GtcrnProcess` runs sqrt-Hann STFT →
  `Run(mix + carried caches)` → ISTFT overlap-add per 256-sample hop
  (reference: sherpa-onnx `offline-speech-denoiser-gtcrn-impl.h`).
  Expect on the synth clip: hiss sections drop, voice bursts stay near
  input level. Any `Run` failure latches to passthrough (never mute).
  Offline comparison against DTLN-NS/Hush (`tools/ns_eval.py`, engine
  Off, real speech plus hiss/keyboard/babble at 15 dB): GTCRN is the
  most transparent on hiss and keyboard (−11 / −46 dB noise, speech
  within 0.3 dB, best STOI), Hush is the only backend that reliably
  removes background talkers (−34 dB) but ducks speech 2–3 dB and costs
  the most, and DTLN-NS is the cheapest yet barely touches clicks or
  talkers. Pick GTCRN for hiss/keyboard, Hush for talkers, DTLN-NS for
  the weakest CPU; Off in a quiet room. Full tables and caveats:
  `tools/AEC_EVAL.md`. Selecting NS with the splice off turns 48 kHz
  output back on automatically; with every stage off, Off/AEC3 chains
  run natively at 48 kHz. The NS itself still runs at 16 kHz.
- **LocalVQE: still v0 plumbing** (files validated, mic passthrough until
  the Kalman host DSP is ported — see contract above).
- **High-band splice (48 kHz output, default ON):** the other route to
  48 kHz — capture the mic at 48 kHz, run the 16 kHz engine on the low
  band, and splice the REAL 8–24 kHz band from the original mic onto
  the output. No synthesis, so no metallic edge. The chain itself stays
  16 kHz (every NS backend is 16 kHz); only the output is 48 kHz. The
  16 kHz NS models cannot reach above 8 kHz, so a broadband transient
  (keyboard click) would come back with its sharp part intact: a
  dedicated **high-band NS** (STFT spectral subtraction with a
  decision-directed a priori SNR, over the 8–24 kHz bins) removes it,
  and a VAD closes the high band while the chain says "no voice".
  Measured on real speech with a realistic keyboard click: click
  −14.2 dB, hiss −8.9 dB (−41.6 dB with the VAD), voice −1 to −3 dB.
  Design + all measurements (including the rejected alternatives):
  `exp/HB_NS_DESIGN.md`.
- **Splice resamplers: fixed.** The old 3-tap decimator (12 kHz folded
  to 4 kHz at only −9.5 dB, −3.5 dB droop at 8 kHz) and linear
  upsampler (7 kHz mirrored to 9 kHz at −4 dB, top droop −6 dB) are
  replaced by a 193-tap linear-phase FIR pair: flat to 7.5 kHz, aliases −76 dB,
  images −55 dB, group delay exactly 32 @16 kHz per direction. The
  loopback reference is delayed by the same 32 samples. The high-band
  mix is a live slider (0–100%, fresh default 100% — the confirmed
  natural chain; config slot 32, gen 10+), so the cleaner low band can
  be paired with more air or pulled back if hiss bothers. Turning the
  splice off returns to the native chain rate — 48 kHz for Off/AEC3
  with NS off, 16 kHz when NS must run. `resampler_test`
  covers the DSP offline.

## Usage (MSYS2 UCRT64, from repo root)

```bash
bash exp/get_models.sh
python exp/probe.py exp/models/gtcrn_simple.onnx
cmake -B build -G Ninja && cmake --build build --target gtcrn_smoke localvqe_smoke
build/gtcrn_smoke.exe mic.wav out.wav
build/localvqe_smoke.exe mic.wav ref.wav out.wav
build/gtcrn_smoke.exe --synth 5 out.wav   # no WAVs needed
cmake --build build --target resampler_test && build/resampler_test.exe
```

WAVs must be 16 kHz mono int16. Both tools never mute: missing/broken model
→ passthrough + non-zero exit note (same fail-open contract as DTLN-NS).
