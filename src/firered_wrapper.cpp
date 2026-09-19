// ============================================================
//  FireRed Stream-VAD wrapper — FireRedTeam/FireRedVAD (Apache-2.0),
//  ONNX export via leospark/FireRedVAD-Engineering (MIT).
//
//  Provenance:
//    models/model_with_caches.onnx (+ .onnx.data sidecar, sha-pinned
//      at vendor time) — streaming export, validated <1.19e-7 vs
//      PyTorch by the exporter.
//    models/firered_cmvn.bin — 160 f32LE (80 global means + 80
//      inverse stds), generated from the official cmvn.ark with
//      kaldiio (mean=sum/count, istd=1/sqrt(sumsq/count-mean^2),
//      variance floor 1e-20), cross-checked against the safetensors
//      vectors (maxdiff ~1e-7). Generation is mechanical and logged
//      in THIRD-PARTY.txt; parity re-verifiable from golden files.
//
//  Parity (temp harness, golden_stream.safetensors):
//    fbank vs golden feat: meanabs 0.0025 (rel 1.8e-4) — PASS
//    streaming probs vs golden: meanabs 5.4e-5 — PASS
//    cost 0.067 ms/frame — same budget as Silero (<1 ms)
// ============================================================
#include "firered_wrapper.h"

#include "pocketfft_hdronly.h"
#include "onnxruntime_cxx_api.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <complex>
#include <string>
#include <vector>
#include <fstream>

#ifndef FIRERED_PI
#define FIRERED_PI 3.14159265358979323846
#endif

#define FR_FLEN 400
#define FR_NFFT 512
#define FR_NB   80
#define FR_NCACHE 8
#define FR_CACHELEN (128 * 19)

struct FireRedHandle {
    char lastError[256] = { 0 };

    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::string featName;
    std::string cacheNames[FR_NCACHE];
    std::string outName;
    std::string newCacheNames[FR_NCACHE];
    std::vector<float> caches[FR_NCACHE];

    double win[FR_FLEN];
    double melW[FR_NB][257];
    float cmvnMean[FR_NB];
    float cmvnIstd[FR_NB];

    float buf[FR_FLEN] = { 0 };  // sliding 400-sample window
    float accum[FIRERED_HOP] = { 0 };
    int accumUsed = 0;
};

static void FireRedSetError(FireRedHandle* h, const char* msg) {
    if (!h || !msg) return;
    strncpy(h->lastError, msg, sizeof(h->lastError) - 1);
    h->lastError[sizeof(h->lastError) - 1] = '\0';
}

static bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

static void FireRedInitFrontend(FireRedHandle* h) {
    for (int i = 0; i < FR_FLEN; i++)
        h->win[i] = pow(0.5 - 0.5 * cos(2.0 * FIRERED_PI * i / (FR_FLEN - 1)), 0.85);
    auto mel = [](double f) { return 2595.0 * log10(1.0 + f / 700.0); };
    auto inv = [](double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); };
    double mlo = mel(20.0), mhi = mel(8000.0);
    double pts[FR_NB + 2];
    for (int i = 0; i < FR_NB + 2; i++) pts[i] = inv(mlo + (mhi - mlo) * i / (FR_NB + 1));
    memset(h->melW, 0, sizeof(h->melW));
    for (int b = 0; b < FR_NB; b++)
        for (int k = 0; k < 257; k++) {
            double f = k * 16000.0 / 512.0, w = 0;
            if (f >= pts[b] && f <= pts[b + 1] && pts[b + 1] > pts[b])
                w = (f - pts[b]) / (pts[b + 1] - pts[b]);
            else if (f >= pts[b + 1] && f <= pts[b + 2] && pts[b + 2] > pts[b + 1])
                w = (pts[b + 2] - f) / (pts[b + 2] - pts[b + 1]);
            h->melW[b][k] = w;
        }
}

// One 400-sample window -> 80 CMVN-normalized log-mel features.
static void FireRedFeaturize(FireRedHandle* h, float feat[FR_NB]) {
    double x[FR_FLEN];
    double mean = 0;
    for (int i = 0; i < FR_FLEN; i++) mean += h->buf[i];
    mean /= FR_FLEN;
    for (int i = 0; i < FR_FLEN; i++) x[i] = h->buf[i] - mean;
    for (int i = FR_FLEN - 1; i > 0; i--) x[i] -= 0.97 * x[i - 1];
    double td[FR_NFFT] = { 0 };
    for (int i = 0; i < FR_FLEN; i++) td[i] = x[i] * h->win[i];
    std::vector<std::complex<double>> spec(FR_NFFT);
    std::vector<size_t> shape{ (size_t)FR_NFFT }, axes{ 0 };
    std::vector<ptrdiff_t> si{ sizeof(double) }, so{ sizeof(std::complex<double>) };
    pocketfft::r2c(shape, si, so, axes, pocketfft::FORWARD, td, spec.data(), 1.0);
    for (int b = 0; b < FR_NB; b++) {
        double e = 0;
        for (int k = 0; k < 257; k++) e += std::norm(spec[k]) * h->melW[b][k];
        float v = (float)log(e > 1e-10 ? e : 1e-10);
        feat[b] = (v - h->cmvnMean[b]) * h->cmvnIstd[b];
    }
}

static bool NameIsCache(const char* n) {
    std::string s = n ? n : "";
    for (auto& c : s) c = (char)tolower(c);
    return s.find("cache") != std::string::npos;
}

static int FireRedRunFrame(FireRedHandle* h, float* prob) {
    float feat[FR_NB];
    FireRedFeaturize(h, feat);
    try {
        int64_t sh[3] = { 1, 1, FR_NB }, shc[3] = { 1, 128, 19 };
        std::vector<Ort::Value> ins;
        ins.push_back(Ort::Value::CreateTensor<float>(*h->mem, feat, FR_NB, sh, 3));
        for (int c = 0; c < FR_NCACHE; c++)
            ins.push_back(Ort::Value::CreateTensor<float>(
                *h->mem, h->caches[c].data(), FR_CACHELEN, shc, 3));
        const char* inn[9];
        const char* oun[9];
        inn[0] = h->featName.c_str();
        for (int c = 0; c < FR_NCACHE; c++) inn[1 + c] = h->cacheNames[c].c_str();
        oun[0] = h->outName.c_str();
        for (int c = 0; c < FR_NCACHE; c++) oun[1 + c] = h->newCacheNames[c].c_str();
        auto outs = h->sess->Run(Ort::RunOptions{ nullptr }, inn, ins.data(), 9, oun, 9);
        *prob = outs[0].GetTensorMutableData<float>()[0];
        for (int c = 0; c < FR_NCACHE; c++) {
            float* p = outs[1 + c].GetTensorMutableData<float>();
            size_t n = outs[1 + c].GetTensorTypeAndShapeInfo().GetElementCount();
            if (n == FR_CACHELEN) memcpy(h->caches[c].data(), p, n * sizeof(float));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

// ---------------- C API ----------------
extern "C" {

FireRedHandle* FireRedNew(const char* modelPath, const char* cmvnPath) {
    auto* h = new FireRedHandle();
    std::string mp = (modelPath && *modelPath) ? modelPath : "models/model_with_caches.onnx";
    std::string cp = (cmvnPath && *cmvnPath) ? cmvnPath : "models/firered_cmvn.bin";
    if (!FileExists(mp)) {
        FireRedSetError(h, "FireRed model not found in models/ (see README)");
        delete h;
        return nullptr;  // fail-open: caller treats null as gate-off
    }
    std::ifstream cf(cp, std::ios::binary);
    if (!cf.good()) {
        FireRedSetError(h, "FireRed CMVN file not found in models/");
        delete h;
        return nullptr;
    }
    float vec[160];
    cf.read(reinterpret_cast<char*>(vec), sizeof(vec));
    if (!cf) {
        FireRedSetError(h, "FireRed CMVN file truncated");
        delete h;
        return nullptr;
    }
    memcpy(h->cmvnMean, vec, 80 * sizeof(float));
    memcpy(h->cmvnIstd, vec + 80, 80 * sizeof(float));
    FireRedInitFrontend(h);
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "firered");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::string p(mp);
        std::wstring w(p.begin(), p.end());
        h->sess = new Ort::Session(*h->env, w.c_str(), *h->opts);
        Ort::AllocatorWithDefaultOptions alloc;
        int ci = 0, co = 0;
        for (size_t i = 0; i < h->sess->GetInputCount(); i++) {
            std::string n = h->sess->GetInputNameAllocated(i, alloc).get();
            if (NameIsCache(n.c_str())) {
                if (ci < FR_NCACHE) h->cacheNames[ci++] = n;
            } else if (h->featName.empty()) {
                h->featName = n;
            }
        }
        for (size_t i = 0; i < h->sess->GetOutputCount(); i++) {
            std::string n = h->sess->GetOutputNameAllocated(i, alloc).get();
            if (NameIsCache(n.c_str())) {
                if (co < FR_NCACHE) h->newCacheNames[co++] = n;
            } else if (h->outName.empty()) {
                h->outName = n;
            }
        }
        if (h->featName.empty() || ci != FR_NCACHE ||
            h->outName.empty() || co != FR_NCACHE) {
            FireRedSetError(h, "FireRed: unexpected model I/O layout");
            FireRedDestroy(h);
            return nullptr;
        }
        for (int c = 0; c < FR_NCACHE; c++)
            h->caches[c].assign(FR_CACHELEN, 0.0f);
        return h;
    } catch (const std::exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "FireRed ONNX: %s", e.what());
        FireRedSetError(h, buf);
        FireRedDestroy(h);
        return nullptr;
    } catch (...) {
        FireRedSetError(h, "FireRed ONNX: failed to load model");
        FireRedDestroy(h);
        return nullptr;
    }
}

int FireRedPush(FireRedHandle* h, const float* pcm, int n, float* prob) {
    if (!h || !h->sess || !pcm || n <= 0) return 0;
    int done = 0;
    int off = 0;
    while (off < n) {
        int take = FIRERED_HOP - h->accumUsed;
        if (take > n - off) take = n - off;
        memcpy(h->accum + h->accumUsed, pcm + off, take * sizeof(float));
        h->accumUsed += take;
        off += take;
        if (h->accumUsed == FIRERED_HOP) {
            h->accumUsed = 0;
            memmove(h->buf, h->buf + FIRERED_HOP, (FR_FLEN - FIRERED_HOP) * sizeof(float));
            memcpy(h->buf + FR_FLEN - FIRERED_HOP, h->accum, FIRERED_HOP * sizeof(float));
            float p = 0.0f;
            if (FireRedRunFrame(h, &p)) {
                if (prob) *prob = p;
                done = 1;
            }
        }
    }
    return done;
}

void FireRedReset(FireRedHandle* h) {
    if (!h) return;
    for (int c = 0; c < FR_NCACHE; c++)
        std::fill(h->caches[c].begin(), h->caches[c].end(), 0.0f);
    memset(h->buf, 0, sizeof(h->buf));
    memset(h->accum, 0, sizeof(h->accum));
    h->accumUsed = 0;
}

void FireRedDestroy(FireRedHandle* h) {
    if (!h) return;
    delete h->sess;
    delete h->opts;
    delete h->mem;
    delete h->env;
    delete h;
}

const char* FireRedLastError(FireRedHandle* h) {
    if (!h || !h->lastError[0]) return "No FireRed context";
    return h->lastError;
}

} // extern "C"
