# aec-web — local web UI wrapper for AEC Client (DTLN-only v1)

Real-time echo cancellation + noise suppression in your browser, powered
by a plain Python script on your own machine. No `.exe`, no installer,
nothing for antivirus heuristics to flag.

## What you need first

1. **Windows 10 or 11 (64-bit).**
2. **Python 3.10 or newer** — must be installed *before*
   the first run (the starter script uses it to create a local
   environment). Install it with winget:
   ```powershell
   winget install -e --id Python.Python.3.12
   ```
   Close and reopen your terminal afterwards so `python` is on PATH.
   Check with `python --version` (3.12 recommended; 3.14 works too —
   every package below ships wheels for it). Python from the
   Microsoft Store also works but winget is more reliable.
3. **VB-CABLE** (https://vb-audio.com/Cable/, free) — install and reboot.
4. About 500 MB free disk (Python environment + packages + models).

You do NOT need: admin rights, a C++ compiler, CMake, Node, or Docker.
Everything else (FastAPI, numpy, LiteRT, sounddevice) installs itself
on first run.

## Why this exists

`aec_gui.exe` is unsigned and bundles ONNX Runtime + TFLite, so
Microsoft `Wacatac.B!ml` and MaxSecure `susgen` flag it (2/71 false
positive). A code-signing certificate (~$200/yr) is the only fix for
the exe — this project sidesteps it instead: **ship `.py`, not `.exe`.**
Every ML package here is a mainstream reputable wheel; nothing we write
is a PE binary. **Never freeze this with PyInstaller/Nuitka** — that
reintroduces the exact problem.

## Setup

**Easy path:** double-click `web\start.bat`. It creates the local
environment, installs packages, starts the server, and opens the GUI.
Skip to Use below.

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
3. Click Start. Talk with speakers on.
4. In Discord: input = CABLE Output.

## What runs

- **DTLN-AEC 128** (`models/dtln_aec_128_*.tflite` via `ai-edge-litert`)
  — the default desktop profile, ported sample-accurately: 512-block /
  128-shift / 257-bin, Hann-free rFFT like the C++ pocketfft path.
- **DTLN-NS** (`models/dtln_ns_128_*.tflite`) after the engine,
  fail-open — missing/broken models pass audio through, never mute.

16 kHz only (same auto-lock as the desktop DTLN path).

## Files

| File | Role | Mirrors |
|------|------|---------|
| `dsp.py` | rFFT/OLA framing, int16/float, RMS | `dtln_wrapper.cpp` DSP |
| `dtln.py` | DTLN-AEC engine + LiteRT backend | `DtlnNew/Process/Reset` |
| `dtln_ns.py` | DTLN-NS stage + ring + stats | `DtlnNsNew/Process/Stats` |
| `chain.py` | DTLN → NS stacking, fail-open | `main.cpp` frame pump |
| `audio.py` | sounddevice mic/loopback/CABLE I/O | desktop device model |
| `server.py` | FastAPI REST + WS + serves `ui/` | Audio tab semantics |
| `ui/index.html` | The GUI | — |
| `test_offline.py` | WAV-in → WAV-out fidelity harness | — |

## Verify the port

```bash
python web\test_offline.py mic.wav ref.wav out.wav
```

Plays a mic/ref pair through the chain offline. Compare `out.wav`
against the desktop app's output on the same pair — RMS must match
within tolerance (see script output).
