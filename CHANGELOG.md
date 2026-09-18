# Changelog

All notable changes to AEC Client are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Output auto-selects CABLE Input.** Fresh installs (or a saved
  device that disappeared) land on CABLE Input instead of index 0;
  plus a **Listen to myself** tick that routes the cleaned mic to
  your Speaker Reference device for monitoring (nothing goes to
  VB-CABLE). Choice is saved; status shows `[monitor]` while active.
- **VB-CABLE missing warning.** When no CABLE device is present, the
  Devices section shows a red hint (install/enable + Refresh), and
  Start refuses with a status message unless Listen-to-myself is on.
  (Windows lists only enabled devices, so disabled looks the same as
  not installed — the hint covers both.)

### Changed

- **Voice gate is now pure neural VAD.** The adaptive close line
  (noise floor + 0.15) is nuked: fixed hysteresis, open at 0.50,
  close at 0.30 — the uncertain band holds the last decision, so
  quiet speech is never cut by a drifting close line. Hangover,
  fades, and fail-open bypass unchanged.
- **Noisy Room preset no longer enables Speex cleanup.** Speex
  preprocess stacks with the voice gate (double suppression, sounds
  aggressive); the gate alone does the silencing now. The cleanup
  tooltip warns about the stacking.

## [1.3.1] — 2026-09-18

- **Download:** [AEC-Client-v1.3.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.3.1/AEC-Client-v1.3.1-win64.zip) (Windows 10/11 64-bit)

### Fixed

- **Second launch now brings the app window forward.** The
  single-instance lookup used a hardcoded "v1.0.0" window title, so on
  every later version the duplicate exited without focusing the
  running window. The title is now built from the app version.

## [1.3.0] — 2026-09-18

- **Download:** [AEC-Client-v1.3.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.3.0/AEC-Client-v1.3.0-win64.zip) (Windows 10/11 64-bit)

### Added

- **Voice gate (Silero VAD → adaptive gate → output)** — post-AEC
  neural speech detector (snakers4/silero-vad v6.2.1, MIT, ~2.2 MB,
  sha256-verified): speech passes, silence is muted. No setup, no
  recording, ON by default (one checkbox to disable, choice saved)
  - New `src/silero_wrapper.{h,cpp}`: 512-sample streaming chunks
    with 64-sample context (upstream recipe), LSTM state carried
    across chunks, reset on Start/Stop/engine change; already-linked
    ONNX Runtime, no new DLL; < 1 ms per chunk, runs inline on the
    audio thread — no worker, 32 ms cadence
  - Adaptive gate: fixed 0.5 open line with a noise-floor-tracking
    close line (+0.15, clamped) against flutter; ~30 ms fade open,
    ~50 ms fade to hard mute, 300 ms hangover against clipping tails
  - 16 and 48 kHz live (model is 16 kHz fixed; at 48 kHz a
    miniaudio downsample feeds only the detector — proven
    0.945/0.008 vs native 0.956/0.010 speech/noise, so audio stays
    full-rate with no accuracy cost); green **SPEAKING** / grey
    **SILENT** header pill + live probability readout in the Audio tab

### Removed

- **32 kHz sample rate** — rates are now 16 / 48 kHz only: the
  dropdown, `Noisy Room` preset (now 48 kHz), and saved configs
  (old 32 kHz entries land on 48 kHz automatically) all migrated

## [1.2.1] — 2026-09-17

- **Download:** [AEC-Client-v1.2.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.2.1/AEC-Client-v1.2.1-win64.zip) (Windows 10/11 64-bit)

### Fixed

- **LocalVQE word endings no longer clipped.** The SDK noise gate
  (`-45 dBFS`, always on) was muting low-energy speech tails, so words
  sounded cut off — most audible during double-talk. The gate is now off;
  the joint network suppresses background noise on its own
  (`src/localvqe_wrapper.cpp`). If background hiss in silence bothers you,
  the fallback is re-enabling the gate at `-60 dBFS` (same line).

## [1.2.0] — 2026-09-17

- **Download:** [AEC-Client-v1.2.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.2.0/AEC-Client-v1.2.0-win64.zip) (66 MB, Windows 10/11 64-bit)

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