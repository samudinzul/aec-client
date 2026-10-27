// Offline checks for exp/resampler.h. Desktop-only, console app:
//   cmake --build build --target resampler_test
//   build/resampler_test.exe
// No models, no audio devices, no ONNX Runtime. Returns non-zero on
// failure so scripts can gate on it.
#include "resampler.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const double PI = 3.14159265358979323846;
static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static double toneMag(const float* x, int n, double f, double fs) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * PI * f * (double)i / fs;
        re += (double)x[i] * std::cos(a);
        im -= (double)x[i] * std::sin(a);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)n;
}

static double toneMag16(const int16_t* x, int n, double f, double fs) {
    std::vector<float> f32((size_t)n);
    for (int i = 0; i < n; ++i) f32[(size_t)i] = (float)x[i] * (1.0f / 32768.0f);
    return toneMag(f32.data(), n, f, fs);
}

static double db(double x) {
    return 20.0 * std::log10(std::max(x, 1e-12));
}

int main() {
    rs::Init();

    std::printf("resampler: %d taps, group delay %d @48k (%d @16k)\n",
                rs::kTaps, rs::kHalf, rs::kHalf / 3);

    // ---- prototype -------------------------------------------------
    {
        double sum = 0.0, sym = 0.0;
        for (int i = 0; i < rs::kTaps; ++i) {
            sum += rs::g_h[i];
            sym = std::max(sym, std::fabs((double)rs::g_h[i] -
                                          (double)rs::g_h[rs::kTaps - 1 - i]));
        }
        check(std::fabs(sum - 1.0) < 1e-4, "prototype DC gain = 1");
        check(sym < 1e-6, "prototype is symmetric (linear phase)");
    }

    // ---- down: passband, alias, length, state continuity -----------
    {
        const int n = 4800;
        std::vector<int16_t> in((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * 6000.0 * (double)i / 48000.0;
            in[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
        }

        rs::Down3 d1;
        std::vector<int16_t> out;
        out.reserve((size_t)n / 3);
        int16_t block[160];
        bool blocksOk = true;
        for (int off = 0; off < n; off += 480) {
            const int got = d1.process(in.data() + off, 480, block, 160);
            if (got != 160) blocksOk = false;
            out.insert(out.end(), block, block + got);
        }
        check(blocksOk, "down: every 480-sample block -> 160 samples");
        check((int)out.size() == n / 3, "down: total length is n/3");

        // Input amplitude is 0.5, so the passband expectation is 0.5.
        const double pass = toneMag16(out.data(), (int)out.size(), 6000.0, 16000.0);
        check(std::fabs(pass - 0.5) < 0.03, "down: 6 kHz passband within +/-0.5 dB");

        rs::Down3 d2;
        std::vector<int16_t> out2((size_t)n / 3);
        const int got2 = d2.process(in.data(), n, out2.data(), (int)out2.size());
        check(got2 == n / 3, "down: one-shot length is n/3");
        check(std::memcmp(out.data(), out2.data(), out.size() * sizeof(int16_t)) == 0,
              "down: block and one-shot output match (state carries)");

        // A device may hand us a frame that is not a multiple of 3. The
        // decimation phase must carry so the split run matches one call.
        {
            std::vector<int16_t> odd(481);
            for (int i = 0; i < 481; ++i) {
                const double a = 2.0 * PI * 6000.0 * (double)i / 48000.0;
                odd[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
            }
            int16_t one[200];
            rs::Down3 dA;
            const int nA = dA.process(odd.data(), 481, one, 200);

            rs::Down3 dB;
            int16_t p1[160], p2[8];
            const int n1 = dB.process(odd.data(), 480, p1, 160);
            const int n2 = dB.process(odd.data() + 480, 1, p2, 8);
            bool same = (nA == n1 + n2);
            for (int i = 0; same && i < n1; ++i) same = (p1[i] == one[i]);
            for (int i = 0; same && i < n2; ++i) same = (p2[i] == one[n1 + i]);
            check(same, "down: 480+1 frames match one 481-frame call");
        }

        std::vector<int16_t> aliasIn((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * 12000.0 * (double)i / 48000.0;
            aliasIn[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
        }
        rs::Down3 d3;
        std::vector<int16_t> aliasOut((size_t)n / 3);
        d3.process(aliasIn.data(), n, aliasOut.data(), (int)aliasOut.size());
        const double alias = toneMag16(aliasOut.data(), (int)aliasOut.size(), 4000.0, 16000.0);
        check(db(alias) < -60.0, "down: 12 kHz -> 4 kHz alias below -60 dB");
    }

    // ---- up: passband, image, length, state continuity -------------
    {
        const int n = 1600;
        std::vector<int16_t> in((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * 4000.0 * (double)i / 16000.0;
            in[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
        }

        rs::Up3 u1;
        std::vector<float> out;
        out.reserve((size_t)n * 3);
        float block[480];
        for (int off = 0; off < n; off += 160) {
            u1.process(in.data() + off, 160, block);
            out.insert(out.end(), block, block + 480);
        }
        check((int)out.size() == n * 3, "up: total length is 3n");

        const double pass = toneMag(out.data(), (int)out.size(), 4000.0, 48000.0);
        check(std::fabs(pass - 0.5) < 0.03, "up: 4 kHz passband within +/-0.5 dB");

        rs::Up3 u2;
        std::vector<float> out2((size_t)n * 3);
        u2.process(in.data(), n, out2.data());
        check(std::memcmp(out.data(), out2.data(), out.size() * sizeof(float)) == 0,
              "up: block and one-shot output match (state carries)");

        std::vector<int16_t> imgIn((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * 7500.0 * (double)i / 16000.0;
            imgIn[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
        }
        rs::Up3 u3;
        std::vector<float> imgOut((size_t)n * 3);
        u3.process(imgIn.data(), n, imgOut.data());
        const double image = toneMag(imgOut.data(), (int)imgOut.size(), 8500.0, 48000.0);
        check(db(image) < -40.0, "up: 7.5 kHz -> 8.5 kHz image below -40 dB");
    }

    // ---- group delay ----------------------------------------------
    {
        std::vector<int16_t> imp(480, 0);
        imp[0] = 32767;
        rs::Down3 d;
        int16_t dOut[160];
        d.process(imp.data(), 480, dOut, 160);
        int peak = 0;
        for (int i = 1; i < 160; ++i)
            if (std::abs((int)dOut[i]) > std::abs((int)dOut[peak])) peak = i;
        check(peak == 32, "down: impulse peak at 32 samples (exact group delay)");

        std::vector<int16_t> imp16(160, 0);
        imp16[0] = 32767;
        rs::Up3 u;
        float uOut[480];
        u.process(imp16.data(), 160, uOut);
        int uPeak = 0;
        for (int i = 1; i < 480; ++i)
            if (std::fabs(uOut[i]) > std::fabs(uOut[uPeak])) uPeak = i;
        check(uPeak >= 94 && uPeak <= 98, "up: impulse peak at 96 samples");
    }

    // ---- round trip: the splice's low-band path -------------------
    // 16k -> 48k (up) -> 16k (down). Unity gain and flat at 6 kHz.
    {
        const int n = 1600;
        std::vector<int16_t> in((size_t)n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * PI * 6000.0 * (double)i / 16000.0;
            in[(size_t)i] = (int16_t)std::lrint(0.5 * std::sin(a) * 32767.0);
        }
        rs::Up3 u;
        std::vector<float> up((size_t)n * 3);
        u.process(in.data(), n, up.data());

        std::vector<int16_t> up16((size_t)n * 3);
        for (int i = 0; i < n * 3; ++i)
            up16[(size_t)i] = rs::Clamp16((int)std::lrint((double)up[(size_t)i] * 32768.0));

        rs::Down3 d;
        std::vector<int16_t> down((size_t)n);
        const int got = d.process(up16.data(), n * 3, down.data(), n);
        check(got == n, "round trip: 3n in -> n out");

        // Offset 128 skips both FIR transients; 1344 samples is exactly
        // 504 cycles at 6 kHz, so the DFT does not leak.
        const double pass = toneMag16(down.data() + 128, 1344, 6000.0, 16000.0);
        check(std::fabs(pass - 0.5) < 0.03, "round trip: 6 kHz level within +/-0.5 dB");
    }

    // ---- DC level ---------------------------------------------------
    {
        std::vector<int16_t> dc(480, 16384);
        rs::Down3 d;
        int16_t dOut[160];
        d.process(dc.data(), 480, dOut, 160);
        check(std::abs((int)dOut[159] - 16384) <= 2, "down: DC level preserved");

        std::vector<int16_t> dc16(160, 16384);
        rs::Up3 u;
        float uOut[480];
        u.process(dc16.data(), 160, uOut);
        check(std::fabs(uOut[479] - 0.5f) < 0.01f, "up: DC level preserved");
    }

    std::printf(g_fail ? "\nFAIL=%d\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
