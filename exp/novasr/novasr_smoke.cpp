// novasr_smoke — offline harness for the exp NovaSR super-resolution stage.
//
// Runs 16 kHz mono int16 through NovaSR (YatharthS/NovaSR, Apache-2.0:
// 52 KB conv net, 16 kHz in -> 48 kHz out, exported to ONNX with
// torch/ORT parity 1.000000) and writes 48 kHz out. Prints the same
// one-pole hiRMS A/B line as nkf_smoke --hb48 (pure linear upsample vs
// neural), so mirror-vs-neural settles in one glance:
//
//   nova48: pure hiRMS <empty> | nova hiRMS <air> | fullRMS ... | peak ...
//
// Usage (MSYS2 UCRT64, from repo root):
//   cmake -B build -G Ninja && cmake --build build --target novasr_smoke
//   build/novasr_smoke.exe in16.wav out48.wav [novasr.onnx]
//   build/novasr_smoke.exe --synth 5 out48.wav
//
// --synth voice carries real mids (harmonics to ~6 kHz + frication),
// unlike the LF-only fixtures elsewhere: dull synths give any BWE
// nothing to extend (measured 58 vs 59 on gen_test_pair). Model path
// defaults to models/novasr.onnx (staged by CMake); a missing model
// writes the pure upsample instead (fail-open) and returns 1.
// Whole-file inference (like upstream FastSR.infer); the chunked-halo
// streaming variant comes with live integration, not this proof tool.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"

namespace {

constexpr int kInSr = 16000;
constexpr int kOutSr = 48000;

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
    if (sr != kInSr || ch != 1 || bits != 16) {
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

bool WriteWav48Mono(const char* path, const std::vector<int16_t>& s) {
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
    const uint32_t sr = kOutSr;
    memcpy(h + 24, &sr, 4);
    const uint32_t byteRate = kOutSr * 2;
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

double Rms(const int16_t* p, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += (double)p[i] * p[i];
    return sqrt(s / (n ? (double)n : 1.0));
}

// Same one-pole 8 kHz highband meter as nkf_smoke --hb48 (HighRms48):
// numbers compare directly across the two harnesses.
double HighRms48(const int16_t* p, size_t n) {
    const float a = 1.0f - expf(-2.0f * 3.14159265f * 8000.0f / 48000.0f);
    float lp = 0.0f;
    double acc = 0.0;
    for (size_t j = 0; j < n; j++) {
        const float v = (float)p[j];
        lp += a * (v - lp);
        const float h = v - lp;
        acc += (double)h * h;
    }
    return sqrt(acc / (n ? (double)n : 1.0));
}

// Pure linear 16->48 (what the cable gets today): the A/B baseline.
void LinearUpsample3(const std::vector<float>& x, std::vector<float>& base) {
    const int n = (int)x.size();
    base.assign((size_t)n * 3, 0.0f);
    if (n <= 0) return;
    if (n == 1) { base[0] = base[1] = base[2] = x[0]; return; }
    for (int j = 0; j < n * 3; j++) {
        const float pos = (float)j * (float)(n - 1) / (float)(n * 3 - 1);
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        if (i0 > n - 2) i0 = n - 2;
        const float frac = pos - (float)i0;
        base[(size_t)j] = x[(size_t)i0] * (1.0f - frac) + x[(size_t)i0 + 1] * frac;
    }
}

// Mid-rich deterministic voice: harmonics to ~6 kHz + AM + light
// frication, so the top octaves have something real to extend.
void Synth(double seconds, Wav& mic) {
    const size_t n = (size_t)(seconds * kInSr);
    mic.samples.assign(n, 0);
    unsigned seed = 11;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        const double nz = ((double)(seed >> 8) / (1u << 24) - 0.5);
        const double t = (double)i / kInSr;
        const double am = 0.5 + 0.5 * sin(2.0 * 3.14159265 * 2.7 * t);
        double v = 0.0;
        for (int h = 1; h <= 40; h++) {
            const double f = 130.0 * h;
            if (f > 6200.0) break;
            v += sin(2.0 * 3.14159265 * f * t + 0.3 * h) / pow(h, 0.8);
        }
        const double sec = t - floor(t / 2.0) * 2.0;
        const double fric = (sec >= 1.0) ? nz * 0.6 : nz * 0.08;
        mic.samples[i] = ClampS16((int)lrint(am * (v * 2600.0 + fric * 2600.0)));
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    double synthSecs = 5.0;
    const char *inPath = nullptr, *outPath = nullptr, *modelArg = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 2 < argc) {
            synth = true;
            synthSecs = atof(argv[++i]);
            outPath = argv[++i];
        } else if (!inPath && !synth) {
            inPath = argv[i];
        } else if (!outPath) {
            outPath = argv[i];
        } else {
            modelArg = argv[i];
        }
    }

    // Model resolution: explicit arg wins, else the repo exp bundle,
    // else the exe-adjacent copy CMake stages (build/models/). exp weights
    // never live in the official models/ dir.
    std::string modelPath = modelArg ? modelArg : "";
    if (modelPath.empty()) {
        const char* cands[] = {"exp/models/novasr.onnx", "models/novasr.onnx"};
        for (size_t c = 0; c < 2; c++) {
            FILE* f = fopen(cands[c], "rb");
            if (f) { fclose(f); modelPath = cands[c]; break; }
        }
        if (modelPath.empty()) modelPath = "exp/models/novasr.onnx";
    }

    Wav mic;
    if (synth) {
        Synth(synthSecs, mic);
        printf("synth: %.1f s mid-rich voice\n", synthSecs);
    } else {
        if (!inPath || !outPath) {
            printf("usage: novasr_smoke in16.wav out48.wav [novasr.onnx]\n"
                   "   or: novasr_smoke --synth SECS out48.wav\n");
            return 2;
        }
        if (!ReadWav16Mono(inPath, mic)) return 2;
    }
    const size_t n = mic.samples.size();
    if (n < (size_t)kInSr) { printf("need >= 1 s of audio\n"); return 2; }

    std::vector<float> x(n);
    for (size_t i = 0; i < n; i++) x[i] = (float)mic.samples[i] / 32768.0f;

    // Baseline first (always available, even without the model).
    std::vector<float> pure;
    LinearUpsample3(x, pure);
    std::vector<int16_t> pure48(pure.size());
    for (size_t j = 0; j < pure.size(); j++)
        pure48[j] = ClampS16((int)lrintf(pure[j] * 32768.0f));

    std::vector<int16_t> nova48;
    bool neural = false;
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "novasr-smoke");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        std::wstring widePath(modelPath.begin(), modelPath.end());
        Ort::Session session(env, widePath.c_str(), opts);
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::AllocatedStringPtr inNm = session.GetInputNameAllocated(0, alloc);
        Ort::AllocatedStringPtr outNm = session.GetOutputNameAllocated(0, alloc);
        printf("graph: %s -> %s\n", inNm.get(), outNm.get());
        const char* inPtr = inNm.get();
        const char* outPtr = outNm.get();
        std::vector<int64_t> shape = {1, 1, (int64_t)n};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeCPU);
        Ort::Value inT = Ort::Value::CreateTensor<float>(
            mem, x.data(), n, shape.data(), shape.size());
        std::vector<Ort::Value> outs = session.Run(
            Ort::RunOptions{nullptr}, &inPtr, &inT, 1, &outPtr, 1);
        float* y = outs[0].GetTensorMutableData<float>();
        size_t m = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        nova48.assign(m, 0);
        double peak = 0.0;
        for (size_t j = 0; j < m; j++) {
            nova48[j] = ClampS16((int)lrintf(y[j] * 32768.0f));
            const double v = fabs((double)nova48[j]);
            if (v > peak) peak = v;
        }
        neural = true;
        printf("nova48: pure hiRMS %6.0f | nova hiRMS %6.0f | fullRMS %6.0f | peak %6.0f\n",
               HighRms48(pure48.data(), pure48.size()),
               HighRms48(nova48.data(), nova48.size()),
               Rms(nova48.data(), nova48.size()), peak);
        if (!WriteWav48Mono(outPath, nova48)) return 1;
        printf("wrote %s (%zu samples @48k, neural)\n", outPath, nova48.size());
    } catch (const Ort::Exception& e) {
        printf("novasr: %s -> fail-open pure upsample\n", e.what());
    }
    if (!neural) {
        printf("nova48: pure hiRMS %6.0f (model absent, passthrough)\n",
               HighRms48(pure48.data(), pure48.size()));
        if (!WriteWav48Mono(outPath, pure48)) return 1;
        printf("wrote %s (%zu samples @48k, passthrough)\n", outPath, pure48.size());
        return 1;
    }
    return 0;
}
