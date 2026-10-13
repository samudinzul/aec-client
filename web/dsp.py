"""Shared DSP primitives for aec-web — faithful port of the C++ wrappers.

Matches src/dtln_wrapper.cpp + src/dtln_ns_wrapper.cpp bit-for-bit
in framing semantics:

- 512-sample block, 128-sample shift, 257-bin spectrum, 16 kHz only
- rFFT with NO window and scale 1.0 (pocketfft r2c FORWARD, 1.0).
  numpy.fft.rfft is exactly that; numpy.fft.irfft carries the 1/N,
  which matches the C++ ``estTd[i] / DTLN_BLOCK_LEN`` step.
- int16 <-> float via / 32768.0 (AEC path truncates on the way back,
  the NS->int16 path rounds — see helpers below; both mirror main.cpp).
"""

import numpy as np


# Cached 61-tap FIR lowpass (fc ~7 kHz, normalized), reused on
# every resample call — building it (sinc + Hann) on each call
# was wasted work in the hot audio path. float32, not float64:
# the desktop resamples in float32 too, halves the convolve memory
# traffic, and drops the float64->float32 converts per call; the
# ~1e-7 coefficient/output difference is inaudible and the smoke
# RMS/peak figures are unchanged (verified A/B on hardware).
_FIR = None
_FIR_UP = None


def _build_fir():
    taps = 61
    n = np.arange(taps) - taps // 2
    fc = 7000.0 / 48000.0
    h = np.sinc(2 * fc * n) * (0.54 - 0.46 * np.cos(2 * np.pi * (n + taps // 2) / (taps - 1)))
    return h / h.sum()


_FIR = _build_fir().astype(np.float32)
_FIR_UP = (_FIR * np.float32(3.0)).astype(np.float32)  # upsampling gain x3


class _Ring:
    """Lock-free circular buffer over a numpy array.

    One producer, one consumer (the audio pump); every _Ring is
    used by exactly one worker thread, so no concurrency
    protection is needed. _count tracks the number of elements so
    size/free stay correct even when the write/read heads wrap.
    """

    __slots__ = ("buf", "cap", "w", "r", "dropped", "_count")

    def __init__(self, cap, dtype):
        self.buf = np.zeros(cap, dtype=dtype)
        self.cap = cap
        self.w = 0
        self.r = 0
        self.dropped = 0
        self._count = 0

    @property
    def size(self):
        return self._count

    @property
    def free(self):
        return self.cap - self._count

    def push(self, src):
        n = len(src)
        if n > self.free:
            self.dropped += n - self.free
            return
        if self.w + n <= self.cap:
            self.buf[self.w:self.w + n] = src
            self.w += n
        else:
            part = self.cap - self.w
            self.buf[self.w:] = src[:part]
            self.buf[:n - part] = src[part:]
            self.w = n - part
        self._count += n

    def pop(self, n):
        """n samples from the read head; returns a copy."""
        assert n <= self._count
        idx = self.r
        if idx + n <= self.cap:
            out = self.buf[idx:idx + n]
        else:
            out = np.concatenate([self.buf[idx:], self.buf[:idx + n - self.cap]])
        self.r += n
        self.r %= self.cap
        self._count -= n
        return out.copy()


class _StreamBuf:
    """Append-at-tail / consume-from-head float32 buffer with no
    per-op allocations.

    Replaces the np.concatenate-per-append + slice-copy-per-consume
    pattern in the pump and output callbacks: appends copy into spare
    tail capacity, consumption only advances the head, and the live
    window is compacted (one memmove) only when the tail runs out —
    then the buffer grows instead if compaction is not enough (it
    never shrinks back). Values in/out are identical to the concat
    version; only allocations and GC churn disappear.

    pop() returns a VIEW into the buffer: use it immediately in the
    calling expression (every caller does). A later append may
    compact underneath a retained view — the view keeps its (now
    stale) data, so never hold one across calls.
    """

    __slots__ = ("buf", "head", "tail")

    def __init__(self, cap=4096):
        self.buf = np.zeros(cap, dtype=np.float32)
        self.head = 0
        self.tail = 0

    def __len__(self):
        return self.tail - self.head

    def append(self, x):
        x = np.asarray(x, dtype=np.float32)  # no-op for float32 input
        n = len(x)
        if n == 0:
            return
        if self.tail + n > self.buf.size:
            if self.head:
                live = self.tail - self.head
                self.buf[:live] = self.buf[self.head:self.tail]
                self.tail = live
                self.head = 0
            if self.tail + n > self.buf.size:
                grown = np.zeros(
                    max(self.buf.size * 2, self.tail + n),
                    dtype=np.float32)
                grown[:self.tail] = self.buf[:self.tail]
                self.buf = grown
        self.buf[self.tail:self.tail + n] = x
        self.tail += n

    def pop(self, n):
        """First-n-samples view; advances the head. Use immediately."""
        out = self.buf[self.head:self.head + n]
        self.head += n
        if self.head == self.tail:
            self.head = self.tail = 0  # drained: rewind, never walk far
        return out


BLOCK_LEN = 512
BLOCK_SHIFT = 128
BINS = 257  # rfft(512)

SAMPLE_RATE = 16000
# 8 ms @ 16 kHz. Deliberately equals BLOCK_SHIFT (not the desktop's
# 160-sample pump): one frame feeds exactly one shift, so the steady
# state runs 1.0 shifts/frame instead of 1.25 — ~20% fewer TFLite
# invokes + FFTs for the same audio.
FRAME_SIZE = 128


def rfft_mag(block: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """rFFT of a 512-sample float block -> (complex spectrum, magnitudes)."""
    spec = np.fft.rfft(block)
    return spec, np.abs(spec).astype(np.float32)


def apply_mask_irfft(spec: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """irfft(spec * mask); numpy's irfft already includes the 1/N."""
    return np.fft.irfft(spec * mask, n=BLOCK_LEN).astype(np.float32)


def i16_to_f32(frame: np.ndarray) -> np.ndarray:
    """int16 -> float32, mirrors ``micF[i] = mic[i] / 32768.0f``."""
    return frame.astype(np.float32) / 32768.0


def f32_to_i16_trunc(block: np.ndarray) -> np.ndarray:
    """float [-1, 1] -> int16 with truncation, mirrors the C++ emission::

        float v = h->outBuf[i] * 32768.0f; clip; (int16_t)v

    (clip first, then C-cast truncate toward zero)."""
    v = np.clip(np.asarray(block, dtype=np.float32) * 32768.0, -32768.0, 32767.0)
    return v.astype(np.int16)  # numpy float->int cast truncates, like C


def f32_to_i16_round(block: np.ndarray) -> np.ndarray:
    """float -> int16 with round-half-even, mirrors
    ``clamp_s16((int)lrintf(x * 32768))`` in main.cpp's NS stage."""
    v = np.rint(block * 32768.0)
    return np.clip(v, -32768.0, 32767.0).astype(np.int16)


def rms(x: np.ndarray) -> float:
    """RMS of a float frame (matches the C++ sqrt(sum(x^2)/n))."""
    v = np.asarray(x, dtype=np.float32).ravel()
    if v.size == 0:
        return 0.0
    return float(np.sqrt(np.dot(v, v) / v.size))


# Mirrors main.cpp SoftLimit: linear to -3 dBFS, tanh knee above it,
# then a hard clamp as last-resort safety. Vectorized over the whole
# frame (one np.tanh call) — same cost as the np.clip it replaces,
# so there is no CPU regression vs the hard-clip web build.
_KNEE = np.float32(0.7079)          # -3 dBFS, same constant as the C++ side
_KNEE_COMP = np.float32(1.0) - _KNEE


def soft_limit(x: np.ndarray) -> np.ndarray:
    """Float [-1, 1] mix -> float in int16 domain [-32768, 32767].

    Mirrors ``SoftLimit(v)`` in src/main.cpp exactly:
        a = |v|
        if a <= knee:  v * 32768
        else:          sign(v) * (knee + (1-knee)*tanh((a-knee)/(1-knee))) * 32768
    then clamp_s16. The tanh knee turns headroom into smooth compression
    instead of the square-wave distortion a hard clip at 0 dBFS makes
    (mic gain up to 4x + soundboard gain up to 2x routinely exceed 1.0).
    """
    v = np.asarray(x, dtype=np.float32)
    a = np.abs(v)
    # y = a below the knee, knee + (1-knee)*tanh(...) above.
    y = np.where(a <= _KNEE, a,
                 _KNEE + _KNEE_COMP * np.tanh((a - _KNEE) / _KNEE_COMP))
    out = np.sign(v) * y * np.float32(32768.0)
    return np.clip(out, -32768.0, 32767.0).astype(np.float32)


def _fir_decimate_3(x: np.ndarray) -> np.ndarray:
    """48000 -> 16000: symmetric FIR lowpass (fc ~7 kHz) + take every 3rd.

    Coefficients: windowed sinc, 61 taps, ~60 dB stopband. Reference
    quality only needs aliasing below audibility-vs-AEC tolerance, and
    this is far beyond that. Coefficients are computed once at import.
    """
    x = np.asarray(x)
    if x.size and not np.any(x):
        # Silence fast path: filtering zeros yields zeros (0*x=0,
        # sums of signed zeros stay zero through the int16 emission
        # and RMS downstream), without the float32 convolve. Length
        # replicates convolve(mode="same") exactly: max(len, taps),
        # then every 3rd. The idle-loopback ref hits this constantly.
        return np.zeros((max(x.size, _FIR.size) + 2) // 3,
                        dtype=np.float32)
    y = np.convolve(np.asarray(x, dtype=np.float32), _FIR, mode="same")
    # copy=False: no-op when convolve already yields float32, converts
    # only if the platform returns something else — never a spare copy.
    return y[::3].astype(np.float32, copy=False)


def _fir_interpolate_3(x: np.ndarray) -> np.ndarray:
    """16000 -> 48000: zero-stuff x3 + the same 61-tap lowpass, gain x3.

    Stateless per chunk (same convention as _fir_decimate_3): the
    cable-output callback feeds consecutive frames, so keeping it
    stateless avoids cross-thread filter state.
    """
    x = np.asarray(x)
    if x.size and not np.any(x):
        # Silence fast path (same exactness argument as decimate):
        # zero-stuffing zeros then convolving is zeros out, without
        # the zero buffer + float32 convolve. Length replicates
        # convolve(mode="same") exactly: max(3*len, taps). The idle
        # monitor stream used to burn a full FIR here every frame.
        return np.zeros(max(x.size * 3, _FIR_UP.size), dtype=np.float32)
    up = np.zeros(x.size * 3, dtype=np.float32)
    up[::3] = np.asarray(x, dtype=np.float32)
    y = np.convolve(up, _FIR_UP, mode="same")
    return y.astype(np.float32, copy=False)


def resample_to_16k(pcm_i16: np.ndarray, rate: int) -> np.ndarray:
    """Loopback capture (native mix rate) -> 16 kHz float32 reference."""
    x = i16_to_f32(pcm_i16)
    if rate == SAMPLE_RATE or x.size == 0:
        return x
    if rate == 48000:
        return _fir_decimate_3(x)
    # Generic fallback: linear interpolation (reference-grade only).
    want = int(round(x.size * SAMPLE_RATE / rate))
    if want <= 0:
        return np.zeros(0, dtype=np.float32)
    src = np.linspace(0, 1, x.size)
    dst = np.linspace(0, 1, want)
    return np.interp(dst, src, x).astype(np.float32)


def resample_from_16k(frame_f32: np.ndarray, rate: int) -> np.ndarray:
    """16 kHz float32 -> native-rate float32 (mic input down, cable up)."""
    x = np.asarray(frame_f32, dtype=np.float32)
    if rate == SAMPLE_RATE or x.size == 0:
        return x
    if rate == 48000:
        # x3 polyphase: zero-stuff + the same 61-tap lowpass, gain x3.
        return _fir_interpolate_3(x)
    want = int(round(x.size * rate / SAMPLE_RATE))
    if want <= 0:
        return np.zeros(0, dtype=np.float32)
    return np.interp(np.linspace(0, 1, want), np.linspace(0, 1, x.size), x).astype(np.float32)
