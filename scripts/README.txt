AEC Client v%%VERSION%%
================

Real-time acoustic echo cancellation for Windows.

WHAT IT DOES
------------
Routes your microphone through one of 3 AEC engines, cancels
the sound coming from your speakers, and outputs a clean mic signal
to a virtual audio cable that Discord / Zoom / Teams can use.
A neural voice gate mutes silence and passes speech automatically.

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

4. Pick an engine:
   - WebRTC AEC3          best echo removal (default, 16 / 48 kHz)
   - NKF-AEC              recommended default: most natural voice (16 kHz)
   - DTLN-AEC 512         neural echo + noise canceller (16 kHz)

5. Optional: tick "Noise reduction" under Advanced on AEC3 or NKF-AEC
   for WebRTC noise suppression on top of echo cancellation.

6. Click Start. The green SPEAKING pill means speech is going out;
   grey SILENT means the voice gate muted silence. The gate is on
   by default — uncheck Voice gate in the Audio tab to disable it.

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
- If echo comes back after a long call, click Stop then Start.
- If your voice sounds robotic, try NKF-AEC.
- The X button minimizes to the system tray (toggle in Appearance tab).
- DTLN-AEC needs models/dtln_aec_512_1.tflite +
  models/dtln_aec_512_2.tflite (bundled) and tensorflowlite_c.dll
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
