AEC Web UI v%%VERSION%%
=======================

Real-time acoustic echo cancellation in your browser.
The same engines and models as the desktop app, running
as a plain Python local server - no .exe, so no antivirus
warnings.

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

2. Extract this ZIP anywhere.

3. Double-click web\start.bat.
   - First run: creates a local Python environment and
     installs packages (~1 minute), then starts the
     server and opens the GUI automatically.
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
   removes hiss - untick it (live) to halve model CPU.

5. Click Start. Talk with speakers on. Note: unlike the
   desktop app, this build has no feedback-notch stage,
   so a sustained speaker howl is NOT suppressed -
   keep the mic away from the speakers.

6. In Discord (or Zoom/Teams), open Voice & Video settings:
   - Input Device:  CABLE Output (VB-Audio Virtual Cable)
   - Input Profile: Voice Isolation (Discord's own noise
     cleanup ON; echo and hiss are already removed here).
     Manual alternative: Custom profile with Echo
     Cancellation OFF (this app does it), Noise
     Suppression Krisp, Automatic Gain Control OFF.

WHY NO ANTIVIRUS WARNING
-------------------------
The desktop app is an unsigned .exe that bundles ML
runtimes, which trips two heuristics (Wacatac.B!ml,
susgen). This bundle is Python + mainstream PyPI wheels,
with one optional locally-built accelerator (aec_dsp.pyd,
see ext/README.md) - no packed executables, so those
heuristics have no packed-binary target.

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
  If voices sound clipped, untick "Noise suppression"
  and retest.

LICENSE
-------
MIT License. Source: https://github.com/samudinzul/aec-client
