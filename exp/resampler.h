#pragma once
// ============================================================
//  3:1 polyphase FIR resampler (high-band splice)
// ============================================================
// The splice needs an exact 48 kHz <-> 16 kHz conversion. The old
// implementation used a 3-tap boxcar for the down direction and linear
// interpolation for the up direction:
//
//   down: passband droop -1.9 dB @6 kHz / -3.5 dB @8 kHz,
//         12 kHz folds back to 4 kHz at only -9.5 dB
//   up:   linear interpolation mirrored 7 kHz to 9 kHz at -4 dB,
//         5 kHz to 11 kHz at -12.5 dB, and drooped the 16 kHz band's
//         own top (-2.6 dB @5 kHz, -6.1 dB @7.5 kHz)
//
// Both are replaced by one linear-phase FIR prototype: 193 taps at
// 48 kHz, Kaiser window (~60 dB stopband), cutoff 8 kHz. Decimation
// runs the prototype directly; interpolation splits it into three
// phases and scales by 3 so the zero-stuffed signal keeps its level.
//
// Measured with resampler_test:
//   12 kHz -> 4 kHz alias:  -76 dB   (was -9.5 dB)
//    7.5 kHz -> 8.5 kHz image: -55 dB (was -2.0 dB)
//   passband 0-7.5 kHz:     flat (<0.1 dB)
//
// Group delay: (193-1)/2 = 96 samples @48 kHz = 32 samples @16 kHz.
// The splice adds 2*96 samples to the high-band delay line, and the
// loopback reference is delayed by 32 samples @16 kHz so the AEC still
// sees mic and ref aligned.
//
// Realtime-safe after Init(): no allocation, single-threaded use.
// ============================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rs {

constexpr int kTaps = 193;
constexpr int kHalf = (kTaps - 1) / 2;   // 96 @48k = 32 @16k
constexpr int kHist = (kTaps + 2) / 3;   // 65: longest interpolation phase

inline float g_h[kTaps];

inline double BesselI0(double x) {
    // Power series; beta is ~5.65, so 40 terms is far past convergence.
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 40; ++k) {
        const double t = x / (2.0 * k);
        term *= t * t;
        sum += term;
        if (term < 1e-13 * sum) break;
    }
    return sum;
}

inline void Init() {
    const double pi = 3.14159265358979323846;
    const double fc = 8000.0 / 48000.0;  // top of the 16 kHz band
    const double beta = 5.65326;         // Kaiser, ~60 dB stopband
    const double i0b = BesselI0(beta);
    double sum = 0.0;

    for (int i = 0; i < kTaps; ++i) {
        const int k = i - kHalf;
        const double sinc = (k == 0)
            ? 2.0 * fc
            : std::sin(2.0 * pi * fc * k) / (pi * k);
        const double r = (double)k / (double)kHalf;
        const double w = BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        g_h[i] = (float)(sinc * w);
        sum += g_h[i];
    }
    // Unity DC gain: the decimator keeps the level, the interpolator
    // multiplies by 3 to undo the zero-stuffing loss.
    for (int i = 0; i < kTaps; ++i) g_h[i] /= (float)sum;
}

inline int16_t Clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// 48 kHz -> 16 kHz. process() may be called with any frame count; the
// phase carries over, so a device frame that is not a multiple of 3
// does not shift the output grid. Returns the number written to out.
struct Down3 {
    float hist[kTaps] = {};
    int   pos = 0;    // next write
    int   phase = 2;  // output grid at input times 0, 3, 6, ...

    void reset() {
        std::memset(hist, 0, sizeof(hist));
        pos = 0;
        // Output at t = 0 mod 3: the filter centre (kHalf = 96) then
        // lands exactly on an output sample, so the group delay is a
        // clean 96 @48k = 32 @16k instead of 31/32 depending on phase.
        phase = 2;
    }

    int process(const int16_t* in, int n, int16_t* out, int outCap) {
        int o = 0;
        for (int i = 0; i < n; ++i) {
            hist[pos] = (float)in[i] * (1.0f / 32768.0f);
            if (++pos == kTaps) pos = 0;
            if (++phase < 3) continue;
            phase = 0;
            int idx = pos;
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                if (--idx < 0) idx = kTaps - 1;
                acc += g_h[k] * hist[idx];
            }
            if (o < outCap)
                out[o++] = Clamp16((int)std::lrintf(acc * 32768.0f));
        }
        return o;
    }
};

// 16 kHz -> 48 kHz. Exactly 3*n floats are written to out. The input
// is int16 (the 16 kHz chain's output); the result is [-1, 1] float.
struct Up3 {
    float hist[kHist] = {};
    int   pos = 0;    // next write

    void reset() {
        std::memset(hist, 0, sizeof(hist));
        pos = 0;
    }

    void process(const int16_t* in, int n, float* out) {
        for (int i = 0; i < n; ++i) {
            hist[pos] = (float)in[i] * (1.0f / 32768.0f);
            for (int p = 0; p < 3; ++p) {
                float acc = 0.0f;
                int idx = pos;
                for (int k = p; k < kTaps; k += 3) {
                    acc += g_h[k] * hist[idx];
                    if (--idx < 0) idx = kHist - 1;
                }
                out[3 * i + p] = 3.0f * acc;
            }
            if (++pos == kHist) pos = 0;
        }
    }
};

} // namespace rs
