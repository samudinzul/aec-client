"""DTLN noise reduction — Python port of src/dtln_ns_wrapper.cpp.

Same 512-block / 128-shift / 257-bin DSP as the AEC pair, minus the
loud-playback feed:
  model_1: [mag(257), states] -> [mask(257), states]
  model_2: [est(512), states] -> [block(512), states]

Framing mirrors the C++ exactly: 2048-sample input queue, 128-shift
processing, 4096-sample output ring prefilled to 512 at New (zeros —
first ~32 ms drain silence, same as desktop), passthrough while
priming, pass-through latch on backend failure. Fail-open: never mute.
"""

from collections import deque

import numpy as np

from . import dsp
from .dsp import BLOCK_LEN, BLOCK_SHIFT, BINS, rms

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # pragma: no cover
    Interpreter = None

Q_CAP = 2048
RING_CAP = 4096
RING_PREFILL = 512


def _classify_2(sizes):
    """Port of ClassifyDtns2: 2-input role map (feat, states)."""
    if len(sizes) == 2 and sizes[1] >= sizes[0]:
        return 1, 0
    s = max(range(len(sizes)), key=lambda i: sizes[i])
    if len(sizes) == 2 and sizes[0] == sizes[1]:
        s = 1
    return s, (0 if s else 1)


class DtlnNs:
    """Streaming DTLN-NS. Mirrors DtlnNsNew/DtlnNsProcess/DtlnNsReset."""

    def __init__(self, model_prefix="models/dtln_ns_128"):
        self.model_prefix = model_prefix
        self.last_error = ""
        self.backend = 0  # 0=none, 1=tflite
        self.i1 = self.i2 = None
        try:
            self._load(model_prefix)
            self.backend = 1
        except Exception as e:
            self.last_error = (
                f"dtln_ns model pair not found in models/ (NS stays off): {e}"
            )
        self.mic_buf = np.zeros(BLOCK_LEN, dtype=np.float32)
        self.out_buf = np.zeros(BLOCK_LEN, dtype=np.float32)
        self.q = deque()
        self.ring = deque([0.0] * RING_PREFILL if self.backend else [])
        self.dropped = 0
        self.in_rms = 0.0
        self.out_rms = 0.0

    def _load(self, prefix):
        if Interpreter is None:
            raise RuntimeError("ai-edge-litert is not installed")
        self.i1 = Interpreter(model_path=prefix + "_1.tflite")
        self.i1.allocate_tensors()
        self.i2 = Interpreter(model_path=prefix + "_2.tflite")
        self.i2.allocate_tensors()
        d1 = self.i1.get_input_details()
        s1 = [int(np.prod(d["shape"])) for d in d1]
        self.m1_state, self.m1_feat = _classify_2(s1)
        self.d1 = d1
        o1 = self.i1.get_output_details()
        c0 = int(np.prod(o1[0]["shape"]))
        c1 = int(np.prod(o1[1]["shape"])) if len(o1) > 1 else 0
        self.o1_prim, self.o1_state = (0, 1) if c0 <= c1 else (1, 0)
        d2 = self.i2.get_input_details()
        s2 = [int(np.prod(d["shape"])) for d in d2]
        self.m2_state, self.m2_feat = _classify_2(s2)
        self.d2 = d2
        self.o2 = self.i2.get_output_details()
        self.states1 = np.zeros(s1[self.m1_state], dtype=np.float32)
        self.states2 = np.zeros(s2[self.m2_state], dtype=np.float32)

    @property
    def ready(self):
        return self.backend != 0

    def reset(self):
        self.mic_buf.fill(0)
        self.out_buf.fill(0)
        self.q.clear()
        self.ring.clear()
        self.dropped = 0
        self.in_rms = self.out_rms = 0.0
        if self.backend:
            self.states1.fill(0)
            self.states2.fill(0)

    def _run_first(self, mag):
        vals = [None] * len(self.d1)
        vals[self.m1_feat] = mag
        vals[self.m1_state] = self.states1
        try:
            for det, arr in zip(self.d1, vals):
                self.i1.set_tensor(
                    det["index"], np.asarray(arr, dtype=np.float32).reshape(det["shape"])
                )
            self.i1.invoke()
            mask = np.asarray(
                self.i1.get_tensor(self.o1[self.o1_prim]["index"]), dtype=np.float32
            ).reshape(-1)[:BINS].copy()
            if len(self.o1) > 1:
                st = np.asarray(
                    self.i1.get_tensor(self.o1[self.o1_state]["index"]), dtype=np.float32
                ).reshape(-1)
                if st.size == self.states1.size:
                    self.states1 = st.copy()
            return mask
        except Exception:
            return np.ones(BINS, dtype=np.float32)

    def _run_second(self, est):
        vals = [None] * len(self.d2)
        vals[self.m2_feat] = est
        vals[self.m2_state] = self.states2
        try:
            for det, arr in zip(self.d2, vals):
                self.i2.set_tensor(
                    det["index"], np.asarray(arr, dtype=np.float32).reshape(det["shape"])
                )
            self.i2.invoke()
            out = np.asarray(
                self.i2.get_tensor(self.o2[0]["index"]), dtype=np.float32
            ).reshape(-1)[:BLOCK_LEN].copy()
            if len(self.o2) >= 2:
                st = np.asarray(
                    self.i2.get_tensor(self.o2[1]["index"]), dtype=np.float32
                ).reshape(-1)
                if st.size == self.states2.size:
                    self.states2 = st.copy()
            return out
        except Exception:
            return np.asarray(est, dtype=np.float32).copy()

    def _process_shift(self, mic_new):
        self.mic_buf = np.roll(self.mic_buf, -BLOCK_SHIFT)
        self.mic_buf[-BLOCK_SHIFT:] = mic_new
        spec, mag = dsp.rfft_mag(self.mic_buf)
        mask = self._run_first(mag)
        est = dsp.apply_mask_irfft(spec, mask)
        out_block = self._run_second(est)
        self.out_buf += out_block
        for i in range(BLOCK_SHIFT):
            if len(self.ring) < RING_CAP:
                self.ring.append(float(self.out_buf[i]))
            else:
                self.dropped += 1
        self.out_buf = np.roll(self.out_buf, -BLOCK_SHIFT)
        self.out_buf[-BLOCK_SHIFT:] = 0.0

    def process(self, frame_f32):
        """Float in -> float out (same length). Mirrors DtlnNsProcess."""
        frame = np.asarray(frame_f32, dtype=np.float32)
        n = len(frame)
        self.in_rms = rms(frame)
        if not self.ready:
            self.out_rms = self.in_rms
            return frame.copy()
        if len(self.q) + n > Q_CAP:
            self.out_rms = self.in_rms
            return frame.copy()
        self.q.extend(float(v) for v in frame)
        while len(self.q) >= BLOCK_SHIFT:
            mic_new = np.array([self.q.popleft() for _ in range(BLOCK_SHIFT)],
                               dtype=np.float32)
            self._process_shift(mic_new)
        out = np.empty(n, dtype=np.float32)
        for i in range(n):
            out[i] = self.ring.popleft() if self.ring else frame[i]
        self.out_rms = rms(out)
        return out

    def stats(self):
        return {
            "backend": self.backend,
            "rCount": len(self.ring),
            "inRms": self.in_rms,
            "outRms": self.out_rms,
            "dropped": self.dropped,
            "error": self.last_error,
        }
