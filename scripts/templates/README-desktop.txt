AEC Client v%%VERSION%%
======================

Real-time acoustic echo cancellation for Windows.

> **IMPORTANT — READ THIS BEFORE DOUBLE-CLICKING aec_gui.exe**
>
> This app is unsigned, which is the whole story: unsigned executables
> that bundle ML runtimes (ONNX Runtime + TFLite) draw heuristic
> attention, and this app legitimately uses both for echo cancellation
> and noise suppression. Current release scans clean (0 detections on
> the ZIP; the bare exe draws at most one minor-vendor reputation
> flag — Defender, Bitdefender, Kaspersky and ESET pass it).
> False positives get disputed per release, never "fixed" with packers.
>
> What you'll see depends on your machine:
>
> - **A warning screen** ("Windows protected your PC") → click
>   **More info → Run anyway**.
> - **Windows Defender deletes the file** → extract the ZIP to any
>   folder, open PowerShell **as Administrator** in that folder, and
>   run:
>
>   ```
>   Add-MpPreference -ExclusionPath "."
>   ```
>
>   Then run aec_gui.exe normally.
>
> The app works fine once allowed. This warning is not malware — it's
> the cost of not paying for a code-signing certificate (~$200/yr).
> Source: https://github.com/samudinzul/aec-client

WHAT IT DOES
------------
Routes your microphone through a processing profile (echo canceller +
post stage), cancels the sound coming from your speakers, and outputs a
clean mic signal to a virtual audio cable that Discord / Zoom / Teams
can use.

REQUIREMENTS
------------
- Windows 10 or 11 (64-bit)
- VB-CABLE virtual audio cable (free):
  https://vb-audio.com/Cable/

QUICK START
-----------
1. Install VB-CABLE if you haven't already. Reboot.

2. Double-click aec_gui.exe

3. Select your devices:
   - Microphone:        your physical microphone
   - Speaker Reference: your physical speakers (the sound to cancel)
   - Output:            CABLE Input (VB-Audio Virtual Cable)

4. Pick an engine at the top of the Audio tab — the label shows the
   engine and its chain (the engine is not configurable on its own):
   - DTLN-AEC 128        dual-LSTM echo canceller — the default:
     best echo + noise handling, with noise suppression
     (16 kHz, 48 kHz with high-band splice).
   - WebRTC AEC3         the Chrome canceller — reliable, well-tested
     baseline, strongest canceller (16 kHz, 48 kHz optional).
   - NKF-AEC             tiny neural Kalman core — for weak CPUs (16 kHz).
   WebRTC AEC3 and NKF-AEC are hidden by default.
   Tick "Show other AEC engines" on the Appearance tab to reveal
   them; unticking falls back to DTLN-AEC 128.

5. Noise suppression for every engine: Off / DTLN-NS / Hush / GTCRN.
   GTCRN is the default ("default; hiss + keyboard"). While ticked
   the profile label reads "engine -> NS/Hush/GTCRN".
   High-band splice (default on) gives 48 kHz output with real 8-24 kHz
   highs. The sample rate under Processing is independent of the
   profile.

6. Click Start. The status line shows the active chain, e.g.
   "Running (16000 Hz, DTLN-AEC + NS)".

7. In Discord (or Zoom/Teams), open Voice & Video settings:
   - Input Device:       CABLE Output (VB-Audio Virtual Cable)
   - Input Profile:       Voice Isolation (Discord's own noise
      cleanup ON; echo and hiss are already removed by this app).
      Manual alternative: Custom profile with Echo Cancellation OFF
      (this app does it), Noise Suppression Krisp (NKF-AEC on weak
      PCs), Automatic Gain Control OFF.

TIPS
----
- Weak or low-end PC? In order of impact: (1) pick the NKF-AEC
  profile (tick "Show other AEC engines" on the Appearance tab
  to reveal it), (2) untick "Noise suppression" on the Audio tab
  to halve the neural models loaded, (3) stay at the default 16000
  sample rate. RAM is rarely the bottleneck - the models are tiny
  (NKF is 45 KB) - and every engine is well under 2% CPU on a
  typical desktop.
- Self-monitoring ("Listen to myself") routes output to your speakers.
  Prefer headphones for mic tests to avoid feedback loops.
- If echo comes back after a long call, click Stop then Start.
- Very loud speakers can let some echo through (or make AEC3 mistake
  your voice for echo) — keep them moderate, or switch to
  DTLN-AEC 128.
- If your voice sounds processed or robotic on AEC3, stay on the
  default DTLN-AEC 128 (or try NKF-AEC if it sounds clean on your
  setup).
- The X button minimizes to the system tray (toggle in Appearance tab).
- DTLN needs models/dtln_aec_128_1.tflite +
  models/dtln_aec_128_2.tflite (bundled) and tensorflowlite_c.dll
  (bundled). Without them it reports "Failed to load DTLN model".
- DTLN needs models/dtln_ns_128_1.tflite +
  models/dtln_ns_128_2.tflite (bundled) for noise suppression;
  without them it reports the NS error and passes audio through.

SOUNDBOARD
----------
Drop .wav / .mp3 / .flac / .ogg sound files into the sounds\ folder
(refresh the Soundboard tab to load them). Click a pad to play a
sound on top of your voice - one pad at a time, a new click takes
over from whatever is still playing. The Soundboard tab also has a
gain slider, a "play to my speakers" monitor toggle with a device
dropdown (so you hear the pads locally while voice apps still get
them through CABLE), and a Stop-all button. Files are decoded once
at startup, so a pad click never touches the disk.

TROUBLESHOOTING
---------------
- "No devices found": click Refresh in the Devices section.
- App won't start: make sure all DLLs are in the same folder as the .exe.
- Choppy audio: keep the 16000 sample rate (Processing section).
- DTLN quiet: it runs at 16 kHz only (auto-locked).

LICENSE
-------
MIT License. See LICENSES/THIRD-PARTY.txt for third-party credits.
Source: https://github.com/samudinzul/aec-client