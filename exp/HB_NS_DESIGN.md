# High-band NS — validated design (measured, not guessed)

## Problem
The 16 kHz NS models (DTLN-NS / Hush / GTCRN) cannot reach above 8 kHz.
The splice copies the 8-24 kHz band from the raw 48 kHz mic, so a
keyboard click loses its low band to the NS and keeps its sharp part.

## THE CLICK MODEL MATTERS
The first version of this gate was tuned against a WHITE-NOISE click.
That was the bug:

| click | high/low band ratio | old gate |
|---|---|---|
| white noise (the old test) | +4.95 dB | -34 dB (killed) |
| **realistic keyboard click** | **-12.86 dB** | **0 dB (passed)** |
| mouse click | -3.47 dB | -9.2 dB |

A real keycap bottoming out is a damped resonance at 2-6 kHz plus a
short switch tick. Its energy above 8 kHz is ~13 dB BELOW its body, so
any "high band vs low band" ratio never rises. The gate did nothing.
(The click model lives in the offline harness: a damped-mode + tick
generator, not white noise.)

## Chosen design
- Mic captured at 48 kHz. The high band is delayed by the chain latency
  so it lands on the same instant as the processed low band. The system
  latency does NOT grow (the output is already that old because of the
  chain). The high-band NS's own 256-sample STFT latency is subtracted
  from that delay.
- A REAL noise suppressor for the high band: STFT spectral subtraction
  (512-point, 256 hop, sqrt-Hann analysis+synthesis) with a
  decision-directed a priori SNR, applied only to the bins above 8 kHz.
  The click is a 1-5 ms broadband burst, so the decision-directed
  estimator (whose a priori SNR cannot jump on a transient) suppresses
  it; a fricative lasts 30-100 ms and survives.
- A VAD gate: the high band is closed entirely while the 16 kHz chain's
  post-NS output is below -72 dBFS (digital silence). That removes hiss
  and a click typed between words, with no cost to speech.

## Measured (48 kHz, 8-24 kHz band, real speech from web/sounds/)
  the high-band NS alone:
    keyboard click  -14.2 dB | mouse click -13.6 dB | hiss -8.9 dB
    voice  -2.7 dB (fahhh) / -1.3 dB (shut-your-f-mouth)
  with the VAD:
    keyboard click  -13.4 dB | hiss -41.6 dB | voice -3.0 / -1.7 dB

## REJECTED alternatives (all measured)
1. **Reuse the 16 kHz NS verdict** (pre-NS minus post-NS): DTLN-NS
   removes 8-20 dB of REAL speech, so the verdict says "noise" during
   the voice -> the voice loses its air.
2. **Standalone STFT spectral subtraction, no decision-directed term**:
   a click and a speech onset look the same spectrally.
3. **High/low band level ratio**: see above — a real click's energy is
   below 8 kHz.
4. **Running the 16 kHz NS on the high band** (band translation: complex
   downconvert by 8 kHz, decimate to 16 kHz, denoise, shift back):
   kills the click (-32.9 dB) but eats the voice's air (-35.5 dB) —
   a 16 kHz speech model does not recognise band-shifted speech. This
   was measured with the REAL Hush model (web/hush_onnx.py), not a
   stand-in.
5. **A broadband transient detector on the low band**: the click's
   0-8 kHz burst is only ~13 dB above the speech, so the transient is
   masked by the voice it rides on.

## Latency (measured with web/dtln.py, web/aec3.py, web/nkf.py,
## cross-correlation against a 4 ms burst, 16 kHz samples)
  DTLN-AEC 480 | AEC3 128 | NKF 480 | DTLN-NS 896
  chain latency (DTLN-AEC + DTLN-NS) = 1376 @16k = 4128 @48k = 86 ms
  Estimates (not measured): LocalVQE 512, Hush 320, GTCRN 512.
  A wrong estimate costs a timbre flam only — the click suppression
  does not depend on it (the NS is self-contained).

## The architectural truth
The splice injects 8-24 kHz content that NO suppressor has ever seen.
A click that the 16 kHz NS kills in the low band is resurrected in the
high band. The high-band NS above removes -14 dB of it; removing it
COMPLETELY needs a 48 kHz NS (none exists in this repo).
NovaSR does not have this problem by construction: its highs are
synthesized FROM the cleaned 16 kHz output, so a click killed in the
low band has no click in the highs.
