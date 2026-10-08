# Changelog

All notable changes to AEC Client are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **Desktop NKF backstop release on voiced speech.** Attack now
  needs two consecutive center-stable tonal windows (tracked
  dominant-bin runs: a howl parks on one bin for 1.3 s+, voice
  wanders), and a single qualifying heal window releases
  (BS_HEAL_RUN 1 — field logs showed heal alternating with run
  forever, so consecutive windows never arrived). A false trigger
  on voice releases within ~2 s instead of latching forever;
  fixed-center howls still attack and hold. Known trade:
  fast-sweeping howls never hold one bin long enough to attack
  (accepted: stable loop tones, the dangerous case, always do).
- **Desktop NKF anti-wander + backstop floor.** TDC no longer
  teleports between periodicity artifacts (coarse pass needs a
  clearly-better peak to displace the delay, near-ties keep the
  smaller lag; applied motion slew-limited to 512 samples/update —
  the 800 ms search range is preserved for genuine long round
  trips). The howl backstop now floors at −24 dB instead of diving
  to −84 dB, so a false trigger attenuates rather than mutes.
- **Desktop NKF reference-parity fixes.** Two deviations from the
  official `nkf.py` closed: the analysis/synthesis window is now the
  periodic Hann the model trained on (`sin²(π·i/N)` instead of the
  symmetric `sin(π·i/(N−1))` — exact 50%-overlap sum), and silent
  far-end frames now skip inference with mic passthrough
  (`lpb-history mean < 1e-5`, same quantity the reference checks),
  which also saves the ONNX invoke on silence. Hop stays 512
  (halving it would double inference CPU for a retraining-scale
  fidelity question — deliberately deferred).

### Changed

- **Desktop: DTLN-NS on every engine, WPE/notch removed.** The NKF
  path now runs the same DTLN-NS post stage as DTLN/AEC3 (NKF is
  16 kHz-only, so the stage always applies when ticked) instead of
  its own WPE dereverb + adaptive notch. `src/wpe.*`,
  `src/notch.*`, `src/speech_gate.h` and `models/gtcrn_stream.onnx`
  (long-unused leftover) are deleted; the UI shows one NS tick for
  all engines, the profile label reads `engine -> NS`, and the
  status line reports `+ NS`. Config slots for the retired toggles
  are preserved positionally (written 0, gen bumped to 9).
  Motivation: the NKF output-suppression investigation showed the
  backstop trimming a wandering-TDC residual — one shared,
  well-understood post stage beats three engine-specific ones.

### Added

- **Web NKF aggressive-on-loop brakes.** A confirmed loop now
  leans on everything downstream of the Kalman core — never the
  core itself (no freeze, no un-expose: both sustain howls per
  desktop field tests). Self-releasing brakes: a -6 dB wire trim
  while engaged (~0.5 s release) that ESCALATES to -12/-18 dB on
  gapless loud sustain (marginal stability at high coupling
  howls forever at -6; bursty voice resets the run so mic tests
  never escalate), single-window backstop attack on tonal-stable
  wire (heal still demands proof), eager TDC cadence, plus a
  no-cancel watchdog (loud on both legs with wire-as-loud-as-mic
  past grace + ~5 s trims like a loop — covers clipped-ADC,
  beyond-range, jitter-chaotic AND false-confident locks by
  keying on cancellation evidence; skipped failed-open; gated on
  echo-significance so loud voice over quiet music never trips
  it). Closed-loop synth at coupling 1.3–2.0, clipped and
  long-delay: bounded, trimmed, released; failed-open stays
  braked too. Telemetry gains `loopDb`/`nocancelHot`; both web
  UIs show them. (`web/nkf.py`, `web/test_nkf_loop.py` L1–L7.)

- **Desktop NKF parity: intrinsic RES + aggressive-on-loop brakes.**
  Port of the web-proven stages back into the C++ they came from
  (same constants, same order): the periodic-Hann Kalman core
  gains a Wiener residual suppressor (per-bin gain from its own
  echohat, echo-minority bins pass at unity); the wrapper gains
  the -6 dB loop trim with -12/-18 dB escalation on gapless loud
  sustain, single-window backstop attack while looped, eager TDC
  cadence while looped, and the no-cancel watchdog (cancellation
  evidence, echo-significance gated, skipped failed-open).
  `NkfState` grows `loopDb`/`resDb`/`nocancelHot` (appended);
  the UI shows loop-trim and RES lines next to Backstop trim;
  the smoke table prints `loopDb`/`resDb` columns (verdict logic
  untouched). (`NKFImpl.h/.cpp`, `src/nkf_wrapper.{h,cpp}`,
  `src/main.cpp`, `tools/nkf_smoke.cpp`. Needs a desktop
  `--fresh` rebuild to verify (no Windows toolchain here).

- **Web NKF intrinsic residual suppressor (Wiener post-filter).**
  The linear Kalman core passes nonlinear distortion, loud
  residue and late tails, and DTLN-NS is noise (not echo)
  suppression — so the NKF engine now carries its own
  suppressor, like AEC3's built-in one: per-bin gain from the
  core's internal `echohat` (echo-majority bins trim toward
  -20 dB, the rest pass at unity, floored so nothing gates).
  Double-talk safety is structural (minority-echo bins never
  attenuate); silent far-end releases fast and passes through.
  Part of the engine, not a chain stage — the NS toggle still
  governs DTLN-NS only. Measured: +18 dB on echo-only synth,
  bit-identical output with no far-end energy, healthy
  fingerprint intact on doubletalk (locked, exposed, 0 resets,
  0.0 dB backstop). Gated by `enable_res` (default on) for A/B;
  `resDb` telemetry mirrors in both web UIs.
  (`web/nkf.py` RES_* constants, `web/test_nkf_res.py` T1–T3.)

- **Web engines: WebRTC AEC3 + NKF-AEC (desktop parity).** The web
  chain runs the same PROFILES table as the desktop (DTLN-AEC 128
  / WebRTC AEC3 / NKF-AEC) with the same experimental-engines
  gating, live hot-swap (streams are engine-agnostic), and DTLN-NS
  stacked after every engine. AEC3 is `pywebrtc-audio`'s
  EchoCanceller (the real Chrome code: AEC + HP filter, no NS/AGC
  — 36.9 dB echo cut on the synth pair). NKF is a faithful NumPy
  port of `NKFImpl` + the wrapper stages (periodic-Hann core,
  1/16 level scaling, TDC alignment, shadow/crossfade exposure,
  divergence guard, loop telemetry, howl backstop with the -24 dB
  floor) running the same `models/nkf.onnx` — the offline synth
  shows the healthy fingerprint (TDC locks the true 800-sample
  delay, exposed, 0 guard resets, 0.0 dB backstop on voice).
  Missing backends disable their profile options instead of
  failing at Start; NKF lag/lock/exposure/backstop telemetry
  mirrors the desktop lines in both web UIs.
  (`web/aec3.py`, `web/nkf.py`, `pywebrtc-audio` + `onnxruntime`
  in `web/requirements.txt`.)

- **Web UI optional Rust accelerator (`ext/aec_dsp`).** PyO3
  extension (rustfft + numpy crates, maturin build) replacing the
  hot DSP kernels in `web/dsp.py` — int16/float converts (exact),
  rFFT/masked-iFFT (≤5e-6 vs pocketfft), RMS, FIR + linear
  resamplers (exact).   `dsp.py` falls back to NumPy when the
  extension is absent; end-to-end smoke output is identical
  (`frames=250 out_rms=902.3 peak=8385`). Measured ~8% chain-CPU
  reduction on the test machine; TFLite `invoke()` is unchanged
  (moving inference itself to Rust needs the TF Lite C toolchain
  — tracked as follow-up in `ext/README.md`). Single worker now
  pinned explicitly (`workers=1`) in `server.py`/`gui.py`.

- **Web UI mic preamp gain.** A `Microphone level` slider sits under
  More → Levels (0–200%, live, default 100%, like the desktop
  client). It amplifies the mic before echo cancellation
  (`micGain` pref, linear 0.1×…4.0× clamped server-side); boosting
  also amplifies background noise, so the hint points at the Mic
  meter for clipping.

### Fixed

- **Web UI 48 kHz output silence.** `dsp.resample_from_16k` called a
  `_fir_interpolate_3` that was never defined, so any cable output
  running at 48 kHz raised `NameError` inside the PortAudio callback
  (swallowed → silence). The zero-stuff ×3 + 61-tap lowpass (gain ×3)
  upsampler now exists; a 160-sample frame round-trips to 480 samples
  at unity DC gain.

- **Web UI NS double model load.** `DtlnNs.__init__` loaded its
  `.tflite` pair twice (6 live interpreters instead of 4); it now
  loads once and builds its rings in the same step.

### Changed

- **Web UI DTLN-only CPU pass (no new engines).** Same audio, less
  Python overhead: `np.roll` buffer shifts replaced with in-place
  memmove slices, `rms()` in float32, one `astype` per frame in the
  chain, no redundant `lpb_buf.copy()` into the second model, cached
  second-model output classify, and a batched int16 write in the
  no-scipy notch fallback loop. Smoke output was bit-identical
  at the time (`frames=200 out_rms=938.7 peak=9733`); the numbers
  below supersede it after the 128-sample pump change.

### Fixed

- **Web UI pump-frame starvation corrupting NS output.** The pump
  assembled 160-sample frames while every stage shifts 128 samples,
  so the AEC output ring starved on the first frames (32 zero
  samples injected per frame) and the NS output ring fell back to
  raw mic input for short reads. The zeros fed the NS LSTM, whose
  states then diverged for the whole stream (smoke: `rms=938.7
  peak=9733` with mixed-in raw signal vs `rms=902.3 peak=8385`
  fully denoised). The pump now uses 128-sample frames
  (`FRAME_SIZE == BLOCK_SHIFT`): production meets consumption
  exactly, the fallbacks never fire past priming, and output is
  the clean trajectory. Note this is a correctness fix, not a CPU
  win — invokes per second of audio are unchanged.

### Removed

- **Web UI notch + speech-gate stages.** The adaptive feedback notch
  (and the `SpeechGate` that existed only to drive it) are gone from
  the web chain: `web/notch.py` and `web/speech_gate.py` deleted,
  `notchEnabled` pref and `notch`/`gate` state keys removed, both UIs
  and docs updated. Rationale: the desktop runs those stages on the
  NKF path only; the web chain is DTLN-only, and NS is the switchable
  post stage. Side effect: scipy is no longer needed (it only
  vectorized the notch bypass) and drops out of the install. Note:
  sustained speaker howl is no longer suppressed — keep the mic away
  from the speakers.

## [1.10.1] — 2026-09-29 (web UI: 2026-10-04)

### Added

- **DTLN noise reduction post stage.** The DTLN-AEC pair cancels echo
  but leaves background noise; a second DTLN-NR pair (same 512-block /
  128-shift / 257-bin DSP, single mic feed) now runs after the engine
  on the DTLN and WebRTC AEC3 paths — `src/dtln_ns_wrapper.{h,cpp}`,
  `models/dtln_ns_128_{1,2}.tflite` (networkedaudio port of
  breizhn/DTLN denoise). WebRTC NS is retired; NKF keeps its own WPE
  dereverb + adaptive notch instead. A `Noise suppression (DTLN-NS)`
  checkbox and a `g_nsEnabled` pref gate it; fail-open pass-through on
  any inference error.

- **Web UI** (`web/`): the default DTLN-AEC 128 +
  DTLN-NS chain as a pure-Python local server with
  a GUI — a second distribution alongside
  the desktop exe, shipped as a separate zip
  (`AEC-Web-vX-win64.zip`, first attached to the
  v1.10.1 release page) with no PE binary and
  therefore none of the antivirus false positives.
  One-click `web/start.bat` (venv + packages +
  server + GUI; a second click reopens the GUI
  against the running server), FastAPI REST +
  WebSocket meters, live device rescan, and a
  device list that mirrors the desktop's filtering
  (`main.cpp` `BuildDisplayIndices`: pseudo-devices
  dropped, MME 31-char truncation deduped against
  the DirectSound/WASAPI twins, CABLE Input
  auto-picked, WASAPI > DirectSound > MME >
  WDM-KS preference).
- **Feedback suppression on the DTLN path (web)**:
  ports of the desktop speech gate
  (`web/speech_gate.py`, from `src/speech_gate.h`)
  and adaptive LMS notch (`web/notch.py`, from
  `src/notch.cpp`) run after the engine — a
  sustained howl is suppressed after ~3.5 s
  (measured −80 dB) while broadband speech never
  latches a section (engage-gated; adaptation is
  frozen while the gate says "talking").
- **Native app window for the web UI** (`web/gui.py`,
  pywebview on Edge WebView2 — preinstalled on
  Windows 10/11): `start.bat` opens the GUI in a
  window instead of a browser tab; no browser is
  needed. The server runs in-process behind the
  window and the models are preloaded before it
  opens, so Start is instant even on the very
  first process; closing the window stops the
  server. Clicking `start.bat` again attaches
  another window to the running server. If WebView2
   or pywebview is unavailable, the default browser
   opens instead — the GUI never depends on one
   engine.
- **Minimize to system tray (web UI)** — the new
  Appearance tab's "Minimize to system tray" tick
  (on by default) hides the native window to the
  tray instead of quitting: the server keeps
  running, and the tray icon (drawn at runtime, no
  image asset) brings the window back or quits.
  With the tray on, the console hides once the
  page loads (no taskbar button) — only the
  native window is visible — and stays hidden
  while the window is trayed; Show restores
  both.
  pywebview's cancellable closing event does the
  hiding, its loaded event minimizes the console;
  pystray + Pillow (mainstream wheels)
  provide the tray. The tab is hidden in a browser
  tab, where there is no window to tray. When
  start.bat runs inside Windows Terminal, the
  terminal window is left untouched (it holds the
  user's own tabs) and a note explains that
  double-clicking start.bat gives the fully
  hidden, tray-only experience.

### Changed

- **Post-stage panel is engine-aware.** WPE + notch only render for the
  NKF engine; the DTLN-NS checkbox only renders for DTLN/AEC3. Each
  toggle restarts the chain live.
- **Post-stage prefs are engine-scoped.** Switching engines (profile
  switch, load, reset-to-defaults) resets each stage to its engine's
  defaults, so a toggle left on for another engine cannot leak into a
  chain that doesn't run it and make Start look broken after a switch.
- **Experimental engines hidden behind an Appearance checkbox.**
  WebRTC AEC3 and NKF-AEC are no longer in the profile list by
  default — DTLN-AEC 128 is the only visible engine. Ticking the
  checkbox reveals them; unticking falls back to DTLN so the
  selection can never be left on a hidden engine.
- **The Advanced collapsible is renamed to "More".**

- **Web audio I/O is callback-mode** (pyaudiowpatch),
  like the desktop's miniaudio callbacks — blocking
  read/write threads could wedge on an unconsumed
  CABLE Input (Discord/Zoom closed) and take the
  server process down on Stop. The output callback
  now delivers exactly `frame_count` frames and
  returns silence in microseconds when starved.
- **WebSocket auto-reconnect** with exponential
  backoff; the page survives a server restart
  without a manual refresh.

### Performance

- **Lower CPU on modern CPUs.** The audio path was hot from per-call
  FIR filter rebuilds and one-by-one deque pulls: the 61-tap
  resampling filter is now computed once at import, `DtlnAec` /
  `DtlnNs` pull 128-sample shifts from preallocated numpy ring
  buffers instead of 128 `popleft()` calls per shift, and int16
  clipping uses float32. When `scipy` is installed, the notch's
  passive (non-adapting, unengaged) bypass path is computed with a
  vectorized IIR instead of a per-sample Python loop.

### Fixed

- **Requirements parsing with pip 26.x.** The `pywebview` /
  `pystray` / `pyaudiowpatch` comment blocks used whitespace-
  indented continuation lines that pip 26.x rejects; all
  requirements are now single-line.

- **Ort::Session takes a wchar_t path on Windows.** The ONNX backend
  passed a `const char*` to the Session constructor, which only accepts
  `ORTCHAR_T*` (wchar_t on Windows) — same fix the DTLN wrapper
  applies.
- **CMake parses a trailing `//` comment as a source filename.**
  `src/dtln_ns_wrapper.cpp   // DTLN noise reduction (DTLN/AEC3 only)`
  was treated as a source path; the comment is now a bare filename.
- **API names + Gui:: namespace in the DTLN-NS wiring.** `main.cpp`
  called the stage with the old `Dtns*` names; the public API is
  `DtlnNs*` (`DtnsLastError`). Also dropped a phantom `Gui::` namespace
  (the codebase uses ` ImGui::` directly).

- **Web app looked dead until something played on the
  speakers.** The pump required the WASAPI loopback to
  deliver a chunk before processing any mic chunk, but
  the loopback starves while the speakers are idle
  (WASAPI delivers no frames until something plays) —
  so with no audio playing, mic chunks were dropped
  waiting for a reference that never came: both meters
  dead, CABLE Input silent, until Discord opened and
  woke the loopback. A missing ref now reads as digital
  silence, the desktop's ref-ring underflow path
  (`main.cpp` `output_callback`: "missing ref reads as
  digital silence, which is exactly what 'nothing
  playing' means to the AEC").
- **Web feedback notch cut speech for the whole
  session.** A howl (Discord mic-test playback)
  latched a 60 Hz notch that never un-latched —
  the C++ design never releases (its notch only
  runs on the NKF path), but the web runs it on
  the DTLN path, so the cut sat in the voice
  band forever and muffled every later sentence
  (the "speech cutting" report). A latched
  section now releases after ~1 s of removing
  <15% of its block energy (or of silence),
  riding out the engine's ~1 s residual at the
  howl frequency; a returning howl re-latches
  in ~200 ms.
- **Feedback-suppression latency.** The gate's
  continuous-stuck threshold drops from 4.5 s
  (the desktop NKF value) to 3 s on the web
  path, so a sustained howl is suppressed after
  ~3.5 s instead of ~5 s.
- **Status line shows the gate state**
  (`speech` / `quiet` / `tone?` — sustained
  loudness, possibly a howl) next to the notch
  frequency, so suppression is visible live.
- **WebSocket dropped on every Stop** — a TOCTOU
  race between the stop endpoint (nulling
  runner/chain state from a worker thread) and
  the meter loop's check-then-read. Chain state
  and the runner are now snapshotted before use;
  verified with 4 concurrent readers over 400
  stop/start cycles, zero errors.
- **Device lists** no longer show MME/DirectSound
  pseudo-devices ("Microsoft Sound Mapper",
  "Primary Sound Driver"), duplicate host-API
  entries, or 31-char-truncated names; full names
  are recovered from the loopback generator when
  only MME enumerates.
- **Web Start took ~5 s on the first process.**
  The DTLN model pairs loaded lazily on the first
  Start; they are now preloaded before the port
  opens (the console prints "Loading models…"),
   so the GUI appears only once everything is
   resident and Start is instant. Stop→Start stays
   instant too — the models remain resident across
   stops.
- **Console flood in the native window.** The tray
   watcher polls `/api/prefs` once a second (so the
   Appearance tick applies live) and the meters
   stream over WebSocket; at uvicorn's `info` level
   every request logged, flooding the console. The
   in-process server now logs at `warning` — errors
   still appear — and start.bat passes
   `--disable-pip-version-check` to silence pip's
   upgrade notice.

## [1.10.0] — 2026-09-28

### Added

- **Three engine profiles replace engines + presets.** One combo
  at the top of the Audio tab selects the engine; the label shows the
  engine name directly, plus `→ WPE` / `→ Notch` while the post
  stages are ticked (no hidden state):
  - *DTLN-AEC 128* — dual-LSTM echo + noise canceller (16 kHz).
    Default: best echo + reverb handling.
  - *WebRTC AEC3* — the canceller Chrome and Google Meet use.
    Reliable, well-tested baseline, strongest canceller.
  - *NKF-AEC* — tiny neural Kalman core. Cheap canceller for weak
    CPUs.
  Sample rate, gains and devices are independent knobs (only the
  16-kHz-only engines force the rate down). Old configs migrate: the
  five v1.9 presets map onto the three profiles, old *Manual* lands
  on the profile for its engine.
- **Dereverb (WPE) as a global post stage.** The in-tree streaming
  single-channel Weighted Prediction Error dereverberator returns
  (removed from inside NKF in this round, now a standalone stage stacked after
  every engine) and is extended to 48 kHz (frame geometry scales
  512/1536 with the same 32 ms frames). Speech-gated predictor bound
  protects voice level; model-free (pocketfft only), ~32 ms delay.
- **Feedback suppression (adaptive notch).** Two cascaded
  second-order notches (~60 Hz) whose centers track narrowband
  howling/ringing via LMS on the quadrature component, slew-limited
  (8000 Hz/s), frozen while you talk (voice can never latch it) and
  exact-bypass until a tone is actually captured (>45% energy removed
  for ~200 ms, engage-gating) — idle voice passes bit-exact. Runs at
  16/48 kHz.
- Both post stages are always-visible ticks, **ON by default for
  every profile**, persist across restarts and are never reset by a
  profile switch (config gen 6).

- **NKF time-delay compensation searches 800 ms, coarse-to-fine
  over partial ranges** (was one full-rate sweep of 300 ms): a 4×
  box-decimated pass over as much history as exists, then ±8
  raw-sample refinement. The partial range means a typical round
  trip locks in the first half second instead of waiting for the
  whole buffer to fill — echo harness locks 40/120/250 ms at
  0.14/0.21/0.35 s with correlation 1.000 at the exact sample,
  mic-test at 0.49 s (previously the 3 s grace lock). The search
  runs eagerly every 256 ms until the first confident peak, then
  every 512 ms; delay jumps still need a 0.55 peak, and an
  on-screen dwell (grace) lock does not claim confidence. A
  no-peak grace now logs the best score it saw
  (`grace lock (no peak; last d=… sc=…)`) so out-of-range vs
  sub-threshold is diagnosable from `nkf-phase.log`.

- **Live NKF status telemetry on the Audio tab** via the new
  `NkfGetState()` (`src/nkf_wrapper.h`): `failed open (guard)`
  / `shadow (warm-up)` / `shadow (settling)` / `live`,
  with `delay ~XX ms (locked|estimated)` and a
  `(loop, adapting)` flag while a self-monitor loop is active,
  plus a hover tooltip.

- The Notch checkbox tooltip now shows live telemetry: the two
  section frequencies, engaged/bypassed state, and whether
  adaptation is frozen (voice), running (sustained tone), or idle.

### Changed

- **Pipeline is now engine → WPE → Notch → meters → limiter →
  gain.** Meters show what Discord hears (post-stage). Output status
  line reports the stacked chain ("16000 Hz, AEC3 + WPE + Notch").
  The speech flag driving both stages comes from a cheap RMS
  hysteresis detector on the engine output (attack ~20 ms, release
  ~300 ms) — no voice model needed.
- **Config generation 6.** Slot 7 = WPE tick, slot 17 = notch tick.
  Gen ≤ 5 files still load: presets migrate to profiles, old Manual
  setups land on the profile for their engine, and both post stages
  are forced ON (the slots held the retired preprocess/DFN and
  custom-profile flags).

### Fixed

- **Howl backstop deepened too slowly and muted the near-end voice.**
  The howl guard used to trim the wire in −6 dB steps (halving each
  repeat), so a marginal loop rang for tens of seconds before the
  trim actually broke it. It now quarters the wire gain per repeat
  (−12 dB), breaking a marginal loop in ~2 repeats (~6 s), and caps
  at 6 repeats so pathological cases cannot pin the mic at mute.
  Separately, the release clause required the wire to be *loud*
  (`outMs >= BS_OUT_MS`) — but the guard's whole job is to keep the
  wire quiet, so that condition could never be satisfied while the
  trim was deep, and the guard held −84 dB forever, muting the
  near-end voice after the howl died. The release now requires only
  real audio at the mic (`micMs >= BS_MIC_MS`) plus depth ≤ −1 dB
  (engine not amplifying). Verified end-to-end: howl builds, guard
  attacks to −36 dB, releases at t=14.34 (cancel proven), and the
  near-end voice passes at full level.

- **NKF crashed the whole app ~3 s after Start.** The engine linked
  against a prebuilt `libnkf_aec.dll` from before this round's header
  change; `m_frozen` was *inserted* at the top of `NKFImpl.h`'s
  private section instead of appended, shifting every member offset.
  The exe built with the new layout, the stale DLL read the old one →
  garbage members → hard SEGV on the first `ProcessBlock` (phase log:
  `model loaded … TDC grace lock … first ProcessBlock`, then nothing).
  The class's own append-only rule is restored (`m_frozen` is now the
  last member) **and** the engine is compiled directly into
  `aec_gui` — no separate DLL, so header and code can never drift
  again. The DLL build step and release entry are gone.

- **NKF barely cancelled echo at real-world levels.** A delay
  sweep of the stock model showed it *amplifies* when fed the
  wrapper's int16/32768 floats at voice level but cancels
  21–30 dB on the same signals scaled ~16× down — the ONNX core
  has a narrow operating level. `NkfProcess` now scales mic/ref
  ×1/16 into the engine and the result ×16 back out, so the
  guard, mix and emit all stay in the real domain. Echo-only
  harness at 40/120/250 ms delay: 25.8/27.9/29.1 dB cancelled,
  100 % live, zero guard resets.

- **NKF's mic-test howl: NKF itself stopped cancelling inside the
  loop.** Two policies kept the feedback loop alive: the detector
  pinned NKF on raw mic until a *confident* delay lock — and if the
  300 ms search missed the real round trip (app + Discord + speaker
  + loopback easily exceeds it) the grace lock never became
  confident, so the wire carried raw mic forever — and the fallback
  for un-confident locks *froze* adaptation, so the filter never
  learned the loop path either. A closed-loop test
  (`nkf_howl_test`: mic = β·out delayed, ref = loopback, seed
  noise) shows the frozen policy howling at coupling β ≥ 1.2 —
  full-scale ringing, sustained — while the level-scaled engine
  adapting straight through the loop stabilises it all the way to
  β = 1.8 (0 guard resets), and the cold-start howl dies within
  seconds of the delay lock. New policy: NKF is **never un-exposed
  for a loop and never frozen in one** — the Kalman adapts
  continuously so loop gain stays below 1 (the only real fix;
  WPE/notch alone cannot hold a near-field loop), the warm-up
  shadow drains unconditionally, and the search range extends to
  800 ms with partial-range scheduling so the delay locks as soon
  as its data exists. Mic-test harness: loop engaged at 0.39 s,
  delay locked at 0.49 s (was the 3 s grace), 100 % live, exact
  192-sample lag, zero guard resets, no give-up.

- **NKF mic-test howl: the notch checkbox could not stop it.**
  A sustained loud tone (howling/ringing loop) pinned the RMS
  speech detector on for ever, so `NotchSetSpeech(1)` never
  released: the notch froze adaptation *and* engagement
  (bit-exact bypass) while the checkbox stayed ticked — toggling
  WPE/notch changed nothing. The shared speech gate
  (`src/speech_gate.h`) now carries a stuck-speech watchdog: after
  ~5 s of continuous "speech" (no 250 ms quiet frame in between)
  notch adaptation is released; the next real gap re-arms plain
  hysteresis. WPE keeps the raw voice flag — voice protection is
  unchanged. Harness-proven end-to-end with the real NKF engine:
  gate releases at 5.0 s, the notch locks the 1100 Hz tone and
  removes 45 dB, bursty speech still freezes the notch and never
  trips the watchdog.

- **Intermittent rings are caught too.** The watchdog only
  counted *continuous* ON — any >0.8 s quiet gap re-armed it from
  scratch, so ring-under-voice that starts/stops (mic-test playback
  bursts) never released the notch. Two-tier trip replaces it:
  4.5 s unbroken loudness (fast path, unchanged feel) or 6.8 s of
  burst evidence that decays with a 700 ms gap time constant (the
  EMA's own ~0.8 s release lag counts as ON, so pauses drain hard;
  ordinary conversation peaks near 6.0 s and stays under). Harness:
  2.5 s ring bursts trip inside burst 3 and the notch removes
  38 dB; a 2 s/1.2 s conversation pattern never trips.
- **Low-frequency howls (80–150 Hz) can be notched.** The search
  floor was 150 Hz — lower tones were unreachable (both sections
  clamped above the tone and the latch never accrued). Floor is
  now 80 Hz: a 110 Hz ring is captured at −50 dB; voice-bypass
  suites remain bit-exact.

### Removed

- **Silero voice gate** — detector, calibration, the SPEAKING/SILENT
  pill, dual-feed resamplers and the "Push down silence" checkbox are
  all gone (models/silero_vad.onnx no longer ships).
- **Near-end protector** (dry-mic blend) — no detector exists to
  gate it.
- **Noise reduction checkbox** — the WebRTC NS stages in AEC3/NKF
  are gone (AEC3 keeps its high-pass filter).
- **DeepFilterNet (cut before release).** The v2.0 pre-release line
  stacked a full DeepFilterNet3 port after the engine; detail
  testing showed the LSNR gating modulated voice level audibly
  (voice pumping up/down mid-sentence), so the stage, its wrapper
  and its ~8 MB of model files were cut before the release shipped.
  WPE + the adaptive notch carry the noise/reverb/howl cleanup
  instead.
- **Engine combo and Quick presets** — the engine only changes
  through the profile menu.
- **GTCRN dead code** (gtcrn_wrapper) and the Silero voice gate.

### Notes

- NKF self-monitor loop policy: in the mic-test topology
  (ref carries our own recent output back) the loop detector
  **never yanks NKF to raw mic and never freezes it** — like
  DTLN/AEC3, NKF stays exposed and *keeps adapting* through the
  loop. Freezing looked safe when it was added (adaptation had
  diverged back then), but that divergence was the pre-scale
  level bug; with the scaled engine the closed-loop test shows
  adaptation converging and holding the loop down at coupling
  1.8 while the frozen variant howls from 1.2. The 8 s
  convergence hold, the confident-lock release condition and
  the freeze switch are all gone. The detector now feeds
  telemetry only; guard trips remain the backstop.

## [1.9.2] — 2026-09-26

### Changed

- **Runtime payload ~38 MB → ~6 MB.** `onnxruntime.dll` is now the
  official Microsoft x64 build (v1.29.0) instead of the MSYS2 one,
  UPX-compressed 27.5 MB → 4.9 MB, and the 97-file absl/onnx/
  protobuf/re2 dependency cluster it dragged in is gone (nothing but
  ORT imported them). The VC++ 14 runtime (msvcp140/vcruntime140,
  ~900 KB) ships app-local instead; link-time headers/import lib are
  still MSYS2's (same 1.29.0 version). Install zip shrinks by about a
  third (~26.5 MB → 17.7 MB) and the release drops from 117 to 23 files.

## [1.9.1] — 2026-09-25

### Changed

- **Voice gate: smoother, breathier (field-test follow-up).** The
  gate now opens within ~40 ms (speech onsets are never cut), closes
  only by gliding (slow release + asymmetric decision smoothing — a
  single low frame can't yank it down), and the knee keeps breaths
  and weak speech at about −2 to −5 dB instead of dragging them
  toward the shelf. The −12 dB silence shelf and 800 ms hangover are
  unchanged: only firm, sustained silence is pushed down.

## [1.9.0] — 2026-09-25

### Added

- **Near-end protector (all engines).** After echo cancellation and
  the voice gate, near-end speech is compared against the raw mic:
  when the engine's output sits more than 15 dB below the mic the
  difference is blended back in, so loud near-end speech is ducked at
  most 15 dB instead of being eaten. Speech-only (holds during
  silence), smooth attack/release, never clips.
- **Soft limiter.** Output peaks now run through a −3 dBFS knee
  (`tanh` above it) instead of a hard `int16` clamp, so over-driven
  passages (high mic gain, hot levels) gently saturate rather than
  crackle. Below the knee the signal is bit-transparent.
- **NKF: "Dereverb (WPE)" stage (default ON).** Replaces the GTCRN
  "Dry voice" model with a model-free streaming Weighted Prediction
  Error dereverberation post-filter: single-channel, per-bin
  recursive weighted least squares (5 taps, 2-frame delay,
  forgetting 0.995), pocketfft STFT (512/128), chain order
  **NS → WPE**. ~32 ms latency, hard-bounded with a speech-adaptive
  predictor cap — voice is cut at most ~2.5 dB while the near end
  talks (measured −2.7 dB on synthetic vowels), up to 6 dB between
  speech where only reverb/echo tails remain, never adds gain,
  fail-open pass-through. Toggleable live (same engine restart as
  Noise reduction).

### Changed

- **Voice gate: dual-feed detector (loud-speaker fix).** The gate now
  combines two Silero streams, each with its own detector state: the
  engine-cleaned output (always — the engine strips speaker playback,
  so this tracks the person), plus the raw mic when the loopback isn't
  the dominant thing in the mic. The previous raw-mic-only feed judged
  the speaker's playback on loud sessions (music/game audio isn't
  speech), which parked the gate in its knee and ducked normal speech
  until you yelled — and false-fired the near-end protector into
  blending raw echo back in. Either arm can rescue a decision;
  neither can veto. Gate processing is skipped entirely when the
  checkbox is off (no wasted inference, smoothed state stays live).
- **No hard mutes anywhere (fade-out fail-open).** Frame-size
  mismatches and mic underruns that previously emitted instant zeros
  now fade the last good frame out instead — gaps read as a natural
  duck, not a click. All engine wrappers (AEC3, DTLN, NKF) ship mic
  on any internal error (fail-open) rather than silence.
- **Config generation 4.** `aec_config.txt` retires the two legacy
  slots (dry/residual flags); gen ≤ 3 files still load (dry slot is
  skipped, residual ignored, WPE defaults ON), gen 4 stores the WPE
  flag.

### Removed

- **NKF: "Residual echo kill (AEC3)" pass.** The post-NKF WebRTC
  AEC3 stage is gone; NKF is now strictly linear + WebRTC NS +
  WPE. Tradeoff accepted: under extreme speaker volumes some echo
  leftovers may pass that the nonlinear suppressor used to eat.
  (The standalone AEC3 engine keeps its full pipeline unchanged.)
- **NKF: "Dry voice" (GTCRN).** Superseded by WPE — `gtcrn_stream.onnx`
  is no longer bundled or required; the release model allowlist
  shrinks accordingly.

## [1.8.1] — 2026-09-24

### Added

- **"Low CPU (NKF)" quick preset.** One click selects NKF-AEC at
  16 kHz with **Residual echo kill OFF**, Dry voice OFF, and Noise
  reduction OFF — bare neural Kalman core for busy PCs. The old
  always-on residual AEC3 pass is not cheaper than standalone AEC3;
  leave it off for CPU, tick it back on Audio if echo returns.
  Appended as preset index 4 (no config migration; pgen stays 3).

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

### Docs

- **Corrected overstated NKF CPU claims.** README/UI no longer call
  NKF the "lightest" engine without qualification: the paper's
  "low complexity" is the tiny neural core vs other neural AECs, not
  a measured win over WebRTC AEC3. Default NKF still stacks residual
  AEC3 (toggleable since 1.8.0). Engine table, picker labels, and
  packaged README tips now say residual-on ≈ a second AEC3 pass;
  Low CPU preset = bare NKF. "Under 2%" applies to all engines.

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
