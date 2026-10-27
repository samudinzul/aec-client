"""Processing chain — mirrors main.cpp's frame pump + ReinitEngine.

  engine (int16 -> int16, selectable: Off (no AEC) / DTLN-AEC 128 /
  WebRTC AEC3 / NKF-AEC — same profiles as the desktop, NKF-AEC
  hidden unless experimental, WebRTC AEC3 a regular choice now).
  Engine Off passes mic straight to the NS stage (denoise-only).
  NS stage (float -> float, fail-open — runs after the engine on
  every path, like the desktop). Backend is selectable: "dtln"
  (DTLN-NS pair, default, ships in models/), "hush" (Hush via
  the user-supplied weya_nc library + bundle — see web/hush.py;
  "off" skips the stage). "gtcrn" is the experimental desktop
  backend ported to Python (web/gtcrn.py) for tools/ns_eval.py
  and tools/aec_eval.py; it is not exposed in the web UI.
  mic preamp gain + float -> int16 (round-half-even + clamp,
  like ``clamp_s16(lrintf)``)

Notes on fidelity vs the desktop app:
- 16 kHz only (the neural models are 16 kHz by design; the desktop
  forces NKF/DTLN down to 16 kHz too — aec-web runs 16 kHz always,
  so NS always applies when ready).
- No notch / speech gate: the desktop retired those stages.
- Ref silence when speakers are idle: the loopback stream delivers
  silence naturally, same as the desktop ref-ring-underflow path.
- Engine swap is live (no stream reconfigure needed — unlike
  devices): the pump keeps running, the new engine starts reset.
"""

import importlib.util
import math
import threading
import time

import numpy as np

from . import dsp
from .dtln import DtlnAec
from .dtln_ns import DtlnNs
from .gtcrn import gtcrn_available
from .hush import HushNs, hush_available


# Desktop PROFILES table (src/main.cpp), minus the filter column
# the web pump has no use for. Off is last so saved profile slots
# keep loading without migration; the UIs list it first.
PROFILES = (
    ("DTLN-AEC 128", "dtln"),
    ("WebRTC AEC3", "aec3"),
    ("NKF-AEC", "nkf"),
    ("Off (no AEC)", "off"),
)

# Engine names hidden unless the experimental flag is ticked
# (desktop parity: WebRTC AEC3 graduated to a regular choice).
EXPERIMENTAL_ENGINES = ("nkf",)


# NS-stage backends: "off" skips the stage, "dtln" is the shipped
# DTLN-NS pair, "hush" is the user-supplied Hush library (fail-closed
# at selection while its files are absent — see web/hush.py), and
# "gtcrn" is the experimental desktop backend ported to Python
# (web/gtcrn.py; used by the aec_eval/ns_eval harnesses, not
# exposed in the web UI).
NS_BACKENDS = ("off", "dtln", "hush", "gtcrn")


# Backend probe cache: state() calls engine_available() for every
# profile on every 10 Hz meter tick (plus REST polls), and each miss
# re-scans sys.path. Backends only appear via (re)install, so a 5 s
# TTL is indistinguishable from live probing at a fraction of the
# stat() traffic.
_PROBE_TTL = 5.0
_probe_cache = {}  # name -> (deadline_monotonic, result)


def engine_available(name):
    """Backend presence probe — finds the module spec WITHOUT
    importing it, so probing never pulls the native libraries
    (onnxruntime, WebRTC) into the process. They load only when
    the engine is actually selected (set_engine), i.e. DTLN-only
    sessions never pay for backends they don't use. A stale/broken
    install (spec present, import fails) still fails closed with
    a clear error at set_engine. Results are cached for _PROBE_TTL
    (see above); a backend installed mid-run shows up a few seconds
    later instead of instantly."""
    try:
        now = time.monotonic()
    except Exception:
        now = 0.0
    hit = _probe_cache.get(name)
    if hit is not None and now < hit[0]:
        return hit[1]
    result = _probe(name)
    try:
        _probe_cache[name] = (time.monotonic() + _PROBE_TTL, result)
    except Exception:
        pass
    return result


def _probe(name):
    if name == "dtln":
        return True  # LiteRT import failure surfaces at load, not here
    if name == "off":
        return True  # no backend — always available
    try:
        if name == "aec3":
            return importlib.util.find_spec("pywebrtc_audio") is not None
        if name == "nkf":
            return importlib.util.find_spec("onnxruntime") is not None
    except Exception:
        # A hostile sys.path entry can make the probe itself raise;
        # a probe that can't answer means "unavailable", never a crash
        # (the 10 Hz meter loop calls this constantly).
        return False
    return False


# Voice-path high-pass: 2nd-order Butterworth (RBJ cookbook),
# 80 Hz, Q = 1/sqrt(2) — mirrors the desktop HpfInit/HpfProcess
# (same fc/Q, same Direct Form I), pinned by test_parity
# --constants (HPF_FC/HPF_Q). Cuts what denoisers leave in the
# basement (hum/rumble/DC) without touching voice. Always on,
# like the limiter — hygiene, not a knob.
HPF_FC = 80.0
HPF_Q = 0.7071

# Gentle downward expander (mirrors ExpanderProcess in src/main.cpp):
# fixed thresholds with hysteresis (NOT an adaptive floor — a follower
# that chases sustained energy mutes held vowels; measured). Opens
# above OPEN_RMS, closes below CLOSE_RMS after HANG_MS hangover,
# 2 ms attack / 5 ms release. Pinned by test_parity --constants.
EXPAND_OPEN_RMS = 150.0
EXPAND_CLOSE_RMS = 100.0
EXPAND_HANG_MS = 200

# Gentle presence shelf (mirrors PresInit/PresProcess in src/main.cpp):
# in-band RBJ high shelf, +4 dB at 5 kHz, S = 1. Lifts consonant
# clarity inside 0-8 kHz without touching lows or hallucinating highs.
# Pinned by test_parity --constants (PRES_FC/PRES_GAIN_DB).
PRES_FC = 5000.0
PRES_GAIN_DB = 4.0


class Chain:
    def __init__(self, model_dir="models", num_threads=2):
        self.model_dir = model_dir
        # Inference thread cap, passed to every LiteRT interpreter.
        # 2, not None: measured invoke latency is identical-or-better
        # than the default (0.044 vs 0.065 ms on DTLN stage 1 — the
        # tiny models don't scale past 2 threads) while the XNNPACK
        # pool holds 2 spinning threads instead of core-count.
        self.num_threads = num_threads
        self.dtln = None
        self.ns = None
        self.ns_backend = "dtln"  # one of NS_BACKENDS; single source of
        # truth for the stage (ns_enabled below is a compat view).
        self.hush = None  # lazy HushNs (user-supplied files; None until
        # first "hush" selection — never loaded unless picked).
        self.hush_error = ""
        self.gtcrn = None  # lazy GtcrnNs (experimental harness backend;
        # None until first "gtcrn" selection — never loaded otherwise).
        self.gtcrn_error = ""
        self.mic_gain = 1.0
        self.running = False
        self.last_error = ""
        self.frames = 0
        self.engine = "dtln"
        self._backend = None  # Aec3Engine / NkfEngine (dtln uses self.dtln)
        self._load_lock = threading.Lock()
        self._swap_lock = threading.Lock()
        # High-pass coefficients (RBJ, 16 kHz — the web chain rate is
        # fixed, so these never change) + Direct Form I state, reset
        # every Start so sessions never inherit filter memory.
        w0 = 2.0 * math.pi * HPF_FC / 16000.0
        c = math.cos(w0)
        alpha = math.sin(w0) / (2.0 * HPF_Q)
        a0 = 1.0 + alpha
        self._hpf_b = ((1.0 + c) / 2.0 / a0, -(1.0 + c) / a0,
                       (1.0 + c) / 2.0 / a0)
        self._hpf_a = (-2.0 * c / a0, (1.0 - alpha) / a0)
        self._hpf_s = [0.0, 0.0, 0.0, 0.0]  # x1, x2, y1, y2
        self._exp_s = {"gain": 1.0, "hang": 0, "open": True}
        # Presence shelf coefficients (RBJ high shelf, 16 kHz fixed —
        # same math as PresInit; Direct Form I state below).
        _a = 10.0 ** (PRES_GAIN_DB / 40.0)
        _w0 = 2.0 * math.pi * PRES_FC / 16000.0
        _cc = math.cos(_w0)
        _ss = math.sin(_w0)
        _al = _ss / 2.0 * 1.4142135623730951
        _be = 2.0 * math.sqrt(_a) * _al
        _a0 = (_a + 1.0) - (_a - 1.0) * _cc + _be
        self._pres_b = (_a * ((_a + 1.0) + (_a - 1.0) * _cc + _be) / _a0,
                        -2.0 * _a * ((_a - 1.0) + (_a + 1.0) * _cc) / _a0,
                        _a * ((_a + 1.0) + (_a - 1.0) * _cc - _be) / _a0)
        self._pres_a = (2.0 * ((_a - 1.0) - (_a + 1.0) * _cc) / _a0,
                        ((_a + 1.0) - (_a - 1.0) * _cc - _be) / _a0)
        self._pres_s = [0.0, 0.0, 0.0, 0.0]

    def _hpf_reset(self):
        self._hpf_s = [0.0, 0.0, 0.0, 0.0]
        self._exp_s = {"gain": 1.0, "hang": 0, "open": True}
        self._pres_s = [0.0, 0.0, 0.0, 0.0]

    def _hpf_run(self, frame_f32):
        # In-place Direct Form I over one frame (any length); mirrors
        # HpfProcess in src/main.cpp apart from float-vs-double
        # internals (inaudible).
        b0, b1, b2 = self._hpf_b
        a1, a2 = self._hpf_a
        x1, x2, y1, y2 = self._hpf_s
        out = np.empty_like(frame_f32, dtype=np.float32)
        for i, x in enumerate(frame_f32):
            x = float(x)
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out[i] = y
        self._hpf_s = [x1, x2, y1, y2]
        return out

    def _exp_run(self, frame_f32, sr=16000):
        # Sample-for-sample mirror of ExpanderProcess (apart from
        # float-vs-double internals): fixed-threshold hysteresis, so
        # sustained voice can never be chased into mute. n = frame
        # length (any).
        st = self._exp_s
        x = np.asarray(frame_f32, dtype=np.float32)
        n = len(x)
        ms = float(np.mean(x.astype(np.float64) ** 2)) if n else 0.0
        open_e = (EXPAND_OPEN_RMS / 32768.0) ** 2
        close_e = (EXPAND_CLOSE_RMS / 32768.0) ** 2
        hang_frames = int(EXPAND_HANG_MS / 1000.0 * sr / (n if n else 1))
        if ms > open_e:
            st["open"] = True
            st["hang"] = hang_frames if hang_frames > 0 else 1
        elif ms < close_e:
            if st["hang"] > 0:
                st["hang"] -= 1
            else:
                st["open"] = False
        # Between thresholds: hold (hysteresis — no chatter).
        want = 1.0 if st["open"] else 0.0
        up_step = 1.0 / (0.002 * sr)
        dn_step = 1.0 / (0.005 * sr)
        g = st["gain"]
        out = np.empty_like(x)
        for i, v in enumerate(x):
            if g < want:
                g += up_step
                if g > want:
                    g = want
            elif g > want:
                g -= dn_step
                if g < want:
                    g = want
            out[i] = v * g
        st["gain"] = g
        return out

    def _pres_run(self, frame_f32):
        # Direct Form I mirror of PresProcess (float-vs-double only).
        b0, b1, b2 = self._pres_b
        a1, a2 = self._pres_a
        x1, x2, y1, y2 = self._pres_s
        out = np.empty_like(np.asarray(frame_f32, dtype=np.float32))
        for i, xv in enumerate(np.asarray(frame_f32, dtype=np.float32)):
            x = float(xv)
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out[i] = y
        self._pres_s = [x1, x2, y1, y2]
        return out

    @property
    def ns_enabled(self):
        """Compat view: old bool toggle (nsBackend != off)."""
        return self.ns_backend != "off"

    @ns_enabled.setter
    def ns_enabled(self, value):
        # Map the legacy toggle without clobbering a hush pick:
        # on from off lands on dtln, on from hush stays hush.
        if value:
            if self.ns_backend == "off":
                self.ns_backend = "dtln"
        else:
            self.ns_backend = "off"

    def set_ns_backend(self, name):
        """Select the NS stage backend (live — no reconfigure needed).

        Unknown names, a hush pick without its files, and a hush
        pick whose library fails to load all fail closed: returns
        False and keeps the previous backend. A successful hush
        pick loads the model immediately (under _load_lock, same
        order as start()) so the switch takes effect mid-call —
        deferring the load to the next Start left the pump
        mislabelled and unprocessed in between.
        """
        if name not in NS_BACKENDS:
            self.last_error = f"unknown NS backend: {name}"
            return False
        if name == "hush" and not hush_available(model_dir=self.model_dir):
            self.last_error = (
                "hush files missing (weya_nc library + model bundle) — "
                "see web/README.md; keeping " + self.ns_backend)
            return False
        with self._load_lock:
            if name == "hush":
                self._ensure_hush()
                if self.hush is None or not self.hush.ready:
                    self.last_error = (self.hush_error
                                       or "hush load failed")
                    return False
            elif name == "gtcrn":
                self._ensure_gtcrn()
                if self.gtcrn is None or not self.gtcrn.ready:
                    self.last_error = (self.gtcrn_error
                                       or "gtcrn load failed")
                    return False
            with self._swap_lock:
                self.ns_backend = name
                if name == "hush" and self.hush is not None:
                    self.hush.reset()
                elif name == "gtcrn" and self.gtcrn is not None:
                    self.gtcrn.reset()
        return True

    def _load_engines(self):
        """Build the model pairs — the slow part
        (~5 s cold: 4 tflite files + interpreter
        setup). Caller holds _load_lock. NS
        missing/broken is fail-open, not fatal:
        the stage stays off and the engine output
        passes through."""
        self.dtln = DtlnAec(f"{self.model_dir}/dtln_aec_128",
                              num_threads=self.num_threads)
        self.ns = DtlnNs(f"{self.model_dir}/dtln_ns_128",
                         num_threads=self.num_threads)

    def preload(self):
        """Load the models at server startup (background
        thread) so the first Start is instant."""
        with self._load_lock:
            if self.running or self.dtln is not None:
                return
            self._load_engines()

    def _ensure_hush(self):
        """Create the Hush backend on first pick (caller holds _load_lock).

        A broken library leaves self.hush_error set and self.hush
        unready — processing then fails open (passthrough), the same
        contract as a missing DTLN-NS pair.
        """
        if self.hush is not None:
            return
        try:
            self.hush = HushNs(model_dir=self.model_dir)
            self.hush_error = "" if self.hush.ready else self.hush.last_error
        except Exception as e:
            self.hush = None
            self.hush_error = f"hush init failed: {e}"

    def _ensure_gtcrn(self):
        """Create the GTCRN backend on first pick (caller holds _load_lock).

        Harness/experimental backend only — the model ships in
        models/, so this normally succeeds; a missing/broken model
        fails closed at selection rather than silently denoising
        nothing.
        """
        if self.gtcrn is not None:
            return
        try:
            from .gtcrn import GtcrnNs
            self.gtcrn = GtcrnNs(model_dir=self.model_dir,
                                 num_threads=self.num_threads)
            self.gtcrn_error = ("" if self.gtcrn.ready
                                else self.gtcrn.last_error)
        except Exception as e:
            self.gtcrn = None
            self.gtcrn_error = f"gtcrn init failed: {e}"

    def set_engine(self, name):
        """Select the echo engine (hot-swappable while running —
        the I/O streams are engine-agnostic, so unlike devices no
        reconfigure is needed; the new engine starts reset).
        "off" means no echo cancellation (mic passes straight to
        the NS stage) and always succeeds. Unknown names and broken
        backends fail closed: returns False and keeps the previous
        engine."""
        if name not in ("dtln", "aec3", "nkf", "off"):
            self.last_error = f"unknown engine: {name}"
            return False
        backend = None
        if name == "off":
            pass  # no backend — process_frame ships mic
        elif name == "aec3":
            from . import aec3
            backend = aec3.Aec3Engine()
            if not backend.ready:
                self.last_error = (backend.last_error
                                   or "AEC3 backend unavailable")
                return False
        elif name == "nkf":
            from . import nkf
            backend = nkf.NkfEngine(model_dir=self.model_dir,
                                    num_threads=self.num_threads)
            if not backend.ready:
                self.last_error = (backend.last_error
                                   or "NKF backend unavailable")
                return False
        with self._swap_lock:
            self.engine = name
            self._backend = backend
        return True

    def start(self):
        with self._load_lock:
            with self._swap_lock:
                engine, backend = self.engine, self._backend
            if engine == "off":
                # Engine Off: no echo canceller to require — but the NS
                # stage still loads below (denoise-only mode). A missing
                # DTLN pair must not fail Start here (nothing needs it).
                if self.dtln is None:
                    self._load_engines()
            elif engine == "dtln" or backend is None:
                # DTLN engine path (or no backend selected):
                # the pair must be ready.
                if self.dtln is None:
                    self._load_engines()
                if not self.dtln.ready:
                    self.last_error = (self.dtln.last_error
                                       or "Failed to load DTLN model")
                    self.dtln = None
                    self.ns = None
                    self.running = False
                    return False
            else:
                # AEC3/NKF path: the backend was validated at
                # set_engine; reset it so every Start is cold.
                # (DTLN-NS still loads below — it post-processes
                # every engine.)
                backend.reset()
                if self.dtln is None:
                    self._load_engines()
            self.running = True
            self.frames = 0
            self._hpf_reset()  # fresh filter memory every Start
            # NS broken is fail-open (pre-existing contract):
            # start succeeds, the error rides along for the UI.
            # The error reported is the ACTIVE backend's: a hush pick
            # with unloadable files still starts (passthrough) instead
            # of stranding the voice path.
            with self._swap_lock:
                ns_backend = self.ns_backend
            if ns_backend == "hush":
                self._ensure_hush()
                if self.hush is not None:
                    self.hush.reset()
                self.last_error = (self.hush_error if self.hush is None
                                   else ("" if self.hush.ready
                                         else self.hush.last_error))
            elif ns_backend == "gtcrn":
                self._ensure_gtcrn()
                if self.gtcrn is not None:
                    self.gtcrn.reset()
                self.last_error = (self.gtcrn_error
                                   if self.gtcrn is None
                                   else ("" if self.gtcrn.ready
                                         else self.gtcrn.last_error))
            else:
                self.last_error = "" if self.ns.ready else self.ns.last_error
            return True

    def stop(self):
        # Models stay resident (preloaded at server
        # startup); a new session just resets the
        # DSP state, so Stop -> Start is instant too.
        with self._load_lock:
            if self.dtln is not None:
                self.dtln.reset()
            if self.ns is not None:
                self.ns.reset()
            if self.hush is not None:
                self.hush.reset()
            if self.gtcrn is not None:
                self.gtcrn.reset()
            with self._swap_lock:
                if self._backend is not None:
                    self._backend.reset()
            self.running = False
            self.frames = 0

    def process_frame(self, mic_i16, ref_i16):
        """One int16 frame in -> int16 frame out. Never mutes."""
        mic = np.asarray(mic_i16, dtype=np.int16)
        ref = np.asarray(ref_i16, dtype=np.int16)
        if not self.running or self.dtln is None:
            return mic.copy()
        if self.mic_gain != 1.0:
            # Mic preamp: amplify the mic (and everything it
            # picked up, echo included) before the engine.
            # The != 1.0 guard keeps the default path
            # bit-identical (no extra conversion).
            mic = np.clip(mic.astype(np.float32) * self.mic_gain,
                          -32768.0, 32767.0).astype(np.int16)
        with self._swap_lock:
            engine, backend = self.engine, self._backend
            ns_backend = self.ns_backend
        try:
            if engine == "off":
                cleaned = mic.copy()  # no AEC — straight to the NS stage
            elif engine == "dtln" or backend is None:
                cleaned = self.dtln.process(mic, ref)
            else:
                cleaned = backend.process(mic, ref)
            if ns_backend != "off":
                f = cleaned.astype(np.float32) / 32768.0
                staged = False
                try:
                    if ns_backend == "hush" and self.hush is not None:
                        f = self.hush.process(f)
                        staged = True
                    elif (ns_backend == "gtcrn" and self.gtcrn is not None
                            and self.gtcrn.ready):
                        f = self.gtcrn.process(f)
                        staged = True
                    elif (ns_backend == "dtln" and self.ns is not None
                            and self.ns.ready):
                        f = self.ns.process(f)
                        staged = True
                except Exception:
                    pass  # fail-open: keep engine output
                if staged:
                    cleaned = dsp.f32_to_i16_round(f)
            # Voice-path high-pass (80 Hz) on every path including
            # engine-Off and NS-off, mirroring the desktop (post-NS,
            # pre-meter/limiter): hum/rumble/DC the stages leave behind.
            fh = self._hpf_run(cleaned.astype(np.float32) / 32768.0)
            # Expander just after the high-pass (desktop parity):
            # digital-black floor without onset chopping.
            fh = self._exp_run(fh)
            # Presence shelf just after the expander (desktop parity):
            # in-band clarity lift, meters read the shipped signal.
            fh = self._pres_run(fh)
            cleaned = dsp.f32_to_i16_round(fh)
            self.frames += 1
            return cleaned
        except Exception as e:  # fail-open: ship mic, never silence
            self.last_error = f"chain error (fail-open): {e}"
            return mic.copy()

    def state(self):
        # Snapshot first: the stop endpoint tears these
        # down from another thread, and a check-then-use
        # race here killed the WebSocket on every Stop.
        ns_obj = self.ns
        hush_obj = self.hush
        gtcrn_obj = self.gtcrn
        dtln_obj = self.dtln
        with self._swap_lock:
            engine, backend = self.engine, self._backend
            ns_backend = self.ns_backend
        if ns_backend == "hush":
            ns = (hush_obj.stats() if hush_obj is not None else {
                "backend": 0, "rCount": 0, "inRms": 0.0,
                "outRms": 0.0, "dropped": 0,
                "error": self.hush_error,
            })
        elif ns_backend == "gtcrn":
            ns = (gtcrn_obj.stats() if gtcrn_obj is not None else {
                "backend": 0, "rCount": 0, "inRms": 0.0,
                "outRms": 0.0, "dropped": 0,
                "error": self.gtcrn_error,
            })
        elif ns_backend == "off":
            # Stage skipped: report it off (backend 0), not the
            # DTLN object's readiness — consumers read backend as
            # "stage running" (status suffix, classic page).
            ns = {
                "backend": 0, "rCount": 0, "inRms": 0.0,
                "outRms": 0.0, "dropped": 0, "error": "",
            }
        else:
            ns = ns_obj.stats() if ns_obj is not None else {
                "backend": 0, "rCount": 0, "inRms": 0.0,
                "outRms": 0.0, "dropped": 0, "error": "",
            }
        ns_active = bool(ns["backend"]) and ns_backend != "off"
        name = {n: d for d, n in PROFILES}.get(engine, "DTLN-AEC 128")
        nkf_st = None
        if engine == "nkf" and backend is not None:
            try:
                nkf_st = backend.get_state()
            except Exception:
                nkf_st = None
        ns_tag = " + Hush" if (ns_backend == "hush" and ns_active) else (
            " + GTCRN" if (ns_backend == "gtcrn" and ns_active) else (
                " + DTLN-NS" if ns_active else ""))
        return {
            "running": self.running,
            "engine": name if self.running else "",
            "chain": (name + ns_tag) if self.running else "",
            "nsEnabled": 1 if ns_backend != "off" else 0,
            "nsBackend": ns_backend,
            "nsBackends": {
                # DTLN pair ships in models/ (None = not loaded yet,
                # assume present until the load says otherwise).
                "dtln": (dtln_obj.backend != 0)
                        if dtln_obj is not None else True,
                # The model ships in models/ (release) or the data dir;
                # only the DLL stays user-supplied.
                "hush": hush_available(model_dir=self.model_dir),
                # Experimental harness backend; model ships in models/.
                "gtcrn": (gtcrn_obj.ready if gtcrn_obj is not None
                          else gtcrn_available(model_dir=self.model_dir)),
            },
            "micGain": self.mic_gain,
            "sampleRate": 16000,
            "dtlnBackend": dtln_obj.backend if dtln_obj else 0,
            "ns": ns,
            "engines": {n: engine_available(n) for _, n in PROFILES},
            "nkf": nkf_st,
            "frames": self.frames,
            "error": self.last_error,
            "dtlnError": dtln_obj.last_error if dtln_obj else "",
        }
