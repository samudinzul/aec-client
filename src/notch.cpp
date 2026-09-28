// ============================================================
//  LMS adaptive notch filter — "Feedback suppression" post stage
//
//  Two cascaded second-order adaptive notches that track narrowband
//  howling/ringing loops (acoustic feedback) without touching the
//  rest of the voice band. Per-sample cost is a handful of multiplies
//  — negligible CPU, zero latency.
//
//  Per section (zeros on the unit circle at e^{+-jw}, poles at
//  r*e^{+-jw}, bandwidth ~60 Hz at both rates):
//      y = x - 2cos(w) x1 + x2 + 2r cos(w) y1 - r^2 y2
//  Frequency adapts by steepest descent on the output power using
//  the exact sensitivity signal s = dy/dw (Nehorai-style gradient,
//  EMA-smoothed so broadband noise averages to ~0 and only coherent
//  tones pull the notch):
//      s  = 2 sin(w) x1 - 2 r sin(w) y1 + a1 s1 + a2 s2
//      g' = EMA(y*s),  p' = EMA(s*s)
//      dw = -mu * g' / (p' + eps)
//
//  Voice safety (the release theme — nothing may sound unnatural):
//    - adaptation FREEZES while the near-end person talks
//      (NotchSetSpeech): voice harmonics never capture a notch;
//      the filter keeps applying at its parked frequencies.
//    - adaptation also pauses below a ~-60 dBFS silence floor
//      (no random walk on the noise floor).
//    - dw is slew-limited (8000 Hz/s) and w is clamped to
//      [150 Hz, 0.45*sr]: any input, however pathological, moves a
//      notch at a bounded rate into a bounded band.
//    - a 60 Hz cut parked on a vowel harmonic removes a single
//      narrow band: bounded, static coloration — never pumping.
//
//  Fail-open: state is finite by construction (clamps + slew), the
//  stage passes audio unconditionally.
// ============================================================
#include "notch.h"

#include <cmath>
#include <cstring>
#include <new>

namespace {

constexpr int    kSections        = 2;
constexpr double kPi              = 3.14159265358979323846;
constexpr double kBandwidthHz     = 60.0;      // notch width
constexpr double kMinHz           = 150.0;
constexpr double kInitHz0         = 500.0;     // spread start frequencies
constexpr double kInitHz1         = 1500.0;
constexpr double kSlewHzPerSec    = 8000.0;    // max track rate
constexpr double kMu              = 0.005;     // NLMS step: linear regime
                                                  // near equilibrium (0.05
                                                  // saturated the slew cap
                                                  // with EMA lag -> limit
                                                  // cycle ±150 Hz, null too
                                                  // shallow to latch)
constexpr double kEmaAlpha        = 0.9995;    // gradient smoothing (~64 ms*2)
constexpr double kBlockAlpha      = 0.967;     // power EMA per 10 ms block (~300 ms)
constexpr double kEngRatio        = 0.45;      // engage: >=45% energy removed
                                                  // (two equal tones =
                                                  // exactly 50%)
constexpr int    kEngBlocks       = 20;        // sustained ~200 ms to latch
constexpr double kDeadFrac        = 0.002;     // dead-zone: park within ~1.5 Hz
constexpr double kEps             = 1e-20;
constexpr double kFloorRms        = 1e-3;      // ~-60 dBFS: adapt above this

struct Section {
    double w  = 0.0;      // center frequency (rad/sample)
    double wInit = 0.0;   // startup frequency (telemetry)
    bool engaged = false; // latched: notch actually applied to output
    double pinE = 0.0, poutE = 0.0;   // block-power EMA (engage test)
    int engCount = 0;     // consecutive blocks over kEngRatio
    double r  = 0.0;      // pole radius (set from bandwidth)
    double b1 = 0.0;      // -2 cos(w)
    double a1 = 0.0;      // +2 r cos(w)
    double a2 = 0.0;      // -r^2
    double sw = 0.0;      // sin(w)
    double x1 = 0, x2 = 0;   // input history
    double y1 = 0, y2 = 0;   // output history
    double s1 = 0, s2 = 0;   // sensitivity history
    double emaG = 0.0;    // EMA of y*s
    double emaP = 0.0;    // EMA of s*s
};

}  // namespace

struct NotchHandle {
    int sr = 0;
    double wMin = 0.0, wMax = 0.0, dwMax = 0.0;
    Section sec[kSections];
    int speaking = 1;               // frozen until told otherwise
    bool failed = false;
};

static void SectionSetFreq(Section* s, double w) {
    s->w = w;
    s->sw = std::sin(w);
    const double cw = std::cos(w);
    s->b1 = -2.0 * cw;
    s->a1 = 2.0 * s->r * cw;
    s->a2 = -s->r * s->r;
}

static void SectionInit(Section* s, double sr, double hz) {
    // Pole radius for ~kBandwidthHz: BW ~= (1-r)*sr/pi  =>  r = 1 - pi*BW/sr.
    s->r = 1.0 - kPi * kBandwidthHz / sr;
    if (s->r < 0.5) s->r = 0.5;
    s->x1 = s->x2 = s->y1 = s->y2 = s->s1 = s->s2 = 0.0;
    s->emaG = s->emaP = 0.0;
    s->pinE = s->poutE = 0.0;
    s->engCount = 0;
    s->engaged = false;
    s->wInit = hz;
    SectionSetFreq(s, 2.0 * kPi * hz / sr);
}

extern "C" {

NotchHandle* NotchNew(int sampleRate) {
    if (sampleRate != 16000 && sampleRate != 48000) return nullptr;
    auto* h = new (std::nothrow) NotchHandle();
    if (!h) return nullptr;
    h->sr = sampleRate;
    h->wMin = 2.0 * kPi * kMinHz / sampleRate;
    h->wMax = 2.0 * kPi * 0.45 * sampleRate / sampleRate;
    // Per-sample slew for kSlewHzPerSec: dw = 2*pi*(slew/sr)/sr.
    h->dwMax = 2.0 * kPi * (kSlewHzPerSec / (double)sampleRate) /
               (double)sampleRate;
    SectionInit(&h->sec[0], sampleRate, kInitHz0);
    SectionInit(&h->sec[1], sampleRate, kInitHz1);
    return h;
}

void NotchSetSpeech(NotchHandle* h, int speaking) {
    if (!h) return;
    h->speaking = speaking ? 1 : 0;
}

void NotchProcess(NotchHandle* h, int16_t* buf, int n) {
    if (!h || !buf || n <= 0) return;

    // Block gate: adapt only above the silence floor (uses this
    // block's level; one block stale is fine, like WpeSetSpeech).
    double p = 0.0;
    for (int i = 0; i < n; i++) p += (double)buf[i] * buf[i];
    const double rms = std::sqrt(p / (double)n) / 32768.0;
    const bool adapt = !h->speaking && rms > kFloorRms;
    double accPin[kSections] = {0, 0}, accPout[kSections] = {0, 0};

    for (int i = 0; i < n; i++) {
        double x = (double)buf[i] / 32768.0;
        for (int k = 0; k < kSections; k++) {
            Section* s = &h->sec[k];
            const double xin = x;

            // Notch difference equation.
            const double y = x + s->b1 * s->x1 + s->x2 +
                             s->a1 * s->y1 + s->a2 * s->y2;

            // Sensitivity dy/dw (same denominator recursion).
            const double g = 2.0 * s->sw * s->x1 -
                             2.0 * s->r * s->sw * s->y1 +
                             s->a1 * s->s1 + s->a2 * s->s2;

            if (adapt) {
                s->emaG = kEmaAlpha * s->emaG + (1.0 - kEmaAlpha) * (y * g);
                s->emaP = kEmaAlpha * s->emaP + (1.0 - kEmaAlpha) * (g * g);
                double dwe = -kMu * s->emaG / (s->emaP + kEps);
                if (dwe > h->dwMax) dwe = h->dwMax;
                if (dwe < -h->dwMax) dwe = -h->dwMax;
                // Dead-zone: below 2% of the slew cap the estimate is
                // noise, not signal — moving here made the notch
                // limit-cycle around the tone (partial null forever).
                // Frozen instead: it settles deep and stays put.
                const double ddead = kDeadFrac * h->dwMax;
                if (dwe > ddead || dwe < -ddead) {
                    double w = s->w + dwe;
                    if (w < h->wMin) w = h->wMin;
                    if (w > h->wMax) w = h->wMax;
                    if (w != s->w) SectionSetFreq(s, w);
                }
            }

            // Engage telemetry: how much narrowband energy would this
            // section remove right now? (measured on internal state, so
            // it works while bypassed too)
            accPin[k] += xin * xin;
            accPout[k] += y * y;

            // Shift histories (filter keeps running while bypassed, so
            // adaptation stays coherent; the output just isn't rerouted).
            s->x2 = s->x1; s->x1 = xin;
            s->y2 = s->y1; s->y1 = y;
            s->s2 = s->s1; s->s1 = g;
            x = s->engaged ? y : xin;
        }
        double v = x * 32768.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        buf[i] = (int16_t)v;
    }

    // Engage gate: latch a section once it has demonstrably captured a
    // tone (>50% of its input energy removed, sustained ~200 ms while
    // adapting). Voice is broadband — a 60 Hz cut removes a tiny
    // fraction — so no-howl users run a mathematically exact bypass.
    for (int k = 0; k < kSections; k++) {
        Section* s = &h->sec[k];
        s->pinE = kBlockAlpha * s->pinE + (1.0 - kBlockAlpha) * accPin[k];
        s->poutE = kBlockAlpha * s->poutE + (1.0 - kBlockAlpha) * accPout[k];
    }
    // Only the strongest candidate accrues a latch streak; rivals decay
    // (a tie keeps the lower index). Once one section swallows the tone
    // the other's ratio collapses anyway, so a single howl ends up with
    // one dedicated notch instead of two fighting ones.
    int best = -1;
    double bestRatio = -1.0;
    double ratio[kSections];
    for (int k = 0; k < kSections; k++) {
        Section* s = &h->sec[k];
        ratio[k] = s->engaged ? -1.0
                              : (s->pinE - s->poutE) / (s->pinE + kEps);
        if (ratio[k] > bestRatio) { bestRatio = ratio[k]; best = k; }
    }
    for (int k = 0; k < kSections; k++) {
        Section* s = &h->sec[k];
        if (s->engaged) continue;
        if (adapt && k == best && ratio[k] > kEngRatio) {
            if (++s->engCount >= kEngBlocks) s->engaged = true;
        } else if (adapt && ratio[k] > kEngRatio) {
            if (s->engCount > 0) s->engCount--;
        } else {
            s->engCount = 0;
        }
    }
}

void NotchReset(NotchHandle* h) {
    if (!h) return;
    SectionInit(&h->sec[0], h->sr, kInitHz0);
    SectionInit(&h->sec[1], h->sr, kInitHz1);
    h->speaking = 1;
    h->failed = false;
}

int NotchEngaged(NotchHandle* h, int idx) {
    if (!h || idx < 0 || idx >= kSections) return 0;
    return h->sec[idx].engaged ? 1 : 0;
}

void NotchDestroy(NotchHandle* h) {
    delete h;
}

double NotchFreq(NotchHandle* h, int idx) {
    if (!h || idx < 0 || idx >= kSections) return 0.0;
    return h->sec[idx].w * (double)h->sr / (2.0 * kPi);
}

}  // extern "C"
