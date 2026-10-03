AEC Client v%%VERSION%%
======================

Real-time acoustic echo cancellation for Windows.

> **IMPORTANT — READ THIS BEFORE DOUBLE-CLICKING aec_gui.exe**
>
> This app is unsigned, and two heuristics flag it: Microsoft
> `Wacatac.B!ml` and MaxSecure `Trojan.Malware.300983.susgen`. Both are
> false positives — they key on unsigned Windows executables that bundle
> ML runtimes (ONNX Runtime + TFLite), and this app legitimately uses both
> for echo cancellation and noise suppression. Only 2 of 71 antivirus
> vendors flagged it.
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
     best echo + noise handling, with DTLN-NS noise suppression
     (16 kHz).
   - WebRTC AEC3         the Chrome canceller — reliable, well-tested
     baseline, strongest canceller + DTLN-NS (16 kHz, 48 kHz optional).
   - NKF-AEC             tiny neural Kalman core — for weak CPUs,
     with WPE dereverb + adaptive notch (16 kHz).
   WebRTC AEC3 and NKF-AEC are experimental: hidden by default.
   Tick "Show experimental engines" on the Appearance tab to reveal
   them; unticking falls back to DTLN-AEC 128.

5. Post stages are engine-aware — only the stages that actually run
   for the selected engine show as ticks (under the More panel):
   - DTLN-AEC 128 / WebRTC AEC3: "Noise suppression (DTLN-NS)"
     — a second DTLN pair that removes background noise.
   - NKF-AEC: "Dereverb (WPE)" (eats reverb tails, ~32 ms delay) and
     "Feedback suppression (notch)" (kills howling/ringing tones).
   While ticked the profile label reads "engine -> NS" (DTLN/AEC3)
   or "engine -> WPE -> Notch" (NKF). Switching engines resets each
   stage to its default, so a tick left on for another engine can
   never leak into a chain that doesn't run it.
   The sample rate under More is also independent of the profile.

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
- CPU-sensitive? Try "NKF-AEC". DTLN/AEC3/NKF are all usually well
  under 2% on a typical desktop; the post stages add a fraction of a
  percent.
- Self-monitoring (Listen to myself, Discord mic test) on SPEAKERS
  loops your voice back into the mic — NKF adapts live and is the most
  sensitive to that loop; DTLN/AEC3 tolerate it. Prefer headphones
  for mic tests.
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
- The WPE and notch post stages are model-free (in-tree DSP) —
  nothing to install; toggle them off if you want the raw engine.

TROUBLESHOOTING
---------------
- "No devices found": click Refresh in the Devices section.
- App won't start: make sure all DLLs are in the same folder as the .exe.
- Choppy audio: switch to the 16000 sample rate under More.
- DTLN quiet: it runs at 16 kHz only (auto-locked).

LICENSE
-------
MIT License. See LICENSES/THIRD-PARTY.txt for third-party credits.
Source: https://github.com/samudinzul/aec-client