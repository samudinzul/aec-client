"""Desktop-vs-web NKF parity proof (offline, no hardware).

Usage (from repo root):
    python -m web.test_parity --constants
    python -m web.test_parity --run mic.wav ref.wav desk_out.wav [web_out.wav]
    python -m web.test_parity --run mic.wav ref.wav desk_out.wav [web_out.wav] --engine dtln|aec3|nkf

Two independent answers to "how do I know the builds mirror each other":

1. --constants: every load-bearing tuned constant is extracted from
   BOTH implementations (src/nkf_wrapper.cpp + NKFImpl.h vs web/nkf.py;
   DTLN geometry src/dtln_wrapper.cpp vs web/dsp.py) and compared. Any
   drift fails loudly with the exact name. AEC3 has no tunable
   constants — both sides run stock WebRTC (echo canceller on,
   mobile off, HP on, gains/NS off), so it is covered by --run only.

2. --run: the web engine processes mic/ref through the same pump
   the desktop smoke uses and the outputs are compared (see --run
   docstring below). --engine picks nkf (default), dtln, or aec3;
   missing backends SKIP gracefully (fail-open design). NOTE: the
   desktop only ships an offline runner for NKF (nkf_smoke.exe) — a
   desktop dtln/aec3 out.wav has no producer yet, so cross-side
   --run comparison is NKF-only until a desktop dtln/aec3 harness
   exists. The web side of all three runs regardless.
"""

import re
import sys
import wave

CPP = "src/nkf_wrapper.cpp"
IMPL_H = "third_party/REAL_TIME_NKF_AEC/c/NKFImpl.h"
PY = "web/nkf.py"
DTLN_CPP = "src/dtln_wrapper.cpp"
DTLN_PY = "web/dsp.py"

# (label, [(file, regex-with-one-capture-group), ...]) — one entry per
# side; values compare as floats.
CONSTANTS = [
    ("NKF_BLOCK_SHIFT", [(CPP, r"NKF_BLOCK_SHIFT\s*=\s*([0-9]+)"),
                         (PY, r"^BLOCK_SHIFT\s*=\s*([0-9]+)")]),
    ("TDC_DMAX", [(CPP, r"TDC_DMAX\s*=\s*([0-9]+)"),
                  (PY, r"^TDC_DMAX\s*=\s*([0-9]+)")]),
    ("TDC_WIN", [(CPP, r"TDC_WIN\s*=\s*([0-9]+)"),
                 (PY, r"^TDC_WIN\s*=\s*([0-9]+)")]),
    ("TDC_DEC", [(CPP, r"TDC_DEC\s*=\s*([0-9]+)"),
                 (PY, r"^TDC_DEC\s*=\s*([0-9]+)")]),
    ("TDC_EAGER", [(CPP, r"TDC_EAGER\s*=\s*([0-9]+)"),
                   (PY, r"^TDC_EAGER\s*=\s*([0-9]+)")]),
    ("TDC_PERIOD", [(CPP, r"TDC_PERIOD\s*=\s*([0-9]+)"),
                    (PY, r"^TDC_PERIOD\s*=\s*([0-9]+)")]),
    ("TDC_LOCK_GRACE", [(CPP, r"TDC_LOCK_GRACE\s*=\s*([0-9]+)"),
                        (PY, r"^TDC_LOCK_GRACE\s*=\s*([0-9]+)")]),
    ("TDC_MIN_PEAK", [(CPP, r"TDC_MIN_PEAK\s*=\s*([0-9.]+)"),
                      (PY, r"^TDC_MIN_PEAK\s*=\s*([0-9.]+)")]),
    ("TDC_JUMP_PEAK", [(CPP, r"TDC_JUMP_PEAK\s*=\s*([0-9.]+)"),
                       (PY, r"^TDC_JUMP_PEAK\s*=\s*([0-9.]+)")]),
    ("TDC_JUMP", [(CPP, r"TDC_JUMP\s*=\s*([0-9]+)"),
                  (PY, r"^TDC_JUMP\s*=\s*([0-9]+)")]),
    ("TDC_SLEW", [(CPP, r"TDC_SLEW\s*=\s*([0-9]+)"),
                  (PY, r"^TDC_SLEW\s*=\s*([0-9]+)")]),
    ("TDC_TIE_EPS", [(CPP, r"TDC_TIE_EPS\s*=\s*([0-9.]+)"),
                     (PY, r"^TDC_TIE_EPS\s*=\s*([0-9.]+)")]),
    ("TDC_FINE", [(CPP, r"TDC_FINE\s*=\s*([0-9]+)"),
                  (PY, r"^TDC_FINE\s*=\s*([0-9]+)")]),
    ("TDC_MIN_MEAN_E", [(CPP, r"TDC_MIN_MEAN_E\s*=\s*([0-9.e-]+)"),
                        (PY, r"^TDC_MIN_MEAN_E\s*=\s*([0-9.e-]+)")]),
    ("TDC_SHADOW", [(CPP, r"TDC_SHADOW\s*=\s*([0-9]+)"),
                    (PY, r"^TDC_SHADOW\s*=\s*([0-9]+)")]),
    ("TDC_FADE", [(CPP, r"TDC_FADE\s*=\s*([0-9]+)"),
                  (PY, r"^TDC_FADE\s*=\s*([0-9]+)")]),
    ("ENG_DIV", [(CPP, r"ENG_IN_SCALE\s*=\s*1\.0f\s*/\s*([0-9.]+)"),
                 (PY, r"^ENG_IN_SCALE\s*=\s*1\.0\s*/\s*([0-9.]+)")]),
    ("LOOP_WIN", [(CPP, r"LOOP_WIN\s*=\s*([0-9]+)"),
                  (PY, r"^LOOP_WIN\s*=\s*([0-9]+)")]),
    ("LOOP_DMAX", [(CPP, r"LOOP_DMAX\s*=\s*([0-9]+)"),
                   (PY, r"^LOOP_DMAX\s*=\s*([0-9]+)")]),
    ("LOOP_DEC", [(CPP, r"LOOP_DEC\s*=\s*([0-9]+)"),
                  (PY, r"^LOOP_DEC\s*=\s*([0-9]+)")]),
    ("LOOP_PERIOD", [(CPP, r"LOOP_PERIOD\s*=\s*([0-9]+)"),
                     (PY, r"^LOOP_PERIOD\s*=\s*([0-9]+)")]),
    ("LOOP_MIN_PEAK", [(CPP, r"LOOP_MIN_PEAK\s*=\s*([0-9.]+)"),
                       (PY, r"^LOOP_MIN_PEAK\s*=\s*([0-9.]+)")]),
    ("LOOP_ON", [(CPP, r"LOOP_ON\s*=\s*([0-9]+)"),
                 (PY, r"^LOOP_ON\s*=\s*([0-9]+)")]),
    ("LOOP_QUIET", [(CPP, r"LOOP_QUIET\s*=\s*([0-9]+)"),
                    (PY, r"^LOOP_QUIET\s*=\s*([0-9]+)")]),
    ("LOOP_TRIM", [(CPP, r"LOOP_TRIM\s*=\s*([0-9.]+)"),
                   (PY, r"^LOOP_TRIM\s*=\s*([0-9.]+)")]),
    ("LOOP_TRIM_DEEP", [(CPP, r"LOOP_TRIM_DEEP\s*=\s*([0-9.]+)"),
                        (PY, r"^LOOP_TRIM_DEEP\s*=\s*([0-9.]+)")]),
    ("LOOP_TRIM_FLOOR", [(CPP, r"LOOP_TRIM_FLOOR\s*=\s*([0-9.]+)"),
                         (PY, r"^LOOP_TRIM_FLOOR\s*=\s*([0-9.]+)")]),
    ("LOOP_TAU_ATK", [(CPP, r"LOOP_TAU_ATK\s*=\s*([0-9.]+)"),
                      (PY, r"^LOOP_TAU_ATK\s*=\s*([0-9.]+)")]),
    ("LOOP_TAU_REL", [(CPP, r"LOOP_TAU_REL\s*=\s*([0-9.]+)"),
                      (PY, r"^LOOP_TAU_REL\s*=\s*([0-9.]+)")]),
    ("LOOP_LOUD_BLOCKS", [(CPP, r"LOOP_LOUD_BLOCKS\s*=\s*([0-9]+)"),
                          (PY, r"^LOOP_LOUD_BLOCKS\s*=\s*([0-9]+)")]),
    ("NOCANCEL_HOT_AFTER", [(CPP, r"NOCANCEL_HOT_AFTER\s*=\s*([0-9]+)"),
                            (PY, r"^NOCANCEL_HOT_AFTER\s*=\s*([0-9]+)")]),
    ("NOCANCEL_ECHO_RATIO", [(CPP, r"NOCANCEL_ECHO_RATIO\s*=\s*([0-9.]+)"),
                             (PY, r"^NOCANCEL_ECHO_RATIO\s*=\s*([0-9.]+)")]),
    ("RAIL_MS", [(CPP, r"RAIL_MS\s*=\s*([0-9.]+)"),
                 (PY, r"^RAIL_MS\s*=\s*([0-9.]+)")]),
    ("RAIL_BLOCKS", [(CPP, r"RAIL_BLOCKS\s*=\s*([A-Z_]+)"),
                     (PY, r"^RAIL_BLOCKS\s*=\s*([A-Z_]+)")]),
    ("RAIL_QUIET_TOL", [(CPP, r"RAIL_QUIET_TOL\s*=\s*([0-9]+)"),
                        (PY, r"^RAIL_QUIET_TOL\s*=\s*([0-9]+)")]),
    ("BS_BIN_RUN", [(CPP, r"BS_BIN_RUN\s*=\s*([0-9]+)"),
                    (PY, r"^BS_BIN_RUN\s*=\s*([0-9]+)")]),
    ("BS_FRAMES", [(CPP, r"BS_FRAMES\s*=\s*([0-9]+)"),
                   (PY, r"^BS_FRAMES\s*=\s*([0-9]+)")]),
    ("BS_RUN", [(CPP, r"BS_RUN\s*=\s*([0-9]+)"),
                (PY, r"^BS_RUN\s*=\s*([0-9]+)")]),
    ("BS_HEAL_RUN", [(CPP, r"BS_HEAL_RUN\s*=\s*([0-9]+)"),
                     (PY, r"^BS_HEAL_RUN\s*=\s*([0-9]+)")]),
    ("BS_TONAL", [(CPP, r"BS_TONAL\s*=\s*([0-9.]+)"),
                  (PY, r"^BS_TONAL\s*=\s*([0-9.]+)")]),
    ("BS_HITS", [(CPP, r"BS_HITS\s*=\s*([0-9]+)"),
                 (PY, r"^BS_HITS\s*=\s*([0-9]+)")]),
    ("BS_MIC_MS", [(CPP, r"BS_MIC_MS\s*=\s*([0-9.e-]+)"),
                   (PY, r"^BS_MIC_MS\s*=\s*([0-9.e-]+)")]),
    ("BS_HEAL_D", [(CPP, r"BS_HEAL_D\s*=\s*(-?[0-9.]+)"),
                   (PY, r"^BS_HEAL_D\s*=\s*(-?[0-9.]+)")]),
    ("NOTCH_Q_BW", [(CPP, r"NOTCH_Q_BW\s*=\s*([0-9.]+)"),
                    (PY, r"^NOTCH_Q_BW\s*=\s*([0-9.]+)")]),
    ("NOTCH_SLEW", [(CPP, r"NOTCH_SLEW\s*=\s*([0-9.]+)"),
                    (PY, r"^NOTCH_SLEW\s*=\s*([0-9.]+)")]),
    ("NOTCH_RAMP_BLOCKS", [(CPP, r"NOTCH_RAMP_BLOCKS\s*=\s*([0-9]+)"),
                           (PY, r"^NOTCH_RAMP_BLOCKS\s*=\s*([0-9]+)")]),
    ("NOTCH_FMIN", [(CPP, r"NOTCH_FMIN\s*=\s*([0-9.]+)"),
                    (PY, r"^NOTCH_FMIN\s*=\s*([0-9.]+)")]),
    ("NOTCH_FMAX", [(CPP, r"NOTCH_FMAX\s*=\s*([0-9.]+)"),
                    (PY, r"^NOTCH_FMAX\s*=\s*([0-9.]+)")]),
    ("GUARD_HOT_BLOCKS", [(CPP, r"GUARD_HOT_BLOCKS\s*=\s*([0-9]+)"),
                          (PY, r"^GUARD_HOT_BLOCKS\s*=\s*([0-9]+)")]),
    ("GUARD_EMA", [(CPP, r"GUARD_EMA\s*=\s*([0-9.]+)"),
                   (PY, r"^GUARD_EMA\s*=\s*([0-9.]+)")]),
    ("GUARD_HOT_MEAN_E", [(CPP, r"GUARD_HOT_MEAN_E\s*=\s*([0-9.]+)"),
                          (PY, r"^GUARD_HOT_MEAN_E\s*=\s*([0-9.]+)")]),
    ("GUARD_RATIO", [(CPP, r"GUARD_RATIO\s*=\s*([0-9.]+)"),
                    (PY, r"^GUARD_RATIO\s*=\s*([0-9.]+)")]),
    ("GUARD_CLIP_MEAN_E", [(CPP, r"GUARD_CLIP_MEAN_E\s*=\s*([0-9.]+)"),
                           (PY, r"^GUARD_CLIP_MEAN_E\s*=\s*([0-9.]+)")]),
    ("GUARD_MAX_RESETS", [(CPP, r"GUARD_MAX_RESETS\s*=\s*([0-9]+)"),
                          (PY, r"^GUARD_MAX_RESETS\s*=\s*([0-9]+)")]),
    ("RES_EMA", [(IMPL_H, r"RES_EMA\s+\(?([0-9.]+)"),
                 (PY, r"^RES_EMA\s*=\s*([0-9.]+)")]),
    ("RES_THRESH", [(IMPL_H, r"RES_THRESH\s+\(?([0-9.]+)"),
                    (PY, r"^RES_THRESH\s*=\s*([0-9.]+)")]),
    ("RES_FLOOR", [(IMPL_H, r"RES_FLOOR\s+\(?([0-9.]+)"),
                   (PY, r"^RES_FLOOR\s*=\s*([0-9.]+)")]),
    ("RES_RELEASE", [(IMPL_H, r"RES_RELEASE\s+\(?([0-9.]+)"),
                     (PY, r"^RES_RELEASE\s*=\s*([0-9.]+)")]),
    ("NKF_BLOCK", [(IMPL_H, r"BLOCK_LEN\s+\(?([0-9]+)"),
                   (PY, r"^BLOCK_LEN\s*=\s*([0-9]+)")]),
    ("NKF_SHIFT", [(IMPL_H, r"BLOCK_SHIFT\s+\(?([0-9]+)"),
                   (PY, r"^BLOCK_SHIFT\s*=\s*([0-9]+)")]),
    ("NKF_BINS", [(IMPL_H, r"FFT_OUT_SIZE\s+\(?([0-9]+)"),
                  (PY, r"^FFT_OUT_SIZE\s*=\s*([0-9]+)")]),
    ("NKF_LEN", [(IMPL_H, r"NKF_LEN\s+\(?([0-9]+)"),
                 (PY, r"^NKF_LEN\s*=\s*([0-9]+)")]),
    # Voice-path high-pass (src/main.cpp HpfInit/HpfProcess vs
    # web/chain.py HPF_FC/HPF_Q + Direct Form I): same fc/Q.
    ("HPF_FC", [("src/main.cpp", r"HPF_FC\s*=\s*([0-9.]+)") ,
                ("web/chain.py", r"^HPF_FC\s*=\s*([0-9.]+)")]),
    ("HPF_Q", [("src/main.cpp", r"HPF_Q\s*=\s*([0-9.]+)"),
               ("web/chain.py", r"^HPF_Q\s*=\s*([0-9.]+)")]),
    ("EXPAND_OPEN_RMS", [("src/main.cpp", r"EXPAND_OPEN_RMS\s*=\s*([0-9.]+)"),
                          ("web/chain.py", r"^EXPAND_OPEN_RMS\s*=\s*([0-9.]+)")]),
    ("EXPAND_CLOSE_RMS", [("src/main.cpp", r"EXPAND_CLOSE_RMS\s*=\s*([0-9.]+)"),
                          ("web/chain.py", r"^EXPAND_CLOSE_RMS\s*=\s*([0-9.]+)")]),
    ("EXPAND_HANG_MS", [("src/main.cpp", r"EXPAND_HANG_MS\s*=\s*([0-9]+)"),
                        ("web/chain.py", r"^EXPAND_HANG_MS\s*=\s*([0-9]+)")]),
    ("PRES_FC", [("src/main.cpp", r"PRES_FC\s*=\s*([0-9.]+)"),
                 ("web/chain.py", r"^PRES_FC\s*=\s*([0-9.]+)")]),
    ("PRES_GAIN_DB", [("src/main.cpp", r"PRES_GAIN_DB\s*=\s*([0-9.]+)"),
                      ("web/chain.py", r"^PRES_GAIN_DB\s*=\s*([0-9.]+)")]),
    # DTLN geometry (block/shift/bins/sample-rate contract shared by
    # src/dtln_wrapper.cpp and web/dsp.py; the DTLN pair has no tuned
    # DSP constants beyond these).
    ("DTLN_BLOCK", [(DTLN_CPP, r"DTLN_BLOCK_LEN\s+([0-9]+)"),
                    (DTLN_PY, r"^BLOCK_LEN\s*=\s*([0-9]+)")]),
    ("DTLN_SHIFT", [(DTLN_CPP, r"DTLN_BLOCK_SHIFT\s+([0-9]+)"),
                    (DTLN_PY, r"^BLOCK_SHIFT\s*=\s*([0-9]+)")]),
    ("DTLN_BINS", [(DTLN_CPP, r"DTLN_BINS\s+([0-9]+)"),
                   (DTLN_PY, r"^BINS\s*=\s*([0-9]+)")]),
]

# Symbolic aliases resolve to the constant they name (one level).
ALIASES = {"LOOP_LOUD_BLOCKS"}


def _read(path):
    with open(path) as f:
        return f.read()


def check_constants():
    paths = set()
    for _, sides in CONSTANTS:
        for path, _ in sides:
            paths.add(path)
    files = {}
    for path in sorted(paths):
        try:
            files[path] = _read(path)
        except OSError as e:
            print(f"SKIP (cannot read {path}: {e})")
            return 2
    # Side tags for the report line (pair entries keep the legacy
    # cpp/py shape; triple entries add the exp side).
    _TAG = {"src/main.cpp": "cpp", "web/chain.py": "py",
            "web/dsp.py": "py", "tools/nkf_smoke.cpp": "cpp"}
    fails = 0
    for label, sides in CONSTANTS:
        vals = []
        for path, rx in sides:
            m = re.search(rx, files[path], re.M)
            if not m:
                print(f"FAIL {label}: pattern not found in {path}")
                fails += 1
                break
            vals.append(m.group(1))
        else:
            first = vals[0]
            if first in ALIASES:
                first = _resolve_alias(files, sides[0][0], first)
            ok = True
            parts = []
            for (path, _), v in zip(sides, vals):
                if v in ALIASES:
                    v = _resolve_alias(files, path, v)
                parts.append(f"{_TAG.get(path, path.split('/')[-1])}={v}")
                try:
                    if float(v) != float(first):
                        ok = False
                except ValueError:
                    if v != first:
                        ok = False
            print(f"{'ok' if ok else 'FAIL'}  {label:20s} "
                  + " ".join(parts))
            if not ok:
                fails += 1
    print("CONSTANTS: " + ("ALL MATCH" if fails == 0
                            else f"{fails} MISMATCHES"))
    return 0 if fails == 0 else 1


def _resolve_alias(files, path, name):
    m = re.search(r"^" + name + r"\s*=\s*([0-9]+)",
                  files[path], re.M)
    if m:
        return m.group(1)
    m = re.search(r"\b" + name + r"\s+\(?([0-9]+)", files[path])
    if m:
        return m.group(1)
    m = re.search(r"\b" + name + r"\s*=\s*([0-9]+)", files[path])
    return m.group(1) if m else name


def _read_wav16(path):
    import numpy as np
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000, f"{path}: need 16 kHz"
        assert w.getnchannels() == 1, f"{path}: need mono"
        assert w.getsampwidth() == 2, f"{path}: need int16"
        raw = w.readframes(w.getnframes())
    return np.frombuffer(raw, dtype=np.int16).copy()


def _write_wav16(path, data):
    import numpy as np
    with wave.open(path, "wb") as w:
        w.setframerate(16000)
        w.setnchannels(1)
        w.setsampwidth(2)
        w.writeframes(np.asarray(data, dtype=np.int16).tobytes())


def _make_engine(name):
    """(engine, ready, note) — missing backends return ready=False."""
    if name == "nkf":
        from .nkf import NkfEngine
        eng = NkfEngine()
        return eng, eng.ready, eng.last_error
    if name == "dtln":
        from .dtln import DtlnAec
        eng = DtlnAec()
        note = "" if eng.ready else eng.last_error
        return eng, eng.ready, note
    if name == "aec3":
        from . import aec3
        if not aec3.available():
            return None, False, "pywebrtc-audio not installed"
        eng = aec3.Aec3Engine()
        return eng, eng.ready, eng.last_error
    return None, False, f"unknown engine: {name}"


def _telemetry(name, eng):
    if name == "nkf":
        st = eng.get_state()
        return (f"lock={st['locked']} resets={st['guardResets']} "
                f"giveup={st['giveUp']} notch={st['notchFreq']:.0f}Hz "
                f"loopDb={st['loopDb']:.1f} resDb={st['resDb']:.1f}")
    if name == "dtln":
        return f"backend={eng.backend}"
    return "ready"


def run_pair(mic_path, ref_path, desk_path, web_path, engine):
    import numpy as np
    from .dsp import FRAME_SIZE
    mic = _read_wav16(mic_path)
    ref = _read_wav16(ref_path)
    desk = _read_wav16(desk_path).astype(np.float32)
    n = min(len(mic), len(ref))
    mic, ref = mic[:n], ref[:n]
    eng, ready, note = _make_engine(engine)
    if not ready:
        print(f"SKIP (web {engine} unavailable: {note})")
        return 2
    out = np.concatenate([eng.process(mic[i:i + FRAME_SIZE],
                                      ref[i:i + FRAME_SIZE])
                          for i in range(0, n, FRAME_SIZE)])
    _write_wav16(web_path, out)
    web = out.astype(np.float32)
    print(f"web[{engine}]: {_telemetry(engine, eng)}")
    # Lag-compensated comparison (block timelines may differ by a
    # constant delay): align by cross-correlation, then compare.
    m = min(len(web), len(desk))
    web, desk = web[:m], desk[:m]
    xc = np.correlate(web - web.mean(), desk - desk.mean(), mode="full")
    lag = int(np.argmax(xc)) - (m - 1)
    a = max(0, lag)
    b = m - max(0, -lag)
    w2, d2 = web[a:b], desk[a - lag:b - lag]
    corr = float(np.dot(w2, d2) / np.sqrt(np.dot(w2, w2) * np.dot(d2, d2)
                                         + 1e-30))
    rms_w = float(np.sqrt(np.mean(w2 ** 2)))
    rms_d = float(np.sqrt(np.mean(d2 ** 2) + 1e-30))
    peak_w = float(np.abs(w2).max())
    peak_d = float(np.abs(d2).max())
    print(f"align lag={lag} corr={corr:.4f} "
          f"rms web/deskt={rms_w:.0f}/{rms_d:.0f} "
          f"peak web/deskt={peak_w:.0f}/{peak_d:.0f}")
    ok = corr >= 0.90 and 0.5 <= rms_w / rms_d <= 2.0
    print("PARITY: " + ("MATCH (mirrored)" if ok else
                         "DIVERGED (investigate)"))
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if args[:1] == ["--constants"]:
        return check_constants()
    if args[:1] != ["--run"]:
        print(__doc__)
        return 2
    rest = args[1:]
    engine = "nkf"
    if "--engine" in rest:
        i = rest.index("--engine")
        try:
            engine = rest[i + 1]
        except IndexError:
            print("error: --engine needs nkf|dtln|aec3")
            return 1
        del rest[i:i + 2]
    if len(rest) not in (3, 4):
        print(__doc__)
        return 2
    mic, ref, desk = rest[:3]
    webp = rest[3] if len(rest) == 4 else "parity_web_out.wav"
    return run_pair(mic, ref, desk, webp, engine)


if __name__ == "__main__":
    sys.exit(main())
