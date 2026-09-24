# Changelog

All notable changes to AEC Client are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.8.1] — 2026-09-24

### Changed

- **Voice gate is softer and more natural.** The Silero decision is
  EMA-smoothed and mapped through a close→open soft knee, so weak
  speech, fricatives (`s`/`f`/`th`), and natural breath pauses only
  partially duck instead of snapping. Hangover extended 500→800 ms;
  close debounce fixed to ~160 ms (was 50 ms despite the comment);
  gentler attack/release ramps. Still a −12 dB floor — never hard
  mute. Fail-open, lock-free, and the no-mid-call-threshold-drift
  contract are unchanged.
- **"Push down silence" is now at the top of the Audio tab** (same
  visibility as Noise reduction) instead of only under Advanced.
  Calibration, sensitivity, and Speaking/Silent status stay under
  Advanced → Voice gate.

## [1.8.0] — 2026-09-24

### Added

- **NKF: "Residual echo kill (AEC3)" toggle.** The post-NKF WebRTC
  AEC3 pass (always-on since 1.7.1) aggressively eats echoey leftovers
  NKF can't remove, but its multi-band suppressor can make voice sound
  processed/robotic on some setups while speakers play. It is now a
  checkbox on NKF-AEC (default ON — current behavior unchanged) so
  users can instantly A/B against raw NKF output.
- **NKF: optional "Dry voice (room reverb)" stage (GTCRN).** New
  checkbox on NKF-AEC only: routes the already echo-cancelled output
  through GTCRN, an ultra-light 16 kHz streaming enhancement model
  (MIT, ~0.5 MB, RTF ≈ 0.07), for a drier mic with less room reverb.
  Off by default — opt-in; costs a small amount of CPU and ~32 ms
  extra delay while enabled, live-toggles with the same brief engine
  restart as Noise reduction. VAD gate, meters and the self-monitor
  loop detector all see the dry signal. While it is on, the residual
  APM's WebRTC NS is skipped automatically (GTCRN denoises itself —
  stacking both only over-suppresses); the Noise reduction checkbox
  still applies whenever Dry voice is off. Missing model file = stage
  silently off (fail-open), NKF itself is unaffected.

### Changed

- **Default DTLN model is now the 128-unit pair (`dtln_aec_128_{1,2}.tflite`).**
  Same DTLN-AEC architecture and 512-sample DSP block; 1.8M params
  (~7 MB) instead of 10.4M (~41 MB) — lower default-engine CPU and a
  much smaller release ZIP. Quality is slightly below the old 512-unit
  pair (upstream still ships both). The 512 files are removed from the
  tree, CMake copy list, and release allowlist.
- **Voice gate (Silero) defaults OFF.** Fresh installs, missing config
  field, and Reset to defaults all start with "Push down silence"
  unchecked (opt-in under Advanced → Voice gate). Existing configs that
  already saved the flag keep their value.

### Fixed

- **DTLN-AEC 128: stage-2 full-scale blowup (tensor role map).**
  Stage 2 of the 128-unit pair has equal-sized inputs
  (`est` / `state` / `lpb` all 512 floats). The wrapper classified
  "state = largest input", so the tie picked index 0 and wrote the
  estimated waveform into the LSTM state tensor — silent mic + silent
  reference produced pegged full-scale output. Roles now follow the
  upstream `run_aec.py` layout (`in[0]=feat, in[1]=state, in[2]=feat`)
  for both TFLite and ONNX. The 512-unit pair was unaffected (state
  was strictly larger); this only bit the new default 128 models.
- **DTLN / engines: no uninitialized output on the early-return path.**
  `DtlnProcess` now zeros the frame when the handle is unusable, and
  `output_callback` zeros `cleanedFrame` before dispatch so a skipped
  engine can never emit stack garbage.
- **NKF crash on engine create/destroy.** `NKFImpl` grew new members;
  they are appended after `m_windows` so existing field-built
  `libnkf_aec.dll` binaries keep stable layout (rebuild the DLL once
  with this tree if you ship a custom build).
- **Audio-thread heap allocations removed (NKF / GTCRN / Silero).**
  Per-block `new`/`delete` and FIFO front-erases are gone; state lives
  in the handle and windows compact once per Process. Same math,
  lower and more stable callback cost under load.
- **ImGui UI glyphs.** Em-dashes, ellipsis, and arrows in app strings
  were rendering as `?` under the default font (no U+2014 etc.).
  Narrow UI strings now use ASCII (` - `, `...`, `->`); the tray
  tooltip keeps Unicode (shell, not ImGui).
- **NKF "experimental" label removed.** With TDC, staged exposure, and
  the divergence guard in place, the picker/docs call NKF the lightest
  engine (still notes the linear/loopback-delay caveat) instead of
  experimental.

### Notes

- Discord preset tooltip now names the recommended Discord **Input
  Profile** (Voice Isolation, or Custom with EC off / Krisp / AGC off).
  Docs only — the app does not change Discord's profile.

## [1.7.1] — 2026-09-22

- **Download:** [AEC-Client-v1.7.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.7.1/AEC-Client-v1.7.1-win64.zip) (Windows 10/11 64-bit)

### Changed

- **First launch and Reset to defaults now select the Windows system
  default devices.** Fresh installs (and the Reset to defaults button)
  pick the OS "Default Device" for microphone and speakers instead of
  whatever happens to sit first in enumeration order. Send-cleaned-sound-to
  still prefers CABLE Input (the routing target voice apps listen to),
  falling back to the system default output when no cable is installed.
  Side fix: saved device selections now survive restarts — the first
  scan after launch honors the config's saved indices (previously the
  empty-name lookup reset mic/speakers to the first device every launch).
- **NKF: residual WebRTC AEC3 stage, always on — echo cut
  aggressively.** NKF is strictly *linear*: what it leaves behind
  (near-end double-talk residuals, time-varying/speaker-distortion
  echo) used to ship raw. NKF output now always runs through the
  same AEC3 as the standalone engine (nonlinear multi-band residual
  suppression, delay-agnostic) with the raw ref fed every frame —
  the "echoey" sound gets actively suppressed. The stage also runs
  on shadow / fail-open mic passthrough, so echo kill never depends
  on NKF being exposed (Discord mic test / Listen to myself included;
  AEC3 tolerates self-monitoring by design). The Noise reduction
  checkbox still toggles only WebRTC NS on top (Moderate).

### Fixed

- **NKF blowout: TDC + staged exposure + divergence guard.**
  NKF-AEC is a *linear* canceller whose upstream requires the far-end
  to be delay-aligned (their GCC-PHAT "-a" TDC) — the real-time WASAPI
  path never aligned it, so the Kalman filter diverged until output
  pegged full-scale. The wrapper now estimates the ref-to-mic lag by
  normalized cross-correlation (eager every ~64 ms until first lock,
  then ~0.5 s; 0–100 ms search, jump-gated) and feeds NKF a
  delay-aligned reference. Output exposure is staged on one continuous
  block timeline: warm-up and a 512 ms post-lock shadow phase carry mic
  while the engine runs monitored-only, then a 256 ms crossfade brings
  NKF in — cold-start spikes are never heard, and there are no
  engagement holes or replays. A guard watches output-vs-input energy
  (~100 ms catch); trips drop back to shadow rather than exposing a
  reset, and after six trips the session fails open to permanent mic
  passthrough. A **self-monitor loop detector** correlates ref against
  our own recent output (~every 128 ms, box-decimated): with "Listen
  to myself" or a Discord mic test on speakers, ref carries our output
  back around the acoustic loop, and NKF's live Kalman adaptation
  inside that loop can ring — while the loop is detected, exposure
  holds on mic (DTLN/AEC3 tolerate self-monitoring; NKF now sits it
  out). Lowest-CPU engine made safe to use.

## [1.7.0] — 2026-09-22

- **Download:** [AEC-Client-v1.7.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.7.0/AEC-Client-v1.7.0-win64.zip) (Windows 10/11 64-bit)

### Changed

- **Default engine is now DTLN-AEC 512.** Fresh installs, Reset to
  defaults, the Discord preset, and retired/invalid config remaps all
  land on DTLN (cleanest output, removes noise too). The picker is
  reordered DTLN → AEC3 → NKF: WebRTC AEC3 becomes the second choice
  (strongest echo cut), NKF-AEC demoted to third.
- **NKF-AEC demoted from default.** Field reports of blown-out /
  overflowing output after extended testing, on top of the engine's
  documented constraints: it is a *linear* echo canceller and its
  upstream requires time-delay compensation — real-time WASAPI
  loopback has variable delay and no alignment, so the Kalman filter
  can diverge (the ICASSP 2023 paper itself warns of "unacceptable
  results" under covariance misestimation). Kept as an option for the
  lightest CPU; picker label, tooltips, README, and packaged quick
  start now say "experimental — may distort".

### Fixed

- **NKF's Noise reduction pass fails open on APM errors.**
  `ProcessStream`'s return code was ignored — on failure the previous
  frame's buffer replayed forever (harsh looping full-scale noise).
  Errors now bypass NS for that frame, shipping the raw NKF output.

## [1.6.1] — 2026-09-22

- **Download:** [AEC-Client-v1.6.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.6.1/AEC-Client-v1.6.1-win64.zip) (Windows 10/11 64-bit)

### Fixed

- **Reset to defaults now leaves Push down silence ON**, matching a
  fresh install (the gate has defaulted ON since 1.3.0, and a missing
  config field loads ON). OFF-on-Reset dated from 1.4.0, when Reset
  also cleared the now-removed "Only my voice" tick; with that layer
  nuked in 1.6.0 the remaining half was a plain inconsistency.

## [1.6.0] — 2026-09-22

- **Download:** [AEC-Client-v1.6.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.6.0/AEC-Client-v1.6.0-win64.zip) (Windows 10/11 64-bit)

### Added

- **Noise reduction checkbox (Audio tab, under the preset picker).** Extra WebRTC
  background-noise suppression (Moderate: hiss, fans) on top of echo
  removal for WebRTC AEC3 and NKF-AEC — off by default, live-togglable
  (the engine restarts for a split second, same path as switching
  presets mid-call). Hidden on DTLN, which already removes noise
  itself. Persisted in the retired preprocess config slot, so
  `aec_config.txt` stays positionally aligned; presets leave it alone,
  Reset-to-defaults turns it off.

### Removed

- **SpeexDSP and LocalVQE engines cut.** Both failed the double-talk
  voice-preservation test (your voice gets cut when both sides talk at
  once), same as the earlier legacy demotion — now removed outright.
  Old configs pointing at them auto-remap to WebRTC AEC3 on load, so
  nothing breaks; the picker shows the three engines that earn their
  slot: WebRTC AEC3, NKF-AEC, DTLN-AEC. 1,028 files and ~326k lines of
  vendored code (GGML tree, SpeexDSP sources, models) dropped from the
  repo.
- **Owner-only voice gate (PVAD/ECAPA) layer removed.** The
  speaker-recognition gate that only ever enabled itself for one
  enrolled voice is gone: sources, model paths, enrollment flow, and
  all UI. The regular Silero voice gate ("Push down silence") is
  unchanged on every engine.

### Fixed

- **AEC3 loud-speaker caveat documented, not coded.** Live testing:
  speakers much louder than the mic make AEC3's suppressor read
  double-talk as "all echo" and clamp the human voice (ratio-driven
  suppressor, no identity concept — the industry-wide #1 AEC failure
  mode). No code lever exists in the packaged WebRTC API (config
  struct published, no setter), so the fix is guidance: gain
  structure first (speakers down, mic up/closer), NKF second
  (least near-end distortion by design). Picker tooltip, Quick
  Start, and ranking footnote updated; DTLN's noise-suppression 5/5
  stands, AEC3's echo-removal edge is level-qualified.

### Changed

- **Ranking columns renamed to outcomes.** "Voice quality /
  Double-talk" become "Voice preservation / Echo removal" (a brief
  "Noise suppression" stint was dropped — it made echo-only engines
  look broken at a job they were never hired for; DTLN's denoising
  stays in its Best-For line). Preservation is governed by the
  hardest case (double-talk).

## [1.5.0] — 2026-09-21

- **Download:** [AEC-Client-v1.5.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.5.0/AEC-Client-v1.5.0-win64.zip) (Windows 10/11 64-bit)

### Added

- **Legacy engines demoted.** SpeexDSP and LocalVQE failed the
  double-talk voice-preservation test (your voice gets cut when both
  sides talk at once), so the engine picker now shows AEC3 / NKF /
  DTLN by default. A "Show legacy engines" tick under the picker
  reveals them; saved legacy configs land on AEC3 unless the tick is
  on. "Low CPU" preset moved to NKF, "Noisy Room" to DTLN.
- **DEC baseline residual cleanup spiked then nuked.** Harness-promising
  (29 dB echo cut on synth) but failed live (aggressive voice cut
  with AEC3) and failed isolation: on echo-free speech it keeps 17%
  with silent ref and 0.07% with active ref — the cascade sees
  (cleaned output, live speakers) exactly when AEC3 already did its
  job, and slams the mask shut. Full revert of wrapper, UI, docs,
  weights (FireRed treatment). Lesson: a full-AEC mask is the wrong
  shape for a post-AEC polisher; residual work needs a model trained
  for residual, not a repurposed echo canceller.
- **Engine ranking refreshed from research + testing.** NKF promoted
  above DTLN on voice preservation (AECMOS-Other 4.02 vs 3.73,
  Jiang et al. ICASSP 2023); DTLN keeps the cleanliness crown
  (AECMOS-Echo 4.31, plus noise removal). Legacy rows demoted on
  stars to match the failed double-talk test. Presets reordered to
  the same ranking (AEC3 group, then NKF, then DTLN) — a saved
  preset index from an older config may point at a renamed slot;
  re-pick once.
- **Presets minimally de-generalized.** "Echo-Heavy Room" now runs
  AEC3 at 16 kHz (faster per-band convergence for long reverb tails;
  full-band returns when the room allows). "High Quality" is cut —
  it duplicated Discord's settings and only confused; old configs
  remap (HQ→Discord) via a new preset-generation trailer field.
- **NKF is the default engine.** "Discord (recommended)" runs NKF
  @16 kHz; fresh installs and Reset land there too. "Low CPU" is
  cut (it duplicated the new Discord NKF settings). Four preset
  slots, each distinct; pre-cut configs remap again (Low CPU→
  Discord, old Noisy→new Noisy) via preset generation 3.
- **AEC3 dethroned on naturalness.** Its suppressor leaves voices
  sounding processed/cleaned, so Voice drops to 4 and DTLN (worst
  near-end quality of the three at AECMOS-Other 3.73) to 3; NKF
  takes "most natural voice". AEC3 keeps double-talk 5 and the
  "strongest echo removal" crown — muscle, not beauty.

## [1.4.0] — 2026-09-20

- **Download:** [AEC-Client-v1.4.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.4.0/AEC-Client-v1.4.0-win64.zip) (Windows 10/11 64-bit)

### Added

- **First-run setup card + Advanced collapse.** Newcomers (no config
  file) get a 3-step live checklist (mic, speakers, CABLE) that
  retires after the first Start; engine/levels/gate live under an
  "Advanced" header with no close-X (always visible, toggle only,
  state remembered, open on reset). Returning users unaffected.
- **Reference status line.** Live Levels reports "receiving speaker
  audio" vs "silent (normal if nothing is playing…)" — informs,
  never false-warns.
- **Plain-language UI.** "Your microphone", "Your speakers",
  "Send cleaned sound to", human engine descriptions, dev-only text
  (filenames, check-ms, params) moved out; Mic/Output gains merged
  into one "Microphone level"; "Custom" preset renamed
  "Manual settings"; Output row hidden while self-monitoring; dead
  rate combo replaced with "16 kHz (automatic)"; Start/Stop +
  Live Levels visible on every tab.
- **FireRed Stream-VAD spiked then nuked.** Harness-verified but
  failed the live listening test — full revert of wrapper, picker,
  and docs. Silero stands alone again.
- **Missing voice-gate model now fails open.** A broken Silero
  handle previously read as eternal silence; it now nulls to
  gate-off with the correct UI notice.
- **Enrollment monitor no longer echoes.** "Learn my voice" played
  raw mic at full volume with no AEC running — speaker output looped
  back into the mic as audible echo. The monitor is now a sidetone
  tracking the Microphone level at x0.7 (near the calibration
  loudness, loop gain kept below feedback).
- **Gentler gate dynamics.** 500 ms hangover (was 300 ms) bridges
  word pauses and keeps endings intact; the uncertain band between
  the open/close lines now leans open instead of just holding, so
  onsets are partway through before the open line even trips.
  True silence still releases as before.
- **One-tap voice-gate calibration.** "Calibrate for my mic" works
  idle or running (auto-starts audio with monitor routing — you'll
  hear yourself with live meters, no CABLE needed; always back to
  idle after with Start clickable) and listens 5 s
  while you speak, then sets open/close thresholds from measured
  probs (quiet mics land ~0.20/0.12, loud mics stay ~0.45/0.27); rejects flat/silent takes
  with a keep-old message. Step-process like Learn my voice, with
  Re-calibrate + Reset (0.50/0.30); re-calibrate after mic/engine
  switch. Bounded one-shot — no mid-call drift, so the v1.3.1
  adaptive-mute failure can't recur.
- **Owner-only voice gate PVAD (experimental, off by default).**
  "Only my voice": one tap on "Learn my voice" opens a mic-only
  session with live monitoring, counts down 8 seconds, and saves
  your voiceprint — no need to Start first (running sessions stop
  first, then enroll; idle stays idle after). A worker-thread ECAPA
  check (~1 Hz) then also mutes OTHER voices (TV, family) — only
  you pass. "Forget my voice" deletes the voiceprint (two-step
  confirm). Timing stays with the Silero gate; identity only ever
  mutes on measured mismatch (fail-open otherwise). New
  `src/pvad_wrapper.*`; voiceprint stays on the PC, model untracked.
  Harness: same-voice 0.840 vs cross-voice 0.673 (synth).

### Fixed

- **Reset to defaults now lands clean.** It stops a live session
  (or enrollment capture) first — never reconfigures running devices —
  "Send cleaned sound to" picks CABLE Input (index 0 only if no
  virtual cable is installed), and both voice-gate ticks — Push down
  silence + Only my voice — come back unticked (a kept voiceprint is
  preserved, just disabled). Advanced collapses for a simple landing.
- **Footer buttons live on the Audio tab only.** Start/Reset no
  longer show under Appearance/About; Live Levels still do.

- **Mic/out no longer die while the speakers are idle.** The output
  path waited for loopback frames that never come when nothing plays,
  so CABLE-output sessions sat dead (frozen meters included) until
  sound played on the speakers. Missing ref now reads as digital
  silence — which is what "nothing playing" means to the AEC.
- **Gate ducking fixed with a silence floor.** The voice gate used to  ride the output gain to full mute on every prob wobble (audible
  pumping on all engines, piling onto engine ducking in double-talk).
  Silence now shelves to −12 dB instead of mute, release needs ~160 ms
  of firm "silent" before fading, and the uncertain band leans open
  half as hard. Tradeoff: faint background passes at −12 dB.

## [1.3.2] — 2026-09-18

- **Download:** [AEC-Client-v1.3.2-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.3.2/AEC-Client-v1.3.2-win64.zip) (Windows 10/11 64-bit)

### Added

- **Output auto-selects CABLE Input.** Fresh installs (or a saved
  device that disappeared) land on CABLE Input instead of index 0;
  plus a **Listen to myself** tick that routes the cleaned mic to
  your Speaker Reference device for monitoring (nothing goes to
  VB-CABLE). Choice is saved; status shows `[monitor]` while active.
- **VB-CABLE missing warning.** When no CABLE device is present, the
  Devices section shows a red hint (install/enable + Refresh), and
  Start refuses with a status message unless Listen-to-myself is on.
  (Windows lists only enabled devices, so disabled looks the same as
  not installed — the hint covers both.)

### Changed

- **Voice gate is now pure neural VAD.** The adaptive close line
  (noise floor + 0.15) is nuked: fixed hysteresis, open at 0.50,
  close at 0.30 — the uncertain band holds the last decision, so
  quiet speech is never cut by a drifting close line. Hangover,
  fades, and fail-open bypass unchanged.
- **SpeexDSP is 16 kHz only, gated by Silero again.** The gate
  under-scored Speex output off a downsampled 48 kHz feed, so Speex
  joins the 16 kHz-only club (native feed, 0.50 / 0.30 like every
  other engine) — old Speex-at-48 kHz configs and the Noisy Room
  preset migrate automatically. The gate bypass experiment is reverted.
- **Speex cleanup (preprocess) retired.** The Silero gate does the
  silencing and the two stacked sounded aggressive — the checkbox is
  gone, old configs with it on land on gate-only.

## [1.3.1] — 2026-09-18

- **Download:** [AEC-Client-v1.3.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.3.1/AEC-Client-v1.3.1-win64.zip) (Windows 10/11 64-bit)

### Fixed

- **Second launch now brings the app window forward.** The
  single-instance lookup used a hardcoded "v1.0.0" window title, so on
  every later version the duplicate exited without focusing the
  running window. The title is now built from the app version.

## [1.3.0] — 2026-09-18

- **Download:** [AEC-Client-v1.3.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.3.0/AEC-Client-v1.3.0-win64.zip) (Windows 10/11 64-bit)

### Added

- **Voice gate (Silero VAD → adaptive gate → output)** — post-AEC
  neural speech detector (snakers4/silero-vad v6.2.1, MIT, ~2.2 MB,
  sha256-verified): speech passes, silence is muted. No setup, no
  recording, ON by default (one checkbox to disable, choice saved)
  - New `src/silero_wrapper.{h,cpp}`: 512-sample streaming chunks
    with 64-sample context (upstream recipe), LSTM state carried
    across chunks, reset on Start/Stop/engine change; already-linked
    ONNX Runtime, no new DLL; < 1 ms per chunk, runs inline on the
    audio thread — no worker, 32 ms cadence
  - Adaptive gate: fixed 0.5 open line with a noise-floor-tracking
    close line (+0.15, clamped) against flutter; ~30 ms fade open,
    ~50 ms fade to hard mute, 300 ms hangover against clipping tails
  - 16 and 48 kHz live (model is 16 kHz fixed; at 48 kHz a
    miniaudio downsample feeds only the detector — proven
    0.945/0.008 vs native 0.956/0.010 speech/noise, so audio stays
    full-rate with no accuracy cost); green **SPEAKING** / grey
    **SILENT** header pill + live probability readout in the Audio tab

### Removed

- **32 kHz sample rate** — rates are now 16 / 48 kHz only: the
  dropdown, `Noisy Room` preset (now 48 kHz), and saved configs
  (old 32 kHz entries land on 48 kHz automatically) all migrated

## [1.2.1] — 2026-09-17

- **Download:** [AEC-Client-v1.2.1-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.2.1/AEC-Client-v1.2.1-win64.zip) (Windows 10/11 64-bit)

### Fixed

- **LocalVQE word endings no longer clipped.** The SDK noise gate
  (`-45 dBFS`, always on) was muting low-energy speech tails, so words
  sounded cut off — most audible during double-talk. The gate is now off;
  the joint network suppresses background noise on its own
  (`src/localvqe_wrapper.cpp`). If background hiss in silence bothers you,
  the fallback is re-enabling the gate at `-60 dBFS` (same line).

## [1.2.0] — 2026-09-17

- **Download:** [AEC-Client-v1.2.0-win64.zip](https://github.com/samudinzul/aec-client/releases/download/v1.2.0/AEC-Client-v1.2.0-win64.zip) (66 MB, Windows 10/11 64-bit)

### Added

- **DTLN-AEC 512 engine** — dual-signal LSTM echo + noise canceller
  (Westhausen & Meyer, ICASSP 2021, MIT)
  - New `src/dtln_wrapper.{h,cpp}`: 512-sample block / 128-sample shift /
    257-bin FFT pipeline with overlap-add, LSTM state carry-over, and
    int16 ↔ float bridging to the 160-sample audio callback
  - Dual backend probed in order: TFLite pair
    (`models/dtln_aec_512_1.tflite` + `_2.tflite` via `tensorflowlite_c.dll`
    loaded at runtime, no link-time dependency) then ONNX pair
    (`models/dtln_aec_512_1.onnx` + `_2.onnx` via the already-linked
    ONNX Runtime)
  - `ENGINE_DTLN = 4` wired through `ReinitEngine`, audio callback,
    `StartAEC` guard (`Failed to load DTLN model`), 16 kHz auto-lock,
    engine dropdown + tooltip, and shutdown cleanup
  - About tab now lists five engines and credits DTLN-AEC; old
    `aec_config.txt` engine indices are clamped on load
  - CMake copies any present `models/dtln_aec_512_*` files and an optional
    `libs/tensorflowlite_c.dll` next to `aec_gui.exe`; build succeeds with
    no DTLN files (engine reports missing models at Start)
- `APP_VERSION` bumped to `1.2.0`

### Notes

- DTLN runs at 16 kHz only; model files are user-supplied (see README
  “Download Models”) until vendored into a release
- `ldd` check: 0 `not found` after the change

## [1.1.1] — 2026-09-16

### Fixed

- Engine dropdown tooltip now documents all four engines (was missing LocalVQE v1.4-AEC)
- Sample-rate lock tooltip covers both 16 kHz-only engines (NKF-AEC / LocalVQE)
- Start / Reset to defaults footer (plus Live Levels) now shows on the Audio tab only;
  hidden on Appearance and About tabs

## [1.1.0] — 2026-09-16

### Added

- **LocalVQE v1.4-AEC engine** — compact neural echo canceller
  - Echo-only model: removes far-end echo while preserving voice, room tone, and background noise
  - 203K parameters, 2.8 MB model size
  - DAF (Deep Adaptive Filtering) front-end
  - Built-in residual noise gate (-45 dBFS default)
  - Very low CPU usage (~0.83 ms per 16 ms frame at 16 kHz)
  - Fixed 16 kHz sample rate (auto-locked when selected)
- About tab now lists four AEC engines and credits LocalVQE
- README updated with full technical stack, engine comparison table, and NKF-AEC / LocalVQE documentation
- Build automation: CMakeLists now copies all required runtime DLLs next to `aec_gui.exe` on every build
- LocalVQE diagnostic logging: `localvqe_diag.txt`, `localvqe_stdout.txt`, `localvqe_stderr.txt` written on launch for troubleshooting

### Changed

- Engine dropdown reordered for clarity: SpeexDSP, WebRTC AEC3, NKF-AEC, LocalVQE
- `EngineType` enum extended with `ENGINE_LOCALVQE = 3`
- Copyright holder updated to `samudinzul`

### Fixed

Multiple DLL bundling issues discovered when testing the release on a clean Windows PC (no MSYS2 installed):

- **Missing `glfw3.dll`** — GLFW window library was not bundled
- **Missing `libabsl_*.dll`** — Abseil runtime required by WebRTC AEC3
- **Missing ONNX Runtime dependencies** — `libonnx.dll`, `libprotobuf-lite.dll`, `libre2-11.dll`, `libutf8_validity.dll`
- **Missing `libgomp-1.dll`** — GNU OpenMP runtime required by all GGML CPU backends. Without it, LocalVQE failed with `Registered backends (0)` and refused to load
- **Missing `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`** — MinGW runtime dependencies

The release ZIP now ships **123 DLLs** and runs on a fresh Windows 10/11 installation with no developer toolchain.

### Notes

- LocalVQE runs at 16 kHz only
- Verified on two machines:
  - AMD Ryzen 5 5600 (dev machine)
  - AMD Ryzen 3 3200G (fresh Windows 11 installation)

## [1.0.0] — 2026-09-15

### Added

- Three AEC engines: WebRTC AEC3, SpeexDSP, NKF-AEC (experimental)
- Real-time audio processing with drift correction
- Selectable sample rate (16 / 32 / 48 kHz)
- Device filtering (hides virtual cables on Mic/Reference dropdowns)
- Presets for common scenarios (Discord, Low CPU, High Quality, Noisy Room)
- Live level meters with peak hold
- Optional system tray minimize
- Single-instance protection (mutex)
- Wallpaper customization
- Settings auto-save to `aec_config.txt`
- Built with Dear ImGui + GLFW + miniaudio

[1.1.1]: https://github.com/samudinzul/aec-client/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/samudinzul/aec-client/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/samudinzul/aec-client/releases/tag/v1.0.0