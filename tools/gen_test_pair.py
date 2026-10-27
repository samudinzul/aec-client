"""Deterministic mic/ref test pair for smoke + parity runs.

Usage (from repo root):  python tools/gen_test_pair.py [mic.wav ref.wav]

Writes 10 s of 16 kHz mono int16: near-end voice in the 1st/3rd thirds
(double-talk) with an echo-only middle third (far-end voice at 105 Hz
echoed with a 1200-sample delay + room tap). Fixed seed, so every
machine generates the byte-identical pair — the reference input for
`nkf/dtln/aec3_smoke ... [--no-ns]` and `web.test_parity --run`.
"""

import sys
import wave

import numpy as np


def main():
    mic_path, ref_path = (sys.argv[1:3] + ["mic.wav", "ref.wav"])[:2]
    sr, secs = 16000, 10
    rng = np.random.default_rng(7)
    t = np.arange(sr * secs) / sr
    vib = 1 + 0.01 * np.sin(2 * np.pi * 5 * t)
    far = np.zeros_like(t)
    for h in range(1, 7):
        far += (np.sin(2 * np.pi * 105 * h * np.cumsum(vib) / sr
                       + rng.uniform(0, 6.28)) / (h ** 0.8))
    far = 0.30 * far * (0.5 + 0.5 * np.sin(2 * np.pi * 2.1 * t)) ** 2
    near = np.zeros_like(t)
    for h in range(1, 7):
        near += (np.sin(2 * np.pi * 150 * h * np.cumsum(vib) / sr
                        + rng.uniform(0, 6.28)) / (h ** 0.8))
    gate = ((t < secs / 3) | (t > 2 * secs / 3)).astype(float)
    near = 0.25 * near * (0.5 + 0.5 * np.sin(2 * np.pi * 2.7 * t)) ** 2 * gate
    dly = 1200
    echo = np.concatenate([np.zeros(dly), 0.5 * far[:-dly]])
    echo[1:] += 0.15 * echo[:-1]
    mic = np.clip(near + echo + 0.004 * rng.standard_normal(len(t)),
                  -1, 1)
    for name, arr in ((mic_path, mic), (ref_path, far)):
        with wave.open(name, "wb") as w:
            w.setframerate(sr)
            w.setnchannels(1)
            w.setsampwidth(2)
            w.writeframes((np.clip(arr, -1, 1) * 32767).astype(
                np.int16).tobytes())
    print(f"wrote {mic_path} {ref_path} "
          f"(10 s: double-talk / echo-only / double-talk)")


if __name__ == "__main__":
    main()
