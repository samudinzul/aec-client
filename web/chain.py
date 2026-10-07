"""Processing chain — mirrors main.cpp's frame pump + ReinitEngine.

  engine (int16 -> int16, selected profile: DTLN-AEC 128 /
  WebRTC AEC3 / NKF-AEC — same PROFILES table as the desktop,
  same experimental gating in the server prefs)
  DTLN-NS (float -> float, fail-open, on/off toggle — runs after
  the engine on every path, like the desktop)
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

import threading

import numpy as np

from . import dsp
from .dtln import DtlnAec
from .dtln_ns import DtlnNs


# Desktop PROFILES table (src/main.cpp), minus the filter column
# the web pump has no use for.
PROFILES = (
    ("DTLN-AEC 128", "dtln"),
    ("WebRTC AEC3", "aec3"),
    ("NKF-AEC", "nkf"),
)


def engine_available(name):
    """Backend import probe — no models loaded, no side effects."""
    if name == "dtln":
        return True  # LiteRT import failure surfaces at load, not here
    if name == "aec3":
        from . import aec3
        return aec3.available()
    if name == "nkf":
        from . import nkf
        return nkf.available()
    return False


class Chain:
    def __init__(self, model_dir="models", num_threads=None):
        self.model_dir = model_dir
        # Inference thread cap, passed to every LiteRT interpreter.
        # None keeps the LiteRT default; a small number trims the
        # XNNPACK pool (and its spin-wait) on weak CPUs.
        self.num_threads = num_threads
        self.dtln = None
        self.ns = None
        self.ns_enabled = True
        self.mic_gain = 1.0
        self.running = False
        self.last_error = ""
        self.frames = 0
        self.engine = "dtln"
        self._backend = None  # Aec3Engine / NkfEngine (dtln uses self.dtln)
        self._load_lock = threading.Lock()
        self._swap_lock = threading.Lock()

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
        try:
            if engine == "dtln" or backend is None:
                cleaned = self.dtln.process(mic, ref)
            else:
                cleaned = backend.process(mic, ref)
            if self.ns_enabled and self.ns is not None and self.ns.ready:
                f = cleaned.astype(np.float32) / 32768.0
                try:
                    f = self.ns.process(f)
                except Exception:
                    pass  # fail-open: keep engine output
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
        dtln_obj = self.dtln
        with self._swap_lock:
            engine, backend = self.engine, self._backend
        ns = ns_obj.stats() if ns_obj is not None else {
            "backend": 0, "rCount": 0, "inRms": 0.0,
            "outRms": 0.0, "dropped": 0, "error": "",
        }
        name = {n: d for d, n in PROFILES}.get(engine, "DTLN-AEC 128")
        nkf_st = None
        if engine == "nkf" and backend is not None:
            try:
                nkf_st = backend.get_state()
            except Exception:
                nkf_st = None
        return {
            "running": self.running,
            "engine": name if self.running else "",
            "chain": (name + " + NS") if (self.running and ns["backend"]
                                           and self.ns_enabled) else (
                name if self.running else ""),
            "nsEnabled": 1 if self.ns_enabled else 0,
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
