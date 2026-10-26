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

## 48 <-> 16 kHz resamplers (fixed)
The splice originally used a 3-tap boxcar downsampler and linear
interpolation upsampler. Both were measurably wrong:

| metric | old | new (193-tap Kaiser FIR) |
|---|---|---|
| 48->16 passband | -1.9 dB @6 kHz, -3.5 dB @8 kHz | flat to 7.5 kHz (<0.1 dB) |
| 48->16 alias | 12 kHz -> 4 kHz at -9.5 dB | -76 dB |
| 16->48 image | 7 kHz -> 9 kHz at -4 dB; -6.1 dB droop at 7.5 kHz | -55 dB (7.5 -> 8.5 kHz) |
| group delay | 1 / 0.5 samples | 96 @48k = 32 @16k each |

Design: one symmetric 193-tap prototype, cutoff 8 kHz at 48 kHz,
Kaiser beta 5.65326 (~60 dB), DC-normalized. Decimation runs it
directly; interpolation splits it into three phases and scales by 3.
The output grid starts at input sample 0, so the group delay is
exactly 96 @48 kHz per direction. The high-band delay line therefore
adds `2 * 96` samples, and the loopback reference is delayed by 32
samples @16 kHz so the AEC still sees mic and ref aligned (miniaudio's
own 48->16 resampler contributes ~1 sample on the ref side).
Cost: ~193 MAC per output sample per direction, well under 1% of a
core at 16 kHz.

The high-band mix is now a live UI slider (0-100%, default 70% after
the FIR resampler; 50% = the original balance, config slot 32 / gen 10).
The cleaner low band no longer droops at the crossover, so 70-100% is
usable for more air; lower it if raw-top hiss or click residue bothers
you. The slider applies without a restart.

Verification: `resampler_test` (desktop console target) checks DC
gain, linear phase, passband, aliases/images, exact group delay,
block/one-shot state continuity (including a 480+1 split frame), and
the full 16->48->16 round trip. `ALL PASS` with g++ -O2 on the Linux
host; on Windows:
`cmake --build build --target resampler_test` then
`build/resampler_test.exe`.

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
