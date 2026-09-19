# Model Downloads — AEC Client

All neural-engine model files live in `models/` (required at **build and run time**:
the CMake post-build step copies them next to `aec_gui.exe`, and
`scripts/make-release.sh` bundles them into the release ZIP).
`tensorflowlite_c.dll` lives in `libs/`.

Sizes and SHA-256 hashes below were verified against the v1.3.2 release
(plus the experimental Unreleased addition: ECAPA).

## Required models

| File | Size | Engine | Download from |
|------|------|--------|---------------|
| `nkf.onnx` | ~45 KB | NKF-AEC | Bundled with this repo (simplified with onnxsim; upstream: [fjiang9/NKF-AEC](https://github.com/fjiang9/NKF-AEC), via [REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC)) |
| `localvqe-v1.4-aec-200K-f32.gguf` | ~2.8 MB | LocalVQE v1.4-AEC | [LocalAI-io/LocalVQE on Hugging Face](https://huggingface.co/LocalAI-io/LocalVQE/resolve/main/localvqe-v1.4-aec-200K-f32.gguf) |
| `silero_vad.onnx` | ~2.2 MB | Voice gate (Silero VAD v6.2.1, MIT) | [snakers4/silero-vad (raw)](https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx) |
| `dtln_aec_512_1.tflite` | ~17.4 MB | DTLN-AEC 512 (stage 1) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |
| `dtln_aec_512_2.tflite` | ~24.2 MB | DTLN-AEC 512 (stage 2) | [breizhn/DTLN-aec pretrained_models](https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models) |

## Optional models (experimental features, Unreleased)

Missing files degrade gracefully (see Notes) — the build works without them.

| File | Size | Feature | Download from |
|------|------|---------|---------------|
| `ecapa-speaker-v1.onnx` | ~83 MB | "Only my voice" owner gate (ECAPA embedding, Apache-2.0) | [vedk00/ecapa-voxceleb-speaker-embedding-onnx](https://huggingface.co/vedk00/ecapa-voxceleb-speaker-embedding-onnx/resolve/main/model/ecapa-speaker-v1.onnx) |

## Runtime library (in `libs/`)

| File | Size | Purpose | Download from |
|------|------|---------|---------------|
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite C API runtime, loaded at runtime by the DTLN engine | Bundled with the [official AEC-Client releases](https://github.com/samudinzul/aec-client/releases) (no direct upstream Windows binary URL; extract it from any release ZIP) |

## Quick restore (MSYS2 UCRT64 bash, from repo root)

```bash
mkdir -p models

# LocalVQE v1.4-AEC (2.8 MB)
curl -L -o models/localvqe-v1.4-aec-200K-f32.gguf \
  "https://huggingface.co/LocalAI-io/LocalVQE/resolve/main/localvqe-v1.4-aec-200K-f32.gguf"

# Silero VAD voice gate v6.2.1 (~2.2 MB, MIT)
curl -L -o models/silero_vad.onnx \
  "https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"

# DTLN-AEC 512 pair (~17 MB + ~24 MB)
curl -L -o models/dtln_aec_512_1.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_512_1.tflite"
curl -L -o models/dtln_aec_512_2.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_512_2.tflite"

# NKF-AEC model ships with the repo backup (models/nkf.onnx).
# tensorflowlite_c.dll ships with the official release ZIPs (extract to libs/).

# ECAPA speaker embedding for "Only my voice" (~83 MB, Apache-2.0)
curl -L -o models/ecapa-speaker-v1.onnx \
  "https://huggingface.co/vedk00/ecapa-voxceleb-speaker-embedding-onnx/resolve/main/model/ecapa-speaker-v1.onnx"
```

## Verification (SHA-256 — five v1.3.2 files plus Unreleased additions)

```
569f7c3cfac96b1e093229c3ca10b5d892f5b1906105b644e457f8245b4f7383  dtln_aec_512_1.tflite
fb423d867ab25d5f4716bd369c7126b6c84926175c019b832e71bf21de0e9907  dtln_aec_512_2.tflite
b6e43138588a83bfe903ab5e143b4020b91c1e1629f5a575ac5855ff0003c731  localvqe-v1.4-aec-200K-f32.gguf
1d46987c5d3b4b7a555b054947fa4ae19e38999d3500cbb2cb10d8b9005f81d0  nkf.onnx
1a153a22f4509e292a94e67d6f9b85e8deb25b4988682b7e174c65279d8788e3  silero_vad.onnx  (matches upstream v6.2.1)
f46380bbaeddb929fb3a10ab63a4b1877a50e3d1e5fdd55a1b618d5651d3f64e  ecapa-speaker-v1.onnx
```

Check with: `sha256sum models/*`

## Notes

- Without the DTLN pair, the DTLN-AEC engine reports `Failed to load DTLN model`
  on Start; all other engines are unaffected. (Alternative: convert the pair to
  `models/dtln_aec_512_1.onnx` + `models/dtln_aec_512_2.onnx` — the build already
  links ONNX Runtime, so no extra DLL is needed. The release script currently
  expects the `.tflite` pair.)
- Without `silero_vad.onnx`, the voice gate reports "model not found" and audio
  passes through unchanged.
- Without `ecapa-speaker-v1.onnx`, "Only my voice" reports the voice model
  as missing and the owner check stays unavailable; everything else works.
- `models/voiceprint.bin` is generated locally by "Learn my voice" — never
  committed, never shipped, deletable via "Forget my voice".
- The release script (`scripts/make-release.sh`) hard-requires the five
  required model files — a missing one aborts the release. The four
  experimental files are intentionally outside the allowlist and never ship.
- The release script (`scripts/make-release.sh`) hard-requires all five model
  files — a missing file aborts the release with `missing model: ...`.
