AEC Client v%%VERSION%%

Highlights since v%%PREV%%:
%%HIGHLIGHTS%%

Quality path: desktop `aec_gui.exe` (GTCRN default NS, 48 kHz high-band splice).
Frozen fallback: web build (16 kHz only, no GTCRN / high-band splice).

Install — desktop app:
1. Install VB-CABLE (https://vb-audio.com/Cable/, free) and reboot.
2. Download AEC-Client-v%%VERSION%%-win64.zip from Assets below and extract it anywhere. (Right-click the ZIP -> Properties -> Unblock first to skip first-run prompts.)
3. Run aec_gui.exe, pick the DTLN-AEC 128 profile (default) and your devices (CABLE Input is picked automatically), click Start.
4. In Discord: input = CABLE Output, Input Profile = Voice Isolation. (Zoom/Teams: input = CABLE Output; turn off their echo cancellation.)

Install — web UI (same engines, pure Python, zero VirusTotal detections):
1. Same VB-CABLE as above, plus a one-time Python 3.10–3.14 install (`winget install -e --id Python.Python.3.12`, or python.org with "Add python.exe to PATH" ticked), then reopen the terminal so `python` is on PATH.
2. Download AEC-Web-v%%VERSION%%-win64.zip from Assets below and extract it anywhere (Unblock first, as above).
3. Double-click web\start.bat — first run takes ~1 min (local venv + packages), then opens the GUI in a native window. Click it again any time to attach another window; close the window (or Ctrl+C) to stop.
4. Same Discord routing as the desktop app (CABLE Output + Voice Isolation).

> SmartScreen notice: this app is unsigned (no paid code-signing certificate), so Windows may show "Windows protected your PC" on first launch. This is expected: click More info -> Run anyway.
> Every release is built straight from public source (https://github.com/samudinzul/aec-client) — audit it, rebuild it, or scan the ZIP on VirusTotal if unsure.

Full changelog in CHANGELOG.md.
Compare: https://github.com/samudinzul/aec-client/compare/v%%PREV%%...v%%VERSION%%