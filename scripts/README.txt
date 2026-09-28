AEC Client v%%VERSION%%
================

Real-time acoustic echo cancellation for Windows.

WHAT IT DOES
------------
Routes your microphone through a processing profile (echo canceller +
noise filter), cancels the sound coming from your speakers, and
outputs a clean mic signal to a virtual audio cable that Discord /
Zoom / Teams can use.

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

4. Pick an engine at the top of the Audio tab — the label shows
   the engine and its chain (the engine is not configurable on
   its own):
   - DTLN-AEC 128        dual-LSTM core — the default: best echo
     + reverb handling, its own noise removal (16 kHz).
   - WebRTC AEC3         the Chrome canceller — reliable,
     well-tested baseline (16 kHz, 48 kHz optional).
   - NKF-AEC             tiny Kalman core — for weak CPUs
     (16 kHz).

5. Optional: two ticks under the profile control the post chain —
   "Dereverb (WPE)" (eats reverb tails, ~32 ms delay) and
   "Feedback suppression (notch)" (kills howling/ringing tones).
   Both are ON by default for every profile, persist across
   restarts, and are never reset by picking a profile; while
   ticked the profile label reads "engine -> WPE -> Notch".
   The sample rate under Advanced is also independent of the
   profile.

6. Click Start. The status line shows the active chain, e.g.
   "Running (16000 Hz, AEC3 + WPE + Notch)".

7. In Discord (or Zoom/Teams), open Voice & Video settings:
   - Input Device:       CABLE Output (VB-Audio Virtual Cable)
   - Input Profile:       Voice Isolation (Discord's own noise
      cleanup ON; echo and hiss are already removed by this app).
      Manual alternative: Custom profile with Echo Cancellation OFF
      (this app does it), Noise Suppression Krisp (NKF-AEC on weak
      PCs), Automatic Gain Control OFF.

TIPS
----
- CPU-sensitive? Try "NKF-AEC". DTLN/AEC3/NKF are all usually
  well under 2% on a typical desktop; the WPE + notch post stages
  add a fraction of a percent.
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
- Choppy audio: switch to the 16000 sample rate under Advanced.
- DTLN quiet: it runs at 16 kHz only (auto-locked).

LICENSE
-------
MIT License. See LICENSES/THIRD-PARTY.txt for third-party credits.
Source: https://github.com/samudinzul/aec-client
