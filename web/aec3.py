"""WebRTC AEC3 engine — mirrors src/aec3_wrapper.cpp.

Desktop config (Aec3New): echo canceller on, mobile_mode off,
high-pass filter on, both gain controllers OFF, WebRTC noise
suppression retired (DTLN-NS is the global post stage, stacked
after every engine in Chain). ``pywebrtc_audio.EchoCanceller``
is exactly that configuration: pure AEC3 plus the always-on
capture HP filter — no NS, no AGC. ``stream_delay_ms=0`` matches
the desktop (no explicit delay; the internal estimator handles
it).

Frame bridging: AEC3 is 10 ms-native, but the web pump delivers
128-sample frames. The binding zero-pads any short tail *per
call*, so feeding it 128s would disrupt the adaptive filter
every 8 ms (periodic warble over the voice). This engine
accumulates to exact 160-sample blocks before calling the
binding and drains exactly the input length per call: after a
one-time 128-sample mic-pad prime at start/reset (fail-open,
never silence), the pipeline runs gaplessly with no holes or
replays at a steady ≤8 ms latency.

Fail-open like the C++ wrapper: a missing package or a dead
handle ships mic, never silence.
"""

import numpy as np

try:
    from pywebrtc_audio import EchoCanceller
except ImportError:  # optional backend — Chain reports it unavailable
    EchoCanceller = None


def available() -> bool:
    return EchoCanceller is not None


class Aec3Engine:
    """int16 mic + ref -> int16 cleaned, 16 kHz only."""

    # AEC3's native frame: 10 ms @ 16 kHz. Only exact multiples go
    # to the binding (see module docstring for why).
    _BLOCK = 160

    def __init__(self, sample_rate=16000):
        self.ready = False
        self.last_error = ""
        self._ec = None
        self._in_mic = []  # pending mic samples awaiting a full block
        self._in_ref = []  # pending ref samples (lockstep with _in_mic)
        self._out = []     # cleaned int16 samples awaiting drain
        if EchoCanceller is None:
            self.last_error = ("pywebrtc-audio is not installed "
                               "(pip install pywebrtc-audio)")
            return
        try:
            self._ec = EchoCanceller(sample_rate=sample_rate,
                                     num_channels=1,
                                     stream_delay_ms=0)
            self.ready = True
        except Exception as e:
            self.last_error = f"AEC3 init failed: {e}"

    def reset(self):
        try:
            if self._ec is not None:
                self._ec.reset()
        except Exception:
            pass
        self._in_mic = []
        self._in_ref = []
        self._out = []

    def process(self, mic_i16, ref_i16):
        mic = np.asarray(mic_i16, dtype=np.int16)
        if not self.ready or self._ec is None or len(mic) == 0:
            return mic.copy()
        try:
            ref = np.asarray(ref_i16, dtype=np.int16)
            n = min(len(mic), len(ref))
            if n == 0:
                return mic.copy()
            self._in_mic.extend(mic[:n].tolist())
            self._in_ref.extend(ref[:n].tolist())
            while len(self._in_mic) >= self._BLOCK:
                mb = (np.asarray(self._in_mic[:self._BLOCK],
                                 dtype=np.float32) / 32768.0)
                rb = (np.asarray(self._in_ref[:self._BLOCK],
                                 dtype=np.float32) / 32768.0)
                del self._in_mic[:self._BLOCK]
                del self._in_ref[:self._BLOCK]
                out = self._ec.process(mb, rb)
                out = np.asarray(out, dtype=np.float32).reshape(-1)
                out = out[:self._BLOCK]
                # Truncating cast, like the desktop's (int16_t)v — not
                # the rounded DTLN path (see Chain for that stage).
                out = np.clip(out, -1.0, 1.0) * 32768.0
                self._out.extend(np.where(out >= 0, np.floor(out),
                                          np.ceil(out)).astype(np.int16)
                                 .tolist())
            if len(self._out) >= n:
                res = np.asarray(self._out[:n], dtype=np.int16)
                del self._out[:n]
                return res
            # Startup/reset prime only: the binding hasn't emitted
            # enough yet — pad the shortfall with mic passthrough
            # (fail-open, never silence). The padded samples stay
            # queued, so every input sample is still processed exactly
            # once; only this first drain overlaps.
            res = (self._out + mic[len(self._out):n].tolist())
            self._out = []
            return np.asarray(res, dtype=np.int16)
        except Exception:
            return mic.copy()
