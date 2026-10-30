AEC Web UI v%%VERSION%%
=======================

Real-time acoustic echo cancellation in your browser.
The same 16 kHz engines and models as the desktop app's
16 kHz path, running as a plain Python local server -
no .exe, so no antivirus warnings.

STATUS AND SCOPE
----------------
This is the frozen 16 kHz fallback build. It ships the
proven DTLN-AEC 128 / WebRTC AEC3 / NKF-AEC chain with
DTLN-NS or Hush.

It does NOT include the desktop app's newer GTCRN noise
suppression, the 48 kHz high-band splice, automatic
48 kHz output. For the best sound quality,
use the desktop AEC Client v1.15.0 or newer.

This bundle stays available because it is pure Python +
data and scans clean with antivirus. It receives
security, compatibility, and release fixes only - no
new audio features.

REQUIREMENTS
------------
- Windows 10 or 11 (64-bit)
- Python 3.10 - 3.14 (one-time install):

    winget install -e --id Python.Python.3.12

  Then CLOSE AND REOPEN your terminal (or reboot) so
  python is on PATH. Check with: python --version
- VB-CABLE virtual audio cable (free):
  https://vb-audio.com/Cable/  (install and reboot)
- About 500 MB free disk (Python environment + packages)
- Internet on first run (downloads the packages)

QUICK START
-----------
1. Install Python and VB-CABLE (see above), reboot.

2. Right-click the ZIP -> Properties -> Unblock, then extract
   it anywhere (skips first-run prompts from the download mark).

3. Double-click web\start.bat.
   - First run: creates a local Python environment and
     installs packages (~1 minute), then starts the
     server and opens the GUI automatically. All 3
     engines included (DTLN-AEC 128 and WebRTC AEC3 are
     regular choices; NKF-AEC unlocks by ticking "Show
     experimental engines" in Appearance).
   - The console window IS the server log - close it or
     press Ctrl+C to stop.
   - Click start.bat again any time to reopen the GUI
     while the server keeps running.

4. In the GUI, pick your devices:
   - Microphone:        your physical microphone
   - Speaker Reference: your physical speakers
                        (the "[Loopback]" entry)
   - Output:            CABLE Input (picked automatically)

   Quiet mic? Raise the "Microphone level" slider under
   More -> Levels (0-200%, live). "Noise suppression"
   removes hiss - select Off (live) to halve model CPU.

5. Click Start. Talk with speakers on. Keep the mic away
   from the speakers - no canceller holds a loud
   mic-to-speaker loop forever (the NKF profile trims a
   sustained howl instead of feeding it).

SOUNDBOARD
----------
Drop .wav / .mp3 / .flac / .ogg sound files into the
web\sounds\ folder (refresh the Soundboard tab to
load them). Click a pad to play a sound on top of your
voice - one pad at a time, a new click takes over from
whatever is still playing. The Soundboard tab also has
a gain slider, a "play to my speakers" monitor toggle
and a device dropdown (so you hear the pads locally
while voice apps still get them through CABLE), and a
Stop-all button. Files are decoded once at startup, so
a pad click never touches the disk.

6. In Discord (or Zoom/Teams), open Voice & Video settings:
   - Input Device:  CABLE Output (VB-Audio Virtual Cable)
   - Input Profile: Voice Isolation (Discord's own noise
     cleanup ON; echo and hiss are already removed here).
     Manual alternative: Custom profile with Echo
     Cancellation OFF (this app does it), Noise
     Suppression Krisp, Automatic Gain Control OFF.

WEAK OR LOW-END PC?
-------------------
In order of impact:
1. Pick the NKF-AEC profile (tick "Show experimental
   engines" in Appearance to reveal it) - smallest
   engine, designed for weak CPUs.
2. Select Off in the "Noise suppression" radio on the
   Audio tab - halves the neural models loaded.
3. Skip the native window: instead of web\start.bat,
   run "python -m web.server" in a terminal and open
   http://localhost:8000/ in a browser you already
   have open. Saves the WebView2 renderer processes
   (~100 MB+ RAM).
Models are tiny (NKF is 45 KB), so RAM is rarely the
bottleneck - the Python runtime itself uses more than
the audio chain ever will.

WHY NO ANTIVIRUS WARNING
-------------------------
This bundle is Python + mainstream PyPI wheels
only - no .exe, no .dll, no .pyd, no packed executables,
so antivirus heuristics have no target: recent release ZIPs
scan 0 detections on VirusTotal. (A local-only Rust
accelerator exists in ext/ for developers; it never
ships in this ZIP. The desktop .exe is unsigned and carries
a one-vendor reputation flag; see the repo README for the
full record and the dispute process.)

Do NOT freeze or pack this with PyInstaller/Nuitka -
that would reintroduce the exact problem.

TROUBLESHOOTING
---------------
- The window flashes and disappears: run web\start.bat
  from an open cmd.exe window so the error stays on
  screen (startup errors are also logged to
  web\start.log).
- "no Python found": install Python 3.10-3.14, close and
  reopen your terminal, then try again.
- Device dropdowns empty: click Rescan in the Devices
  section.
- Nothing cancelled: the Speaker Reference must be the
  loopback of the speakers that are actually playing
  sound.
- Choppy audio: keep the speaker volume moderate - very
  loud speakers make any canceller leave echo behind.
  If voices sound clipped, select Off in "Noise
  suppression" and retest.

LICENSE
-------
MIT License. Source: https://github.com/samudinzul/aec-client
