AEC Client v%%VERSION%%
================

Real-time acoustic echo cancellation for Windows.

WHAT IT DOES
------------
Routes your microphone through one of 3 AEC engines, cancels
the sound coming from your speakers, and outputs a clean mic signal
to a virtual audio cable that Discord / Zoom / Teams can use.
A neural voice gate is available (off by default) to push silence down
and pass speech automatically.

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

4. Pick an engine (or a Quick preset on the Audio tab):
   - DTLN-AEC 128         recommended default: neural echo + noise (16 kHz)
   - WebRTC AEC3          strongest echo removal (16 / 48 kHz; 48 kHz costs more CPU)
   - NKF-AEC              small neural core (16 kHz), strictly linear.
                          "Dereverb (WPE)" (default ON) and "Noise
                          reduction" stack on top. "Low CPU (NKF)"
                          preset = bare NKF with both off.

5. Optional: tick "Noise reduction" under Advanced on AEC3 or NKF-AEC
   for WebRTC noise suppression on top of echo cancellation. On
   NKF-AEC, "Dereverb (WPE)" (default ON) strips room reverb after
   the canceller (small extra CPU, ~32 ms extra delay) — untick it
   to hear the raw canceller output.

6. Click Start. Optional: tick "Push down silence (neural voice
   detector)" at the top of the Audio tab — the green SPEAKING pill
   then means speech is going out and grey SILENT means silence is
   pushed down. Soft gate keeps breaths natural. OFF by default.
   Calibrate under Advanced -> Voice gate.

7. In Discord (or Zoom/Teams), open Voice & Video settings:
   - Input Device:       CABLE Output (VB-Audio Virtual Cable)
   - Input Profile:       Voice Isolation (one tap: Discord's noise
      cleanup ON; echo is already removed by this app — DTLN and the
      optional Noise reduction checkbox also remove noise, bare
      AEC3/NKF-AEC are echo-only).
      Manual alternative: Custom profile with Echo Cancellation OFF
      (this app does it), Noise Suppression Krisp (Standard on weak
      PCs), Automatic Gain Control OFF.

TIPS
----
- CPU-sensitive? Try the "Low CPU (NKF)" preset (bare NKF, dereverb
  and noise reduction off). DTLN/AEC3/NKF are all usually well under
  2% on a typical desktop.
- Self-monitoring (Listen to myself, Discord mic test) on SPEAKERS
  loops your voice back into the mic — NKF adapts live and is the most
  sensitive to that loop; DTLN/AEC3 tolerate it. Prefer headphones
  for mic tests.
- If echo comes back after a long call, click Stop then Start.
- Your voice is protected: the near-end protector keeps the chain
  from ducking you more than 15 dB, even in double-talk. Very loud
  speakers can still let some echo through (or make AEC3 cut mid-
  sentence) — keep them moderate, or switch to DTLN-AEC.
- If your voice sounds processed or robotic on AEC3, stay on the
  default DTLN-AEC (or try NKF-AEC if it sounds clean on your setup).
- The X button minimizes to the system tray (toggle in Appearance tab).
- DTLN-AEC needs models/dtln_aec_128_1.tflite +
  models/dtln_aec_128_2.tflite (bundled) and tensorflowlite_c.dll
  (bundled). Without them it reports "Failed to load DTLN model".
- The voice gate needs models/silero_vad.onnx (bundled). Without
  it the gate stays off and audio passes through unchanged.

TROUBLESHOOTING
---------------
- "No devices found": click Refresh in the Devices section.
- App won't start: make sure all DLLs are in the same folder as the .exe.
- Choppy audio: switch to the 16000 sample rate in the Engine section.
- DTLN quiet: it runs at 16 kHz only (auto-locked).

LICENSE
-------
MIT License. See LICENSES/THIRD-PARTY.txt for third-party credits.
Source: https://github.com/samudinzul/aec-client
