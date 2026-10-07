"""Offline fidelity harness — WAV-in -> WAV-out through the web chain.

Usage (from repo root):
    python web/test_offline.py mic.wav ref.wav out.wav

Reads two 16 kHz mono WAVs, runs them through Chain (DTLN-AEC + NS),
writes the cleaned output. Compare against the desktop app's output
on the same pair: prints RMS + peak so you can judge the match.

Also doubles as a smoke test with synthetic audio (no WAVs needed):
    python web/test_offline.py --smoke
"""

import sys
import wave

import numpy as np


def read_wav16(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000, f"{path}: need 16 kHz"
        assert w.getnchannels() == 1, f"{path}: need mono"
        assert w.getsampwidth() == 2, f"{path}: need int16"
        raw = w.readframes(w.getnframes())
    return np.frombuffer(raw, dtype=np.int16).copy()


def write_wav16(path, data):
    with wave.open(path, "wb") as w:
        w.setframerate(16000)
        w.setnchannels(1)
        w.setsampwidth(2)
        w.writeframes(np.asarray(data, dtype=np.int16).tobytes())


def _synth_pair(seconds=4, seed=3):
    import numpy as np
    rng = np.random.default_rng(seed)
    sr = 16000
    t = np.arange(sr * seconds) / sr
    ref = (0.4 * rng.standard_normal(len(t))).astype(np.float32)
    dly = 800
    echo = np.concatenate([np.zeros(dly, np.float32),
                           0.7 * ref[:len(ref) - dly]])
    burst = (((t % 2.0) < 0.5).astype(np.float32)
             * (0.25 * np.sin(2 * np.pi * 220 * t)).astype(np.float32))
    mic = np.clip(echo + burst, -1, 1)
    return ((mic * 32767).astype(np.int16),
            (ref * 32767).astype(np.int16))


def smoke():
    import numpy as np

    from .chain import PROFILES, Chain, engine_available
    from .dsp import FRAME_SIZE

    mic, ref = _synth_pair(2)
    ch = Chain()
    ok = ch.start()
    print(f"chain start: {ok} backend={ch.state()['dtlnBackend']} "
          f"ns={ch.state()['ns']['backend']} err={ch.state()['error']!r}")
    out = []
    for i in range(0, len(mic), FRAME_SIZE):
        out.append(ch.process_frame(mic[i:i + FRAME_SIZE],
                                    ref[i:i + FRAME_SIZE]))
    out = np.concatenate(out)
    assert np.all(np.isfinite(out.astype(np.float32)))
    print(f"frames={ch.state()['frames']} out_rms={np.sqrt(np.mean(out.astype(float)**2)):.1f} "
          f"peak={np.abs(out).max()} ns_dropped={ch.state()['ns']['dropped']}")
    if not ok:
        print("SMOKE OK (fail-open passthrough, models missing?)")
        return 2

    # Every selectable engine: 4 s synth pair, finite output, sane
    # state. Missing backends SKIP (fail-open design), errors FAIL.
    mic4, ref4 = _synth_pair(4)
    for _label, name in PROFILES:
        if name == "dtln":
            continue
        if not engine_available(name):
            print(f"engine {name}: SKIP (backend missing)")
            continue
        ec = Chain()
        assert ec.set_engine(name), f"{name}: set_engine refused"
        assert ec.start(), f"{name}: start failed: {ec.last_error}"
        # Hot-swap mid-run (server swaps live without stream
        # reconfigure): must not crash or mute.
        o = []
        for i in range(0, len(mic4), FRAME_SIZE):
            if i == len(mic4) // 2:
                assert ec.set_engine("dtln"), "swap back to dtln failed"
                assert ec.set_engine(name), f"swap back to {name} failed"
            o.append(ec.process_frame(mic4[i:i + FRAME_SIZE],
                                      ref4[i:i + FRAME_SIZE]))
        o = np.concatenate(o).astype(np.float32)
        assert np.all(np.isfinite(o)), f"{name}: non-finite output"
        st = ec.state()
        extra = ""
        if name == "nkf" and st["nkf"]:
            k = st["nkf"]
            extra = (f" lag={k['lagSamples']} lock={k['locked']} "
                     f"exposed={k['exposed']} resets={k['guardResets']} "
                     f"giveup={k['giveUp']} bs={k['backstopDb']:.1f}dB")
            assert k["giveUp"] == 0, f"{name}: failed open on synth"
            assert k["guardResets"] == 0, f"{name}: guard tripped on synth"
            assert abs(k["backstopDb"]) < 0.5, f"{name}: backstop trimmed voice"
        print(f"engine {name}: PASS frames={st['frames']}{extra}")
        ec.stop()
    print("SMOKE OK")
    return 0


def main():
    from .chain import Chain

    if len(sys.argv) == 2 and sys.argv[1] == "--smoke":
        return smoke()
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    mic, ref, outp = read_wav16(sys.argv[1]), read_wav16(sys.argv[2]), sys.argv[3]
    n = min(len(mic), len(ref))
    mic, ref = mic[:n], ref[:n]
    ch = Chain()
    if not ch.start():
        print(f"chain failed to start: {ch.last_error}")
        return 2
    from .dsp import FRAME_SIZE
    out = []
    for i in range(0, n, FRAME_SIZE):
        m, r = mic[i:i + FRAME_SIZE], ref[i:i + FRAME_SIZE]
        if len(m) < FRAME_SIZE:  # zero-pad the tail like a final pump frame
            m = np.pad(m, (0, FRAME_SIZE - len(m)))
            r = np.pad(r, (0, FRAME_SIZE - len(r)))
        out.append(ch.process_frame(m, r))
    out = np.concatenate(out)[:n]
    write_wav16(outp, out)
    print(f"wrote {outp}: rms={np.sqrt(np.mean(out.astype(float)**2)):.1f} "
          f"peak={np.abs(out).max()} frames={ch.state()['frames']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
