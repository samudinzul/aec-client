"""Processing chain — mirrors the DTLN path in main.cpp's frame pump.

  engine (int16 -> int16)
  int16 -> float (/ 32768)
  DTLN-NS (float -> float, fail-open)
  float -> int16 (round-half-even + clamp, like ``clamp_s16(lrintf)``)

Notes on fidelity vs the desktop app:
- 16 kHz only (the DTLN-NR model is 16 kHz; other rates skip NS —
  aec-web runs 16 kHz always, so NS always applies when ready).
- The desktop speech gate only drives the NKF-only WPE stage; the
  DTLN path ignores it, so aec-web v1 carries no gate.
- Ref silence when speakers are idle: the loopback stream delivers
  silence naturally, same as the desktop ref-ring-underflow path.
"""

import threading

import numpy as np

from . import dsp
from .dtln import DtlnAec
from .dtln_ns import DtlnNs
from .notch import Notch
from .speech_gate import SpeechGate

# One frame is FRAME_SIZE samples at SAMPLE_RATE.
FRAME_DUR_MS = 1000.0 * dsp.FRAME_SIZE / dsp.SAMPLE_RATE


class Chain:
    def __init__(self, model_dir="models"):
        self.model_dir = model_dir
        self.dtln = None
        self.ns = None
        self.ns_enabled = True
        self.notch_enabled = True
        self.running = False
        self.last_error = ""
        self.frames = 0
        # Near-end speech gate + feedback-suppression
        # notch (ports of the desktop app's post stages;
        # the desktop runs the notch on the NKF path only,
        # the web chain adds it to the DTLN path — it is
        # engine-agnostic, fail-open DSP).
        self.gate = SpeechGate()
        self.notch = None
        self._load_lock = threading.Lock()

    def _load_engines(self):
        """Build the model pairs — the slow part
        (~5 s cold: 4 tflite files + interpreter
        setup). Caller holds _load_lock. NS
        missing/broken is fail-open, not fatal:
        the stage stays off and the engine output
        passes through."""
        self.dtln = DtlnAec(f"{self.model_dir}/dtln_aec_128")
        self.ns = DtlnNs(f"{self.model_dir}/dtln_ns_128")

    def preload(self):
        """Load the models at server startup (background
        thread) so the first Start is instant."""
        with self._load_lock:
            if self.running or self.dtln is not None:
                return
            self._load_engines()

    def start(self):
        with self._load_lock:
            if self.dtln is None:
                self._load_engines()
            if not self.dtln.ready:
                self.last_error = (self.dtln.last_error
                                   or "Failed to load DTLN model")
                self.dtln = None
                self.ns = None
                self.running = False
                return False
            self.notch = Notch(dsp.SAMPLE_RATE)
            self.gate = SpeechGate()
            self.running = True
            self.frames = 0
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
            self.notch = None
            self.gate = SpeechGate()
            self.running = False
            self.frames = 0

    def process_frame(self, mic_i16, ref_i16):
        """One int16 frame in -> int16 frame out. Never mutes."""
        mic = np.asarray(mic_i16, dtype=np.int16)
        ref = np.asarray(ref_i16, dtype=np.int16)
        if not self.running or self.dtln is None:
            return mic.copy()
        try:
            cleaned = self.dtln.process(mic, ref)
            # Speech gate on the ENGINE output — never
            # on the stages' own output, so a notch cut
            # can't flip the gate that froze it.
            cf = cleaned.astype(np.float32)
            self.gate.update(dsp.rms(cf), FRAME_DUR_MS)
            if self.ns_enabled and self.ns is not None and self.ns.ready:
                f = cf / 32768.0
                try:
                    f = self.ns.process(f)
                except Exception:
                    pass  # fail-open: keep engine output
                cleaned = dsp.f32_to_i16_round(f)
            # Feedback-suppression notch: adapts only
            # while the gate says quiet-and-not-speaking;
            # a sustained howl trips the gate's stuck
            # detector, which releases the notch so it
            # can latch the tone. Fail-open: any failure
            # leaves the frame untouched.
            if self.notch_enabled and self.notch is not None:
                try:
                    self.notch.set_speech(self.gate.for_notch())
                    self.notch.process(cleaned)
                except Exception:
                    pass
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
        ns = ns_obj.stats() if ns_obj is not None else {
            "backend": 0, "rCount": 0, "inRms": 0.0,
            "outRms": 0.0, "dropped": 0, "error": "",
        }
        return {
            "running": self.running,
            "engine": "DTLN-AEC 128" if self.running else "",
            "chain": "DTLN-AEC + NS" if (self.running and ns["backend"]
                                           and self.ns_enabled) else (
                "DTLN-AEC" if self.running else ""),
            "nsEnabled": 1 if self.ns_enabled else 0,
            "notchEnabled": 1 if self.notch_enabled else 0,
            "sampleRate": 16000,
            "dtlnBackend": dtln_obj.backend if dtln_obj else 0,
            "ns": ns,
            "notch": {
                "engaged": ([self.notch.engaged(0),
                             self.notch.engaged(1)]
                            if self.notch else [0, 0]),
                "hz": ([round(self.notch.freq(0)),
                        round(self.notch.freq(1))]
                       if self.notch else [0, 0]),
            },
            "gate": {
                "on": 1 if self.gate.on else 0,
                "stuck": 1 if self.gate.stuck else 0,
                "runMs": round(self.gate.run_ms),
                "evMs": round(self.gate.evidence_ms),
            },
            "frames": self.frames,
            "error": self.last_error,
            "dtlnError": dtln_obj.last_error if dtln_obj else "",
        }
