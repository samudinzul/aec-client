# Library Downloads — AEC Client

All link-time / runtime binaries that are **not** installed via MSYS2 packages
live in `libs/`. (MSYS2-provided DLLs — WebRTC, ONNX Runtime, GLFW, MinGW
runtime, Abseil, etc. — are copied from `C:/msys64/ucrt64/bin` automatically by
the CMake post-build step; install them with `pacman` per `README.md`.)

## Files

| File | Size | Purpose | Download / restore from |
|------|------|---------|-------------------------|
| `aec.dll` | ~185 KB | SpeexDSP wrapper (classic DSP engine), loaded at runtime | Build from [thewh1teagle/aec](https://github.com/thewh1teagle/aec) (Rust), **or** extract from any [official AEC-Client release ZIP](https://github.com/samudinzul/aec-client/releases) |
| `libaec.a` | ~4 KB | MinGW import library for `aec.dll` (needed at **link time**) | Regenerate locally with `dlltool` (see below) — no download needed |
| `aec.def` | 165 B | Module-definition file listing `aec.dll` exports (input to `dlltool`) | Ships with the repo; regenerate with `gendef` (see below) |
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite C API runtime, loaded at runtime by the DTLN engine | Extract from any [official AEC-Client release ZIP](https://github.com/samudinzul/aec-client/releases) (upstream project is [tensorflow/tensorflow](https://github.com/tensorflow/tensorflow) Lite C API, which publishes no stable direct Windows binary URL) |

## How each file is used by the build (`CMakeLists.txt`)

- `libs/libaec.a` → linked into `aec_gui.exe` (`target_link_libraries`, line 65).
- `libs/aec.dll` → copied next to `aec_gui.exe` by the post-build step (line 95).
- `libs/tensorflowlite_c.dll` (or `libs/libtensorflowlite_c.dll`) → copied next to
  `aec_gui.exe` if present (lines 194–202); the DTLN wrapper loads it at runtime,
  no link-time dependency.

## Regenerating `libaec.a` / `aec.def` (MSYS2 UCRT64 bash, from repo root)

You only need `aec.dll` — everything else is derived:

```bash
# 1. Regenerate the .def from the DLL's actual exports
#    (writes ./aec.def in the current directory — move it into libs/)
gendef libs/aec.dll
mv aec.def libs/aec.def
# Expected exports: AecCancelEcho, AecDestroy, AecNew

# 2. Build the MinGW import library from the .def
#    (use absolute paths: dlltool is a native exe and misresolves
#    MSYS relative paths depending on the shell's CWD)
dlltool -D C:/aec-opencode/libs/aec.dll \
        -d C:/aec-opencode/libs/aec.def \
        -l C:/aec-opencode/libs/libaec.a
# The "Path components stripped from dllname" warning is benign —
# dlltool records just "aec.dll", which is what we want at runtime.
```

## Verification (SHA-256, v1.3.2)

```
e53da2d3960e2ff4de7787385c20890cf095ab4742b981441cba62799e38b8a1  aec.def
db1b211859d2e115e42dfb1a84683bff5409d628533fd4dd7d8ccfffccda75ec  aec.dll
5293d671c52bf97350f6701fdad9a8c59129e7603e65b43027c37dd0139e9715  libaec.a
882e6d8f9866ff84f23d4b964c145b7f0f0a8907fa830dcd8c499e7c46bf3365  tensorflowlite_c.dll
```

Check with: `sha256sum libs/*`

Note: `libaec.a` is a locally generated artifact — its hash will differ if you
regenerate it with a different binutils version. What matters is that it was
built from the `aec.dll` + `aec.def` above. (`aec.def` is the original 165-byte
CRLF file; a hand-written LF recreation is 155 bytes and functionally identical —
same three exports.)

## Quick restore (fresh clone is missing `libs/*.dll` + `libaec.a`)

```bash
# From an official release ZIP (all three binaries):
unzip -o -j AEC-Client-v1.3.2-win64.zip \
  "AEC-Client-v1.3.2-win64/aec.dll" \
  "AEC-Client-v1.3.2-win64/tensorflowlite_c.dll" -d libs/

# Then regenerate the import lib (commands above).
```

## Notes

- Without `libs/aec.dll` + `libs/libaec.a`, the top-level configure/link fails —
  the SpeexDSP engine has no system-package fallback (unlike WebRTC/ONNX/GLFW).
- Without `libs/tensorflowlite_c.dll`, everything still builds; only the
  DTLN-AEC engine is affected at runtime (it falls back to the ONNX model pair
  if present, else reports `Failed to load DTLN model`).
- See `MODELS.md` for the neural-model files in `models/`.
