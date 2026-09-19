// ============================================================
//  PVAD wrapper — personalized voice gate (identity layer).
//
//  Timing (WHEN to open) stays with the Silero gate. This wrapper
//  answers WHO: ECAPA-TDNN embedding (192-d) cosine-scored against
//  an enrolled voiceprint. Proven in the temp harness: same-voice
//  0.840 vs cross-voice 0.673 on synthetic voices (real voices
//  separate wider; 0.60 default threshold awaits ear calibration).
//
//  Model: vedk00/ecapa-voxceleb-speaker-embedding-onnx (Apache-2.0).
//  Frontend: kaldi-style 80-bin log-mel frontend (25 ms window /
//  10 ms hop, povey, preemphasis, DC cut, log floor 1e-10) +
//  utterance mean-norm.
// ============================================================
#include "pvad_wrapper.h"

#include "pocketfft_hdronly.h"
#include "onnxruntime_cxx_api.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <complex>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>

#ifndef PVAD_PI
#define PVAD_PI 3.14159265358979323846
#endif

#define PV_FLEN 400
#define PV_FSHIFT 160
#define PV_NFFT 512
#define PV_NB 80
#define PV_ENROLL_MAX (16000 * 10)
#define PV_ENROLL_MIN 16000

struct PvadHandle {
    char lastError[256] = { 0 };
    std::mutex mtx;

    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess = nullptr;
    Ort::MemoryInfo* mem = nullptr;

    double win[PV_FLEN];
    double melW[PV_NB][257];

    std::vector<float> enrolled;  // 192-d voiceprint, empty = none
    float threshold = PVAD_DEFAULT_THRESHOLD;

    std::vector<float> enrollBuf;  // enrollment accumulation (float mono)
    bool enrolling = false;
};

static void PvadSetError(PvadHandle* h, const char* msg) {
    if (!h || !msg) return;
    strncpy(h->lastError, msg, sizeof(h->lastError) - 1);
    h->lastError[sizeof(h->lastError) - 1] = '\0';
}

static bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

static void PvadInitFrontend(PvadHandle* h) {
    for (int i = 0; i < PV_FLEN; i++)
        h->win[i] = pow(0.5 - 0.5 * cos(2.0 * PVAD_PI * i / (PV_FLEN - 1)), 0.85);
    auto mel = [](double f) { return 2595.0 * log10(1.0 + f / 700.0); };
    auto inv = [](double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); };
    double mlo = mel(20.0), mhi = mel(8000.0);
    double pts[PV_NB + 2];
    for (int i = 0; i < PV_NB + 2; i++) pts[i] = inv(mlo + (mhi - mlo) * i / (PV_NB + 1));
    memset(h->melW, 0, sizeof(h->melW));
    for (int b = 0; b < PV_NB; b++)
        for (int k = 0; k < 257; k++) {
            double f = k * 16000.0 / 512.0, w = 0;
            if (f >= pts[b] && f <= pts[b + 1] && pts[b + 1] > pts[b])
                w = (f - pts[b]) / (pts[b + 1] - pts[b]);
            else if (f >= pts[b + 1] && f <= pts[b + 2] && pts[b + 2] > pts[b + 1])
                w = (pts[b + 2] - f) / (pts[b + 2] - pts[b + 1]);
            h->melW[b][k] = w;
        }
}

// Featurize n samples (16 kHz float mono) -> mean-normed [nf x 80].
// Returns frame count (0 if too short).
static int PvadFeaturize(PvadHandle* h, const float* sig, int n,
                         std::vector<float>& feat) {
    if (n < PV_FLEN) return 0;
    int nf = 1 + (n - PV_FLEN) / PV_FSHIFT;
    feat.assign((size_t)nf * PV_NB, 0.0f);
    std::vector<std::complex<double>> spec(PV_NFFT);
    std::vector<size_t> shape{ (size_t)PV_NFFT }, axes{ 0 };
    std::vector<ptrdiff_t> si{ sizeof(double) }, so{ sizeof(std::complex<double>) };
    double td[PV_NFFT];
    for (int f = 0; f < nf; f++) {
        const float* s = sig + f * PV_FSHIFT;
        double mean = 0;
        for (int i = 0; i < PV_FLEN; i++) mean += s[i];
        mean /= PV_FLEN;
        double x[PV_FLEN];
        for (int i = 0; i < PV_FLEN; i++) x[i] = s[i] - mean;
        for (int i = PV_FLEN - 1; i > 0; i--) x[i] -= 0.97 * x[i - 1];
        memset(td, 0, sizeof(td));
        for (int i = 0; i < PV_FLEN; i++) td[i] = x[i] * h->win[i];
        pocketfft::r2c(shape, si, so, axes, pocketfft::FORWARD, td, spec.data(), 1.0);
        for (int b = 0; b < PV_NB; b++) {
            double e = 0;
            for (int k = 0; k < 257; k++) e += std::norm(spec[k]) * h->melW[b][k];
            feat[(size_t)f * PV_NB + b] = (float)log(e > 1e-10 ? e : 1e-10);
        }
    }
    for (int b = 0; b < PV_NB; b++) {
        double m = 0;
        for (int f = 0; f < nf; f++) m += feat[(size_t)f * PV_NB + b];
        m /= nf;
        for (int f = 0; f < nf; f++) feat[(size_t)f * PV_NB + b] -= (float)m;
    }
    return nf;
}

// Caller holds h->mtx. Returns 1 and fills emb[192] on success.
static int PvadEmbedLocked(PvadHandle* h, const float* sig, int n,
                           std::vector<float>& emb) {
    std::vector<float> feat;
    int nf = PvadFeaturize(h, sig, n, feat);
    if (nf < 10) return 0;
    try {
        int64_t sh[3] = { 1, nf, PV_NB }, sl[1] = { 1 };
        float one = 1.0f;
        Ort::Value ins[2] = {
            Ort::Value::CreateTensor<float>(*h->mem, feat.data(), feat.size(), sh, 3),
            Ort::Value::CreateTensor<float>(*h->mem, &one, 1, sl, 1),
        };
        const char* inn[2] = { "features", "feature_lens" };
        const char* oun[1] = { "embedding" };
        auto outs = h->sess->Run(Ort::RunOptions{ nullptr }, inn, ins, 2, oun, 1);
        float* e = outs[0].GetTensorMutableData<float>();
        size_t c = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        if (c != PVAD_EMB) return 0;
        emb.assign(e, e + PVAD_EMB);
        return 1;
    } catch (...) {
        return 0;
    }
}

static float PvadCosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != PVAD_EMB || b.size() != PVAD_EMB) return -2.0f;
    double d = 0, na = 0, nb = 0;
    for (int i = 0; i < PVAD_EMB; i++) {
        d += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    return (float)(d / (sqrt(na * nb) + 1e-12));
}

// ---------------- C API ----------------
extern "C" {

PvadHandle* PvadNew(const char* modelPath) {
    auto* h = new PvadHandle();
    std::string mp = (modelPath && *modelPath) ? modelPath : "models/ecapa-speaker-v1.onnx";
    if (!FileExists(mp)) {
        PvadSetError(h, "PVAD model not found in models/ (see README)");
        delete h;
        return nullptr;
    }
    PvadInitFrontend(h);
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "pvad");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::string p(mp);
        std::wstring w(p.begin(), p.end());
        h->sess = new Ort::Session(*h->env, w.c_str(), *h->opts);
        h->enrollBuf.reserve(PV_ENROLL_MAX);
        return h;
    } catch (const std::exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "PVAD ONNX: %s", e.what());
        PvadSetError(h, buf);
        delete h->sess;
        delete h->opts;
        delete h->mem;
        delete h->env;
        delete h;
        return nullptr;
    } catch (...) {
        PvadSetError(h, "PVAD ONNX: failed to load model");
        delete h;
        return nullptr;
    }
}

void PvadDestroy(PvadHandle* h) {
    if (!h) return;
    delete h->sess;
    delete h->opts;
    delete h->mem;
    delete h->env;
    delete h;
}

const char* PvadLastError(PvadHandle* h) {
    if (!h || !h->lastError[0]) return "No PVAD context";
    return h->lastError;
}

void PvadBeginEnroll(PvadHandle* h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->mtx);
    h->enrollBuf.clear();
    h->enrolling = true;
}

void PvadFeedEnroll(PvadHandle* h, const float* pcm, int n) {
    if (!h || !pcm || n <= 0) return;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (!h->enrolling) return;
    size_t room = PV_ENROLL_MAX > h->enrollBuf.size()
        ? PV_ENROLL_MAX - h->enrollBuf.size() : 0;
    size_t take = (size_t)n < room ? (size_t)n : room;
    h->enrollBuf.insert(h->enrollBuf.end(), pcm, pcm + take);
}

int PvadFinishEnroll(PvadHandle* h) {
    if (!h) return 0;
    std::lock_guard<std::mutex> lk(h->mtx);
    h->enrolling = false;
    if (!h->sess || h->enrollBuf.size() < (size_t)PV_ENROLL_MIN) return 0;
    std::vector<float> emb;
    if (!PvadEmbedLocked(h, h->enrollBuf.data(), (int)h->enrollBuf.size(), emb))
        return 0;
    h->enrolled = emb;
    h->enrollBuf.clear();
    h->enrollBuf.shrink_to_fit();
    return 1;
}

int PvadHasVoiceprint(PvadHandle* h) {
    if (!h) return 0;
    std::lock_guard<std::mutex> lk(h->mtx);
    return h->enrolled.size() == PVAD_EMB ? 1 : 0;
}

void PvadClearVoiceprint(PvadHandle* h) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->mtx);
    h->enrolled.clear();
    h->enrolled.shrink_to_fit();
    h->enrolling = false;
    h->enrollBuf.clear();
    h->enrollBuf.shrink_to_fit();
}

float PvadVerify(PvadHandle* h, const float* pcm, int n) {
    if (!h || !pcm || n <= 0) return -2.0f;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (!h->sess || h->enrolled.size() != PVAD_EMB) return -2.0f;  // fail-open
    std::vector<float> emb;
    if (!PvadEmbedLocked(h, pcm, n, emb)) return -2.0f;  // fail-open
    return PvadCosine(h->enrolled, emb);
}

int PvadSave(PvadHandle* h, const char* path) {
    if (!h || !path) return 0;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (h->enrolled.size() != PVAD_EMB) return 0;
    std::ofstream f(path, std::ios::binary);
    if (!f.good()) return 0;
    float thr = h->threshold;
    f.write(reinterpret_cast<const char*>(h->enrolled.data()), PVAD_EMB * sizeof(float));
    f.write(reinterpret_cast<const char*>(&thr), sizeof(thr));
    return f.good() ? 1 : 0;
}

int PvadLoad(PvadHandle* h, const char* path) {
    if (!h || !path) return 0;
    std::lock_guard<std::mutex> lk(h->mtx);
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return 0;
    std::vector<float> emb(PVAD_EMB);
    f.read(reinterpret_cast<char*>(emb.data()), PVAD_EMB * sizeof(float));
    float thr = PVAD_DEFAULT_THRESHOLD;
    f.read(reinterpret_cast<char*>(&thr), sizeof(thr));
    if (!f) return 0;
    h->enrolled = emb;
    if (thr > 0.0f && thr < 1.0f) h->threshold = thr;
    return 1;
}

void PvadSetThreshold(PvadHandle* h, float t) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (t > 0.0f && t < 1.0f) h->threshold = t;
}

float PvadThreshold(PvadHandle* h) {
    if (!h) return PVAD_DEFAULT_THRESHOLD;
    std::lock_guard<std::mutex> lk(h->mtx);
    return h->threshold;
}

} // extern "C"
