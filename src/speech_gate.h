#pragma once
#include <cmath>

// ============================================================
//  Near-end speech gate for the post stages (WPE + adaptive notch)
// ============================================================
// Header-only so the Linux harnesses exercise the exact code the
// audio callback runs.
//
// One RMS-hysteresis detector on the ENGINE output feeds two flags:
//
//   forWpe   — raw voice flag: tight WPE predictor bound while
//              talking. Never force-released (voice protection first).
//
//   forNotch — adaptation gate for the feedback-suppression notch.
//              Two complementary stuck detectors, both fed by the ON
//              flag (hysteresis: ema > kOn starts, ema < kOff ends):
//
//              1. continuous run: ON without a single release for
//                 kRunMs (4.5 s) — a howl never pauses. Fast and
//                 exact for the classic sustained tone.
//
//              2. burst evidence: +1 ms per ON frame, decayed with
//                 kGapTauMs (700 ms) while quiet, trip at kBurstMs
//                 (6.8 s). The old continuous counter re-armed from
//                 scratch on every gap, so ring-under-voice that
//                 starts/stops (Discord mic-test playback) never
//                 released the notch. The EMA's ~0.8 s release lag
//                 counts as ON, so gaps drain hard; ordinary
//                 conversation (short utterances + real pauses) peaks
//                 near 6.0 s and stays under the threshold. Long
//                 silence drains evidence to 0 and re-arms.
//
// Safe even while the notch adapts during a "stuck" stretch:
// notch.cpp engage-gating only latches a section after >45% of its
// input energy is demonstrably removed (~200 ms). Voice is
// broadband — a 60 Hz cut removes a tiny fraction — so voice can
// never latch a cut; unengaged sections are an exact bypass.
//
// Attack ~20 ms, release ~300 ms. Thresholds are int16-domain RMS
// of the engine output (-30 dBFS on / -36 dBFS off); the detector
// never looks at the stages' own output, so a notch cut can't flip
// the gate that froze it.

struct SpeechGate {
    float ema        = 0.0f;
    bool  on         = true;   // speech-safe default (matches stage defaults)
    bool  stuck      = false;  // sustained-loudness override active
    float runMs      = 0.0f;   // continuous-ON time (reset on release)
    float evidenceMs = 0.0f;   // burst evidence (decays while quiet)
};

// Update for one frame; frameDurMs is the audio frame duration
// (10 ms in this app — derived from fs/sr so rates stay correct).
inline void SpeechGateUpdate(SpeechGate* g, float frameRms, float frameDurMs) {
    const float kAtk = 1.0f - expf(-frameDurMs / 20.0f);    // ~0.39 @ 10 ms
    const float kRel = 1.0f - expf(-frameDurMs / 300.0f);   // ~0.033 @ 10 ms
    const float kOn  = 500.0f;   // int16 RMS: -30 dBFS
    const float kOff = 250.0f;   // -36 dBFS (hiss floor sits below)
    const float kRunMs    = 4500.0f;  // continuous ON to trip
    const float kBurstMs  = 6800.0f;  // burst evidence to trip
    const float kGapTauMs = 700.0f;   // evidence drain time constant

    g->ema += (frameRms > g->ema ? kAtk : kRel) * (frameRms - g->ema);

    if (g->on) {
        if (g->ema < kOff) g->on = false;      // a real gap: release
    } else if (g->ema > kOn) {
        g->on = true;                          // fresh onset
    }

    if (g->on) {
        g->runMs += frameDurMs;
        g->evidenceMs += frameDurMs;
    } else {
        g->runMs = 0.0f;
        g->evidenceMs *= expf(-frameDurMs / kGapTauMs);
    }
    g->stuck = g->runMs >= kRunMs || g->evidenceMs >= kBurstMs;
}

// Flags for this frame (1 = asserted).
inline int SpeechGateForWpe(const SpeechGate* g)   { return g->on ? 1 : 0; }
inline int SpeechGateForNotch(const SpeechGate* g) {
    return (g->on && !g->stuck) ? 1 : 0;
}
