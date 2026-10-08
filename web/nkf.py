"""NKF-AEC engine — NumPy port of src/nkf_wrapper.cpp +
third_party/REAL_TIME_NKF_AEC/c/NKFImpl.{h,cpp}.

Same algorithm, same constants, same staging — only the language
changed ( PocketFFT -> numpy.fft, ONNX Runtime C++ -> onnxruntime
pip package, same models/nkf.onnx file):

- Core (_NkfCore, mirrors NKFImpl): 1024-sample blocks / 512
  shift @16 kHz, periodic Hann (sin^2(pi*i/1024) — NOT symmetric),
  double-precision rFFT, 4-frame loopback history, silence skip
  (history mean < 1e-5 freezes states and emits mic), Kalman
  predict/update around the ONNX invoke, echo subtraction, an
  intrinsic Wiener residual suppressor (per-bin gain from the
  core's own echohat — echo-majority bins trim toward -20 dB,
  the rest pass at unity; part of the engine like AEC3's own
  suppressor, independent of the NS toggle), inverse FFT +
  weighted overlap-add.
- Level scaling (mirrors ENG_IN_SCALE/OUT_SCALE): the stock model
  is level-sensitive — int16/32768 audio (~0.1-0.2 rms) scales
  DOWN by 16 into the engine and back UP after. Guard, mix and
  emit all stay in the real domain.
- Wrapper (NkfEngine, mirrors NkfHandle/NkfProcessImpl): TDC
  ref->mic alignment by normalized cross-correlation (x4
  box-decimated coarse sweep + full-rate refine, near-tie keeps
  the smaller lag, slew-limited, eager/periodic cadence, 3 s
  grace lock), shadow (16 blocks mic on wire) + 256 ms
  crossfade exposure, divergence guard (3 hot blocks -> filter
  reset + re-shadow, 6 resets -> fail-open mic), self-monitor
  loop detector (drives downstream brakes only — the engine
  itself keeps adapting: -6 dB wire trim, single-window
  backstop attack, eager TDC; all self-release), no-cancel
  watchdog (loud on both legs with wire-as-loud-as-mic past
  grace + ~5 s trims like a loop — covers clipped-ADC,
  beyond-range, jitter-chaotic AND false-confident locks by
  keying on cancellation evidence, not lock flags; skipped
  failed-open), howl backstop (tonal-hold trim to a -24 dB
  floor, single-window release on proven cancellation).

Deliberate omissions (desktop-only plumbing, not DSP):
- NkfPhase transition log (desktop crash diagnostics; the
  server has its own logs). All counters it fed are kept.
- SetFrozen (no callers in src/ — dead flag).
- NKF_FORCE_GIVEUP is honored (test hook for fail-open).

Fail-open like the C++ wrapper: a missing model/package or an
inference exception ships mic, never silence.
"""

import math
import os

import numpy as np

try:
    import onnxruntime as ort
except ImportError:  # optional backend — Chain reports it unavailable
    ort = None

# ---- Block parameters (must match NKFImpl.h) -------------------------
SAMPLE_RATE = 16000
BLOCK_LEN = 1024
BLOCK_SHIFT = 512
FFT_OUT_SIZE = 513
NKF_LEN = 4

# ---- Level scaling (stock-model sweet spot) --------------------------
ENG_IN_SCALE = 1.0 / 16.0
ENG_OUT_SCALE = 16.0

# ---- TDC --------------------------------------------------------------
TDC_DMAX = 12800
TDC_WIN = 1024
TDC_DEC = 4
TDC_EAGER = 1024
TDC_PERIOD = 8192
TDC_LOCK_GRACE = 48000
TDC_MIN_PEAK = 0.35
TDC_JUMP_PEAK = 0.55
TDC_JUMP = 80
TDC_SLEW = 512
TDC_TIE_EPS = 0.02
TDC_FINE = 8
TDC_MIN_MEAN_E = 1e-5
TDC_SHADOW = 16
TDC_FADE = 8

# ---- Self-monitor loop detector (telemetry only) ----------------------
LOOP_WIN = 1024
LOOP_DMAX = 12800
LOOP_DEC = 4
LOOP_PERIOD = 2048
LOOP_MIN_PEAK = 0.45
LOOP_ON = 2
LOOP_QUIET = 16

# ---- Aggressive-on-loop policy ------------------------------------------
# A confirmed loop leans on everything downstream of the Kalman core —
# never on the core itself (no freeze, no un-expose: field-verified
# howl fuel). Three independent, self-releasing brakes:
#   1. wire trim (-6 dB loop-gain cut while engaged),
#   2. single-window backstop attack on tonal-stable wire,
#   3. eager TDC cadence (loop delay may be shifting).
LOOP_TRIM = 0.5       # wire multiplier while looped
LOOP_TAU_ATK = 0.35   # trim engages within a few 512-blocks
LOOP_TAU_REL = 0.15   # ...and lets go over ~0.5 s once clear
# Escalation: a braked loop that stays LOUD is not under control
# (marginal stability: coupling x trim ~= 1 sustains full-scale
# mush forever). Every 3 continuous loud seconds deepen the trim
# one step, floored. Any quiet-ish block resets the run, so bursty
# voice in a mic test never escalates — only gapless sustain does
# (howls don't pause; speech does). Bounded, fast-releasing, and
# the backstop/heal logic is untouched above it.
LOOP_TRIM_DEEP = 0.25   # -12 dB after ~3 s sustained loud
LOOP_TRIM_FLOOR = 0.125  # -18 dB after ~6 s; never deeper
LOOP_LOUD_BLOCKS = 96   # 96 x 512-sample blocks ~= 3 s per step

# ---- No-cancel watchdog ---------------------------------------------------
# Clipped ADC, beyond-range or jitter-chaotic delay, or a confident
# but WRONG lock (clipped correlation peaks anywhere): the Kalman
# runs misaligned and the loop detector may be blind too. The robust
# signal isn't lock confidence — it's CANCELLATION EVIDENCE: loud on
# both legs with wire-as-loud-as-mic (depth above the proven-
# cancellation bar) for this long means nothing is being cancelled,
# whatever the cause — trim it like a loop. Releases the moment
# cancellation resumes or either leg goes quiet. Skipped failed-open
# (passthrough is the documented give-up contract, not a failure).
NOCANCEL_HOT_AFTER = 80000  # ~5 s of loud, uncancelled audio
NOCANCEL_ECHO_RATIO = 0.25  # ref within 6 dB of mic = worth fixing

# ---- Howl backstop -----------------------------------------------------
BS_BIN_RUN = 40
BS_FRAMES = 64
BS_RUN = 2
BS_HEAL_RUN = 1
BS_MIN_TARGET = 0.0625
BS_TONAL = 0.33
BS_HITS = 38
BS_MIC_MS = 8.4e-5
BS_OUT_MS = 9.3e-6
BS_HEAL_D = -1.0
BS_TAU_ATK = 0.35
BS_TAU_REL = 0.08

# ---- Divergence guard --------------------------------------------------
GUARD_HOT_BLOCKS = 3
GUARD_EMA = 0.125
GUARD_HOT_MEAN_E = 0.05
GUARD_RATIO = 4.0
GUARD_CLIP_MEAN_E = 0.5
GUARD_MAX_RESETS = 6

# ---- Intrinsic residual echo suppressor (Wiener post-filter) -----------
# The Kalman core is a *linear* canceller: nonlinear distortion, loud
# residue and late tails pass through it. Like AEC3's built-in
# suppressor (part of the engine, not a chain stage), a per-bin Wiener
# gain from the core's own echohat cleans up, with the NS toggle's
# meaning untouched (it still governs DTLN-NS only).
# Double-talk safety is structural, not tuned: bins where echo is the
# minority of mic energy pass at unity; only echo-majority bins are
# trimmed, floored so nothing fully gates (no musical gating).
RES_EMA = 0.15      # power smoothing per 512-block (~150 ms tail)
RES_THRESH = 0.5    # suppress only echo-majority bins
RES_FLOOR = 0.1     # deepest per-bin trim (-20 dB)
RES_RELEASE = 0.25  # echo-power decay per silent block (fast release
                    # so a voice onset after far-end silence is untouched)


def available() -> bool:
    return ort is not None


def _periodic_hann(n):
    s = np.sin(np.pi * np.arange(n, dtype=np.float64) / n)
    return (s * s).astype(np.float32)


class _NkfCore:
    """Kalman core (NKFImpl): 512-sample blocks in/out, float audio
    in the engine domain (caller scales). NOT thread-safe."""

    def __init__(self, model_path, enable_res=True):
        self.enable_res = enable_res
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = (
            ort.GraphOptimizationLevel.ORT_ENABLE_ALL)
        self._sess = ort.InferenceSession(
            model_path, sess_options=opts,
            providers=["CPUExecutionProvider"])
        self._in_names = [i.name for i in self._sess.get_inputs()]
        self._out_names = [o.name for o in self._sess.get_outputs()]
        self._hann = _periodic_hann(BLOCK_LEN)
        self.reset()

    def reset(self):
        self.mic_buf = np.zeros(BLOCK_LEN, np.float32)
        self.lpb_buf = np.zeros(BLOCK_LEN, np.float32)
        self.out_buf = np.zeros(BLOCK_LEN, np.float32)
        # lpb_hist[frame, bin] (matches lpb_real[j*513+i]); h[bin, tap]
        # (matches h[NKF_LEN*i+j]) so per-bin math needs no transpose.
        self.lpb_hist = np.zeros((NKF_LEN, FFT_OUT_SIZE), np.float32)
        self.h_prior = np.zeros((FFT_OUT_SIZE, NKF_LEN), np.complex128)
        self.h_post = np.zeros((FFT_OUT_SIZE, NKF_LEN), np.complex128)
        self.states = [np.zeros((1, FFT_OUT_SIZE, 18), np.float32)
                       for _ in range(4)]
        self.res_pe = np.zeros(FFT_OUT_SIZE, np.float64)
        self.res_pm = np.zeros(FFT_OUT_SIZE, np.float64)
        self.res_db = 0.0

    def _res_apply(self, enh, mic_spec, echohat, far_active):
        """Intrinsic residual echo suppressor (Wiener post-filter).

        Per-bin gain from the core's own echo estimate: bins where
        echo is the minority of mic energy pass at unity (double-talk
        safety by construction); echo-majority bins trim toward
        RES_FLOOR. Output-only — Kalman states never see it.
        """
        if not self.enable_res:
            return enh
        if far_active:
            pe_inst = np.abs(echohat.astype(np.complex128)) ** 2
            pm_inst = np.abs(mic_spec.astype(np.complex128)) ** 2
            self.res_pe += RES_EMA * (pe_inst - self.res_pe)
            self.res_pm += RES_EMA * (pm_inst - self.res_pm)
            dom = np.clip(self.res_pe / (self.res_pm + 1e-12), 0.0, 1.0)
            gain = 1.0 - (1.0 - RES_FLOOR) * np.clip(
                (dom - RES_THRESH) / (1.0 - RES_THRESH), 0.0, 1.0)
        else:
            # Far-end silence: release fast so a voice onset is
            # untouched, and pass through (echohat is zero anyway).
            self.res_pe *= RES_RELEASE
            gain = np.ones(FFT_OUT_SIZE, np.float64)
        inst_db = float(20.0 * np.log10(gain.mean() + 1e-9))
        self.res_db += 0.25 * (inst_db - self.res_db)
        return enh * gain

    def process_block(self, mic_new, lpb_new):
        """One 512-sample engine-domain block in -> 512 float out."""
        self.mic_buf = np.concatenate(
            [self.mic_buf[BLOCK_SHIFT:], np.asarray(
                mic_new, dtype=np.float32)[:BLOCK_SHIFT]])
        self.lpb_buf = np.concatenate(
            [self.lpb_buf[BLOCK_SHIFT:], np.asarray(
                lpb_new, dtype=np.float32)[:BLOCK_SHIFT]])

        mic_spec = np.fft.rfft(self.mic_buf.astype(np.float64)
                                * self._hann)
        lpb_spec = np.fft.rfft(self.lpb_buf.astype(np.float64)
                                * self._hann)
        self.lpb_hist = np.concatenate(
            [self.lpb_hist[1:], lpb_spec.astype(np.complex64)[None, :]])
        far_active = bool(
            np.abs(self.lpb_hist.astype(np.complex128)).mean() >= 1e-5)

        dh_re = np.zeros((FFT_OUT_SIZE, NKF_LEN), np.float32)
        dh_im = np.zeros((FFT_OUT_SIZE, NKF_LEN), np.float32)
        if far_active:
            dh_c = self.h_post - self.h_prior
            dh_re = dh_c.real.astype(np.float32)
            dh_im = dh_c.imag.astype(np.float32)
            self.h_prior = self.h_post.copy()

        # Feature tensors [513,1,9] = [4xLpb | e | 4xdh] (float32).
        # dh is real-valued (post-minus-prior cast to float32), so
        # its imaginary channels are identically zero.
        lpb_re = self.lpb_hist.real.astype(np.float32)
        lpb_im = self.lpb_hist.imag.astype(np.float32)
        err = (mic_spec - np.sum(
            self.lpb_hist.astype(np.complex128) * self.h_prior.T,
            axis=0)).astype(np.complex64)
        feat = np.zeros((FFT_OUT_SIZE, 1, 2 * NKF_LEN + 1), np.float32)
        feat[:, 0, 0:NKF_LEN] = lpb_re.T
        feat[:, 0, NKF_LEN] = err.real
        feat[:, 0, NKF_LEN + 1:] = dh_re
        feat_i = np.zeros((FFT_OUT_SIZE, 1, 2 * NKF_LEN + 1), np.float32)
        feat_i[:, 0, 0:NKF_LEN] = lpb_im.T
        feat_i[:, 0, NKF_LEN] = err.imag
        feat_i[:, 0, NKF_LEN + 1:] = dh_im

        if far_active:
            feeds = {"in_real": feat, "in_imag": feat_i,
                     "in_hrr": self.states[0], "in_hir": self.states[1],
                     "in_hri": self.states[2], "in_hii": self.states[3]}
            outs = self._sess.run(self._out_names, feeds)
            kg = (outs[0][:, :, 0].astype(np.float64)
                  + 1j * outs[1][:, :, 0].astype(np.float64))
            self.states = [np.asarray(o, dtype=np.float32)
                           for o in outs[2:6]]
            # hPost = hPrior + e*kg (complex, per bin and tap —
            # expanded real/imag exactly like the C++ update).
            e64 = err.astype(np.complex128)
            er, ei = e64[:, None].real, e64[:, None].imag
            self.h_post = self.h_prior + (
                (er * kg.real - ei * kg.imag)
                + 1j * (ei * kg.real + er * kg.imag))

        if far_active:
            echohat = np.sum(
                self.lpb_hist.astype(np.complex128) * self.h_post.T,
                axis=0)
            enh = mic_spec - echohat
        else:
            echohat = np.zeros(FFT_OUT_SIZE, np.complex128)
            enh = mic_spec
        enh = self._res_apply(enh, mic_spec, echohat, far_active)
        est = np.fft.irfft(enh).astype(np.float32)
        self.out_buf = np.concatenate(
            [self.out_buf[BLOCK_SHIFT:], np.zeros(BLOCK_SHIFT,
                                                  np.float32)])
        self.out_buf += est * self._hann
        return self.out_buf[:BLOCK_SHIFT].copy()


class NkfEngine:
    """Full staged engine (NkfHandle): int16 in/out at 16 kHz, any
    pump frame size (128 on the web pump; 512-block internals)."""

    def __init__(self, model_dir="models", num_threads=None,
                 enable_res=True):
        self.ready = False
        self.last_error = ""
        self._core = None
        self._enable_res = enable_res
        if ort is None:
            self.last_error = ("onnxruntime is not installed "
                               "(pip install onnxruntime)")
            self._reset_state()
            return
        try:
            self._core = _NkfCore(f"{model_dir}/nkf.onnx",
                                  enable_res=enable_res)
            self.ready = True
        except Exception as e:
            self.last_error = f"NKF model load failed: {e}"
            self._core = None
        self.bs_enabled = os.getenv("NKF_BACKSTOP", "1") != "0"
        self._reset_state()
        if os.getenv("NKF_FORCE_GIVEUP"):
            self.give_up = True

    # -- state (mirrors NkfHandle fields + NkfReset) ---------------------
    def _reset_state(self):
        self.mic_accum = []
        self.ref_hist = []
        self.out_accum = []  # int16 wire samples awaiting drain
        self.out_hist = []   # float wire history (loop detector tap)
        self.mic_head = 0
        self.out_head = 0
        self.total = 0
        self.mic_consumed = 0
        self.mic_win = []
        self.align_delay = 0
        self.since_tdc = 0
        self.tdc_locked = False
        self.tdc_confident = False
        self.mic_env = self.ref_env = self.out_env = 0.0
        self.hot_blocks = 0
        self.resets = 0
        self.give_up = False
        self.shadow_blocks = TDC_SHADOW
        self.fade_pos = TDC_FADE
        self.since_loop = 0
        self.loop_conf = 0
        self.loop_quiet = 0
        self.nocancel_hot_samples = 0
        self.nocancel_hot = False
        self.loop_loud_run = 0
        self.last_window_cancelled = False
        self.last_tdc_sc = -2.0
        self.last_tdc_d = 0
        self.dep_mic = self.dep_out = 0.0
        self.dep_samples = 0
        self.bs_gain = 1.0
        self.bs_target = 1.0
        self.loop_gain = 1.0
        self.bs_hits = self.bs_frames = 0
        self.bs_run = self.bs_heal = self.bs_attacks = 0
        self.bs_active = False
        # NOTE: bsBin* intentionally survive reset (desktop NkfReset
        # leaves them — tonal-center memory is session-lifetime).
        self.bs_bin_last = -1000000
        self.bs_bin_run = 0
        self.bs_bin_best = 0
        self.bs_bin_dom = -1
        n = BLOCK_SHIFT
        self.bs_hann = 0.5 - 0.5 * np.cos(
            2.0 * np.pi * np.arange(n, dtype=np.float64) / n)

    def reset(self):
        self._reset_state()
        if self._core is not None:
            try:
                self._core.reset()
            except Exception:
                pass

    def get_state(self):
        exposed = bool(self.tdc_locked and not self.give_up
                       and self.shadow_blocks == 0
                       and self.fade_pos >= TDC_FADE)
        return {
            "lagSamples": int(self.align_delay),
            "confident": 1 if self.tdc_confident else 0,
            "locked": 1 if self.tdc_locked else 0,
            "exposed": 1 if exposed else 0,
            "loopActive": 1 if self.loop_conf >= LOOP_ON else 0,
            "nocancelHot": 1 if self.nocancel_hot else 0,
            "guardResets": int(self.resets),
            "giveUp": 1 if self.give_up else 0,
            "backstopDb": float(20.0 * np.log10(self.bs_gain + 1e-9)),
            "resDb": float(self._core.res_db)
            if self._core is not None else 0.0,
            "loopDb": float(20.0 * np.log10(self.loop_gain + 1e-9)),
        }

    # -- TDC -------------------------------------------------------------
    @staticmethod
    def _box4(v):
        return v.reshape(-1, 4).mean(axis=1)

    def _estimate_delay(self):
        self.since_tdc = 0
        if len(self.mic_win) < TDC_WIN:
            return
        if len(self.ref_hist) < TDC_WIN + BLOCK_SHIFT:
            return
        d_max = len(self.ref_hist) - TDC_WIN
        if d_max > TDC_DMAX:
            d_max = TDC_DMAX
        d_max &= ~(TDC_DEC - 1)
        if d_max < BLOCK_SHIFT:
            return
        m = np.asarray(self.mic_win[-TDC_WIN:], dtype=np.float64)
        front = self.total - len(self.ref_hist)
        ref_lo = self.total - TDC_WIN - d_max
        if ref_lo < front:
            return
        r = np.asarray(self.ref_hist[ref_lo - front:
                                      ref_lo - front + TDC_WIN + d_max],
                       dtype=np.float64)
        # Coarse pass (x4 box-decimated).
        wd = TDC_WIN // TDC_DEC
        db_count = d_max // TDC_DEC
        md = self._box4(m)
        rd = self._box4(r)
        mic_ed = float(np.dot(md, md))
        if mic_ed / wd < TDC_MIN_MEAN_E:
            return
        dpref = np.concatenate([[0.0], np.cumsum(rd * rd)])
        if dpref[-1] / len(rd) < TDC_MIN_MEAN_E:
            return
        stride = np.lib.stride_tricks.as_strided(
            rd, shape=(db_count + 1, wd),
            strides=(rd.strides[0], rd.strides[0]))[::-1]
        # Row db of `stride` = Rd[DB-db : DB-db+WD] (db=0 is lag 0).
        num = stride @ md
        off = np.arange(db_count + 1)
        ref_ed = dpref[db_count - off + wd] - dpref[db_count - off]
        with np.errstate(divide="ignore", invalid="ignore"):
            sc = np.where(ref_ed > 0, num / np.sqrt(mic_ed * ref_ed),
                          -np.inf)
        c_best, c_best_db = -2.0, 0
        for db in range(db_count + 1):
            if sc[db] > c_best + TDC_TIE_EPS:
                c_best, c_best_db = float(sc[db]), db
        # Fine pass (full rate around the coarse peak).
        mic_e = float(np.dot(m, m))
        if mic_e / TDC_WIN < TDC_MIN_MEAN_E:
            return
        pref = np.concatenate([[0.0], np.cumsum(r * r)])
        if pref[-1] / len(r) < TDC_MIN_MEAN_E:
            return
        d_lo = max(0, c_best_db * TDC_DEC - TDC_FINE)
        d_hi = min(d_max, c_best_db * TDC_DEC + TDC_FINE)
        # Explicit per-lag dots (<=17 estimates): the refine range
        # rarely touches dMax, so a reversed strided view would
        # misalign rows to lags. Clarity over cleverness.
        best, best_d = -2.0, self.align_delay
        for lag in range(d_lo, d_hi + 1):
            seg = r[d_max - lag:d_max - lag + TDC_WIN]
            num = float(np.dot(m, seg))
            re_ = pref[d_max - lag + TDC_WIN] - pref[d_max - lag]
            if re_ <= 0.0:
                continue
            s = num / math.sqrt(mic_e * re_)
            if s > best:
                best, best_d = s, lag
        self.last_tdc_d = best_d
        self.last_tdc_sc = float(best)
        jump = abs(best_d - self.align_delay) > TDC_JUMP
        if best >= (TDC_JUMP_PEAK if jump else TDC_MIN_PEAK):
            target = best_d
            step = target - self.align_delay
            if step > TDC_SLEW:
                target = self.align_delay + TDC_SLEW
            elif step < -TDC_SLEW:
                target = self.align_delay - TDC_SLEW
            self.align_delay = target
            self.tdc_locked = True
            self.tdc_confident = True

    # -- Divergence guard (per 512-block, real domain) --------------------
    def _guard_trip(self, mic_b, ref_b, out_b):
        # NOTE: mic/ref EMAs are maintained by the block loop (they
        # must track live audio even when the engine is skipped, e.g.
        # failed-open); only the out EMA lives here with the trip.
        m = float(np.mean(np.asarray(mic_b) ** 2))
        r = float(np.mean(np.asarray(ref_b) ** 2))
        o = float(np.mean(np.asarray(out_b) ** 2))
        self.out_env += GUARD_EMA * (o - self.out_env)
        hot = (self.out_env > GUARD_HOT_MEAN_E
               and self.out_env > GUARD_RATIO * self.mic_env
               and self.out_env > GUARD_RATIO * self.ref_env)
        if o > GUARD_CLIP_MEAN_E and o > GUARD_RATIO * max(m, r):
            hot = True
        if hot:
            self.hot_blocks += 1
        elif self.hot_blocks > 0:
            self.hot_blocks -= 1
        return self.hot_blocks >= GUARD_HOT_BLOCKS

    # -- Self-monitor loop detector (telemetry only) ----------------------
    def _detect_loop(self):
        self.since_loop = 0
        w = LOOP_WIN
        if len(self.ref_hist) < w or len(self.out_hist) < w + BLOCK_SHIFT:
            return
        d_max = len(self.out_hist) - w
        if d_max > LOOP_DMAX:
            d_max = LOOP_DMAX
        d_max &= ~(LOOP_DEC - 1)
        if d_max < BLOCK_SHIFT:
            return
        wd = w // LOOP_DEC
        rf = np.asarray(self.ref_hist[-w:], dtype=np.float64)
        of = np.asarray(self.out_hist[-(w + d_max):], dtype=np.float64)
        rd = self._box4(rf)
        od = self._box4(of)
        e_r = float(np.dot(rd, rd))
        e_o = float(np.dot(od, od))
        if e_r / wd < TDC_MIN_MEAN_E or e_o / len(od) < TDC_MIN_MEAN_E:
            self.loop_quiet += 1
            if self.loop_quiet >= LOOP_QUIET:
                self.loop_conf = 0
                self.loop_quiet = 0
            return
        self.loop_quiet = 0
        pref = np.concatenate([[0.0], np.cumsum(od * od)])
        off = d_max // LOOP_DEC
        wins = np.lib.stride_tricks.as_strided(
            od, shape=(off + 1, wd),
            strides=(od.strides[0], od.strides[0]))[::-1]
        num = wins @ rd
        seg = (pref[off - np.arange(off + 1) + wd]
               - pref[off - np.arange(off + 1)])
        with np.errstate(divide="ignore", invalid="ignore"):
            sc = np.where(seg > 0, num / np.sqrt(e_r * seg), -2.0)
        if float(np.max(sc)) >= LOOP_MIN_PEAK:
            if self.loop_conf < 4:
                self.loop_conf += 1
        elif self.loop_conf > 0:
            self.loop_conf -= 1

    # -- Howl backstop ------------------------------------------------------
    def _frame_tonal(self, v):
        n = BLOCK_SHIFT
        vv = np.asarray(v, dtype=np.float64)
        spec = np.fft.rfft((vv - vv.mean()) * self.bs_hann)
        p = np.zeros(n // 2 + 2, dtype=np.float64)
        p[1:n // 2 + 1] = np.abs(spec[1:]) ** 2
        tot = p[1:n // 2 + 1].sum()
        if not tot > 1e-30:
            return False, -1
        best3, best_k = 0.0, -1
        for k in range(1, n // 2 + 1):
            s = p[k - 1] + p[k] + p[k + 1]
            if s > best3:
                best3, best_k = s, k
        return (bool(best3 / tot >= BS_TONAL), int(best_k))

    def _backstop_window(self):
        frames = self.bs_frames
        mic_ms = self.dep_mic / max(1, self.dep_samples)
        depth = 10.0 * np.log10(
            (self.dep_out + 1e-12) / (self.dep_mic + 1e-12))
        # Cancellation evidence for the no-cancel watchdog (pure
        # measurement — independent of the tonal logic below).
        self.last_window_cancelled = depth <= BS_HEAL_D
        floors = mic_ms >= BS_MIC_MS and depth <= BS_HEAL_D
        tonal = frames > 0 and self.bs_hits >= BS_HITS
        stable = self.bs_bin_best >= BS_BIN_RUN
        if tonal and stable:
            self.bs_heal = 0
            if self.bs_run < 1000:
                self.bs_run += 1
            # Looped howls attack after ONE window: a tonal-stable
            # wire while the loop detector is engaged is the howl
            # signature itself — waiting costs howl seconds. Heal
            # still demands proof (unchanged below).
            need_run = 1 if self.loop_conf >= LOOP_ON else BS_RUN
            if self.bs_run >= need_run and self.bs_enabled:
                want = 0.25 ** (self.bs_attacks + 1)
                if want < BS_MIN_TARGET:
                    want = BS_MIN_TARGET
                if want < self.bs_target:
                    self.bs_target = want
                    self.bs_active = True
                    if self.bs_attacks < 6:
                        self.bs_attacks += 1
        elif (not tonal or not stable) and floors:
            self.bs_run = 0
            if self.bs_heal < 1000:
                self.bs_heal += 1
            if self.bs_heal >= BS_HEAL_RUN and self.bs_active:
                self.bs_target = 1.0
                self.bs_active = False
                self.bs_attacks = 0
                self.bs_run = self.bs_heal = 0
        else:
            self.bs_run = 0
            self.bs_heal = 0
        self.dep_mic = self.dep_out = 0.0
        self.dep_samples = 0
        self.bs_hits = self.bs_frames = 0
        self.bs_bin_best = 0
        self.bs_bin_dom = -1

    # -- Frame pump ----------------------------------------------------------
    def process(self, mic_i16, ref_i16):
        """int16 in -> int16 out (any frame size; 128 on the web pump).
        Fail-open: any failure ships mic."""
        try:
            return self._process(mic_i16, ref_i16)
        except Exception:
            self.give_up = True
            mic = np.asarray(mic_i16, dtype=np.int16)
            return mic.copy()

    def _process(self, mic_i16, ref_i16):
        mic = np.asarray(mic_i16, dtype=np.int16)
        ref = np.asarray(ref_i16, dtype=np.int16)
        n = min(len(mic), len(ref))
        if self._core is None or not self.ready:
            return mic.copy()
        if n == 0:
            return mic.copy()
        mf = (mic[:n].astype(np.float32) / 32768.0).tolist()
        rf = (ref[:n].astype(np.float32) / 32768.0).tolist()
        self.mic_accum.extend(mf)
        self.mic_win.extend(mf)
        self.ref_hist.extend(rf)
        self.total += n
        if len(self.mic_win) > TDC_WIN:
            del self.mic_win[:-TDC_WIN]

        if not self.give_up:
            self.since_tdc += n
            # Eager while looped: loop delay may be shifting, and
            # alignment is the cheapest stabilization available.
            looped = self.loop_conf >= LOOP_ON
            need = (TDC_EAGER if (looped or not (self.tdc_locked
                                                 and self.tdc_confident))
                    else TDC_PERIOD)
            if self.since_tdc >= need:
                self._estimate_delay()
            if not self.tdc_locked and self.total >= TDC_LOCK_GRACE:
                self.tdc_locked = True
        self.since_loop += n
        if self.since_loop >= LOOP_PERIOD:
            self._detect_loop()

        shift = BLOCK_SHIFT
        while len(self.mic_accum) - self.mic_head >= shift:
            base = self.mic_head
            mic_b = np.asarray(self.mic_accum[base:base + shift],
                               dtype=np.float32)
            need_start = self.mic_consumed - self.align_delay
            front = self.total - len(self.ref_hist)
            ref_b = np.zeros(shift, np.float32)
            for i in range(shift):
                p = need_start + i
                if front <= p < front + len(self.ref_hist):
                    ref_b[i] = self.ref_hist[p - front]
            # Level followers run on EVERY block (engine or not), so
            # the no-lock watchdog sees live audio even failed-open.
            self.mic_env += GUARD_EMA * (
                float(np.mean(mic_b.astype(np.float64) ** 2))
                - self.mic_env)
            self.ref_env += GUARD_EMA * (
                float(np.mean(ref_b.astype(np.float64) ** 2))
                - self.ref_env)
            # No-cancel watchdog: count blocks that are loud on
            # both legs, echo-significant (ref within 6 dB of mic —
            # a loud talker over quiet music has nothing to fix and
            # must never trip this), with no proven cancellation
            # lately (see _backstop_window). Anything else resets it.
            echo_big = (self.ref_env >= BS_MIC_MS
                        and self.ref_env >= NOCANCEL_ECHO_RATIO * self.mic_env)
            if (self.mic_env < BS_MIC_MS or not echo_big
                    or self.last_window_cancelled):
                self.nocancel_hot_samples = 0
            else:
                self.nocancel_hot_samples += shift
            processed = False
            if self.tdc_locked and not self.give_up:
                try:
                    out_b = self._core.process_block(
                        mic_b * ENG_IN_SCALE, ref_b * ENG_IN_SCALE)
                    out_b = (out_b * ENG_OUT_SCALE).astype(np.float32)
                except Exception:
                    self.give_up = True
                    self._core.reset()
                    self.shadow_blocks = TDC_SHADOW
                    self.fade_pos = TDC_FADE
                    continue
                processed = True
                if self._guard_trip(mic_b, ref_b, out_b):
                    self._core.reset()
                    self.mic_env = self.ref_env = self.out_env = 0.0
                    self.hot_blocks = 0
                    self.shadow_blocks = TDC_SHADOW
                    self.fade_pos = TDC_FADE
                    self.resets += 1
                    if self.resets >= GUARD_MAX_RESETS:
                        self.give_up = True
                    processed = False
            else:
                out_b = mic_b

            shadowed = processed and self.shadow_blocks > 0
            g = 0.0
            if processed and not shadowed:
                if self.fade_pos < TDC_FADE:
                    self.fade_pos += 1
                    g = self.fade_pos / TDC_FADE
                else:
                    g = 1.0
            emit = mic_b + (out_b - mic_b) * np.float32(g)

            self.bs_gain += (self.bs_target - self.bs_gain) * (
                BS_TAU_ATK if self.bs_target < self.bs_gain
                else BS_TAU_REL)
            if self.bs_gain != 1.0:
                emit = emit * np.float32(self.bs_gain)

            # Escalation run: pre-gain wire energy (stable measure —
            # post-trim energy would sawtooth against its own trim).
            # Any quiet-ish block restarts it: bursty voice never
            # climbs, gapless sustain does.
            looped = self.loop_conf >= LOOP_ON
            nocancel = (self.total > TDC_LOCK_GRACE
                        and not self.give_up
                        and self.nocancel_hot_samples >= NOCANCEL_HOT_AFTER)
            self.nocancel_hot = nocancel
            ms_pre = float(np.mean(emit.astype(np.float64) ** 2))
            if (looped or nocancel) and ms_pre >= BS_MIC_MS:
                self.loop_loud_run += 1
            else:
                self.loop_loud_run = 0
            if looped or nocancel:
                step = self.loop_loud_run // LOOP_LOUD_BLOCKS
                loop_target = LOOP_TRIM * (0.5 ** min(step, 2))
                if loop_target < LOOP_TRIM_FLOOR:
                    loop_target = LOOP_TRIM_FLOOR
            else:
                loop_target = 1.0
            # Loop-active trim: multiplicative with the backstop, own
            # smoothing. Cuts loop gain below 1 while engaged; the
            # engine keeps adapting underneath (never frozen).
            self.loop_gain += (loop_target - self.loop_gain) * (
                LOOP_TAU_ATK if loop_target < self.loop_gain
                else LOOP_TAU_REL)
            if self.loop_gain != 1.0:
                emit = emit * np.float32(self.loop_gain)

            self.dep_mic += float(np.sum(mic_b.astype(np.float64) ** 2))
            self.dep_out += float(np.sum(emit.astype(np.float64) ** 2))
            self.dep_samples += shift
            tonal, bb = self._frame_tonal(emit)
            if tonal:
                self.bs_hits += 1
                if self.bs_bin_last - 1 <= bb <= self.bs_bin_last + 1:
                    self.bs_bin_run += 1
                else:
                    self.bs_bin_last = bb
                    self.bs_bin_run = 1
                if self.bs_bin_run > self.bs_bin_best:
                    self.bs_bin_best = self.bs_bin_run
                    self.bs_bin_dom = self.bs_bin_last
            else:
                self.bs_bin_last = -1000000
                self.bs_bin_run = 0
            if self.bs_frames + 1 >= BS_FRAMES:
                self.bs_frames += 1
                self._backstop_window()
            else:
                self.bs_frames += 1

            self.out_hist.extend(emit.tolist())
            wire = np.clip(emit * 32768.0, -32768.0, 32767.0)
            self.out_accum.extend(
                np.trunc(wire).astype(np.int16).tolist())

            if shadowed:
                self.shadow_blocks -= 1
                if self.shadow_blocks == 0:
                    self.fade_pos = 0
            self.mic_head += shift
            self.mic_consumed += shift
        if self.mic_head:
            del self.mic_accum[:self.mic_head]
            self.mic_head = 0

        cap = TDC_WIN + TDC_DMAX
        if len(self.ref_hist) > cap:
            del self.ref_hist[:-cap]
        cap_o = LOOP_WIN + LOOP_DMAX
        if len(self.out_hist) > cap_o:
            del self.out_hist[:-cap_o]

        avail = len(self.out_accum) - self.out_head
        out = np.zeros(n, np.int16)
        take = min(n, max(0, avail))
        if take:
            out[:take] = self.out_accum[self.out_head:self.out_head
                                        + take]
        consume = take
        self.out_head += consume
        if self.out_head:
            del self.out_accum[:self.out_head]
            self.out_head = 0
        return out
