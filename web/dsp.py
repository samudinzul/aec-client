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

BLOCK_LEN = 512
BLOCK_SHIFT = 128
BINS = 257  # rfft(512)

SAMPLE_RATE = 16000
FRAME_SIZE = 160  # 10 ms @ 16 kHz, matches the desktop frame pump


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
    v = np.clip(np.asarray(block, dtype=np.float64) * 32768.0, -32768.0, 32767.0)
    return v.astype(np.int16)  # numpy float->int cast truncates, like C


def f32_to_i16_round(block: np.ndarray) -> np.ndarray:
    """float -> int16 with round-half-even, mirrors
    ``clamp_s16((int)lrintf(x * 32768))`` in main.cpp's NS stage."""
    v = np.rint(block * 32768.0)
    return np.clip(v, -32768.0, 32767.0).astype(np.int16)


def rms(x: np.ndarray) -> float:
    """RMS of a float frame (matches the C++ sqrt(sum(x^2)/n))."""
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def _fir_decimate_3(x: np.ndarray) -> np.ndarray:
    """48000 -> 16000: symmetric FIR lowpass (fc ~7 kHz) + take every 3rd.

    Coefficients: windowed sinc, 61 taps, ~60 dB stopband. Reference
    quality only needs aliasing below audibility-vs-AEC tolerance, and
    this is far beyond that.
    """
    taps = 61
    n = np.arange(taps) - taps // 2
    fc = 7000.0 / 48000.0
    h = np.sinc(2 * fc * n) * (0.54 - 0.46 * np.cos(2 * np.pi * (n + taps // 2) / (taps - 1)))
    h = h / h.sum()
    y = np.convolve(x.astype(np.float64), h, mode="same")
    return y[::3].astype(np.float32)


def resample_to_16k(pcm_i16: np.ndarray, rate: int) -> np.ndarray:
    """Loopback capture (native mix rate) -> 16 kHz float32 reference."""
    x = np.asarray(pcm_i16, dtype=np.float32) / 32768.0
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
