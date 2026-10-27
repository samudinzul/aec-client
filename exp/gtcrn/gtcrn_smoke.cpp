// gtcrn_smoke — offline harness for the exp GTCRN NS wrapper.
//
// Runs WAV (or synthetic) mic audio through GtcrnProcess and prints the
// session IO table + a per-second RMS table. v0 expectation: out == mic
// (bypass) — this validates build, WAV plumbing, and that the ONNX graph
// loads with the expected 257-bin input. V1 expectation: out < mic on
// noise, voice preserved.
//
// Usage (MSYS2 UCRT64, from repo root):
//   cmake -B build -G Ninja && cmake --build build --target gtcrn_smoke
//   build/gtcrn_smoke.exe mic.wav out.wav [model.onnx]
//   build/gtcrn_smoke.exe --synth 5 out.wav
//
// WAVs must be 16 kHz mono int16.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gtcrn_wrapper.h"

namespace {

constexpr int kSr = 16000;
constexpr int kFrame = 128;  // chain frame size; wrapper FIFOs internally

struct Wav {
    std::vector<int16_t> samples;
};

bool ReadWav16Mono(const char* path, Wav& w) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return false; }
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { printf("%s: not a WAV\n", path); fclose(f); return false; }
    const int sr = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    const int ch = h[22] | (h[23] << 8);
    const int bits = h[34] | (h[35] << 8);
    if (sr != kSr || ch != 1 || bits != 16) {
        printf("%s: need 16 kHz mono int16 (got %d Hz, %d ch, %d bit)\n",
               path, sr, ch, bits);
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 44, SEEK_SET);
    const size_t n = (size_t)(bytes - 44) / 2;
    w.samples.resize(n);
    const bool ok = fread(w.samples.data(), 2, n, f) == n;
    fclose(f);
    return ok;
}

bool WriteWav16Mono(const char* path, const std::vector<int16_t>& s) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return false; }
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4);
    const uint32_t dataBytes = (uint32_t)s.size() * 2;
    const uint32_t riffSize = 36 + dataBytes;
    memcpy(h + 4, &riffSize, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmtSize = 16;
    memcpy(h + 16, &fmtSize, 4);
    const uint16_t audioFmt = 1, channels = 1;
    memcpy(h + 20, &audioFmt, 2);
    memcpy(h + 22, &channels, 2);
    const uint32_t sr = kSr;
    memcpy(h + 24, &sr, 4);
    const uint32_t byteRate = kSr * 2;
    memcpy(h + 28, &byteRate, 4);
    const uint16_t blockAlign = 2, bits = 16;
    memcpy(h + 32, &blockAlign, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dataBytes, 4);
    fwrite(h, 1, 44, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
    return true;
}

static inline int16_t ClampS16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Voice bursts + hiss: NS should keep the bursts, cut the hiss (V1).
void Synth(double seconds, Wav& mic) {
    const size_t n = (size_t)(seconds * kSr);
    mic.samples.assign(n, 0);
    unsigned seed = 7;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        const double hiss = ((double)(seed >> 8) / (1u << 24) - 0.5) * 3000.0;
        const double t = (double)i / kSr;
        const double sec = t - floor(t / 2.0) * 2.0;
        double m = hiss;
        if (sec < 1.0)
            m += sin(3.14159265 * sec) * 5000.0 *
                 sin(2.0 * 3.14159265 * 220.0 * t);
        mic.samples[i] = ClampS16((int)lrint(m));
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    double synthSecs = 5.0;
    const char *micPath = nullptr, *outPath = nullptr, *modelPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 2 < argc) {
            synth = true;
            synthSecs = atof(argv[++i]);
            outPath = argv[++i];
        } else if (!micPath) {
            micPath = argv[i];
        } else if (!outPath) {
            outPath = argv[i];
        } else if (!modelPath) {
            modelPath = argv[i];
        }
    }
    if (!modelPath) modelPath = "exp/models/gtcrn_simple.onnx";

    Wav mic;
    if (synth) {
        Synth(synthSecs, mic);
        printf("synth: %.1f s voice bursts + hiss\n", synthSecs);
    } else {
        if (!micPath || !outPath) {
            printf("usage: gtcrn_smoke mic.wav out.wav [model.onnx]\n"
                   "   or: gtcrn_smoke --synth SECS out.wav\n");
            return 2;
        }
        if (!ReadWav16Mono(micPath, mic)) return 2;
    }
    if (mic.samples.size() < (size_t)kSr) { printf("need >= 1 s\n"); return 2; }

    GtcrnHandle* g = GtcrnNew(modelPath);
    GtcrnDump(g);
    if (!GtcrnReady(g))
        printf("note: %s -> bypass (fail-open)\n", GtcrnLastError(g));

    std::vector<int16_t> out;
    out.reserve(mic.samples.size());
    float inf[kFrame], outf[kFrame];
    printf("sec  micRMS  outRMS\n");
    double accMic = 0, accOut = 0;
    size_t accN = 0;
    int sec = 0;
    const size_t n = mic.samples.size() - mic.samples.size() % kFrame;
    for (size_t pos = 0; pos < n; pos += kFrame) {
        for (int i = 0; i < kFrame; i++)
            inf[i] = (float)mic.samples[pos + i] / 32768.0f;
        GtcrnProcess(g, inf, outf, kFrame);
        for (int i = 0; i < kFrame; i++) {
            const int16_t v = ClampS16((int)lrintf(outf[i] * 32768.0f));
            out.push_back(v);
            accMic += (double)mic.samples[pos + i] * mic.samples[pos + i];
            accOut += (double)v * v;
        }
        accN += kFrame;
        if (accN >= (size_t)kSr) {
            printf("%3d  %6.0f  %6.0f\n", ++sec,
                   sqrt(accMic / accN), sqrt(accOut / accN));
            accMic = accOut = 0;
            accN = 0;
        }
    }
    if (!WriteWav16Mono(outPath, out)) return 1;
    printf("wrote %s (%zu samples)\n", outPath, out.size());
    GtcrnStats st = {};
    GtcrnStatsGet(g, &st);
    printf("verdict: %s (neural=%d)\n",
           st.neural ? "neural path engaged" : "bypass — plumbing only",
           st.neural);
    GtcrnDestroy(g);
    return 0;
}
