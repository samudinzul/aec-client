# AEC Client

> Real-time acoustic echo cancellation for Windows — use speakers and a microphone at the same time in Discord, Zoom, Teams, and any other voice app.

![screenshot](screenshots/main.png)

## What It Does

AEC Client routes your microphone through one of **three acoustic echo cancellation engines**, subtracts the sound coming from your speakers, and outputs a clean, echo-free signal to a virtual audio cable. Any voice app can then use that clean signal as its microphone input.

No more headphones. No more echo.

## Features

- **3 AEC engines** in one app:
  - **WebRTC AEC3** — best voice quality, used by Chrome and Google Meet
  - **SpeexDSP** — extremely lightweight, phone quality
  - **NKF-AEC** — experimental neural Kalman filter, 16 kHz only
- **Low CPU usage** — under 2% on a typical desktop
- **Low latency** — 30–40 ms round-trip
- **Works with any device** — speakers, earphones, headsets
- **Selectable sample rate** (16 / 32 / 48 kHz)
- **Optional system tray** — runs in the background like a real utility
- **Device filtering** — virtual cables hidden from Mic/Reference dropdowns
- **Live level meters** with peak hold
- **Presets** for common scenarios
- **Clean, modern UI** — built with Dear ImGui

## Requirements

- **Windows 10 / 11 (64-bit)**
- **[VB-CABLE](https://vb-audio.com/Cable/)** — free virtual audio cable

## Quick Start

1. **Install VB-CABLE** if you haven't already. Reboot.
2. **Download** the latest release ZIP and extract it anywhere.
3. **Run aec_gui.exe**.
4. **Select your devices**:
   - **Microphone** -> your physical mic
   - **Speaker Reference** -> your physical speakers
   - **Output** -> CABLE Input (VB-Audio Virtual Cable)
5. **Pick an engine** (WebRTC AEC3 is a good default).
6. **Click Start**.
7. **In Discord** -> Voice & Video settings:
   - **Input Device**: CABLE Output (VB-Audio Virtual Cable)
   - **Echo Cancellation**: OFF
   - **Noise Suppression**: OFF
   - **Automatic Gain Control**: OFF

Done. Talk normally with speakers on.

## How It Works


1. AEC Client captures your **microphone** and a **loopback** of what your speakers are playing.
2. Both streams feed the selected AEC engine, which estimates the echo path and subtracts it.
3. The cleaned signal goes to CABLE Input, which Discord reads as CABLE Output.

## Engine Comparison

| Engine | Quality | CPU | Sample Rate | Notes |
|--------|---------|-----|-------------|-------|
| **WebRTC AEC3** | 5/5 | Moderate | 16/32/48 kHz | Best overall |
| **SpeexDSP** | 2/5 | Very low | 16/32/48 kHz | Phone quality |
| **NKF-AEC** | 4/5 | Low | 16 kHz only | Experimental neural |

## Building from Source

### Prerequisites

- MSYS2 with UCRT64 environment
- pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,glfw,webrtc-audio-processing-1,onnxruntime}

### Build

    # 1. Build the NKF-AEC library
    cd third_party/REAL_TIME_NKF_AEC/c
    cmake -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
    cmake --build build

    # 2. Build the main app
    cd /path/to/aec
    cmake -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
    cmake --build build

The .exe lands in build/aec_gui.exe.

## License

MIT — see [LICENSE](LICENSE).

Third-party credits in [LICENSES/THIRD-PARTY.txt](LICENSES/THIRD-PARTY.txt).

## Credits

- **SpeexDSP** — Xiph.Org Foundation
- **WebRTC Audio Processing** — Google
- **NKF-AEC** — Jiang et al., ICASSP 2023
- **ONNX Runtime** — Microsoft
- **Dear ImGui** — Omar Cornut
- **miniaudio** — David Reid
- **GLFW** — Marcus Geelnard, Camilla Lowy
- **stb_image** — Sean Barrett
