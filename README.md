# AEC Client

> Real-time acoustic echo cancellation for Windows — use speakers and a microphone at the same time in Discord, Zoom, Teams, or any other voice app.

![AEC Client screenshot](screenshots/main.png)

**Built entirely with open-source tools.** No proprietary SDKs. No cloud dependencies. No telemetry.

---

## Table of Contents

- [What It Does](#what-it-does)
- [Features](#features)
- [Quick Start](#quick-start)
- [Voice Gate](#voice-gate)
- [Engine Comparison](#engine-comparison)
- [Runtime Dependencies](#runtime-dependencies)
- [Architecture](#architecture)
- [Technical Stack](#technical-stack)
- [About the Neural Engines](#about-the-neural-engines)
- [Building from Source](#building-from-source)
- [Project Structure](#project-structure)
- [License](#license)
- [Credits](#credits)
- [Contributing](#contributing)
- [Support](#support)

---

## What It Does

AEC Client routes your microphone through one of **three acoustic echo cancellation engines**, subtracts the sound coming from your speakers, and outputs a clean, echo-free signal to a virtual audio cable. Any voice app can then use that clean signal as its microphone input.

```
Microphone ─────────────────┐
                             ├─→ AEC Engine ─→ Voice gate ─→ VB-CABLE ─→ Discord
Speakers (loopback) ────────┘                   (speech passes, silence pushed down)
```

No more headphones. No more echo. No dead-air noise.

---

## Features

- **3 selectable AEC engines** in one app:
  - **DTLN-AEC 128** — recommended default: dual-LSTM echo + noise canceller (ICASSP 2021; 128 LSTM units for lower CPU)
  - **WebRTC AEC3** — strongest echo suppression, same engine used by Chrome and Google Meet (voice sounds processed)
  - **NKF-AEC** — lightest neural Kalman filter (ICASSP 2023; can distort if loopback delay drifts)
- **Noise reduction** (AEC3 and NKF-AEC) — optional WebRTC noise suppression on top of echo cancellation, one checkbox
- **Residual echo kill** (NKF-AEC) — post-NKF WebRTC AEC3 pass, default ON; untick to hear raw NKF
- **Dry voice** (NKF-AEC) — optional GTCRN stage for less room reverb (off by default; ~32 ms extra delay)
- **Low CPU usage** — under 2% on a typical desktop
- **Low latency** — 30–40 ms round-trip
- **Works with any audio device** — speakers, earphones, headsets
- **Selectable sample rate** (16 / 48 kHz)
- **Optional system tray** — runs in the background like a real utility
- **Device filtering** — virtual cables hidden from Mic/Reference dropdowns
- **Live level meters** with peak-hold
- **Presets** for common scenarios (Discord, Echo-Heavy Room, Noisy Room)
- **Clock-drift correction** — stable over long calls
- **Voice gate** — a tiny neural network pushes silence down (−12 dB), passes speech (16/48 kHz; **off by default** — enable under Advanced → Voice gate, then optional one-tap mic calibration)
- **Wallpaper customization**
- **Auto-save settings** to `aec_config.txt`
- **Single-instance protection** — launching twice brings the existing window to front

---

## Quick Start

1. **Install [VB-CABLE](https://vb-audio.com/Cable/)** (free virtual audio cable). Reboot.
2. **Download** the latest release from [Releases](https://github.com/samudinzul/aec-client/releases/latest) and extract it anywhere.
3. **Run `aec_gui.exe`**.
4. **Select your devices** (first run shows a 3-step checklist):
   - **Your microphone** → your physical mic
   - **Your speakers** → your physical speakers
   - **Send cleaned sound to** → `CABLE Input (VB-Audio Virtual Cable)` (picked automatically)
5. **Pick an engine** under Advanced — DTLN-AEC is the default; AEC3 is the strongest echo pick; NKF-AEC is the lightest. Optionally tick **Noise reduction** on AEC3 or NKF-AEC. On NKF you can also use **Residual echo kill** (default ON) and **Dry voice** (optional GTCRN dereverb).
6. **Click Start**. Keep speakers at a moderate volume — very loud
   speakers make any canceller mistake your voice for echo (AEC3 will
   cut you mid-sentence in double-talk). If the room must be loud,
   use DTLN-AEC instead.
7. **In Discord** → Voice & Video settings:
   - **Input Device**: `CABLE Output (VB-Audio Virtual Cable)`
   - **Input Profile**: **Voice Isolation** — one tap that turns on
      Discord's noise cleanup. Echo is already removed by this app
      (DTLN and the optional **Noise reduction** checkbox also remove
      noise; bare AEC3/NKF-AEC are echo-only — which is exactly why
      Discord's cleanup stays useful).
   - Prefer manual control? Pick the **Custom** profile instead:
     Echo Cancellation **OFF** (this app does it — two cancellers
     stacked fight each other), Noise Suppression **Krisp**
     (**Standard** on weak PCs), Automatic Gain Control **OFF**.

Done. Talk normally with speakers on.

> **SmartScreen on first launch?** The app is unsigned (no paid
> code-signing certificate), so Windows may show **"Windows protected
> your PC"**. This is expected: click **More info → Run anyway**.
> Every release is built straight from the public source in this repo
> — audit it, rebuild it, or scan the ZIP on VirusTotal if unsure.

---

## Voice Gate

After echo cancellation, a tiny neural network checks for speech many times a second. Speech passes to Discord; silence is pushed down (−12 dB, not muted — so the level never pumps). **Off by default** — tick **Push down silence** under Advanced → Voice gate to enable. No recording; one-tap calibration optional.

- Green **SPEAKING** pill at the top = speech going out (only when the gate is on). Grey **SILENT** = pushed down.
- It hears *any* speech, not just yours — it sits after the echo canceller, so what's left is overwhelmingly your voice.
- Uncheck **Push down silence** in the Audio tab to pass original audio through.
- **Calibrate for my mic** (under Voice gate, idle or running): starts audio if needed (you'll hear yourself, live meters, no CABLE needed), you speak normally 5 s, then back to idle — press Start to use it. Sets the speech/silence lines for your mic + engine combo. Re-calibrate after switching mic or engine; Reset restores defaults.
- Works at **16 and 48 kHz** (the detector itself is 16 kHz fixed; at 48 kHz an internal downsample feeds only the detector, your audio stays full-rate) — NKF and DTLN run at 16000 (locked); AEC3 offers 16000 or 48000.

---

## Engine Comparison

| Engine | Your voice | Echo removed | CPU | When to use it |
|--------|------------|--------------|-----|----------------|
| **DTLN-AEC 128** *(default)* | Sounds natural | Very good | Moderate | **Start here.** Cleanest overall; also removes background noise. |
| **WebRTC AEC3** | Can sound a bit processed | Strongest | Moderate | Echo still getting through (loud speakers, echoey room). |
| **NKF-AEC** | Usually natural | Good | Lowest | Older or busy PC — lightest on the CPU. |

**Voice** = how natural you sound when both sides talk at once. **Echo** = how much of the other person’s speaker sound is cancelled out.

**Quick pick:**
1. Stay on **DTLN** unless something is wrong.
2. Still hearing echo? Try **AEC3** (strongest cancellation; voice may sound cleaned up).
3. CPU too high? Try **NKF** (lightest; can glitch if speaker delay drifts).

DTLN also cleans noise by itself. AEC3 and NKF can add noise removal with the **Noise reduction** checkbox. Keep speaker volume moderate — very loud speakers make any engine treat your voice as echo.

---

## Runtime Dependencies

The release ZIP bundles all required DLLs:

| File | Size | Purpose |
|------|------|---------|
| `aec_gui.exe` | ~2.4 MB | Main application |
| `libnkf_aec.dll` | ~1.3 MB | NKF neural engine |
| `libwebrtc-audio-processing-1-3.dll` | ~950 KB | WebRTC AEC3 + noise suppression |
| `onnxruntime.dll` | ~28 MB | Neural inference (NKF, DTLN ONNX fallback, Silero, GTCRN) |
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite runtime for DTLN (primary path) |
| `libwinpthread-1.dll` | ~63 KB | MinGW thread runtime |
| `libgcc_s_seh-1.dll` | ~150 KB | GCC runtime |
| `libstdc++-6.dll` | ~2.6 MB | C++ standard library |
| `models/nkf.onnx` | ~45 KB | NKF model |
| `models/dtln_aec_128_{1,2}.tflite` | ~7 MB | DTLN-AEC 128 pair (default engine) |
| `models/silero_vad.onnx` | ~2.2 MB | Voice gate (Silero VAD) |
| `models/gtcrn_stream.onnx` | ~0.5 MB | NKF Dry voice stage (GTCRN) |

**External dependency (user-installed):** [VB-CABLE](https://vb-audio.com/Cable/)

---

## Architecture

How audio flows through the app:

```
Your microphone  ─────────┐
                          ├─→  AEC Engine  ─→  (optional voice gate)  ─→  VB-CABLE  ─→  Discord / Zoom
Your speakers  ───────────┘         removes echo
(loopback = what you hear)
```

1. **Capture** — the mic and the speaker output (loopback) are read at the same time.
2. **Cancel** — the engine subtracts the speaker sound from the mic, leaving your voice.
3. **Optional gate** — if you turn on the voice gate, quiet parts are pushed down so silence does not go out.
4. **Deliver** — the clean signal goes to VB-CABLE; Discord uses that as its microphone.

Settings are saved automatically. Only one copy of the app runs at a time (opening it again focuses the existing window).

---

## Technical Stack

### Languages

| Language | Role |
|----------|------|
| **C++17** | Main application |
| **C** | Third-party libraries (miniaudio, stb_image, GGML via NKF) |
| **CMake** | Build system |
| **Bash** | Build scripts |
| **Markdown** | Documentation |

### Build Toolchain

| Tool | Version | Purpose |
|------|---------|---------|
| **GCC (MinGW-w64)** | 16.2.0 | C/C++ compiler |
| **CMake** | 3.20+ | Build system generator |
| **Ninja** | — | Fast build backend |
| **ccache** | — | Compile caching |
| **gendef / dlltool** | — | Generate MinGW import libraries |
| **MSYS2 UCRT64** | — | Build environment |

### AEC Engines

| Engine | Type | Language | Source |
|--------|------|----------|--------|
| **WebRTC AEC3** | Advanced DSP | C++ | [MSYS2 package](https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-webrtc-audio-processing-1) |
| **NKF-AEC** | Neural (Kalman) | C++ | [William1617/REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC) |
| **DTLN-AEC** | Neural (dual-LSTM) | C++ | [breizhn/DTLN-aec](https://github.com/breizhn/DTLN-aec) |

### Libraries

| Library | Language | Purpose |
|---------|----------|---------|
| **[miniaudio](https://github.com/mackron/miniaudio)** | C (single-header) | Cross-platform audio I/O |
| **[Dear ImGui](https://github.com/ocornut/imgui)** | C++ | Immediate-mode GUI |
| **[GLFW](https://www.glfw.org/)** | C | Window + OpenGL context |
| **[OpenGL](https://www.opengl.org/)** | — | Rendering backend |
| **[stb_image](https://github.com/nothings/stb)** | C (single-header) | Image loading for wallpapers |
| **[ONNX Runtime](https://onnxruntime.ai/)** | C++ | Neural inference (NKF-AEC, DTLN-AEC ONNX fallback, Silero VAD, GTCRN) |
| **[pocketfft](https://github.com/mreineck/pocketfft)** | C++ (header) | FFT for NKF-AEC and DTLN-AEC |
| **[AudioFile](https://github.com/adamstark/AudioFile)** | C++ (header) | WAV I/O for NKF-AEC |

### Windows APIs

| API | Purpose |
|-----|---------|
| **WASAPI** (via miniaudio) | Audio capture, loopback, playback |
| **Win32** (`windows.h`) | Native window handle manipulation |
| **Shell** (`shellapi.h`) | System tray icon |
| **COM** (`ole32`, `uuid`) | Underneath WASAPI |
| **WinMM** (`winmm`) | Legacy audio support |
| **shell32** | Tray icon functions |

### DSP Concepts

- Acoustic echo cancellation via adaptive subband filtering (AEC3)
- Neural Kalman filtering (NKF-AEC) with delay alignment (TDC) + residual WebRTC AEC3 stage
- Dual-signal LSTM echo + noise cancellation (DTLN-AEC)
- Optional streaming speech enhancement / dereverb (GTCRN “Dry voice” on NKF)
- Optional WebRTC noise suppression (AEC3, NKF-AEC)
- Frame buffering at 10–16 ms intervals
- Lock-free SPSC ring buffers (audio thread ↔ UI thread)
- Clock-drift correction (dynamic sample drop/duplicate)
- RMS metering with peak hold + decay
- Self-monitor loop detection (NKF: hold safe exposure while Listen-to-myself rings)

---

## About the Neural Engines

### NKF-AEC

NKF-AEC (Neural Kalman Filtering for Acoustic Echo Cancellation) is a research model published at **ICASSP 2023** by Jiang et al. It combines classical Kalman filtering with a small neural network.

- **Model size**: 45 KB (5.3K parameters)
- **Sample rate**: 16 kHz (auto-locked)
- **Latency**: ~32 ms
- **CPU**: Very low
- **Requires**: ONNX Runtime (28 MB DLL); optional `models/gtcrn_stream.onnx` for Dry voice

Production path adds: **time-delay compensation** (cross-correlation vs loopback), staged exposure (no cold-start spikes), a **divergence guard**, a **self-monitor loop detector** (Listen-to-myself / Discord mic test), an always-on **residual WebRTC AEC3** pass (toggle **Residual echo kill**), and an optional **GTCRN Dry voice** stage.

The wrapper at `src/nkf_wrapper.cpp` handles real-time streaming via the `ProcessBlock()` extension we added.

The full source is patched in `third_party/REAL_TIME_NKF_AEC/` and builds to `libnkf_aec.dll` (1.3 MB).

### GTCRN (Dry voice)

Ultra-light streaming speech enhancement / dereverb used only as the optional NKF **Dry voice** stage (~0.5 MB ONNX, MIT, [Xiaobin-Rong/gtcrn](https://github.com/Xiaobin-Rong/gtcrn)). Off by default; fail-open if the model file is missing.

### DTLN-AEC 128

DTLN-AEC (Dual-signal Transformation LSTM Network) by Westhausen & Meyer (**ICASSP 2021**, 3rd place in the Microsoft AEC Challenge) cancels echo **and** noise with a two-stage LSTM (separation mask + waveform refinement).

- **Model size**: 1.8M parameters (128 LSTM units/layer), two files: `dtln_aec_128_1` + `dtln_aec_128_2` (lighter/faster than the upstream 512-unit pair; same 512-sample DSP block)
- **Sample rate**: 16 kHz (auto-locked)
- **DSP**: 512-sample block, 128-sample shift, 257-bin FFT, overlap-add
- **Runtime (in probe order)**:
  1. **TFLite** — drop `dtln_aec_128_1.tflite` + `dtln_aec_128_2.tflite` from [breizhn/DTLN-aec](https://github.com/breizhn/DTLN-aec) into `models/` plus a Windows `tensorflowlite_c.dll` next to `aec_gui.exe` (loaded at runtime, no rebuild needed)
  2. **ONNX fallback** — `dtln_aec_128_1.onnx` + `dtln_aec_128_2.onnx` in `models/` (reuses the already-linked ONNX Runtime)
- Without either pair the engine reports `Failed to load DTLN model` on Start; all other engines are unaffected.

The wrapper at `src/dtln_wrapper.cpp` handles frame accumulation (128-sample shifts bridged to the 160-sample audio callback), int16 ↔ float conversion, LSTM state carry-over, and graceful fallback to mic on inference failure.

---

## Building from Source

### Prerequisites

Install [MSYS2](https://www.msys2.org/) and open the **UCRT64** terminal. Then:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-cmake
pacman -S mingw-w64-ucrt-x86_64-ninja
pacman -S mingw-w64-ucrt-x86_64-ccache
pacman -S mingw-w64-ucrt-x86_64-glfw
pacman -S mingw-w64-ucrt-x86_64-webrtc-audio-processing-1
pacman -S mingw-w64-ucrt-x86_64-onnxruntime
pacman -S git zip
```

### Clone

```bash
git clone https://github.com/samudinzul/aec-client.git
cd aec-client
```

### Build NKF-AEC Engine

```bash
cd third_party/REAL_TIME_NKF_AEC/c
cmake -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
cmake --build build
cd ../../..
```

Produces `libnkf_aec.dll`.

### Download Models

Models ship in this repo (`models/`). From a fresh clone they should already be present; restore from upstream if missing:

```bash
mkdir -p models

# NKF-AEC (bundled)
#   models/nkf.onnx

# DTLN-AEC 128 pair (~1.9 MB + ~5.0 MB) — required for the default engine
curl -L -o models/dtln_aec_128_1.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_1.tflite"
curl -L -o models/dtln_aec_128_2.tflite \
  "https://raw.githubusercontent.com/breizhn/DTLN-aec/main/pretrained_models/dtln_aec_128_2.tflite"
# Plus Windows tensorflowlite_c.dll next to aec_gui.exe (extract from a release ZIP),
# or convert the pair to models/dtln_aec_128_{1,2}.onnx (ONNX Runtime fallback).

# Silero VAD voice gate (~2.2 MB, MIT, sha256 1a153a22… = upstream v6.2.1)
curl -L -o models/silero_vad.onnx \
  "https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"

# GTCRN Dry voice (NKF optional stage, ~0.5 MB)
#   models/gtcrn_stream.onnx  (see MODELS.md for sha256)
```

See [MODELS.md](MODELS.md) for sizes and SHA-256 checks.

### Build the Main App

```bash
cd /path/to/aec-client
cmake -B build -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/ninja.exe \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build
```

First build: ~1 minute. Subsequent builds with ccache: ~15 seconds.

Output: `build/aec_gui.exe`.

### Run

```bash
cd build
./aec_gui.exe
```

---

## Project Structure

```
aec-client/
├── src/
│   ├── main.cpp                    Entry point, GUI, audio pipeline
│   ├── aec3_wrapper.cpp/.h         WebRTC AEC3 C wrapper
│   ├── nkf_wrapper.cpp/.h          NKF-AEC C wrapper (+ residual AEC3, GTCRN dry stage)
│   ├── dtln_wrapper.cpp/.h         DTLN-AEC C wrapper (TFLite/ONNX)
│   ├── gtcrn_wrapper.cpp/.h        GTCRN streaming wrapper (NKF Dry voice)
│   └── silero_wrapper.cpp/.h       Silero VAD C wrapper (voice gate)
│
├── include/
│   ├── miniaudio.h                 Audio I/O
│   └── stb_image.h                 Image loading
│
├── libs/
│   └── tensorflowlite_c.dll        TFLite runtime for DTLN (bundled in release)
│
├── third_party/
│   ├── imgui/                      Dear ImGui source
│   └── REAL_TIME_NKF_AEC/          NKF neural engine (patched)
│       ├── c/                      C++ source
│       └── python/                 Original model + scripts
│
├── models/
│   ├── nkf.onnx                    NKF model (45 KB)
│   ├── dtln_aec_128_{1,2}.tflite   DTLN-AEC 128 pair (default engine)
│   ├── silero_vad.onnx             Silero VAD model (voice gate, 2.2 MB)
│   └── gtcrn_stream.onnx           GTCRN Dry voice model (~0.5 MB)
│
├── screenshots/
│   └── main.png                    README image
│
├── wallpapers/                     User-supplied backgrounds
│
├── release/                        Distribution builds (gitignored)
│
├── CMakeLists.txt                  Build configuration
├── scripts/
│   ├── make-release.sh             Release bundle + zip builder
│   ├── standardize-releases.sh     Version-keyed release notes writer
│   └── README.txt                  End-user doc template (%%VERSION%%)
├── README.md                       This file
├── LICENSE                         MIT
├── CHANGELOG.md                    Version history
└── .gitignore
```

---

## License

MIT License — see [LICENSE](LICENSE).

Third-party credits in [LICENSES/THIRD-PARTY.txt](LICENSES/THIRD-PARTY.txt).

---

## Credits

- **WebRTC Audio Processing** — Google (BSD-3)
- **NKF-AEC** — Jiang et al., ICASSP 2023 (MIT)
- **DTLN-AEC** — Westhausen & Meyer, ICASSP 2021 (MIT)
- **GTCRN** — Xiaobin Rong et al. (MIT) — NKF Dry voice stage
- **Silero VAD** — Silero Team (MIT)
- **ONNX Runtime** — Microsoft (MIT)
- **Dear ImGui** — Omar Cornut (MIT)
- **miniaudio** — David Reid (MIT-0)
- **GLFW** — Marcus Geelnard & Camilla Löwy (zlib)
- **stb_image** — Sean Barrett (Public Domain)
- **pocketfft** — Max-Planck-Society (BSD-3)
- **AudioFile** — Adam Stark (MIT)
- **VB-CABLE** — VB-Audio (free for personal use)

---

## Contributing

Pull requests welcome. For major changes, please open an issue first.

## Support

- Report bugs: [Issues](https://github.com/samudinzul/aec-client/issues)
- Discussions: [Discussions](https://github.com/samudinzul/aec-client/discussions)
