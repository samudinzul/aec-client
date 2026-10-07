# Model Downloads — AEC Client

All neural-engine model files live in `models/` (required at **build and run time**:
the CMake post-build step copies them next to `aec_gui.exe`, and
`scripts/make-release.sh` bundles them into the release ZIP).
`tensorflowlite_c.dll` lives in `libs/`.

Sizes and SHA-256 hashes below were verified for the files currently in the tree.

## Required models

| File | Size | Engine | Download from |
|------|------|--------|---------------|
| `nkf.onnx` | ~45 KB | NKF-AEC | Bundled with this repo (simplified with onnxsim; upstream: [fjiang9/NKF-AEC](https://github.com/fjiang9/NKF-AEC), via [REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC)) |
| `dtln_aec_128_1.tflite` | ~1.9 MB | DTLN-AEC 128 (stage 1) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |
| `dtln_aec_128_2.tflite` | ~5.0 MB | DTLN-AEC 128 (stage 2) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |
| `dtln_ns_128_1.tflite` | ~1.5 MB | DTLN noise reduction (stage 1) | [networkedaudio/Realtime_AudioDenoise_EchoCancellation](https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation) |
| `dtln_ns_128_2.tflite` | ~2.5 MB | DTLN noise reduction (stage 2) | same |

The DTLN-NR pair is the same 512-block / 128-shift / 257-bin DSP as the
AEC pair, minus the loud-playback feed: `model_1 [mag(257), states] ->
[mask(257), states]`, `model_2 [est(512), states] -> [block(512), states]`.
It runs after the engine on every path (WebRTC NS is retired).

Retired: `silero_vad.onnx` (voice gate) is gone; `gtcrn_stream.onnx`
(the old NKF "dry voice" stage) and the `dfn/` graphs (DeepFilterNet,
evaluated pre-release) were cut before that line shipped, and the
model file itself is deleted. The retired WPE / notch post stages
were model-free in-tree DSP — no files to download.

## Runtime library (in `libs/`)

| File | Size | Purpose | Download from |
|------|------|---------|---------------|
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite C API runtime, loaded at runtime by the DTLN engine | Bundled with the [official AEC-Client releases](https://github.com/samudinzul/aec-client/releases) (no direct upstream Windows binary URL; extract it from any release ZIP) |

## Quick restore (MSYS2 UCRT64 bash, from repo root)

```bash
mkdir -p models

# DTLN-AEC 128 pair (~1.9 MB + ~5.0 MB)
curl -L -o models/dtln_aec_128_1.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_1.tflite"
curl -L -o models/dtln_aec_128_2.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_2.tflite"

# DTLN noise reduction pair (~1.5 MB + ~2.5 MB) — runs after the engine
# on the DTLN and WebRTC AEC3 paths (WebRTC NS is retired).
curl -L -o models/dtln_ns_128_1.tflite \
  "https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation/raw/master/model/model_1.tflite"
curl -L -o models/dtln_ns_128_2.tflite \
  "https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation/raw/master/model/model_2.tflite"

# NKF-AEC model ships with the repo backup (models/nkf.onnx).
# tensorflowlite_c.dll ships with the official release ZIPs (extract to libs/).
```

## Verification (SHA-256)

```
8d241b3a732af8ca140b2e30043e56a6c3c7800c46e22c26f4eea2f70974ad1e  dtln_aec_128_1.tflite
350bb01a1152ae3cabe09fe5e868ef2f7d8b988a9f22aae44f140195f6493126  dtln_aec_128_2.tflite
91281a38e80fe9fd330e28eda7e16fe4e483ee5199a3e687a099939013c25de0  dtln_ns_128_1.tflite
7ae37ec802862d8a65b5cdabfbcbbe22caaf7cd39e79adf574d15837d1520830  dtln_ns_128_2.tflite
1d46987c5d3b4b7a555b054947fa4ae19e38999d3500cbb2cb10d8b9005f81d0  nkf.onnx
```

Check with: `sha256sum models/*`

## Notes

- Without the DTLN pair, the DTLN profile (Voice Isolation) reports
  `Failed to load DTLN model` on Start; the other profiles are unaffected.
- The release script (`scripts/make-release.sh`) hard-requires the model files
  in its allowlist — a missing file aborts the release with `missing model: ...`.
