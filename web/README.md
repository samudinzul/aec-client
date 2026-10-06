# aec-web — local web UI wrapper for AEC Client (DTLN-only v1)

Real-time echo cancellation + noise suppression in a
native app window (or your browser), powered
by a plain Python script on your own machine. No `.exe`, no installer,
nothing for antivirus heuristics to flag.

## What you need first

1. **Windows 10 or 11 (64-bit).**
2. **Python 3.10 or newer** — must be installed *before*
   the first run (the starter script uses it to create a local
   environment), any one of:
   - **cmd.exe or PowerShell:**
     ```powershell
     winget install -e --id Python.Python.3.12
     ```
   - **PowerShell silent install (no winget needed):**
     ```powershell
     Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe" -OutFile "$env:TEMP\python-installer.exe"; Start-Process -Wait -FilePath "$env:TEMP\python-installer.exe" -ArgumentList "/quiet", "PrependPath=1"
     ```
     Downloads the official installer and runs it silently, adding Python to PATH (no admin needed).
   - **python.org installer (manual):** https://www.python.org/downloads/ —
     run it and tick **"Add python.exe to PATH"** on the first screen.
   Close and reopen your terminal afterwards so `python` is on PATH.
   Check with `python --version` (3.12 recommended; 3.14 works too —
   every package below ships wheels for it). Python from the
   Microsoft Store also works, but the two routes above are more reliable.
3. **VB-CABLE** (https://vb-audio.com/Cable/, free) — install and reboot.
4. About 500 MB free disk (Python environment + packages + models).

You do NOT need: admin rights, a C++ compiler, CMake, Node, or Docker.
Everything else (FastAPI, numpy, LiteRT,
pyaudiowpatch, pywebview, pystray, Pillow)
installs itself on first run. `scipy` is optional
(see requirements.txt) and speeds up the notch
bypass when present — install separately if you
want the lowest CPU.

## Why this exists

`aec_gui.exe` is unsigned and bundles ONNX Runtime + TFLite, so
Microsoft `Wacatac.B!ml` and MaxSecure `susgen` flag it (2/71 false
positive). A code-signing certificate (~$200/yr) is the only fix for
the exe — this project sidesteps it instead: **ship `.py`, not `.exe`.**
Every ML package here is a mainstream reputable wheel; nothing we write
is a PE binary. **Never freeze this with PyInstaller/Nuitka** — that
reintroduces the exact problem.

## Setup

**One click:** double-click `web\start.bat`. It creates
the local environment, installs packages (first run
only, ~1 min, needs internet), then opens the GUI in
a native app window (Edge WebView2 — built into
Windows 10/11, so no browser is needed) once the
models are loaded. Click it again any time to open
another window against the running server. Stop:
close the window — the server stops with it — or
press Ctrl+C in the console. The Appearance tab
minimizes to the system tray instead (on by
default — the server keeps running; the console
hides once the UI loads (no taskbar button), so
only the window is visible, and only the tray
icon is visible while the window is closed; Show
restores both, Quit from the tray icon stops
it). Running start.bat inside Windows Terminal?
The terminal window itself stays (it holds your
own tabs) — double-click start.bat instead for
the fully hidden experience, or minimize the
terminal yourself. If WebView2 is ever
missing, the default browser opens instead.

**Manual path** (same thing, step by step):

```bash
cd C:\path\to\aec-client
python -m venv .venv && .venv\Scripts\activate
pip install -r web\requirements.txt
python -m web.server
```

Open http://localhost:8000 — that page IS the GUI.

## Use

1. Install VB-CABLE (https://vb-audio.com/Cable/, free) and reboot.
2. In the page: Microphone = your mic, Speaker Reference = your
   speakers (loopback), Output = CABLE Input.
3. If your mic is quiet, raise the **Mic gain (preamp)** slider
   under Microphone (−12…+12 dB, live, default 0 dB). Boosting
   also boosts background noise — watch the Mic in meter.
4. Click Start. Talk with speakers on.
5. In Discord: input = CABLE Output.
6. Optional, both live: untick **Noise suppression (DTLN-NS)**
   to halve model CPU at the cost of background hiss, or untick
   **Feedback notch** to skip howl suppression and save the
   remaining Python overhead (howls will no longer be caught).

## What runs

- **DTLN-AEC 128** (`models/dtln_aec_128_*.tflite` via `ai-edge-litert`)
  — the default desktop profile, ported sample-accurately: 512-block /
  128-shift / 257-bin, Hann-free rFFT like the C++ pocketfft path.
- **DTLN-NS** (`models/dtln_ns_128_*.tflite`) after the engine,
  fail-open — missing/broken models pass audio through, never mute.
- **Feedback suppression** (`notch.py` + `speech_gate.py`, ports of
  the desktop `notch.cpp` + `speech_gate.h`): a sustained howl
  (speaker too close to the mic, mic-test playback) is tracked and
  cut after ~3.5 s — two 60 Hz adaptive notches that only latch on
  narrowband tones, so voice is never touched. The cut releases
  ~2 s after the howl stops (the engine leaves a brief residual
  at the howl frequency that the notch rides out). Unticking
  **Feedback notch** in the UI skips this stage entirely.
- **Mic preamp** (`chain.py` `mic_gain`, `micGain` pref): −12…+12 dB
  applied to the mic before the engine (linear 0.1×…4.0×, clamped
  server-side), for quiet microphones.

16 kHz only (same auto-lock as the desktop DTLN path).

## Files

| File | Role | Mirrors |
|------|------|---------|
| `dsp.py` | rFFT/OLA framing, int16/float, RMS | `dtln_wrapper.cpp` DSP |
| `dtln.py` | DTLN-AEC engine + LiteRT backend | `DtlnNew/Process/Reset` |
| `dtln_ns.py` | DTLN-NS stage + ring + stats | `DtlnNsNew/Process/Stats` |
| `chain.py` | DTLN → NS → notch stacking, fail-open | `main.cpp` frame pump |
| `speech_gate.py` | RMS-hysteresis voice gate + stuck-tone watchdog | `speech_gate.h` |
| `notch.py` | LMS adaptive feedback-suppression notch (2 sections, 60 Hz) | `notch.cpp` |
| `audio.py` | pyaudiowpatch callback-mode mic/loopback/CABLE I/O, resampled to 16 kHz at the edges | desktop device model |
| `server.py` | FastAPI REST + WS + serves `ui/` | Audio tab semantics |
| `gui.py` | Native window (pywebview/WebView2): in-process server, preloads models before opening, attaches to a running server, system tray, browser fallback | — |
| `ui/index.html` | The GUI | — |
| `test_offline.py` | WAV-in → WAV-out fidelity harness | — |

## Verify the port

```bash
python web\test_offline.py mic.wav ref.wav out.wav
```

Plays a mic/ref pair through the chain offline. Compare `out.wav`
against the desktop app's output on the same pair — RMS must match
within tolerance (see script output).
