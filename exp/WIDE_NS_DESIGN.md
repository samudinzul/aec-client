# A 48 kHz neural NS — the real fix (measured)

## The hint
Hush is a DeepFilterNet3 fork — but Hush deliberately runs at **16 kHz**
("telephony-native: G.711, WebRTC, SIP"). So Hush cannot give us 48 kHz.
The **upstream** DF3 is a full-band 48 kHz system ("A Low Complexity
Speech Enhancement Framework for Full-Band Audio (48kHz) using on Deep
Filtering").

## What upstream ships
`models/DeepFilterNet3_onnx.tar.gz` in Rikorose/DeepFilterNet:
  enc.onnx (1.95 MB) + erb_dec.onnx (3.29 MB) + df_dec.onnx (3.34 MB)
  + config.ini
  config: sr=48000, fft_size=960, hop_size=480, nb_erb=32, nb_df=96,
          df_lookahead=2, conv_lookahead=2, lsnr_min=-15, lsnr_max=35
  **hop_size=480 = exactly 10 ms at 48 kHz = the app's pump frame.**
  Licence: the repo is Apache-2.0 OR MIT (dual) -> compatible.
  The ONNX graphs load in PLAIN onnxruntime (no Rust runtime needed) and
  their I/O matches the shape the existing 16 kHz Hush Python runtime
  (web/hush_onnx.py) already drives.

## Measured (upstream `deep-filter` binary, upstream's own real-speech
## asset assets/clean_freesound_33711.wav, 48 kHz)
  case                        full 0-24k   high 8-24k   voice 0-8k
  clean speech                   -0.18 dB    -0.12 dB    -0.18 dB
  clean + keyboard click         -0.32 dB    -0.36 dB    -0.32 dB
  clean + hiss                   -0.15 dB    -0.43 dB    -0.15 dB
  clean + click + hiss           -0.30 dB    -0.66 dB    -0.29 dB
  click's absolute 8-24 kHz level: 29.53 dB -> -49.45 dB  (-79.0 dB)
  hiss: -202 dB
  RTF 0.135 (7.4x faster than realtime)

NOTE: measured on a CARTOON soundboard clip ("fahhhh") the same model
removes -35 dB. The model is trained on real speech; a synthetic vocal
effect is classified as noise. Always validate on real speech.

## What it changes
A 48 kHz neural NS on the FULL band replaces both 48 kHz workarounds:
  - the high-band splice becomes unnecessary (the NS cleans 0-24 kHz)
  - the NovaSR tail becomes unnecessary (it was the "synthesized highs"
    workaround for exactly this gap)
  - the keyboard click dies completely (-79 dB) instead of -14 dB
  - the 16 kHz ceiling on the NS stage is gone
Chain becomes:
  mic 48k -> AEC -> DF3-48 NS -> 48k out
  (AEC3 supports 48 kHz natively; DTLN-AEC is 16 kHz and would need an
  upsample between the AEC and the NS)

## The work
1. 48 kHz DF3 ONNX runtime in Python: adapt web/hush_onnx.py (fft 960,
   hop 480, wnorm 1/960 instead of 1/320, the 2-frame lookahead delay,
   ERB widths at 48 kHz) and validate it reproduces the Rust binary.
2. C++ wrapper for the desktop (port the runtime, like
   src/dtln_ns_wrapper.cpp) -> a new NS backend.
3. Staging + licensing: Apache-2.0/MIT, attribution in
   LICENSES/THIRD-PARTY.txt. The release script bans staged filenames
   containing "dfn"/"deepfilter" (Hush's bundle was renamed for the
   same reason) -> the new bundle needs a compliant name.
4. Latency: model lookahead 2 (20 ms) + the NS framing, on top of the
   AEC's.

## Where the Python port actually stands (web/dfn48.py)
DONE:
  - the module exists, imports, computes the 48 kHz ERB widths (32
    bands, summing to 481 bins) and the Vorbis window
  - the ONNX bundle loads in plain onnxruntime; the GRU state surgery
    works (the 48 kHz export keeps the R matrix in a Constant NODE
    instead of an initializer -> the R-dims have to be read off that
    node's value tensor; the Hush export uses an initializer)
  - the coefs layout is generalized from the 16 kHz (64,5,2) to
    (NB_DF,5,2)
  - the model RUNS: the click's 8-24 kHz band is killed (-229 dB) and
    the hiss is gone

REMAINING (the exact gap, verified by measurement):
  the upstream runtime (libDF/src/tract.rs process() + df()) keeps a
  7-frame queue of ENHANCED spectra (df_order + lookahead) and a
  5-frame queue of NOISY spectra (df_order). At frame t it applies
  the mask+coefs computed from frame t's features to the frame t-2
  spectrum (y[df_order-1] of the 7-deep queue), then runs the 5-frame
  deep filter over the noisy queue. That pairing is what gives the
  model its 2 frames of LOOKAHEAD: the decision is made from data up
  to t but applied to the frame t-2, and the output stream therefore
  runs 2 frames behind the input.

  This port instead enhances the CURRENT frame and delays the output
  stream by 2 frames, so the coefs are paired with the wrong frame.
  Measured symptom: the deep-filter range (0-4.8 kHz = the first 96
  of the 481 bins) comes out ~12 dB too low vs the upstream runtime,
  while 4-24 kHz is within 1-2 dB. The click is killed but the voice
  is over-suppressed.

  Fix: keep both queues, apply the mask+DF to the queue entry at
  index df_order-1, and synthesize THAT spectrum.

  The ground truth for the comparison loop is the upstream Linux
  binary (the same model, the same runtime):
    deep-filter-0.5.6-x86_64-unknown-linux-musl (the release asset)
    + models/DeepFilterNet3_onnx.tar.gz
  Cross-correlate the two outputs and require corr > 0.99.

## Fetching the model
  https://raw.githubusercontent.com/Rikorose/DeepFilterNet/main/models/DeepFilterNet3_onnx.tar.gz
  (8 MB; Apache-2.0/MIT; upstream config: sr=48000 fft_size=960
  hop_size=480 nb_erb=32 nb_df=96 df_lookahead=2)
  The release script bans staged filenames containing "dfn"/
  "deepfilter", so the staged copy needs a compliant name (the Hush
  bundle was renamed the same way) plus the attribution in
  LICENSES/THIRD-PARTY.txt.

