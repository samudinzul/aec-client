"""NKF closed-loop regression tests (offline, no hardware).

Usage (from repo root):
    python -m web.test_nkf_loop

Simulates a self-monitor feedback loop the way Listen-to-myself /
a Discord mic test creates one: our own wire output returns through
the speakers into BOTH the mic (coupling gain G, delay Dm) and the
loopback ref (delay Dr). Schedule: 2 s clean (converge + lock),
7 s looped at coupling 1.3 (loop gain > 1 without cancellation —
past what an unprotected loop survives), 3 s released (G = 0,
silent ref).

L1 no-blowout — wire stays finite and off full-scale the whole
   run, and the engine never gives up (no fail-open bailout).
L2 brakes-engage — the loop detector fires mid-loop AND the wire
   trim is active (loopDb < -1 dB): suppression, not disengagement.
L3 release — after the loop ends the detector clears and the trim
   lets go (|loopDb| < 0.5 dB): mic tests keep working.
L4 single-window attack — unit check on _backstop_window: a
   tonal-stable window attacks after ONE window while looped,
   versus two unlooped (same fields, loop_conf the only change).

Exit code: 0 = all pass, 1 = a failure.
"""

import sys

import numpy as np

from .nkf import LOOP_ON, NkfEngine

SR = 16000
FRAME = 128


def _run_closed_loop(coupling=1.3, d_mic=800, d_ref=800, hot=False):
    """Returns (wire_out, snapshots). Snapshots are get_state() dicts
    taken at 4 s (mid-loop), 9 s (late-loop) and at the end
    (post-release). hot=True drives the mic 3x into ADC clipping,
    like loud speakers + high mic gain in a real monitor loop."""
    eng = NkfEngine()
    assert eng.ready, f"NKF backend unavailable: {eng.last_error}"
    n = SR * 12
    t = np.arange(n) / SR
    near = (((t % 2.0) < 0.5).astype(np.float32)
            * (0.2 * np.sin(2 * np.pi * 220 * t)).astype(np.float32))
    wire = np.zeros(n, np.float32)  # what the speakers (will) play
    snaps = {}
    for i in range(0, n, FRAME):
        sec = i / SR
        g = coupling if 2.0 <= sec < 9.0 else 0.0
        live_ref = sec >= 9.0  # release phase: silent ref
        m = near[i:i + FRAME].copy()
        r = np.zeros(FRAME, np.float32)
        for j in range(FRAME):
            t_out = i + j - d_mic
            if g and t_out >= 0:
                m[j] += g * wire[t_out]
            # Loopback hears the speakers: our own wire output
            # (pre-loop the wire carries near-end leakage — exactly
            # what a mic test feeds back).
            t_ref = i + j - d_ref
            r[j] = 0.0 if (live_ref or t_ref < 0) else wire[t_ref]
        mi = m * 32767.0
        if hot:
            mi = mi * 3.0
        o = eng.process(np.clip(mi, -32768, 32767).astype(np.int16),
                        (np.clip(r, -1, 1) * 32767).astype(np.int16))
        wire[i:i + FRAME] = o.astype(np.float32) / 32768.0
        if i == 4 * SR:
            snaps["mid"] = eng.get_state()
        if i == 9 * SR:
            snaps["late"] = eng.get_state()
    snaps["end"] = eng.get_state()
    return wire, snaps


def l1_no_blowout(wire, snaps):
    ok = bool(np.all(np.isfinite(wire)))
    peak = float(np.abs(wire).max())
    ok = ok and peak < 0.999 and snaps["end"]["giveUp"] == 0
    print(f"L1 no-blowout: finite={bool(np.all(np.isfinite(wire)))} "
          f"peak={peak:.3f} giveup={snaps['end']['giveUp']} "
          f"({'PASS' if ok else 'FAIL'})")
    return ok


def l2_brakes_engage(snaps):
    mid = snaps["mid"]
    ok = mid["loopActive"] == 1 and mid["loopDb"] < -1.0
    print(f"L2 brakes-engage: loopActive={mid['loopActive']} "
          f"loopDb={mid['loopDb']:.1f}dB exposed={mid['exposed']} "
          f"({'PASS' if ok else 'FAIL: want loop=1, trim active'})")
    return bool(ok)


def l3_release(snaps):
    end = snaps["end"]
    ok = end["loopActive"] == 0 and abs(end["loopDb"]) < 0.5
    print(f"L3 release: loopActive={end['loopActive']} "
          f"loopDb={end['loopDb']:.2f}dB "
          f"({'PASS' if ok else 'FAIL: want loop=0, trim released'})")
    return bool(ok)


def l4_single_window_attack():
    import web.nkf as nkf_mod
    results = []
    for looped, want_target in ((True, True), (False, False)):
        eng = NkfEngine()
        eng.loop_conf = LOOP_ON if looped else 0
        eng.bs_hits = 64
        eng.bs_frames = 64
        eng.bs_bin_best = 45
        eng.bs_bin_dom = 7
        eng.dep_mic = 6e-3 * 64
        eng.dep_out = 5e-4 * 64
        eng.dep_samples = 64 * 512
        eng._backstop_window()
        attacked = eng.bs_target < 1.0
        results.append(attacked == want_target)
        print(f"L4 {'looped' if looped else 'unlooped'}: "
              f"bsTarget={eng.bs_target:.4f} "
              f"({'PASS' if attacked == want_target else 'FAIL'})")
    assert nkf_mod.BS_RUN == 2  # pin the asymmetry this test relies on
    return all(results)


def l5_giveup_still_braked():
    """Worst case: engine failed open (permanent mic passthrough) AND
    looped. The Kalman core is out, but the wire brakes — loop trim,
    backstop watch, loop detector — run on the emit path regardless
    of give-up, so the loop must still stay bounded and release."""
    import os
    os.environ["NKF_FORCE_GIVEUP"] = "1"
    try:
        wire, snaps = _run_closed_loop()
    finally:
        del os.environ["NKF_FORCE_GIVEUP"]
    ok = bool(np.all(np.isfinite(wire)))
    peak = float(np.abs(wire).max())
    ok = (ok and peak < 0.999
          and snaps["mid"]["loopActive"] == 1
          and snaps["mid"]["loopDb"] < -1.0
          and snaps["end"]["loopActive"] == 0
          and abs(snaps["end"]["loopDb"]) < 0.5)
    print(f"L5 giveup-loop: finite={bool(np.all(np.isfinite(wire)))} "
          f"peak={peak:.3f} trim={snaps['mid']['loopDb']:.1f}dB "
          f"released={snaps['end']['loopActive'] == 0} "
          f"({'PASS' if ok else 'FAIL'})")
    return ok


def l6_escalation_kills_it():
    """V6 howl conditions (clipped, beyond-range delay, coupling 2):
    the trim must ESCALATE past -6 dB (marginal stability at -6
    sustains full-scale mush), stay finite, and release after."""
    wire, snaps = _run_closed_loop(coupling=2.0, d_mic=20000,
                                   d_ref=20000, hot=True)
    late = snaps["late"]
    ok = (late["loopDb"] < -9.0
          and bool(np.all(np.isfinite(wire)))
          and abs(snaps["end"]["loopDb"]) < 0.5)
    print(f"L6 escalation: loopDb={late['loopDb']:.1f}dB "
          f"peak={float(np.abs(wire).max()):.3f} "
          f"released={abs(snaps['end']['loopDb']) < 0.5} "
          f"({'PASS' if ok else 'FAIL: want escalated past -6dB'})")
    return bool(ok)


def l7_loud_voice_broadband_music():
    """False-positive guard: loud voice over broadband background
    (crowd-like, no loop) must NEVER trim — gaps and decorrelation
    keep every brake disengaged. (Pure-tone music + pure-tone voice
    can correlate and trim; same on desktop, accepted edge.)"""
    eng = NkfEngine()
    assert eng.ready
    sr, dur = 16000, 12
    t = np.arange(sr * dur) / sr
    voice = (((t % 2.0) < 0.5).astype(np.float32)
             * (0.35 * np.sin(2 * np.pi * 180 * t)).astype(np.float32))
    rng = np.random.default_rng(23)
    music = (0.09 * rng.standard_normal(len(t))).astype(np.float32)
    for i in range(0, len(t), 128):
        m = (np.clip(voice[i:i + 128], -1, 1) * 32767).astype(np.int16)
        r = (np.clip(music[i:i + 128], -1, 1) * 32767).astype(np.int16)
        eng.process(m, r)
    st = eng.get_state()
    # NOTE: loopDb is 20*log10(1.0 + 1e-9) ~= 8.7e-09 when the trim
    # never engages — compare with a band, never =="0.0".
    ok = (st["loopActive"] == 0 and st["nocancelHot"] == 0
          and abs(st["loopDb"]) < 0.5 and st["guardResets"] == 0)
    print(f"L7 no-false-trim: loop={st['loopActive']} "
          f"nocancel={st['nocancelHot']} loopDb={st['loopDb']:.2f} "
          f"({'PASS' if ok else 'FAIL'})")
    return bool(ok)


def main():
    if not NkfEngine().ready:
        print("SKIP (NKF backend unavailable)")
        return 0
    wire, snaps = _run_closed_loop()
    results = [l1_no_blowout(wire, snaps),
               l2_brakes_engage(snaps),
               l3_release(snaps),
               l4_single_window_attack(),
               l5_giveup_still_braked(),
               l6_escalation_kills_it(),
               l7_loud_voice_broadband_music()]
    print("LOOP TESTS: " + ("ALL PASS" if all(results) else "FAILURES PRESENT"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
