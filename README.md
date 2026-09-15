# AEC Client

> Real-time acoustic echo cancellation for Windows — use speakers and a microphone at the same time in Discord, Zoom, Teams, or any other voice app.

![AEC Client screenshot](screenshots/main.png)

**Built entirely with open-source tools.** No proprietary SDKs. No cloud dependencies. No telemetry.

---

## Table of Contents

- [What It Does](#what-it-does)
- [Features](#features)
- [Quick Start](#quick-start)
- [Screenshots](#screenshots)
- [Technical Stack](#technical-stack)
- [Architecture](#architecture)
- [Engine Comparison](#engine-comparison)
- [About NKF-AEC (Experimental Engine)](#about-nkf-aec-experimental-engine)
- [Building from Source](#building-from-source)
- [Project Structure](#project-structure)
- [Runtime Dependencies](#runtime-dependencies)
- [License](#license)
- [Credits](#credits)

---

## What It Does

AEC Client routes your microphone through one of **three acoustic echo cancellation engines**, subtracts the sound coming from your speakers, and outputs a clean, echo-free signal to a virtual audio cable. Any voice app can then use that clean signal as its microphone input.

```
Microphone ─────────────────┐
                            ├─→ AEC Engine ─→ VB-CABLE ─→ Discord
Speakers (loopback) ────────┘
```

No more headphones. No more echo.

---

## Features

- **4 selectable AEC engines** in one app:
  - **WebRTC AEC3** — best voice quality, same engine used by Chrome and Google Meet
  - **LocalVQE v1.4-AEC** — echo-only neural model; preserves voice, room tone, and background noise
  - **SpeexDSP** — extremely lightweight, phone quality, lowest CPU
  - **NKF-AEC** — experimental neural Kalman filter (ICASSP 2023)
- **Low CPU usage** — under 2% on a typical desktop
- **Low latency** — 30–40 ms round-trip
- **Works with any audio device** — speakers, earphones, headsets
- **Selectable sample rate** (16 / 32 / 48 kHz)
- **Optional system tray** — runs in the background like a real utility
- **Device filtering** — virtual cables hidden from Mic/Reference dropdowns
- **Live level meters** with peak-hold
- **Presets** for common scenarios (Discord, Low CPU, High Quality, Noisy Room)
- **Clock-drift correction** — stable over long calls
- **Wallpaper customization**
- **Auto-save settings** to `aec_config.txt`

---

## Quick Start

1. **Install [VB-CABLE](https://vb-audio.com/Cable/)** (free virtual audio cable). Reboot.
2. **Download** the latest release from [Releases](https://github.com/samudinzul/aec-client/releases/latest) and extract it anywhere.
3. **Run `aec_gui.exe`**.
4. **Select your devices**:
   - **Microphone** → your physical mic
   - **Speaker Reference** → your physical speakers
   - **Output** → `CABLE Input (VB-Audio Virtual Cable)`
5. **Pick an engine** (WebRTC AEC3 is a good default).
6. **Click Start**.
7. **In Discord** → Voice & Video settings:
   - **Input Device**: `CABLE Output (VB-Audio Virtual Cable)`
   - **Echo Cancellation**: **OFF**
   - **Noise Suppression**: **OFF**
   - **Automatic Gain Control**: **OFF**

Done. Talk normally with speakers on.

---

## Screenshots

| Audio Tab | Appearance Tab | About Tab |
|-----------|----------------|-----------|
| ![Audio](screenshots/main.png) | *coming soon* | *coming soon* |

---

## Technical Stack

### Languages

| Language | Role |
|----------|------|
| **C++17** | Main application |
| **C** | Third-party libraries (miniaudio, stb_image, SpeexDSP) |
| **CMake** | Build system |
| **Rust** | (Indirect) — `thewh1teagle/aec` SpeexDSP wrapper |
| **Bash** | Build scripts |
| **Markdown** | Documentation |

### Build Toolchain

| Tool | Version | Purpose |
|------|---------|---------|
| **GCC (MinGW-w64)** | 16.2.0 | C/C++ compiler |
| **CMake** | 3.20+ | Build system generator |
| **mingw32-make** | — | Make implementation |
| **gendef / dlltool** | — | Generate MinGW import libraries |
| **MSYS2 UCRT64** | — | Build environment |

### AEC Engines

| Engine | Language | Source | Notes |
|--------|----------|--------|-------|
| **SpeexDSP** | C | [Xiph.Org](https://gitlab.xiph.org/xiph/speexdsp) via [thewh1teagle/aec](https://github.com/thewh1teagle/aec) | Lightweight, phone quality |
| **WebRTC AEC3** | C++ | [MSYS2 package](https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-webrtc-audio-processing-1) | Best quality, full-band |
| **NKF-AEC** | C++ | [William1617/REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC) | Experimental neural |

### Libraries

| Library | Language | Purpose |
|---------|----------|---------|
| **[miniaudio](https://github.com/mackron/miniaudio)** | C (single-header) | Cross-platform audio I/O |
| **[Dear ImGui](https://github.com/ocornut/imgui)** | C++ | Immediate-mode GUI |
| **[GLFW](https://www.glfw.org/)** | C | Window + OpenGL context |
| **[OpenGL](https://www.opengl.org/)** | — | Rendering backend |
| **[stb_image](https://github.com/nothings/stb)** | C (single-header) | Image loading for wallpapers |
| **[ONNX Runtime](https://onnxruntime.ai/)** | C++ | Neural inference (NKF-AEC) |
| **[pocketfft](https://github.com/mreineck/pocketfft)** | C++ (header) | FFT for NKF-AEC |
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

- Acoustic echo cancellation via adaptive filtering
- Frame buffering at 10 ms intervals
- Lock-free SPSC ring buffers (audio thread ↔ UI thread)
- Clock-drift correction (dynamic sample drop/duplicate)
- RMS metering with peak hold + decay
- Neural Kalman filtering (NKF-AEC)

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Microphone (WASAPI capture)                                    │
│         ↓                                                       │
│  Lock-free ring buffer  ──┐                                     │
│                            ├──→ AEC Engine ──→ Clean signal    │
│  Speaker loopback (WASAPI) │                                     │
│         ↓                  │                                     │
│  Lock-free ring buffer  ──┘                                     │
│         ↓                                                       │
│  Output → VB-CABLE → Discord                                    │
└─────────────────────────────────────────────────────────────────┘
```

**Design principles:**

- **No locks in audio thread** — SPSC ring buffers use atomic head/tail indices
- **No heap allocation in audio callback** — stack buffers only
- **Drift correction** — skips/duplicates samples if mic/speaker clocks diverge
- **Engine abstraction** — `enum EngineType` + function pointers for runtime swap
- **Settings persistence** — plain text `aec_config.txt`

---

## Engine Comparison

| Engine | Quality | CPU | Sample Rate | Notes |
|--------|---------|-----|-------------|-------|
| **WebRTC AEC3** | ⭐⭐⭐⭐⭐ | Moderate | 16/32/48 kHz | Best overall |
| **LocalVQE v1.4-AEC** | ⭐⭐⭐⭐⭐ | Very low | 16 kHz only | Echo-only, preserves voice |
| **SpeexDSP** | ⭐⭐ | Very low | 16/32/48 kHz | Phone quality |
| **NKF-AEC** | ⭐⭐⭐⭐ | Low | 16 kHz only | Experimental neural |

---

## About NKF-AEC (Experimental Engine)

NKF-AEC is the **only engine in this app that must be built locally** — there are no prebuilt binaries available. Here's why, and how it works.

### What Is NKF-AEC?

**NKF-AEC** (Neural Kalman Filtering for Acoustic Echo Cancellation) is a research model published at **ICASSP 2023** by Jiang et al. It combines classical Kalman filtering with a small neural network to cancel echo at very low CPU cost.

- **Paper**: "Neural Kalman Filtering for Acoustic Echo Cancellation"
- **Original repo**: [fjiang9/NKF-AEC](https://github.com/fjiang9/NKF-AEC) (Python/PyTorch)
- **Real-time C++ port**: [William1617/REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC)

The C++ port is what this app uses. It runs the neural network through **ONNX Runtime** and produces output in real time.

### Why It's Compiled Locally

The `REAL_TIME_NKF_AEC` repository is a **research project**, not a packaged library. It ships as C++ source code only — no `.dll`, no `.lib`, no prebuilt artifacts. To use it, we compile it ourselves.

Additionally, the code needed several patches to work on Windows + MinGW + ONNX Runtime:

| Patch | Reason |
|-------|--------|
| Added `#include <complex>` and `std::complex` | C++ standard library requires explicit inclusion on modern compilers |
| Added `#define PI` | C++ doesn't define `PI` in standard headers |
| Converted `ModelPath` to `std::wstring` | Windows ONNX Runtime requires **wide-string** paths |
| Fixed `m_pEngine->` → `m_pEngine.` | Original code treated a struct as a pointer |
| Fixed `kgreal` → `kg_real` typo | Compilation error |
| Removed orphan `output_fea` block | Leftover from abandoned experiment |
| Changed `Enhance` return type to `int` | Match header declaration |
| **Added `ProcessBlock()` method** | Original API only processed entire WAV files — we added a real-time block-by-block entry point |
| **Fixed `BLOCK_LEN` 512 → 1024** | The ONNX model expects 513-bin FFT (1024-sample block), not 257-bin |

The patched source lives in `third_party/REAL_TIME_NKF_AEC/` and is included in this repo so anyone can build it.

### How It's Built

The NKF library compiles into `libnkf_aec.dll` using CMake + MinGW-w64:

```bash
cd third_party/REAL_TIME_NKF_AEC/c
cmake -B build -G "MinGW Makefiles" \
    -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
cmake --build build
# → produces build/libnkf_aec.dll
```

The DLL links against:
- **ONNX Runtime** (Microsoft) — neural network inference
- **pocketfft** (header-only) — FFT
- **AudioFile** (header-only) — WAV I/O (only for the offline `Enhance()` API)

Our app calls it via a C wrapper (`src/nkf_wrapper.cpp`) that handles streaming audio frame-by-frame.

### Why Ship It Then?

Despite the extra complexity, NKF-AEC is worth including because:

- **Very small neural model** — only 5.3 K parameters (45 KB ONNX file)
- **Low CPU usage** — runs comfortably on any modern CPU
- **Neural approach** — different character from AEC3 and SpeexDSP; useful for comparison
- **Research value** — showcases cutting-edge AEC as an alternative to classical DSP

It's marked **experimental** because:

- Fixed **16 kHz** sample rate (not resampled internally)
- Latency is **32 ms** (larger block size)
- No time-delay compensation (TDC) built in — requires good device alignment
- Quality varies with different mic/speaker geometries

### When to Use It

| Situation | Recommendation |
|-----------|---------------|
| Best voice quality | **WebRTC AEC3** |
| Lowest CPU | **SpeexDSP** |
| Trying something new | **NKF-AEC** |
| Low-end hardware (Atom, old laptop) | **NKF-AEC** or **SpeexDSP** |
| 48 kHz full-band audio | **AEC3** only |

---

## Building from Source

### Prerequisites

Install [MSYS2](https://www.msys2.org/) and open the **UCRT64** terminal. Then:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-cmake
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

### Build the NKF-AEC engine first

NKF-AEC has no prebuilt binaries — it must be compiled from the source in `third_party/`.

```bash
cd third_party/REAL_TIME_NKF_AEC/c
cmake -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
cmake --build build
cd ../../..
```

This produces `build/libnkf_aec.dll`. The main app's CMake will pick it up automatically.

### Build the main app

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/mingw32-make.exe
cmake --build build
```

Output: `build/aec_gui.exe`

### Run

```bash
cd build
./aec_gui.exe
```

---

## Project Structure

```
aec-client/
├── src/                            Your application code (C++)
│   ├── main.cpp                    Entry point, GUI, audio pipeline
│   ├── aec3_wrapper.cpp/.h         WebRTC AEC3 C wrapper
│   └── nkf_wrapper.cpp/.h          NKF-AEC C wrapper
│
├── include/                        Third-party headers
│   ├── miniaudio.h                 Audio I/O
│   ├── libaec.h                    SpeexDSP C API
│   └── stb_image.h                 Image loading
│
├── libs/                           Prebuilt libraries
│   ├── aec.dll                     SpeexDSP runtime
│   ├── libaec.a                    SpeexDSP import library
│   └── aec.def                     Symbol export list
│
├── third_party/
│   ├── imgui/                      Dear ImGui source
│   └── REAL_TIME_NKF_AEC/          NKF neural engine
│       └── c/                      NKF-AEC source
│
├── models/
│   └── nkf.onnx                    Neural model (45 KB)
│
├── screenshots/
│   └── main.png                    README image
│
├── wallpapers/                     User-supplied backgrounds
│
├── release/                        Distribution build (gitignored)
│
├── CMakeLists.txt                  Build configuration
├── README.md                       This file
├── LICENSE                         MIT
├── CHANGELOG.md                    Version history
└── .gitignore
```

---

## Runtime Dependencies

The release ZIP bundles all required DLLs:

| File | Size | Source |
|------|------|--------|
| `aec_gui.exe` | ~2.3 MB | Your build |
| `aec.dll` | ~185 KB | SpeexDSP wrapper |
| `libnkf_aec.dll` | ~1.3 MB | NKF neural engine |
| `libwebrtc-audio-processing-1-3.dll` | ~950 KB | WebRTC AEC3 |
| `onnxruntime.dll` | ~28 MB | Neural inference |
| `libwinpthread-1.dll` | ~63 KB | MinGW thread runtime |
| `libgcc_s_seh-1.dll` | ~150 KB | GCC runtime |
| `libstdc++-6.dll` | ~2.6 MB | C++ standard library |
| `models/nkf.onnx` | ~45 KB | NKF model |

**Note on `libnkf_aec.dll` and `onnxruntime.dll`:**

Unlike other DLLs, these come from building NKF-AEC locally and from the MSYS2 `onnxruntime` package. They are bundled in the release ZIP because NKF-AEC is compiled once at release time — end users don't need to build anything.

**External dependency (user-installed):** [VB-CABLE](https://vb-audio.com/Cable/)

---

## License

MIT License — see [LICENSE](LICENSE).

Third-party credits in [LICENSES/THIRD-PARTY.txt](LICENSES/THIRD-PARTY.txt).

---

## Credits

- **SpeexDSP** — Xiph.Org Foundation (BSD-3)
- **WebRTC Audio Processing** — Google (BSD-3)
- **NKF-AEC** — Jiang et al., ICASSP 2023 (MIT)
- **LocalVQE** — LocalAI (Apache-2.0)
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