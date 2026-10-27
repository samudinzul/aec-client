"""GTCRN noise suppression — Python port of src/gtcrn_wrapper.cpp.

Same streaming DSP as the experimental desktop backend (GTCRN "simple"
0.5 MB ONNX graph, MIT, from the GTCRN repo's stream/onnx_models):

  sqrt-Hann STFT(512) / 256-hop outside the graph, mix[1,257,1,2]
  (real, imag) + three zero-init caches, enh + updated caches out,
  ISTFT overlap-add.

The wrapper FIFOs 160-sample pump frames into 256-sample hops and
returns exactly len(input) per call. While priming (and for the
first couple of pump calls, exactly like the C++), the shortfall is
input passthrough — the stage never mutes. A failed session latches
to passthrough, the same fail-open contract as DtlnNs/HushNs.

This exists so tools/aec_eval.py can compare the desktop's GTCRN
backend against DTLN-NS/Hush on the same signal, without a C++
build. It is not wired into the shipped web UI.
"""

import os

import numpy as np

from .dsp import rms

MODEL_NAME = "gtcrn_simple.onnx"

# STFT geometry (must match src/gtcrn_wrapper.cpp).
N_FFT = 512
HOP = 256
BINS = N_FFT // 2 + 1  # 257


def resolve_model(model_path=None, model_dir=None):
    """Model path: explicit, GTCRN_MODEL env, chain models dir, repo
    models/ (so a bare ``--ns gtcrn`` finds the shipped copy)."""
    if model_path:
        return model_path
    env = os.environ.get("GTCRN_MODEL")
    if env:
        return env
    if model_dir:
        cand = os.path.join(model_dir, MODEL_NAME)
        if os.path.isfile(cand):
            return cand
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, "models", MODEL_NAME)


def gtcrn_available(model_path=None, model_dir=None):
    """Cheap stat only — never loads the ONNX runtime."""
    return os.path.isfile(resolve_model(model_path, model_dir))


def _kind(name, shape, is_input):
    """Session slot role, by name (mirrors KindOf in the C++)."""
    lo = name.lower()
    if "conv" in lo:
        return "conv"
    if "tra" in lo:
        return "tra"
    if "inter" in lo:
        return "inter"
    if "mix" in lo or "enh" in lo:
        return "mix"
    if is_input and 257 in shape:
        return "mix"
    return None


class GtcrnNs:
    """Streaming GTCRN NS. Mirrors GtcrnNew/GtcrnProcess/GtcrnReset."""

    def __init__(self, model_path=None, model_dir=None, num_threads=1):
        self.model_path = resolve_model(model_path, model_dir)
        self.last_error = ""
        self.in_rms = 0.0
        self.out_rms = 0.0
        self.dropped = 0
        self._sess = None
        self._in_names = []
        self._out_names = []
        self._in_kind = []  # role per session input
        self._out_kind = []  # role per session output
        self._mix = np.zeros((1, BINS, 1, 2), dtype=np.float32)
        self._caches = {}  # role -> ndarray
        self._shapes = {}  # role -> tuple
        self._win = np.sin(np.pi * np.arange(N_FFT) / N_FFT).astype(np.float64)
        self._win_buf = np.zeros(N_FFT, dtype=np.float64)
        self._ola = np.zeros(N_FFT, dtype=np.float64)
        self._in_q = np.zeros(0, dtype=np.float32)
        self._out_q = np.zeros(0, dtype=np.float32)
        self._neural = False
        self._latched = False
        try:
            self._load(num_threads)
        except Exception as e:
            self.last_error = f"gtcrn unavailable ({self.model_path}): {e}"
            self._sess = None

    def _load(self, num_threads):
        if not os.path.isfile(self.model_path):
            raise RuntimeError("model not found — run exp/get_models.sh "
                               "or set GTCRN_MODEL")
        import onnxruntime as ort

        opts = ort.SessionOptions()
        opts.intra_op_num_threads = max(1, int(num_threads))
        opts.inter_op_num_threads = 1
        opts.graph_optimization_level = (
            ort.GraphOptimizationLevel.ORT_ENABLE_ALL)
        sess = ort.InferenceSession(
            self.model_path, sess_options=opts,
            providers=["CPUExecutionProvider"])

        ins, outs = sess.get_inputs(), sess.get_outputs()
        self._in_names = [d.name for d in ins]
        self._out_names = [d.name for d in outs]
        self._in_kind = [_kind(d.name, list(d.shape), True) for d in ins]
        self._out_kind = [_kind(d.name, list(d.shape), False) for d in outs]

        # Expected graph: one mix + three cache inputs, four outputs.
        if len(ins) != 4 or len(outs) != 4:
            raise RuntimeError(
                f"unexpected graph: {len(ins)} inputs / {len(outs)} "
                "outputs (need mix + 3 caches)")
        if self._in_kind.count("mix") != 1 or sum(
                k is not None for k in self._in_kind) != 4:
            raise RuntimeError("unexpected input roles: "
                               f"{list(zip(self._in_names, self._in_kind))}")
        for d, kind in zip(ins, self._in_kind):
            shape = tuple(int(v) for v in d.shape)
            if any(v <= 0 for v in shape):
                raise RuntimeError(f"symbolic dims unsupported: {d.name} "
                                   f"{d.shape}")
            if kind == "mix":
                if shape != (1, BINS, 1, 2):
                    raise RuntimeError(f"bad mix shape {shape}")
                continue
            self._shapes[kind] = shape
            self._caches[kind] = np.zeros(shape, dtype=np.float32)

        self._sess = sess

    @property
    def ready(self):
        return self._sess is not None

    def reset(self):
        self._win_buf.fill(0.0)
        self._ola.fill(0.0)
        for arr in self._caches.values():
            arr.fill(0.0)
        self._in_q = np.zeros(0, dtype=np.float32)
        self._out_q = np.zeros(0, dtype=np.float32)
        self._latched = False
        self._neural = False
        self.dropped = 0
        self.in_rms = self.out_rms = 0.0

    def _run_hop(self, hop):
        """One 256-sample hop: window -> STFT -> Run -> ISTFT -> OLA.

        Returns the 256 emitted samples, or None on failure (caller
        latches to passthrough).
        """
        self._win_buf[:-HOP] = self._win_buf[HOP:]
        self._win_buf[-HOP:] = hop
        spec = np.fft.rfft(self._win_buf * self._win)

        self._mix[0, :, 0, 0] = spec.real
        self._mix[0, :, 0, 1] = spec.imag
        feeds = {}
        for name, kind in zip(self._in_names, self._in_kind):
            feeds[name] = self._mix if kind == "mix" else self._caches[kind]
        try:
            outs = self._sess.run(self._out_names, feeds)
        except Exception as e:  # Run failed -> latch
            self.last_error = f"gtcrn Run failed: {e}"
            return None

        enh_idx = self._out_kind.index("mix")
        enh = np.asarray(outs[enh_idx], dtype=np.float32).reshape(BINS, 2)
        for i, kind in enumerate(self._out_kind):
            if i == enh_idx:
                continue
            arr = np.asarray(outs[i], dtype=np.float32)
            dst = self._caches.get(kind)
            if dst is None or arr.size != dst.size:
                self.last_error = "gtcrn cache shape drift — latched off"
                return None
            dst[...] = arr.reshape(dst.shape)
        self._neural = True

        # Inverse rFFT includes the 1/N the C++ applies explicitly;
        # synthesis window + overlap-add complete the sqrt-Hann pair.
        rec = np.fft.irfft(enh[:, 0] + 1j * enh[:, 1], n=N_FFT)
        self._ola += rec * self._win
        emit = self._ola[:HOP].copy()
        self._ola[:-HOP] = self._ola[HOP:]
        self._ola[-HOP:] = 0.0
        return emit.astype(np.float32)

    def process(self, frame_f32):
        """Float in -> float out (same length). Mirrors GtcrnProcess."""
        frame = np.asarray(frame_f32, dtype=np.float32)
        n = len(frame)
        self.in_rms = rms(frame)
        if not self.ready or self._latched:
            self.out_rms = self.in_rms
            return frame.copy()

        if self._in_q.size > 32768:  # stall guard, mirrors the C++
            drop = self._in_q.size - 32768
            self._in_q = self._in_q[drop:]
            self.dropped += int(drop)
        if n:
            self._in_q = np.concatenate((self._in_q, frame))
        while self._in_q.size >= HOP:
            hop = self._in_q[:HOP]
            self._in_q = self._in_q[HOP:]
            emit = self._run_hop(hop)
            if emit is None:
                # Fail-open: current frame untouched; the rest of the
                # queue is dropped rather than replayed out of order.
                self._latched = True
                self._in_q = np.zeros(0, dtype=np.float32)
                self._out_q = np.zeros(0, dtype=np.float32)
                self.out_rms = self.in_rms
                return frame.copy()
            self._out_q = np.concatenate((self._out_q, emit))

        out = np.empty(n, dtype=np.float32)
        if n:
            m = min(n, self._out_q.size)
            if m:
                out[:m] = self._out_q[:m]
                self._out_q = self._out_q[m:]
            if m < n:
                # Priming: shortfall is this frame's own input, exactly
                # like the C++ (never silence, never mute).
                out[m:] = frame[m:]
        self.out_rms = rms(out)
        return out

    def stats(self):
        return {
            "backend": 3 if self.ready else 0,
            "rCount": int(self._out_q.size),
            "inRms": self.in_rms,
            "outRms": self.out_rms,
            "dropped": self.dropped,
            "neural": self._neural,
            "latched": self._latched,
            "error": self.last_error,
        }
