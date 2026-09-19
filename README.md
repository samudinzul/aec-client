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

AEC Client routes your microphone through one of **five acoustic echo cancellation engines**, subtracts the sound coming from your speakers, and outputs a clean, echo-free signal to a virtual audio cable. Any voice app can then use that clean signal as its microphone input.

```
Microphone ─────────────────┐
                             ├─→ AEC Engine ─→ Voice gate ─→ VB-CABLE ─→ Discord
Speakers (loopback) ────────┘                   (speech passes, silence muted)
```

No more headphones. No more echo. No dead-air noise.

---

## Features

- **5 selectable AEC engines** in one app:
  - **WebRTC AEC3** — best voice quality, same engine used by Chrome and Google Meet
  - **LocalVQE v1.4-AEC** — echo-only neural model; preserves voice, room tone, and background noise
  - **SpeexDSP** — extremely lightweight, phone quality, lowest CPU
  - **NKF-AEC** — experimental neural Kalman filter (ICASSP 2023)
  - **DTLN-AEC 512** — dual-LSTM echo + noise canceller (ICASSP 2021, needs model files)
- **Low CPU usage** — under 2% on a typical desktop
- **Low latency** — 30–40 ms round-trip
- **Works with any audio device** — speakers, earphones, headsets
- **Selectable sample rate** (16 / 48 kHz)
- **Optional system tray** — runs in the background like a real utility
- **Device filtering** — virtual cables hidden from Mic/Reference dropdowns
- **Live level meters** with peak-hold
- **Presets** for common scenarios (Discord, Low CPU, High Quality, Noisy Room)
- **Clock-drift correction** — stable over long calls
- **Voice gate** — a tiny neural network mutes silence, passes speech (16/48 kHz, on by default, one checkbox)
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
5. **Pick an engine** under Advanced — WebRTC AEC3 or LocalVQE are good defaults.
6. **Click Start**.
7. **In Discord** → Voice & Video settings:
   - **Input Device**: `CABLE Output (VB-Audio Virtual Cable)`
   - **Echo Cancellation**: **OFF**
   - **Noise Suppression**: **OFF**
   - **Automatic Gain Control**: **OFF**

Done. Talk normally with speakers on.

---

## Voice Gate

After echo cancellation, a tiny neural network checks for speech many times a second. Speech passes to Discord; silence is muted. No setup, no recording — it's on by default.

- Green **SPEAKING** pill at the top = speech going out. Grey **SILENT** = muted.
- It hears *any* speech, not just yours — it sits after the echo canceller, so what's left is overwhelmingly your voice.
- Uncheck **Voice gate** in the Audio tab to pass original audio through.
- **Only my voice** (experimental, off by default): one tap learns your voice (~8 s, you'll hear yourself), and the gate also mutes *other* voices — TV, family, roommates. Your voiceprint never leaves the PC.
- Works at **16 and 48 kHz** (the detector itself is 16 kHz fixed; at 48 kHz an internal downsample feeds only the detector, your audio stays full-rate) — Speex and the neural engines run at 16000 (locked); AEC3 offers 16000 or 48000.

---

## Engine Comparison

| Engine | Voice quality | Double-talk | CPU | Sample Rate | Latency | Best For |
|--------|---------------|-------------|-----|-------------|---------|----------|
| **WebRTC AEC3** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | Moderate | 16/48 kHz | ~40 ms | Best overall |
| **DTLN-AEC 512** | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | Moderate–High | 16 kHz only | ~32 ms | Neural echo + noise (needs models) |
| **LocalVQE v1.4-AEC** | ⭐⭐⭐⭐ | ⭐⭐⭐ | Very low | 16 kHz only | ~32 ms | Natural voice, neural |
| **NKF-AEC** | ⭐⭐⭐ | ⭐⭐⭐ | Low | 16 kHz only | ~32 ms | Experimental neural |
| **SpeexDSP** | ⭐⭐⭐ | ⭐⭐ | Very low | 16 kHz only | ~30 ms | Low-power hardware |

*Voice quality = how natural your voice sounds (single-talk). Double-talk = your voice preserved when both sides speak at once, echo removal breaking ties. Ranks are research-based (published challenge scores, algorithm design, in-app listening) — your ears outrank this table.*

---

## Runtime Dependencies

The release ZIP bundles all required DLLs:

| File | Size | Purpose |
|------|------|---------|
| `aec_gui.exe` | ~2.4 MB | Main application |
| `aec.dll` | ~185 KB | SpeexDSP wrapper |
| `libnkf_aec.dll` | ~1.3 MB | NKF neural engine |
| `liblocalvqe.dll` | ~458 KB | LocalVQE neural engine |
| `libwebrtc-audio-processing-1-3.dll` | ~950 KB | WebRTC AEC3 |
| `onnxruntime.dll` | ~28 MB | Neural inference (NKF, DTLN ONNX fallback, Silero VAD) |
| `ggml.dll` | ~93 KB | GGML runtime (LocalVQE) |
| `ggml-base.dll` | ~860 KB | GGML base |
| `ggml-cpu-*.dll` (15 files) | ~18 MB total | CPU backend variants |
| `libwinpthread-1.dll` | ~63 KB | MinGW thread runtime |
| `libgcc_s_seh-1.dll` | ~150 KB | GCC runtime |
| `libstdc++-6.dll` | ~2.6 MB | C++ standard library |
| `models/nkf.onnx` | ~45 KB | NKF model |
| `models/localvqe-v1.4-aec-200K-f32.gguf` | ~2.8 MB | LocalVQE model |
| `models/dtln_aec_512_*` | user-supplied | DTLN models (optional) |
| `tensorflowlite_c.dll` | user-supplied | TFLite runtime for DTLN (optional) |

**External dependency (user-installed):** [VB-CABLE](https://vb-audio.com/Cable/)

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
- **Sub-10 ms gate decisions** — 512-sample VAD inference inline (< 1 ms), no worker thread
- **Drift correction** — skips/duplicates samples if mic/speaker clocks diverge
- **Engine abstraction** — runtime swap between 5 engines with a single enum + function pointer
- **Settings persistence** — plain text `aec_config.txt`
- **Single-instance mutex** — prevents accidental double launches

---

## Technical Stack

### Languages

| Language | Role |
|----------|------|
| **C++17** | Main application |
| **C** | Third-party libraries (miniaudio, stb_image, SpeexDSP, GGML) |
| **Rust** | (Indirect) — `thewh1teagle/aec` SpeexDSP wrapper |
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
| **SpeexDSP** | Classic DSP | C | [Xiph.Org](https://gitlab.xiph.org/xiph/speexdsp) via [thewh1teagle/aec](https://github.com/thewh1teagle/aec) |
| **WebRTC AEC3** | Advanced DSP | C++ | [MSYS2 package](https://packages.msys2.org/package/mingw-w64-ucrt-x86_64-webrtc-audio-processing-1) |
| **NKF-AEC** | Neural (Kalman) | C++ | [William1617/REAL_TIME_NKF_AEC](https://github.com/William1617/REAL_TIME_NKF_AEC) |
| **LocalVQE** | Neural (echo-only) | C++ | [LocalAI-io/LocalVQE](https://github.com/localai-org/LocalVQE) |
| **DTLN-AEC** | Neural (dual-LSTM) | C++ | [breizhn/DTLN-aec](https://github.com/breizhn/DTLN-aec) |

### Libraries

| Library | Language | Purpose |
|---------|----------|---------|
| **[miniaudio](https://github.com/mackron/miniaudio)** | C (single-header) | Cross-platform audio I/O |
| **[Dear ImGui](https://github.com/ocornut/imgui)** | C++ | Immediate-mode GUI |
| **[GLFW](https://www.glfw.org/)** | C | Window + OpenGL context |
| **[OpenGL](https://www.opengl.org/)** | — | Rendering backend |
| **[stb_image](https://github.com/nothings/stb)** | C (single-header) | Image loading for wallpapers |
| **[ONNX Runtime](https://onnxruntime.ai/)** | C++ | Neural inference (NKF-AEC, DTLN-AEC ONNX fallback, Silero VAD) |
| **[GGML](https://github.com/ggml-org/ggml)** | C/C++ | Neural inference (LocalVQE) |
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

- Acoustic echo cancellation via adaptive filtering (Speex, AEC3)
- Neural Kalman filtering (NKF-AEC)
- Dual-signal LSTM echo + noise cancellation (DTLN-AEC)
- Neural echo-only removal via DAF front-end (LocalVQE)
- Frame buffering at 10–16 ms intervals
- Lock-free SPSC ring buffers (audio thread ↔ UI thread)
- Clock-drift correction (dynamic sample drop/duplicate)
- RMS metering with peak hold + decay
- Residual noise gating (LocalVQE)

---

## About the Neural Engines

### LocalVQE v1.4-AEC

LocalVQE is a compact neural echo canceller from **LocalAI** that uses a **DAF (Deep Adaptive Filtering) front-end** to remove only the far-end echo. It preserves your voice, room tone, and background noise naturally — no robotic artifacts, no dead silence.

- **Model size**: 2.8 MB (203K parameters)
- **Sample rate**: 16 kHz (auto-locked)
- **Latency**: ~32 ms (256-sample hop + processing window)
- **CPU**: ~0.83 ms per 16 ms frame (19× realtime)
- **Residual noise gate**: off since v1.2.1 (was muting word tails; the joint network suppresses noise on its own)

The wrapper at `src/localvqe_wrapper.cpp` handles:
- Frame accumulation (256-sample hops)
- Int16 ↔ float conversion
- Noise gate configuration
- Fallback to raw mic on error

The library builds into `liblocalvqe.dll` (458 KB) using GGML. All the `ggml-*.dll` runtime files are shipped alongside.

### NKF-AEC

NKF-AEC (Neural Kalman Filtering for Acoustic Echo Cancellation) is a research model published at **ICASSP 2023** by Jiang et al. It combines classical Kalman filtering with a small neural network.

- **Model size**: 45 KB (5.3K parameters)
- **Sample rate**: 16 kHz (auto-locked)
- **Latency**: ~32 ms
- **CPU**: Very low
- **Requires**: ONNX Runtime (28 MB DLL)

The wrapper at `src/nkf_wrapper.cpp` handles real-time streaming via the `ProcessBlock()` extension we added.

The full source is patched in `third_party/REAL_TIME_NKF_AEC/` and builds to `libnkf_aec.dll` (1.3 MB).

### DTLN-AEC 512

DTLN-AEC (Dual-signal Transformation LSTM Network) by Westhausen & Meyer (**ICASSP 2021**, 3rd place in the Microsoft AEC Challenge) cancels echo **and** noise with a two-stage LSTM (separation mask + waveform refinement).

- **Model size**: 10.4M parameters (512 LSTM units/layer), two files: `dtln_aec_512_1` + `dtln_aec_512_2`
- **Sample rate**: 16 kHz (auto-locked)
- **DSP**: 512-sample block, 128-sample shift, 257-bin FFT, overlap-add
- **Runtime (in probe order)**:
  1. **TFLite** — drop `dtln_aec_512_1.tflite` + `dtln_aec_512_2.tflite` from [breizhn/DTLN-aec](https://github.com/breizhn/DTLN-aec) into `models/` plus a Windows `tensorflowlite_c.dll` next to `aec_gui.exe` (loaded at runtime, no rebuild needed)
  2. **ONNX fallback** — `dtln_aec_512_1.onnx` + `dtln_aec_512_2.onnx` in `models/` (reuses the already-linked ONNX Runtime)
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

### Build LocalVQE Engine

```bash
cd third_party/LocalVQE/ggml
cmake -B build -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/ninja.exe \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_BUILD_TYPE=Release \
    -DLOCALVQE_BUILD_SHARED=ON \
    -DBUILD_TESTING=OFF \
    -DCMAKE_C_FLAGS="-DLOCALVQE_BUILD" \
    -DCMAKE_CXX_FLAGS="-DLOCALVQE_BUILD"
cmake --build build --target localvqe_shared -j$(nproc)
cd ../../../..
```

Produces `liblocalvqe.dll` and its GGML dependencies.

### Download Models

```bash
mkdir -p models

# LocalVQE v1.4-AEC (2.8 MB)
curl -L -o models/localvqe-v1.4-aec-200K-f32.gguf \
  "https://huggingface.co/LocalAI-io/LocalVQE/resolve/main/localvqe-v1.4-aec-200K-f32.gguf"

# NKF-AEC model is bundled in third_party/REAL_TIME_NKF_AEC/python/nkf.onnx
cp third_party/REAL_TIME_NKF_AEC/python/nkf.onnx models/

# DTLN-AEC 512 (optional — engine stays disabled until these exist)
# Option A (preferred, upstream weights verbatim):
#   download dtln_aec_512_1.tflite + dtln_aec_512_2.tflite from
#   https://github.com/breizhn/DTLN-aec/tree/main/pretrained_models
#   into models/, plus a Windows tensorflowlite_c.dll next to aec_gui.exe
# Option B (no new DLL): convert the pair to
#   models/dtln_aec_512_1.onnx + models/dtln_aec_512_2.onnx

# Silero VAD voice gate (optional — gate reports "model not found" until
# vendored, ~2.2 MB, MIT, sha256 1a153a22… = upstream v6.2.1)
curl -L -o models/silero_vad.onnx \
  "https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"
```

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
│   ├── nkf_wrapper.cpp/.h          NKF-AEC C wrapper
│   ├── localvqe_wrapper.cpp/.h     LocalVQE C wrapper
│   ├── dtln_wrapper.cpp/.h         DTLN-AEC C wrapper (TFLite/ONNX)
│   └── silero_wrapper.cpp/.h       Silero VAD C wrapper (voice gate)
│
├── include/
│   ├── miniaudio.h                 Audio I/O
│   ├── libaec.h                    SpeexDSP C API
│   └── stb_image.h                 Image loading
│
├── libs/
│   ├── aec.dll                     SpeexDSP runtime
│   ├── libaec.a                    SpeexDSP import library
│   └── aec.def                     Symbol export list
│
├── third_party/
│   ├── imgui/                      Dear ImGui source
│   ├── REAL_TIME_NKF_AEC/          NKF neural engine (patched)
│   │   ├── c/                      C++ source
│   │   └── python/                 Original model + scripts
│   └── LocalVQE/                   LocalVQE neural engine
│       └── ggml/                   C++ source + GGML vendor tree
│
├── models/
│   ├── nkf.onnx                    NKF model (45 KB)
│   ├── localvqe-v1.4-aec-200K-f32.gguf   LocalVQE model (2.8 MB)
│   ├── dtln_aec_512_{1,2}.tflite|.onnx  DTLN models (user-supplied, optional)
│   └── silero_vad.onnx             Silero VAD model (2.2 MB, user-supplied, optional)
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

- **SpeexDSP** — Xiph.Org Foundation (BSD-3)
- **WebRTC Audio Processing** — Google (BSD-3)
- **NKF-AEC** — Jiang et al., ICASSP 2023 (MIT)
- **DTLN-AEC** — Westhausen & Meyer, ICASSP 2021 (MIT)
- **LocalVQE** — LocalAI (Apache-2.0)
- **Silero VAD** — Silero Team (MIT)
- **ONNX Runtime** — Microsoft (MIT)
- **GGML** — The GGML authors (MIT)
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
