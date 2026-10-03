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

import numpy as np

from . import dsp
from .dtln import DtlnAec
from .dtln_ns import DtlnNs


class Chain:
    def __init__(self, model_dir="models"):
        self.model_dir = model_dir
        self.dtln = None
        self.ns = None
        self.running = False
        self.last_error = ""
        self.frames = 0

    def start(self):
        self.dtln = DtlnAec(f"{self.model_dir}/dtln_aec_128")
        if not self.dtln.ready:
            self.last_error = self.dtln.last_error or "Failed to load DTLN model"
            self.dtln = None
            self.ns = None
            self.running = False
            return False
        self.ns = DtlnNs(f"{self.model_dir}/dtln_ns_128")
        # NS missing/broken is fail-open, not fatal — same as desktop:
        # the stage stays off and the engine output passes through.
        self.running = True
        self.frames = 0
        self.last_error = "" if self.ns.ready else self.ns.last_error
        return True

    def stop(self):
        self.dtln = None
        self.ns = None
        self.running = False

    def process_frame(self, mic_i16, ref_i16):
        """One int16 frame in -> int16 frame out. Never mutes."""
        mic = np.asarray(mic_i16, dtype=np.int16)
        ref = np.asarray(ref_i16, dtype=np.int16)
        if not self.running or self.dtln is None:
            return mic.copy()
        try:
            cleaned = self.dtln.process(mic, ref)
            if self.ns is not None and self.ns.ready:
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
        ns = self.ns.stats() if self.ns is not None else {
            "backend": 0, "rCount": 0, "inRms": 0.0,
            "outRms": 0.0, "dropped": 0, "error": "",
        }
        return {
            "running": self.running,
            "engine": "DTLN-AEC 128" if self.running else "",
            "chain": "DTLN-AEC + NS" if (self.running and ns["backend"]) else (
                "DTLN-AEC" if self.running else ""),
            "sampleRate": 16000,
            "dtlnBackend": self.dtln.backend if self.dtln else 0,
            "ns": ns,
            "frames": self.frames,
            "error": self.last_error,
            "dtlnError": self.dtln.last_error if self.dtln else "",
        }
