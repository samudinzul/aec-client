# Changelog

## [1.1.0] — 2026-09-16

### Added
- **LocalVQE v1.4-AEC** — new neural echo-cancellation engine
  - Echo-only model (203K params, 2.8 MB)
  - Preserves voice, room tone, and background noise
  - Built-in residual noise gate (-45 dBFS default)
  - Very light CPU (~0.83 ms per 16 ms frame)
- About tab now lists four AEC engines and credits LocalVQE

### Notes
- LocalVQE runs at 16 kHz only (auto-locked when selected)
- Model is bundled in the release ZIP

## [1.0.0] — 2026-09-15

### Added
- Three AEC engines: WebRTC AEC3, SpeexDSP, NKF-AEC (experimental)
- Real-time audio processing with drift correction
- Selectable sample rate (16 / 32 / 48 kHz)
- Device filtering (hides virtual cables on Mic/Reference dropdowns)
- Presets for common scenarios
- Live level meters with peak hold
- Optional system tray minimize
- Wallpaper customization
- Settings auto-save to `aec_config.txt`
- Built with Dear ImGui + GLFW + miniaudio