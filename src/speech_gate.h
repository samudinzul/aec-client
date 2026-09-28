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
//              Natural speech has gaps (EMA dips below kOff, flag
//              releases); a howl/ringing tone never does. If the
//              voice flag stays continuously true > kStuckFrames
//              (~5 s) without ever releasing, this is sustained
//              loudness, not speech — release the notch gate so
//              adaptation can track and latch the tone. A real gap
//              (EMA < kOff) re-arms the plain hysteresis.
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
    float ema      = 0.0f;
    bool  on       = true;   // speech-safe default (matches stage defaults)
    bool  stuck    = false;  // sustained-loudness override active
    int   onFrames = 0;      // continuous-ON frames since last release
};

// Update for one frame; frameDurMs is the audio frame duration
// (10 ms in this app — derived from fs/sr so rates stay correct).
inline void SpeechGateUpdate(SpeechGate* g, float frameRms, float frameDurMs) {
    const float kAtk = 1.0f - expf(-frameDurMs / 20.0f);    // ~0.39 @ 10 ms
    const float kRel = 1.0f - expf(-frameDurMs / 300.0f);   // ~0.033 @ 10 ms
    const float kOn  = 500.0f;   // int16 RMS: -30 dBFS
    const float kOff = 250.0f;   // -36 dBFS (hiss floor sits below)
    const int   kStuckFrames = (int)(5000.0f / frameDurMs); // ~5 s unbroken ON

    g->ema += (frameRms > g->ema ? kAtk : kRel) * (frameRms - g->ema);

    if (!g->on) {
        if (g->ema > kOn) {
            g->on = true;             // fresh onset
            g->onFrames = 0;
            g->stuck = false;
        }
        g->stuck = false;
    } else {
        if (g->ema < kOff) {
            g->on = false;            // a real gap: release + re-arm
            g->onFrames = 0;
            g->stuck = false;
        } else {
            // ON can only persist with ema >= kOff at every evaluation,
            // so continuous ON == sustained loudness.
            if (++g->onFrames >= kStuckFrames) g->stuck = true;
        }
    }
}

// Flags for this frame (1 = asserted).
inline int SpeechGateForWpe(const SpeechGate* g)   { return g->on ? 1 : 0; }
inline int SpeechGateForNotch(const SpeechGate* g) {
    return (g->on && !g->stuck) ? 1 : 0;
}
