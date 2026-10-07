"""WebRTC AEC3 engine — mirrors src/aec3_wrapper.cpp.

Desktop config (Aec3New): echo canceller on, mobile_mode off,
high-pass filter on, both gain controllers OFF, WebRTC noise
suppression retired (DTLN-NS is the global post stage, stacked
after every engine in Chain). ``pywebrtc_audio.EchoCanceller``
is exactly that configuration: pure AEC3 plus the always-on
capture HP filter — no NS, no AGC. ``stream_delay_ms=0`` matches
the desktop (no explicit delay; the internal estimator handles
it). Frames are any length (the binding splits into 10 ms
internally), so the 128-sample pump feeds it directly.

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

    def __init__(self, sample_rate=16000):
        self.ready = False
        self.last_error = ""
        self._ec = None
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

    def process(self, mic_i16, ref_i16):
        mic = np.asarray(mic_i16, dtype=np.int16)
        if not self.ready or self._ec is None or len(mic) == 0:
            return mic.copy()
        try:
            ref = np.asarray(ref_i16, dtype=np.int16)
            n = min(len(mic), len(ref))
            if n == 0:
                return mic.copy()
            out = self._ec.process(mic[:n].astype(np.float32) / 32768.0,
                                   ref[:n].astype(np.float32) / 32768.0)
            out = np.asarray(out, dtype=np.float32).reshape(-1)[:n]
            # Truncating cast, like the desktop's (int16_t)v — not the
            # rounded DTLN path (see Chain for that stage).
            out = np.clip(out, -1.0, 1.0) * 32768.0
            return np.where(out >= 0, np.floor(out),
                            np.ceil(out)).astype(np.int16)
        except Exception:
            return mic.copy()
