// localvqe_smoke — offline harness for the LocalVQE v1.4-AEC wrapper.
//
// Runs WAV mic/ref pairs through LocalVqeProcess and prints bundle status
// + a per-second mic/ref/out RMS table with the frontend delay estimate.
// Expectation: echo (ref content in mic) drops, voice holds; a silent ref
// preserves the mic (upstream runtime rule).
//
// Usage (MSYS2 UCRT64, from repo root):
//   cmake -B build -G Ninja && cmake --build build --target localvqe_smoke
//   build/localvqe_smoke.exe mic.wav ref.wav out.wav [bundleDir]
//   build/localvqe_smoke.exe --synth 5 out.wav
//
// WAVs must be 16 kHz mono int16. Never mutes: not-ready -> passthrough.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "localvqe_wrapper.h"

namespace {

constexpr int kSr = 16000;
constexpr int kFrame = 160;  // app pump frame; wrapper FIFOs to 256 hops

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

// Doubletalk-ish synth: ref = tonal speaker content, mic = delayed echo
// of ref + voice bursts. The canceller should remove the echo, keep bursts.
void Synth(double seconds, Wav& mic, Wav& ref) {
    const size_t n = (size_t)(seconds * kSr);
    mic.samples.assign(n, 0);
    ref.samples.assign(n, 0);
    const int lag = 2048;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / kSr;
        const double r = 8000.0 * sin(2.0 * 3.14159265 * 440.0 * t)
                       + 3000.0 * sin(2.0 * 3.14159265 * 880.0 * t + 1.0);
        ref.samples[i] = ClampS16((int)lrint(r));
        double m = 0.0;
        if (i >= (size_t)lag) m += 0.4 * ref.samples[i - lag];
        const double sec = t - floor(t / 2.0) * 2.0;
        if (sec < 1.0)
            m += sin(3.14159265 * sec) * 4000.0 *
                 sin(2.0 * 3.14159265 * 220.0 * t);
        mic.samples[i] = ClampS16((int)lrint(m));
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    double synthSecs = 5.0;
    const char *micPath = nullptr, *refPath = nullptr, *outPath = nullptr;
    const char* bundle = "exp/models";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 2 < argc) {
            synth = true;
            synthSecs = atof(argv[++i]);
            outPath = argv[++i];
        } else if (!micPath) {
            micPath = argv[i];
        } else if (!refPath && !synth) {
            refPath = argv[i];
        } else if (!outPath) {
            outPath = argv[i];
        } else {
            bundle = argv[i];
        }
    }

    Wav mic, ref;
    if (synth) {
        Synth(synthSecs, mic, ref);
        printf("synth: %.1f s echo-in-mic + voice bursts\n", synthSecs);
    } else {
        if (!micPath || !refPath || !outPath) {
            printf("usage: localvqe_smoke mic.wav ref.wav out.wav [bundleDir]\n"
                   "   or: localvqe_smoke --synth SECS out.wav\n");
            return 2;
        }
        if (!ReadWav16Mono(micPath, mic) || !ReadWav16Mono(refPath, ref))
            return 2;
    }
    const size_t n = mic.samples.size() < ref.samples.size()
                         ? mic.samples.size()
                         : ref.samples.size();
    if (n < (size_t)kSr) { printf("need >= 1 s\n"); return 2; }

    std::string onnx = std::string(bundle) + "/LocalVQEAECResidualMask.onnx";
    std::string front = std::string(bundle) + "/LocalVQEAECFrontend.json";
    std::string cfg = std::string(bundle) + "/LocalVQEAECConfig.json";
    // Fallback: upstream config filename (get_models.sh renames it to
    // LocalVQEAECConfig.json; accept either).
    FILE* probe = fopen(cfg.c_str(), "rb");
    if (!probe) cfg = std::string(bundle) + "/config.json";
    else fclose(probe);
    LocalVqeHandle* v = LocalVqeNew(onnx.c_str(), front.c_str(), cfg.c_str());
    LocalVqeDump(v);
    if (!LocalVqeReady(v)) {
        printf("note: %s -> bypass (fail-open)\n", LocalVqeLastError(v));
    }

    std::vector<int16_t> out;
    out.reserve(n);
    float mf[kFrame], rf[kFrame], of[kFrame];
    printf("sec  micRMS  refRMS  outRMS  delay(conf)\n");
    double accMic = 0, accRef = 0, accOut = 0;
    size_t accN = 0;
    int sec = 0;
    const size_t nfr = n - n % kFrame;
    for (size_t pos = 0; pos < nfr; pos += kFrame) {
        for (int i = 0; i < kFrame; i++) {
            mf[i] = (float)mic.samples[pos + i] / 32768.0f;
            rf[i] = (float)ref.samples[pos + i] / 32768.0f;
        }
        LocalVqeProcess(v, mf, rf, of, kFrame);
        for (int i = 0; i < kFrame; i++) {
            const int16_t w = ClampS16((int)lrintf(of[i] * 32768.0f));
            out.push_back(w);
            accMic += (double)mic.samples[pos + i] * mic.samples[pos + i];
            accRef += (double)ref.samples[pos + i] * ref.samples[pos + i];
            accOut += (double)w * w;
        }
        accN += kFrame;
        if (accN >= (size_t)kSr) {
            LocalVqeStats st = {};
            LocalVqeStatsGet(v, &st);
            printf("%3d  %6.0f  %6.0f  %6.0f  %5d(%.1f)\n", ++sec,
                   sqrt(accMic / accN), sqrt(accRef / accN),
                   sqrt(accOut / accN), st.delaySamples,
                   (double)st.delayConf);
            accMic = accRef = accOut = 0;
            accN = 0;
        }
    }
    if (!WriteWav16Mono(outPath, out)) return 1;
    printf("wrote %s (%zu samples)\n", outPath, out.size());
    LocalVqeStats st = {};
    LocalVqeStatsGet(v, &st);
    printf("verdict: %s (neural=%d delay=%d conf=%.1f)\n",
           st.neural ? "mask engaged" : "bypass — plumbing only",
           st.neural, st.delaySamples, (double)st.delayConf);
    LocalVqeDestroy(v);
    return 0;
}
