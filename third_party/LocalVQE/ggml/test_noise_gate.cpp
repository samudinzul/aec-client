/**
 * Unit test for the residual-echo noise gate.
 *
 * Exercises noise_gate.h directly so we don't need a GGUF model file.
 */

#include "noise_gate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char* what) {
    if (ok) {
        g_pass++;
        std::printf("[PASS] %s\n", what);
    } else {
        g_fail++;
        std::printf("[FAIL] %s\n", what);
    }
}

// Synthetic Gaussian noise scaled to a target dBFS. Deterministic per seed.
static std::vector<float> gauss_at_dbfs(int n, float dbfs, uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = d(g);
    // Normalise to RMS=1 then scale to target.
    double sumsq = 0.0;
    for (float x : v) sumsq += static_cast<double>(x) * x;
    double rms = std::sqrt(sumsq / n);
    float target_rms = std::pow(10.0f, dbfs / 20.0f);
    float scale = static_cast<float>(target_rms / rms);
    for (float& x : v) x *= scale;
    return v;
}

static double rms_dbfs(const float* x, int n) {
    double sumsq = 0.0;
    for (int i = 0; i < n; i++) sumsq += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(sumsq / n + 1e-30);
}

static bool all_zero(const float* x, int n) {
    for (int i = 0; i < n; i++) if (x[i] != 0.0f) return false;
    return true;
}

int main() {
    constexpr int HOP = 256;
    using localvqe::apply_noise_gate;

    // 1. Loud frame stays untouched.
    {
        auto v = gauss_at_dbfs(HOP, -20.0f, 1);
        std::vector<float> before = v;
        apply_noise_gate(v.data(), HOP, -45.0f);
        check(v == before, "loud (-20 dBFS) frame passes through unchanged");
    }

    // 2. Quiet frame at -60 dBFS, threshold -45 → silenced.
    {
        auto v = gauss_at_dbfs(HOP, -60.0f, 2);
        apply_noise_gate(v.data(), HOP, -45.0f);
        check(all_zero(v.data(), HOP),
              "quiet (-60 dBFS) frame zeroed at threshold -45");
    }

    // 3. Quiet frame at -40 dBFS, threshold -45 → passes through (above thr).
    {
        auto v = gauss_at_dbfs(HOP, -40.0f, 3);
        std::vector<float> before = v;
        apply_noise_gate(v.data(), HOP, -45.0f);
        check(v == before,
              "frame above threshold (-40 vs -45) passes through unchanged");
    }

    // 4. Boundary: frame exactly at threshold should be gated (≤).
    // Use a constant-amplitude signal so the RMS lands on exactly -45 dBFS
    // (Gaussian fixtures drift by ~0.1 dB due to normalise-then-scale
    // float rounding, which would put us either side of the comparison).
    {
        const float amp = std::pow(10.0f, -45.0f / 20.0f);
        std::vector<float> v(HOP, amp);
        double measured = rms_dbfs(v.data(), HOP);
        check(std::fabs(measured + 45.0) < 1e-3,
              "constant-amp boundary fixture lands at -45 dBFS exactly");
        apply_noise_gate(v.data(), HOP, -45.0f);
        check(all_zero(v.data(), HOP),
              "frame at threshold gated by ≤ comparison");
    }

    // 5. All-zero input is safe (no NaN, no crash, stays zero).
    {
        std::vector<float> v(HOP, 0.0f);
        apply_noise_gate(v.data(), HOP, -45.0f);
        check(all_zero(v.data(), HOP), "all-zero input stays zero (no NaN)");
    }

    // 6. n == 0 short-circuits.
    {
        float dummy = 1.0f;
        apply_noise_gate(&dummy, 0, -45.0f);
        check(dummy == 1.0f, "n==0 doesn't touch buffer");
    }

    // 7. Multi-frame stream: alternating loud/quiet/loud frames; gate each
    // frame independently (matches the GGML streaming path one-hop-at-a-time).
    {
        constexpr int N_FRAMES = 5;
        std::vector<float> buf(HOP * N_FRAMES);
        const float dbfs[N_FRAMES] = { -20.0f, -60.0f, -20.0f, -55.0f, -25.0f };
        for (int f = 0; f < N_FRAMES; f++) {
            auto frame = gauss_at_dbfs(HOP, dbfs[f], 100u + f);
            std::memcpy(buf.data() + f * HOP, frame.data(),
                        HOP * sizeof(float));
        }
        for (int f = 0; f < N_FRAMES; f++) {
            apply_noise_gate(buf.data() + f * HOP, HOP, -45.0f);
        }
        bool ok = true;
        // Loud frames preserved (RMS in [-30, -15]).
        for (int f : {0, 2, 4}) {
            double db = rms_dbfs(buf.data() + f * HOP, HOP);
            if (db < -30.0 || db > -15.0) { ok = false; std::printf("  frame %d: rms=%.2f dBFS\n", f, db); }
        }
        // Quiet frames zeroed.
        for (int f : {1, 3}) {
            if (!all_zero(buf.data() + f * HOP, HOP)) ok = false;
        }
        check(ok, "alternating loud/quiet stream gated frame-by-frame");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
