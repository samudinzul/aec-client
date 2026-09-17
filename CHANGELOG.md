# Changelog

All notable changes to AEC Client are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] — 2026-09-17

### Added

- **DTLN-AEC 512 engine** — dual-signal LSTM echo + noise canceller
  (Westhausen & Meyer, ICASSP 2021, MIT)
  - New `src/dtln_wrapper.{h,cpp}`: 512-sample block / 128-sample shift /
    257-bin FFT pipeline with overlap-add, LSTM state carry-over, and
    int16 ↔ float bridging to the 160-sample audio callback
  - Dual backend probed in order: TFLite pair
    (`models/dtln_aec_512_1.tflite` + `_2.tflite` via `tensorflowlite_c.dll`
    loaded at runtime, no link-time dependency) then ONNX pair
    (`models/dtln_aec_512_1.onnx` + `_2.onnx` via the already-linked
    ONNX Runtime)
  - `ENGINE_DTLN = 4` wired through `ReinitEngine`, audio callback,
    `StartAEC` guard (`Failed to load DTLN model`), 16 kHz auto-lock,
    engine dropdown + tooltip, and shutdown cleanup
  - About tab now lists five engines and credits DTLN-AEC; old
    `aec_config.txt` engine indices are clamped on load
  - CMake copies any present `models/dtln_aec_512_*` files and an optional
    `libs/tensorflowlite_c.dll` next to `aec_gui.exe`; build succeeds with
    no DTLN files (engine reports missing models at Start)
- `APP_VERSION` bumped to `1.2.0`

### Notes

- DTLN runs at 16 kHz only; model files are user-supplied (see README
  “Download Models”) until vendored into a release
- `ldd` check: 0 `not found` after the change

## [1.1.1] — 2026-09-16

### Fixed

- Engine dropdown tooltip now documents all four engines (was missing LocalVQE v1.4-AEC)
- Sample-rate lock tooltip covers both 16 kHz-only engines (NKF-AEC / LocalVQE)
- Start / Reset to defaults footer (plus Live Levels) now shows on the Audio tab only;
  hidden on Appearance and About tabs

## [1.1.0] — 2026-09-16

### Added

- **LocalVQE v1.4-AEC engine** — compact neural echo canceller
  - Echo-only model: removes far-end echo while preserving voice, room tone, and background noise
  - 203K parameters, 2.8 MB model size
  - DAF (Deep Adaptive Filtering) front-end
  - Built-in residual noise gate (-45 dBFS default)
  - Very low CPU usage (~0.83 ms per 16 ms frame at 16 kHz)
  - Fixed 16 kHz sample rate (auto-locked when selected)
- About tab now lists four AEC engines and credits LocalVQE
- README updated with full technical stack, engine comparison table, and NKF-AEC / LocalVQE documentation
- Build automation: CMakeLists now copies all required runtime DLLs next to `aec_gui.exe` on every build
- LocalVQE diagnostic logging: `localvqe_diag.txt`, `localvqe_stdout.txt`, `localvqe_stderr.txt` written on launch for troubleshooting

### Changed

- Engine dropdown reordered for clarity: SpeexDSP, WebRTC AEC3, NKF-AEC, LocalVQE
- `EngineType` enum extended with `ENGINE_LOCALVQE = 3`
- Copyright holder updated to `samudinzul`

### Fixed

Multiple DLL bundling issues discovered when testing the release on a clean Windows PC (no MSYS2 installed):

- **Missing `glfw3.dll`** — GLFW window library was not bundled
- **Missing `libabsl_*.dll`** — Abseil runtime required by WebRTC AEC3
- **Missing ONNX Runtime dependencies** — `libonnx.dll`, `libprotobuf-lite.dll`, `libre2-11.dll`, `libutf8_validity.dll`
- **Missing `libgomp-1.dll`** — GNU OpenMP runtime required by all GGML CPU backends. Without it, LocalVQE failed with `Registered backends (0)` and refused to load
- **Missing `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`** — MinGW runtime dependencies

The release ZIP now ships **123 DLLs** and runs on a fresh Windows 10/11 installation with no developer toolchain.

### Notes

- LocalVQE runs at 16 kHz only
- Verified on two machines:
  - AMD Ryzen 5 5600 (dev machine)
  - AMD Ryzen 3 3200G (fresh Windows 11 installation)

## [1.0.0] — 2026-09-15

### Added

- Three AEC engines: WebRTC AEC3, SpeexDSP, NKF-AEC (experimental)
- Real-time audio processing with drift correction
- Selectable sample rate (16 / 32 / 48 kHz)
- Device filtering (hides virtual cables on Mic/Reference dropdowns)
- Presets for common scenarios (Discord, Low CPU, High Quality, Noisy Room)
- Live level meters with peak hold
- Optional system tray minimize
- Single-instance protection (mutex)
- Wallpaper customization
- Settings auto-save to `aec_config.txt`
- Built with Dear ImGui + GLFW + miniaudio

[1.1.1]: https://github.com/samudinzul/aec-client/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/samudinzul/aec-client/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/samudinzul/aec-client/releases/tag/v1.0.0