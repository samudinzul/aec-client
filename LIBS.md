# Library Downloads — AEC Client

All link-time / runtime binaries that are **not** installed via MSYS2 packages
live in `libs/`. (MSYS2-provided DLLs — WebRTC, ONNX Runtime, GLFW, MinGW
runtime, Abseil, etc. — are copied from `C:/msys64/ucrt64/bin` automatically by
the CMake post-build step; install them with `pacman` per `README.md`.)

## Files

| File | Size | Purpose | Download / restore from |
|------|------|---------|-------------------------|
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite C API runtime, loaded at runtime by the DTLN engine | Extract from any [official AEC-Client release ZIP](https://github.com/samudinzul/aec-client/releases) (upstream project is [tensorflow/tensorflow](https://github.com/tensorflow/tensorflow) Lite C API, which publishes no stable direct Windows binary URL) |

## How each file is used by the build (`CMakeLists.txt`)

- `libs/tensorflowlite_c.dll` (or `libs/libtensorflowlite_c.dll`) → copied next to
  `aec_gui.exe` if present (post-build step); the DTLN wrapper loads it at runtime,
  no link-time dependency.

## Verification (SHA-256, v1.3.2)

```
882e6d8f9866ff84f23d4b964c145b7f0f0a8907fa830dcd8c499e7c46bf3365  tensorflowlite_c.dll
```

Check with: `sha256sum libs/*`

## Quick restore (fresh clone is missing `libs/tensorflowlite_c.dll`)

```bash
# From an official release ZIP:
unzip -o -j AEC-Client-v1.3.2-win64.zip \
  "AEC-Client-v1.3.2-win64/tensorflowlite_c.dll" -d libs/
```

## Notes

- Without `libs/tensorflowlite_c.dll`, everything still builds; only the
  DTLN-AEC engine is affected at runtime (it falls back to the ONNX model pair
  if present, else reports `Failed to load DTLN model`).
- See `MODELS.md` for the neural-model files in `models/`.