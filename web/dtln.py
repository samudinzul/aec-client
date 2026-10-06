"""DTLN-AEC engine — Python port of src/dtln_wrapper.cpp.

Pipeline mirrors breizhn/DTLN-aec run_aec.py and the C++ wrapper:
  block 512, shift 128, 16 kHz only
  model_1: [mic_mag(257), states, lpb_mag(257)] -> [mask(257), states]
  model_2: [est_td(512), states, lpb_td(512)]   -> [out(512), states]
with overlap-add on a 512-sample output buffer.

Backend: the upstream .tflite pair run directly through Google's
``ai-edge-litert`` wheel (exact weights, no conversion). There is no
ONNX path — the repo carries no DTLN .onnx pairs.
Fail-open everywhere: dead backend ships mic, never silence.

Probed tensor layouts of the shipped models (reshape-to-declared-
shape in ``_feed`` makes the port robust to any flat input):

  dtln_aec_128_1: IN  [1,1,257] mic_mag, [1,2,128,2] states,
                           [1,1,257] lpb_mag
                  OUT [1,1,257] mask,   [1,2,128,2] states
  dtln_aec_128_2: IN  [1,1,512] est_td, [1,2,128,2] states,
                           [1,1,512] lpb_td
                  OUT [1,1,512] block,  [1,2,128,2] states

One concatenated LSTM state tensor per model (512 floats), not
two separate states. Stage 2's three inputs are all 512 floats —
the official in[1]=states layout wins the tie (ClassifyDtln3),
never "largest input".

DSP fidelity vs the C++ wrapper (both verified by
web/test_offline.py --smoke, deterministic across platforms):
  - NO window on the STFT (plain rfft of the ring buffer) —
    neither sqrt-Hann nor Hann, same as breizhn/DTLN-aec
  - magnitudes are raw abs(), no normalization
  - irfft carries the 1/N (numpy), matching pocketfft c2r
    scale=1.0 plus the C++ ``estTd[i] / DTLN_BLOCK_LEN``
  - int16 emission clips then truncates toward zero
"""

from collections import deque

import numpy as np

from . import dsp
from .dsp import BLOCK_LEN, BLOCK_SHIFT, BINS

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # pragma: no cover - surfaced as a clean error below
    Interpreter = None


def _classify_3(sizes):
    """Port of ClassifyDtln3: 3-input role map (feat_a, states, feat_b).

    Official layout: in[0]=mic/est, in[1]=states, in[2]=lpb. Preferred
    when sizes match (state >= feat, or equal tie); otherwise the
    largest input is the state tensor.
    Returns (state_idx, a_idx, b_idx).
    """
    n = len(sizes)
    if n == 3 and sizes[0] == sizes[2] and sizes[1] >= sizes[0]:
        return 1, 0, 2
    s = max(range(n), key=lambda i: sizes[i])
    if n == 3 and sizes[0] == sizes[1] == sizes[2]:
        s = 1
    rest = [i for i in range(n) if i != s]
    if len(rest) >= 2:
        return s, rest[0], rest[-1]
    if len(rest) == 1:
        return s, rest[0], rest[0]
    return 0, 0, 0


def _classify_out(c0, c1):
    """Port of ClassifyDtlnOut: official order when sizes allow."""
    return (0, 1) if c0 <= c1 else (1, 0)


class _TflitePair:
    """One DTLN stage pair (mask stage or filter stage) via LiteRT."""

    def __init__(self, path_1, path_2, feat_len, n_feats):
        if Interpreter is None:
            raise RuntimeError(
                "ai-edge-litert is not installed "
                "(pip install -r requirements.txt)"
            )
        self.feat_len = feat_len
        self.i1 = Interpreter(model_path=path_1)
        self.i1.allocate_tensors()
        self.i2 = Interpreter(model_path=path_2)
        self.i2.allocate_tensors()
        d1 = self.i1.get_input_details()
        d2 = self.i2.get_input_details()
        s1 = [int(np.prod(d["shape"])) for d in d1]
        s2 = [int(np.prod(d["shape"])) for d in d2]
        self.s1_state, self.s1_a, self.s1_b = _classify_3(s1)
        self.s2_state, self.s2_a, self.s2_b = _classify_3(s2)
        self.d1, self.d2 = d1, d2
        self.o1 = self.i1.get_output_details()
        self.o2 = self.i2.get_output_details()
        c0 = int(np.prod(self.o1[0]["shape"]))
        c1 = int(np.prod(self.o1[1]["shape"])) if len(self.o1) > 1 else 0
        self.o1_prim, self.o1_state = _classify_out(c0, c1)
        self.states1 = np.zeros(s1[self.s1_state], dtype=np.float32)
        self.states2 = np.zeros(s2[self.s2_state], dtype=np.float32)
        self._n_feats = n_feats

    def _feed(self, interp, details, values):
        for idx, (det, arr) in enumerate(zip(details, values)):
            interp.set_tensor(
                det["index"], np.asarray(arr, dtype=np.float32).reshape(det["shape"])
            )

    def run_first(self, feat_a, feat_b):
        """mask = model_1(mic_mag, states, lpb_mag); identity on failure."""
        vals1 = [None] * len(self.d1)
        vals1[self.s1_a] = feat_a
        vals1[self.s1_b] = feat_b
        vals1[self.s1_state] = self.states1
        try:
            self._feed(self.i1, self.d1, vals1)
            self.i1.invoke()
            mask = np.asarray(
                self.i1.get_tensor(self.o1[self.o1_prim]["index"]),
                dtype=np.float32,
            ).reshape(-1)[: self.feat_len].copy()
            if len(self.o1) > 1:
                st = np.asarray(
                    self.i1.get_tensor(self.o1[self.o1_state]["index"]),
                    dtype=np.float32,
                ).reshape(-1)
                if st.size == self.states1.size:
                    self.states1 = st.copy()
            return mask
        except Exception:
            return np.ones(self.feat_len, dtype=np.float32)

    def run_second(self, est, lpb_td):
        """out = model_2(est, states, lpb_td); est on failure."""
        vals2 = [None] * len(self.d2)
        vals2[self.s2_a] = est
        vals2[self.s2_b] = lpb_td
        vals2[self.s2_state] = self.states2
        try:
            self._feed(self.i2, self.d2, vals2)
            self.i2.invoke()
            c0 = int(np.prod(self.o2[0]["shape"]))
            oi = 0
            if len(self.o2) >= 2:
                oi = 0 if c0 == BLOCK_LEN else 1
            out = np.asarray(
                self.i2.get_tensor(self.o2[oi]["index"]), dtype=np.float32
            ).reshape(-1)[:BLOCK_LEN].copy()
            if len(self.o2) >= 2:
                st = np.asarray(
                    self.i2.get_tensor(self.o2[1 - oi]["index"]), dtype=np.float32
                ).reshape(-1)
                if st.size == self.states2.size:
                    self.states2 = st.copy()
            return out
        except Exception:
            return np.asarray(est, dtype=np.float32).copy()


class DtlnAec:
    """Streaming DTLN-AEC. Mirrors DtlnNew/DtlnProcess/DtlnReset."""

    def __init__(self, model_prefix="models/dtln_aec_128"):
        self.model_prefix = model_prefix
        self.last_error = ""
        self.backend = 0  # 0=none, 1=tflite
        self.pair = None
        self.mic_ring = None
        self.ref_ring = None
        self.out_ring = None
        self.mic_buf = np.zeros(BLOCK_LEN, dtype=np.float32)
        self.lpb_buf = np.zeros(BLOCK_LEN, dtype=np.float32)
        self.out_buf = np.zeros(BLOCK_LEN, dtype=np.float32)
        try:
            self.pair = _TflitePair(
                model_prefix + "_1.tflite", model_prefix + "_2.tflite",
                BINS, 2,
            )
            self.backend = 1
            self.mic_ring = dsp._Ring(1024, dtype=np.float32)
            self.ref_ring = dsp._Ring(1024, dtype=np.float32)
            self.out_ring = dsp._Ring(512, dtype=np.int16)
        except Exception as e:  # missing files / no litert -> fail-open
            self.last_error = (
                f"DTLN: model pair not found for prefix '{model_prefix}' "
                f"(need *_1.tflite+*_2.tflite and ai-edge-litert): {e}"
            )

    @property
    def ready(self):
        return self.backend != 0

    def reset(self):
        self.mic_buf.fill(0)
        self.lpb_buf.fill(0)
        self.out_buf.fill(0)
        if self.mic_ring is not None:
            self.mic_ring = dsp._Ring(1024, dtype=np.float32)
            self.ref_ring = dsp._Ring(1024, dtype=np.float32)
            self.out_ring = dsp._Ring(512, dtype=np.int16)
        if self.pair is not None:
            self.pair.states1.fill(0)
            self.pair.states2.fill(0)

    def _process_shift(self, mic_new, lpb_new):
        self.mic_buf = np.roll(self.mic_buf, -BLOCK_SHIFT)
        self.lpb_buf = np.roll(self.lpb_buf, -BLOCK_SHIFT)
        self.mic_buf[-BLOCK_SHIFT:] = mic_new
        self.lpb_buf[-BLOCK_SHIFT:] = lpb_new

        mic_spec, mic_mag = dsp.rfft_mag(self.mic_buf)
        _, lpb_mag = dsp.rfft_mag(self.lpb_buf)

        mask = self.pair.run_first(mic_mag, lpb_mag)
        est = dsp.apply_mask_irfft(mic_spec, mask)
        out_block = self.pair.run_second(est, self.lpb_buf.copy())

        self.out_buf = np.roll(self.out_buf, -BLOCK_SHIFT)
        self.out_buf[-BLOCK_SHIFT:] = 0.0
        self.out_buf += out_block

    def process(self, mic_i16, ref_i16):
        """One frame in/out (int16 numpy arrays) — mirrors DtlnProcess."""
        n = len(mic_i16)
        if not self.ready:
            return np.asarray(mic_i16, dtype=np.int16).copy()
        self.mic_ring.push(dsp.i16_to_f32(np.asarray(mic_i16)))
        self.ref_ring.push(dsp.i16_to_f32(np.asarray(ref_i16)))
        while self.mic_ring.size >= BLOCK_SHIFT and self.ref_ring.size >= BLOCK_SHIFT:
            mic_new = self.mic_ring.pop(BLOCK_SHIFT)
            lpb_new = self.ref_ring.pop(BLOCK_SHIFT)
            self._process_shift(mic_new, lpb_new)
            self.out_ring.push(dsp.f32_to_i16_trunc(self.out_buf[:BLOCK_SHIFT]))
            self.out_buf[:BLOCK_SHIFT] = 0.0
        out = np.empty(n, dtype=np.int16)
        if self.out_ring.size >= n:
            out[:] = self.out_ring.pop(n)
        else:
            for i in range(n):
                out[i] = self.out_ring.pop(1) if self.out_ring.size else 0
        return out
