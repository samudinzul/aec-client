"""Processing chain — mirrors main.cpp's frame pump + ReinitEngine.

  engine (int16 -> int16, selected profile: DTLN-AEC 128 /
  WebRTC AEC3 / NKF-AEC — same PROFILES table as the desktop,
  same experimental gating in the server prefs)
  NS stage (float -> float, fail-open — runs after the engine on
  every path, like the desktop). Backend is selectable: "dtln"
  (DTLN-NS pair, default, ships in models/) or "hush" (Hush via
  the user-supplied weya_nc library + bundle — see web/hush.py;
  "off" skips the stage).
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
import threading
import time

import numpy as np

from . import dsp
from .dtln import DtlnAec
from .dtln_ns import DtlnNs
from .hush import HushNs, hush_available


# Desktop PROFILES table (src/main.cpp), minus the filter column
# the web pump has no use for.
PROFILES = (
    ("DTLN-AEC 128", "dtln"),
    ("WebRTC AEC3", "aec3"),
    ("NKF-AEC", "nkf"),
)


# NS-stage backends: "off" skips the stage, "dtln" is the shipped
# DTLN-NS pair, "hush" is the user-supplied Hush library (fail-closed
# at selection while its files are absent — see web/hush.py).
NS_BACKENDS = ("off", "dtln", "hush")


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
        self.mic_gain = 1.0
        self.running = False
        self.last_error = ""
        self.frames = 0
        self.engine = "dtln"
        self._backend = None  # Aec3Engine / NkfEngine (dtln uses self.dtln)
        self._load_lock = threading.Lock()
        self._swap_lock = threading.Lock()

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
        if name == "hush" and not hush_available():
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
            with self._swap_lock:
                self.ns_backend = name
                if name == "hush" and self.hush is not None:
                    self.hush.reset()
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
            self.hush = HushNs()
            self.hush_error = "" if self.hush.ready else self.hush.last_error
        except Exception as e:
            self.hush = None
            self.hush_error = f"hush init failed: {e}"

    def set_engine(self, name):
        """Select the echo engine (hot-swappable while running —
        the I/O streams are engine-agnostic, so unlike devices no
        reconfigure is needed; the new engine starts reset).
        Unknown names and broken backends fail closed: returns
        False and keeps the previous engine."""
        if name not in ("dtln", "aec3", "nkf"):
            self.last_error = f"unknown engine: {name}"
            return False
        backend = None
        if name == "aec3":
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
            if engine == "dtln" or backend is None:
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
            if engine == "dtln" or backend is None:
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
                    elif (ns_backend == "dtln" and self.ns is not None
                            and self.ns.ready):
                        f = self.ns.process(f)
                        staged = True
                except Exception:
                    pass  # fail-open: keep engine output
                if staged:
                    cleaned = dsp.f32_to_i16_round(f)
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
            " + DTLN-NS" if ns_active else "")
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
                "hush": hush_available(),
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
