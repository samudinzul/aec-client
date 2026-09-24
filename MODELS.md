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
| `silero_vad.onnx` | ~2.2 MB | Voice gate (Silero VAD v6.2.1, MIT) | [snakers4/silero-vad (raw)](https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx) |
| `dtln_aec_128_1.tflite` | ~1.9 MB | DTLN-AEC 128 (stage 1) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |
| `dtln_aec_128_2.tflite` | ~5.0 MB | DTLN-AEC 128 (stage 2) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |

## Runtime library (in `libs/`)

| File | Size | Purpose | Download from |
|------|------|---------|---------------|
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite C API runtime, loaded at runtime by the DTLN engine | Bundled with the [official AEC-Client releases](https://github.com/samudinzul/aec-client/releases) (no direct upstream Windows binary URL; extract it from any release ZIP) |

## Quick restore (MSYS2 UCRT64 bash, from repo root)

```bash
mkdir -p models

# Silero VAD voice gate v6.2.1 (~2.2 MB, MIT)
curl -L -o models/silero_vad.onnx \
  "https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"

# DTLN-AEC 128 pair (~1.9 MB + ~5.0 MB)
curl -L -o models/dtln_aec_128_1.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_1.tflite"
curl -L -o models/dtln_aec_128_2.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_2.tflite"

# NKF-AEC model ships with the repo backup (models/nkf.onnx).
# tensorflowlite_c.dll ships with the official release ZIPs (extract to libs/).
```

## Verification (SHA-256)

```
8d241b3a732af8ca140b2e30043e56a6c3c7800c46e22c26f4eea2f70974ad1e  dtln_aec_128_1.tflite
350bb01a1152ae3cabe09fe5e868ef2f7d8b988a9f22aae44f140195f6493126  dtln_aec_128_2.tflite
1d46987c5d3b4b7a555b054947fa4ae19e38999d3500cbb2cb10d8b9005f81d0  nkf.onnx
1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3  silero_vad.onnx  (matches upstream v6.2.1)
```

Check with: `sha256sum models/*`

## Notes

- Without the DTLN pair, the DTLN-AEC engine reports `Failed to load DTLN model`
  on Start; all other engines are unaffected. (Alternative: convert the pair to
  `models/dtln_aec_128_1.onnx` + `models/dtln_aec_128_2.onnx` — the build already
  links ONNX Runtime, so no extra DLL is needed. The release script currently
  expects the `.tflite` pair.)
- Without `silero_vad.onnx`, the voice gate reports "model not found" and audio
  passes through unchanged.
- The release script (`scripts/make-release.sh`) hard-requires the model files
  in its allowlist — a missing file aborts the release with `missing model: ...`.