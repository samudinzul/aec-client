# AEC Client

> Real-time acoustic echo cancellation for Windows — use speakers and a microphone at the same time in Discord, Zoom, Teams, or any other voice app.

![AEC Client screenshot](screenshots/main.png)

**Built entirely with open-source tools.** No proprietary SDKs. No cloud dependencies. No telemetry.

---

## Table of Contents

- [What It Does](#what-it-does)
- [Features](#features)
- [Web UI (Python, no installer)](#web-ui-python-no-installer)
- [Quick Start](#quick-start)
- [Profiles](#profiles)
- [Profile Comparison](#profile-comparison)
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

AEC Client routes your microphone through a **processing profile** (an echo canceller stacked with a noise-suppression post stage), subtracts the sound coming from your speakers, and outputs a clean, echo-free signal to a virtual audio cable. Any voice app can then use that clean signal as its microphone input.

```
Microphone ─────────────────┐
                              ├─→ Engine ─→ DTLN-NS ─→ VB-CABLE ─→ Discord
Speakers (loopback) ────────┘     (AEC3 / NKF / DTLN)
```

No more headphones. No more echo. No dead-air noise.

---

## Features

- **3 engine profiles** — one menu shows the engine and its chain (the engine is not configurable on its own):
  - **DTLN-AEC 128** *(default)* — dual-LSTM echo canceller (ICASSP 2021; 128 LSTM units for lower CPU). DTLN-NS removes background noise on top.
  - **WebRTC AEC3** — the same canceller Chrome and Google Meet use: strongest canceller.
  - **NKF-AEC** — tiny neural Kalman core (ICASSP 2023, 45 KB model), for weak CPUs.
  - AEC3 and NKF-AEC are **experimental** — hidden from the menu by default; tick **Show experimental engines** on the Appearance tab to reveal them.
- **What you see is what runs** — the profile label always reads `engine` or `engine → NS`, and the status line reports the running chain, e.g. `Running (16000 Hz, DTLN-AEC + NS)`.
- **Noise suppression (DTLN-NS)** — a second DTLN pair (same DSP as the echo canceller, minus the loud-playback feed) that removes background noise from the mic. Runs after the engine on every path.
- **Voice never cut** — a soft limiter replaces hard clipping, and every fail path fades instead of muting; NKF keeps its divergence guard, staged exposure, loop brakes and no-cancel watchdog (all self-releasing, all voice-safe by construction).
- **Low CPU usage** — engines typically well under 2% per stream on a typical desktop; the post stages add a fraction of a percent.
- **Low latency** — 30–40 ms round-trip
- **Works with any audio device** — speakers, earphones, headsets
- **Selectable sample rate** (16 / 48 kHz)
- **Optional system tray** — runs in the background like a real utility
- **Device filtering** — virtual cables hidden from Mic/Reference dropdowns
- **Live level meters** (meters show the post-stage signal, i.e. what Discord hears)
- **Clock-drift correction** — stable over long calls
- **Wallpaper customization**
- **Auto-save settings** to `aec_config.txt`
- **Single-instance protection** — launching twice brings the existing window to front

---

## Web UI (Python, no installer)

![AEC Web UI](screenshots/web-ui.png)

All three engine profiles (**DTLN-AEC 128**, **WebRTC AEC3**,
**NKF-AEC**, each optionally followed by DTLN-NS) also run as a
**pure-Python local web app**: no `.exe`, no installer, nothing
for antivirus heuristics to flag — the web release ZIP scans
0 detections on VirusTotal. Python is the only prerequisite (a
one-time install — see Requirements below). Every package is a
mainstream PyPI wheel, and nothing we ship is a PE binary
(release asserts fail the build otherwise). **Never freeze it
with PyInstaller/Nuitka** — that reintroduces the exact problem.

**One click:** double-click `web\start.bat` — it creates a
local Python environment, installs packages (first run only,
~1 min), then opens the GUI in a native app window
(Edge WebView2 — no browser needed). Click it again any
time to open another window while the server keeps
running; close the window (or Ctrl+C) to stop.

Requirements: Windows 10/11 64-bit, Python 3.10+ (one-time,
free), VB-CABLE, ~500 MB free.

**Installing Python** — any one of:
- **cmd.exe or PowerShell:** `winget install -e --id Python.Python.3.12`
- **python.org installer (manual):** https://www.python.org/downloads/ —
  download it yourself, run it, and tick **"Add python.exe to PATH"**
  on the first screen. (No scripted/silent installs anywhere in
  this project — downloaders that fetch and silently run executables
  are exactly what antivirus heuristics flag.)

Then close and reopen your terminal (or reboot) so `python` is
on PATH.

Same DSP, same models, same device model as the desktop app —
mic + speaker loopback in, cleaned voice out to CABLE.
Full docs: [web/README.md](web/README.md).

---

## Quick Start

> **Prefer no installer, zero false positives?** The
> [web UI](#web-ui-python-no-installer) is the same DTLN chain
> as a pure-Python local server — double-click
> `web\start.bat` and you're done. The steps below
> are for the desktop app.

1. **Install [VB-CABLE](https://vb-audio.com/Cable/)** (free virtual audio cable). Reboot.
2. **Download** the latest release from [Releases](https://github.com/samudinzul/aec-client/releases/latest). Right-click the ZIP → Properties → **Unblock** → extract it anywhere (skips SmartScreen first-run prompts from the download mark).
3. **Run `aec_gui.exe`**.
4. **Select your devices** (the Audio tab opens straight at Profile):
   - **Your microphone** → your physical mic
   - **Your speakers** → your physical speakers
   - **Send cleaned sound to** → `CABLE Input (VB-Audio Virtual Cable)` (picked automatically)
5. **Pick an engine** at the top of the Audio tab — the label shows the engine and its chain (e.g. `DTLN-AEC 128 → NS`). Start with the default **DTLN-AEC 128**. The **Noise suppression (DTLN-NS)** tick shows for every engine. Ticks and the sample rate under More can be changed any time without losing the selection.
6. **Click Start**. Keep speakers at a moderate volume. Very loud speakers make any canceller leave echo behind (and can push AEC3 into cutting mid-sentence) — if the room must be loud, stay on DTLN-AEC 128.
7. **In Discord** → Voice & Video settings:
   - **Input Device**: `CABLE Output (VB-Audio Virtual Cable)`
   - **Input Profile**: **Voice Isolation** — one tap that turns on
     Discord's noise cleanup. Echo and hiss are already removed by
     this app (DTLN-NS handles noise on every path).
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
>
> **VirusTotal record (v1.11.0, pre-publish scans).** Both release
> ZIPs scan **0 detections**. The standalone `aec_gui.exe` draws one
> flag — SecureAge only (unsigned-file reputation); Defender,
> Bitdefender, Kaspersky and ESET are clean, and its behavior tab is
> empty of anything real (8 LOW + 10 INFO, zero dropped files, zero
> C2, zero persistence — sandbox noise like the OS trust-checking
> our file and the sandbox renaming it). The older
> `Wacatac.B!ml` / `susgen` pair no longer fires on current builds
> (publisher metadata + execution manifest + accumulated reputation).
> Any residual flag is disputed per release (see `SECURITY.md`) —
> never "fixed" with packers or obfuscation.
>
> **Why anything flags it at all.** The app is unsigned, and signing
> is the only full fix. A code-signing certificate costs ~$200–400/yr,
> and the free Microsoft route (Azure Artifact Signing) requires
> US/Canada residency for individual developers — neither is
> available for this personal build. Signing instructions are in
> [Signing releases](#signing-releases) if that ever changes. The
> build otherwise uses the stock MSYS2 UCRT64 MinGW-w64 toolchain,
> unchanged — no packers, full version metadata, declared
> no-elevation manifest.
>
> **If Windows Defender deletes the file.** On some machines Defender
> quarantines or deletes the exe outright instead of showing the
> warning screen. Extract the ZIP to any folder, open PowerShell
> **as Administrator** in that folder, and add an exclusion:
>
> ```powershell
> Add-MpPreference -ExclusionPath "."
> ```
>
> Then run `aec_gui.exe` normally. The exclusion persists until removed
> (Windows Security → Virus & threat protection → Manage settings →
> Exclusions → Add/remove).
>
> **Zero-flag alternative.** The [web UI](#web-ui-python-no-installer)
> is pure Python + data (release asserts ban all binaries), and its
> ZIP scans 0 detections. Same engines, same models, one click.

---

## Signing releases

The release ZIP is **unsigned by default**. Historically, unsigned binaries with
bundled ML runtimes tripped Microsoft's `Wacatac.B!ml` and MaxSecure's
`susgen` heuristics — neither fires on current builds (metadata +
manifest + reputation), and the standing rule is zero detections before
any ship (see `SECURITY.md` for the verdict record and process).

**A code-signing certificate is the only fix.** Two routes:

- **Paid** — DigiCert or Sectigo EV code-signing cert (~$200–400/yr).
- **Free** — Microsoft **Trusted Signing** (Azure account, no cost).
  Sign with the `az signtool` flow or `signtool sign /f <cert.pfx>`.

To sign a release, rebuild and re-zip with signing enabled:

```bash
# 1. Install the cert in your cert store (or keep the .pfx)
# 2. Rebuild:  rm -rf build && cmake -B build -G Ninja && cmake --build build
# 3. Sign + zip:
AEC_SIGN=1 \
AEC_CERT_SHA1="<your cert thumbprint>" \
AEC_TIMESTAMP_URL="http://timestamp.digicert.com" \
scripts/make-release-desktop.sh
```
(version defaults to `APP_VERSION`; explicit versions must match it.)

The `AEC_SIGN` flag is off by default — releases stay unsigned until you
have a cert. Signing runs before zipping (after the asserts), so a failed
sign never ships and the signature lands inside the ZIP.

---

## Profiles

One combo at the top of the Audio tab selects the engine. The label shows the engine name directly — plus `→ NS` while noise suppression is ticked — so the menu always tells you exactly what runs. The engine never changes any other way.

| Label | Chain | Rate | Post stages | What it is for |
|-------|-------|------|-------------|----------------|
| **DTLN-AEC 128** *(default)* | DTLN-128 → NS | 16 kHz | DTLN-NS | Best echo + noise handling; DTLN-NS removes background noise. |
| **WebRTC AEC3** | WebRTC AEC3 → NS | 16 kHz (48 kHz optional) | DTLN-NS | Reliable, well-tested baseline: strongest canceller + noise suppression. |
| **NKF-AEC** | NKF → NS | 16 kHz | DTLN-NS | Weak CPUs: cheap canceller, same post stage. |

- The **Noise suppression (DTLN-NS)** tick shows for every engine. Switching engines never resets it.
- Sample rate (More → Processing), gains and devices are independent of the profile. Only the 16-kHz-only engines (DTLN, NKF) force the rate back to 16 kHz.
- Status line shows the active chain, e.g. `Running (16000 Hz, DTLN-AEC + NS)`.
- AEC3 and NKF-AEC are experimental: hidden by default, revealed by ticking **Show experimental engines** on the Appearance tab.
- Old configs migrate automatically: the five v1.9 presets map onto these three, and an old *Manual* setup lands on the profile for its engine.

---

## Profile Comparison

| Profile | Your voice | Echo removed | CPU | When to use it |
|--------|------------|--------------|-----|----------------|
| **DTLN-AEC 128** *(default)* | Sounds natural | Very good | Moderate | **Start here.** Cleanest overall; also removes background noise. |
| **WebRTC AEC3** | Can sound a bit processed | Strongest | Moderate | Echo still getting through (loud speakers, echoey room). |
| **NKF-AEC** | Usually natural | Good (linear-only) | Small | Busy PC; or natural voice with a light cleanup chain. |

**Voice** = how natural you sound when both sides talk at once. **Echo** = how much of the other person’s speaker sound is cancelled out.

**Quick pick:**
1. Stay on **DTLN-AEC 128** unless something is wrong.
2. Still hearing echo? Try **WebRTC AEC3** (voice may sound cleaned up).
3. Busy PC? Try **NKF-AEC**. Can glitch if speaker delay drifts.

Keep speaker volume moderate — very loud speakers make any engine treat your voice as echo.

---

## Runtime Dependencies

The release ZIP bundles all required DLLs:

| File | Size | Purpose |
|------|------|---------|
| `aec_gui.exe` | ~2.4 MB | Main application (includes the NKF neural engine) |
| `libwebrtc-audio-processing-1-3.dll` | ~950 KB | WebRTC AEC3 + high-pass filter |
| `onnxruntime.dll` | ~16 MB | Neural inference (NKF, DTLN ONNX fallback; official MS build, uncompressed) |
| `msvcp140/vcruntime140*.dll` | ~900 KB | VC++ 14 runtime (required by onnxruntime.dll) |
| `tensorflowlite_c.dll` | ~4.5 MB | TFLite runtime for DTLN (primary path) |
| `libwinpthread-1.dll` | ~63 KB | MinGW thread runtime |
| `libgcc_s_seh-1.dll` | ~150 KB | GCC runtime |
| `libstdc++-6.dll` | ~2.6 MB | C++ standard library |
| `models/nkf.onnx` | ~45 KB | NKF model |
| `models/dtln_aec_128_{1,2}.tflite` | ~7 MB | DTLN-AEC 128 pair (default profile) |
| `models/dtln_ns_128_{1,2}.tflite` | ~4 MB | DTLN noise reduction pair |

**External dependency (user-installed):** [VB-CABLE](https://vb-audio.com/Cable/)

---

## Architecture

How audio flows through the app:

```
Your microphone  ─────────┐
                            ├─→  Engine  ─→  DTLN-NS  ─→  VB-CABLE  ─→  Discord / Zoom
Your speakers  ───────────┘   (AEC3 / NKF / DTLN)
```

1. **Capture** — the mic and the speaker output (loopback) are read at the same time.
2. **Cancel** — the engine subtracts the speaker sound from the mic, leaving your voice.
3. **Post stages** — the cleaned audio passes the post stage that actually runs for the engine (each with its own tick; meters show this stage — what Discord hears).
4. **Deliver** — the signal runs through the soft limiter and output gain, goes to VB-CABLE; Discord uses that as its microphone.

Settings are saved automatically. Only one copy of the app runs at a time (opening it again focuses the existing window).

---

## Technical Stack

### Languages

| Language | Role |
|----------|------|
| **C++17** | Main application |
| **Python** | Web UI (server, DSP, engines via LiteRT) |
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
| **DTLN-NS** | Neural (dual-LSTM) | ONNX/TFLite | `src/dtln_ns_wrapper.cpp` |

### Libraries

| Library | Language | Purpose |
|---------|----------|---------|
| **[miniaudio](https://github.com/mackron/miniaudio)** | C (single-header) | Cross-platform audio I/O |
| **[Dear ImGui](https://github.com/ocornut/imgui)** | C++ | Immediate-mode GUI |
| **[GLFW](https://www.glfw.org/)** | C | Window + OpenGL context |
| **[OpenGL](https://www.opengl.org/)** | — | Rendering backend |
| **[stb_image](https://github.com/nothings/stb)** | C (single-header) | Image loading for wallpapers |
| **[ONNX Runtime](https://onnxruntime.ai/)** | C++ | Neural inference (NKF-AEC, DTLN-AEC ONNX fallback) |
| **[pocketfft](https://github.com/mreineck/pocketfft)** | C++ (header) | FFT for NKF-AEC, DTLN-AEC |
| **[AudioFile](https://github.com/adamstark/AudioFile)** | C++ (header) | WAV I/O for NKF-AEC |

Web UI only (`web/`):

| Library | Language | Purpose |
|---------|----------|---------|
| **[FastAPI](https://fastapi.tiangolo.com/) + uvicorn** | Python | REST + WebSocket server, serves the GUI |
| **[ai-edge-litert](https://pypi.org/project/ai-edge-litert/)** (LiteRT) | Python | Runs the DTLN `.tflite` models |
| **[pyaudiowpatch](https://pypi.org/project/pyaudiowpatch/)** | Python | PortAudio fork: mic in, WASAPI loopback, CABLE out |
| **[numpy](https://numpy.org/)** | Python | DSP: rFFT/OLA, resampling, metering |

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
- Neural Kalman filtering (NKF-AEC) with delay alignment (TDC)
- Dual-signal LSTM echo + noise cancellation (DTLN-AEC)
- DTLN noise suppression after every engine (on/off tick)
- Soft limiter (−3 dBFS tanh knee) + fail-open fade (never a click)
- Frame buffering at 10–16 ms intervals
- Lock-free SPSC ring buffers (audio thread ↔ UI thread)
- Clock-drift correction (dynamic sample drop/duplicate)
- RMS metering (live levels show the post-stage signal)
- Self-monitor loop brakes (NKF: wire trim + escalation, fast backstop attack, no-cancel watchdog — the engine keeps adapting underneath)

---

## About the Neural Engines

### NKF-AEC

NKF-AEC (Neural Kalman Filtering for Acoustic Echo Cancellation) is a research model published at **ICASSP 2023** by Jiang et al. It combines classical Kalman filtering with a small neural network.

- **Model size**: 45 KB (~5.3K parameters — tiny *neural* core)
- **Sample rate**: 16 kHz (auto-locked)
- **Latency**: ~32 ms
- **CPU**: Small for the NKF core alone (paper RTF 0.09). **Not** measured against AEC3 in this app. The linear Kalman core carries an intrinsic Wiener residual suppressor (per-bin gain from its own echo estimate — the **NKF-AEC** profile is Kalman + RES + DTLN-NS stacked). Monitor loops meet downstream brakes only (wire trim with escalation, fast backstop attack, no-cancel watchdog): the engine never freezes or un-exposes, which field tests showed sustains howls.

The wrapper at `src/nkf_wrapper.cpp` handles real-time streaming via the `ProcessBlock()` extension we added.

The full source is patched in `third_party/REAL_TIME_NKF_AEC/` and compiled directly into `aec_gui.exe` (a standalone `libnkf_aec.dll` went stale against the header once — no separate DLL build anymore).

### Post stage: noise suppression (all engines)

One **Noise suppression (DTLN-NS)** tick shows for every engine — a second DTLN pair (same 512/128/257 DSP as the echo canceller, minus the loud-playback feed) that removes background noise after the engine. The WebRTC NS post-stage was retired; DTLN-NS replaces it everywhere. The tick is a global pref: switching engines never resets it.

- **Fail-open**: a dead or mismatched stage passes audio straight through — it can never mute or blow up the stream. `src/dtln_ns_wrapper.cpp`.

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

### DTLN noise reduction (DTLN-NS)

A second DTLN pair that removes background noise from the mic — same
512-block / 128-shift / 257-bin DSP as the echo canceller, minus the
loud-playback feed. Runs after the engine on every path
(the WebRTC NS post-stage was retired).

- **Runtime (in probe order)**:
  1. **TFLite** — `dtln_ns_128_1.tflite` + `dtln_ns_128_2.tflite` from
     [networkedaudio/Realtime_AudioDenoise_EchoCancellation](https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation)
     (a port of breizhn/DTLN denoise) into `models/`, plus the same
     `tensorflowlite_c.dll` next to `aec_gui.exe`
  2. **ONNX fallback** — `dtln_ns_128_1.onnx` + `dtln_ns_128_2.onnx`
     in `models/` (reuses the already-linked ONNX Runtime)
- **Fail-open**: any inference error (bad tensor, model mismatch, DLL
  issue) leaves the frame untouched — it never mutes the near-end
  voice.
- The wrapper is `src/dtln_ns_wrapper.cpp`; live diagnostics (backend,
  ring fill, dropped samples, in/out RMS) show under the checkbox in
  the More panel.

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
pacman -S mingw-w64-ucrt-x86_64-onnxruntime   # headers + import lib only; the runtime DLL ships in libs/
pacman -S git zip
```

### Clone

```bash
git clone https://github.com/samudinzul/aec-client.git
cd aec-client
```

### Build NKF-AEC Engine

No separate step: `third_party/REAL_TIME_NKF_AEC/c/NKFImpl.cpp` is part
of the main `aec_gui` target and builds with the rest of the app.

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

# DTLN noise reduction pair (~1.5 MB + ~2.5 MB) — runs after the engine
# on every path (WebRTC NS is retired).
curl -L -o models/dtln_ns_128_1.tflite \
  "https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation/raw/master/model/model_1.tflite"
curl -L -o models/dtln_ns_128_2.tflite \
  "https://github.com/networkedaudio/Realtime_AudioDenoise_EchoCancellation/raw/master/model/model_2.tflite"

# Plus Windows tensorflowlite_c.dll next to aec_gui.exe (extract from a release ZIP),
# or convert the pairs to models/dtln_aec_128_{1,2}.onnx and
# models/dtln_ns_128_{1,2}.onnx (ONNX Runtime fallback).

# Silero VAD was retired before any v2.0 release (the voice gate is gone).
# The WPE / adaptive-notch post stages were retired after v1.10.1
# (deleted sources; NKF now uses DTLN-NS like every other engine).
```

See [MODELS.md](MODELS.md) for sizes and SHA-256 checks.

### Build the Main App

```bash
cd /path/to/aec-client
cmake -B build -G Ninja \
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
│   ├── dtln_wrapper.cpp/.h         DTLN-AEC C wrapper (TFLite/ONNX)
│   ├── dtln_ns_wrapper.cpp/.h       DTLN noise reduction (TFLite/ONNX)
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
│   ├── dtln_aec_128_{1,2}.tflite   DTLN-AEC 128 pair (default profile)
│   └── dtln_ns_128_{1,2}.tflite    DTLN noise reduction pair
│
├── web/                        Pure-Python web UI (no .exe — see above)
│   ├── start.bat               One-click launcher (venv + packages + native window)
│   ├── gui.py                  Native window (pywebview/WebView2) + in-process server; browser fallback
│   ├── server.py               FastAPI REST + WebSocket, serves ui/
│   ├── chain.py                DTLN → NS (port of the C++ frame pump)
│   ├── audio.py                pyaudiowpatch callback-mode mic/loopback/CABLE I/O
│   ├── dsp.py                  rFFT/OLA framing, resampling, meters (+ optional Rust ext/)
│   ├── dtln.py / dtln_ns.py    DTLN-AEC / DTLN-NS engines (LiteRT)
│   ├── ui/index.html           The GUI
│   ├── test_offline.py         WAV-in → WAV-out fidelity harness
│   ├── requirements.txt        PyPI wheels
│   └── README.md               Web UI docs
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
│   ├── make-desktop-build.sh      Desktop app build (quiet, app-only, --run to launch)
│   ├── test.sh                     Offline test suite (native + web)
│   ├── make-release-desktop.sh     Desktop release bundle + zip builder
│   ├── make-release-web.sh         Web release bundle + zip builder
│   ├── standardize-releases.sh     Version-keyed release notes writer
│   └── templates/                  End-user doc templates (%%VERSION%%)
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
- **ONNX Runtime** — Microsoft (MIT)
- **Dear ImGui** — Omar Cornut (MIT)
- **miniaudio** — David Reid (MIT-0)
- **GLFW** — Marcus Geelnard & Camilla Löwy (zlib)
- **stb_image** — Sean Barrett (Public Domain)
- **pocketfft** — Max-Planck-Society (BSD-3)
- **AudioFile** — Adam Stark (MIT)
- **VB-CABLE** — VB-Audio (free for personal use)
- **DTLN noise reduction (DTLN-NS)** — networkedaudio, port of breizhn/DTLN denoise (MIT)

---

## Contributing

Pull requests welcome. For major changes, please open an issue first.

## Support

- Report bugs: [Issues](https://github.com/samudinzul/aec-client/issues)
- Discussions: [Discussions](https://github.com/samudinzul/aec-client/discussions)
