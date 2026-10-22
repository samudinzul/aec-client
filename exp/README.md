# exp/ — GTCRN + LocalVQE v1.4 experiments (desktop-only, never shipped)

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
| `novasr/` | NovaSR super-resolution proof (`novasr_smoke` WAV harness + versioned `novasr.onnx`) |
| `models/` | Downloaded weights (gitignored, never committed) |

## Models (all 16 kHz mono, all CPU-realtime)

| Engine | Role | File | Source | License |
|--------|------|------|--------|---------|
| GTCRN | NS (mic-only, no ref needed) | `gtcrn_simple.onnx` ~0.5 MB | [Xiaobin-Rong/gtcrn](https://github.com/Xiaobin-Rong/gtcrn) `stream/onnx_models/` | MIT |
| LocalVQE v1.4-AEC | AEC (mic + ref mandatory, echo-only) | `localvqe-v1.4-aec-200K-f32.gguf` ~3 MB | [LocalAI-io/LocalVQE](https://huggingface.co/LocalAI-io/LocalVQE) | Apache-2.0 |
| LocalVQE v1.4-AEC | same, portable ONNX mask | `LocalVQEAECResidualMask.onnx` ~1 MB + `LocalVQEAECFrontend.json` | [soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32](https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32) | Apache-2.0 |
| NovaSR | super-resolution (16 kHz in → 48 kHz out, whole-file) | `novasr.onnx` 208 KB (baked from `pytorch_model_v1.bin` 53 KB) | [YatharthS/NovaSR](https://huggingface.co/YatharthS/NovaSR) ([code](https://github.com/ysharma3501/NovaSR)) | Apache-2.0 |

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

- **GTCRN: V1 neural path live.** `GtcrnProcess` runs sqrt-Hann STFT →
  `Run(mix + carried caches)` → ISTFT overlap-add per 256-sample hop
  (reference: sherpa-onnx `offline-speech-denoiser-gtcrn-impl.h`).
  Expect on the synth clip: hiss sections drop, voice bursts stay near
  input level. Any `Run` failure latches to passthrough (never mute).
- **LocalVQE: still v0 plumbing** (files validated, mic passthrough until
  the Kalman host DSP is ported — see contract above).
- **NovaSR: proof harness ready.** `novasr.onnx` (baked from the upstream
  `.bin`, torch/ORT parity 1.000000, ~11x CPU RTF single-thread,
  chunked-halo equivalence 0.999998) synthesizes REAL 8–16 kHz highs
  (measured +12 dB band vs input on rich voice; silence floor −42 dBFS,
  gated downstream). No live wiring yet — `novasr_smoke` A/B first.
- **NovaSR live tail (checkbox):** `aec_exp` runs the chain at 16 kHz
  and the output + monitor devices at 48 kHz through `src/novasr_wrapper`
  (overlap-discard streaming, fixed 8 ms lookahead, per-tick fail-open
  to linear upsample). Fail-closed at Start without the staged model;
  ms/tick telemetry in the UI. Official `aec_gui` untouched.
- **High-band splice (checkbox):** the other route to 48 kHz — capture
  the mic at 48 kHz, run the 16 kHz engine on the low band, and splice
  the REAL 8–24 kHz band from the original mic onto the output. No
  synthesis, so no metallic edge. The 16 kHz NS models cannot reach
  above 8 kHz, so a broadband transient (keyboard click) would come back
  with its sharp part intact: a dedicated **high-band NS** (STFT
  spectral subtraction with a decision-directed a priori SNR, over the
  8–24 kHz bins) removes it, and a VAD closes the high band while the
  chain says "no voice". Measured on real speech with a realistic
  keyboard click: click −14.2 dB, hiss −8.9 dB (−41.6 dB with the
  VAD), voice −1 to −3 dB. Design + all measurements (including the
  rejected alternatives): `exp/HB_NS_DESIGN.md`.

## Usage (MSYS2 UCRT64, from repo root)

```bash
bash exp/get_models.sh
python exp/probe.py exp/models/gtcrn_simple.onnx
cmake -B build -G Ninja && cmake --build build --target gtcrn_smoke localvqe_smoke
build/gtcrn_smoke.exe mic.wav out.wav
build/localvqe_smoke.exe mic.wav ref.wav out.wav
build/gtcrn_smoke.exe --synth 5 out.wav   # no WAVs needed
# NovaSR super-resolution proof (its weights are versioned in-tree):
cmake --build build --target novasr_smoke
build/novasr_smoke.exe out16.wav nov48.wav
build/novasr_smoke.exe --synth 5 nov48.wav   # no WAVs needed
```

WAVs must be 16 kHz mono int16. Both tools never mute: missing/broken model
→ passthrough + non-zero exit note (same fail-open contract as DTLN-NS).
