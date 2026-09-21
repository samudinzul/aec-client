// ============================================================
//  DEC residual-echo cleanup wrapper — Microsoft AEC-Challenge
//  ICASSP 2022 baseline (MIT, 5.2 MB), via ONNX Runtime.
//
//  Faithful port of upstream enhance.py to streaming C++:
//    16 kHz, 20 ms sqrt-Hann window, 10 ms hop, 320-point DFT,
//    concat(log10(mic mag^2), log10(far mag^2)) / 20 as [1,1,322]
//    features, 2xGRU-322 states, magnitude mask, windowed OLA.
//
//  DFT is naive O(N^2) against precomputed tables (161x320):
//  ~150K MACs/frame at 100 fps is nothing next to the GRU.
//  Tables + window are built once in DecNew; the audio thread
//  never allocates. Mask is finite-checked before use — any
//  garbage fails open to a dry passthrough, never a blast.
// ============================================================
#include "dec_wrapper.h"

#include "onnxruntime_cxx_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#define DEC_N 320
#define DEC_HOP 160
#define DEC_BINS 161
#define DEC_HIDDEN 322

struct DecHandle {
    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::string inFeatName, inH01Name, inH02Name;
    std::string outMaskName, outH01Name, outH02Name;

    float* cosT = nullptr;   // [DEC_BINS * DEC_N] cos(2*pi*k*n/N)
    float* sinT = nullptr;   // [DEC_BINS * DEC_N] sin(2*pi*k*n/N)
    float win[DEC_N] = { 0 };

    float h01[DEC_HIDDEN] = { 0 };
    float h02[DEC_HIDDEN] = { 0 };
    float inOv[DEC_HOP] = { 0 };    // previous hop, cleaned channel
    float refOv[DEC_HOP] = { 0 };   // previous hop, far-end channel
    float olaCarry[DEC_HOP] = { 0 };  // second half of previous enh frame

    bool ioChecked = false;
    char lastError[256] = { 0 };
};

static void DecFail(DecHandle* h, const char* msg) {
    if (!h) return;
    snprintf(h->lastError, sizeof(h->lastError), "%s", msg);
}

static bool DecFileExists(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

DecHandle* DecNew(const char* onnxPath) {
    DecHandle* h = new (std::nothrow) DecHandle();
    if (!h) return nullptr;
    if (!onnxPath || !DecFileExists(onnxPath)) {
        DecFail(h, "DEC model not found in models/ (see MODELS.md)");
        return h;  // non-null but unusable; Process refuses
    }
    // DFT tables + sqrt-Hann window (built once, never on audio thread).
    h->cosT = new (std::nothrow) float[DEC_BINS * DEC_N];
    h->sinT = new (std::nothrow) float[DEC_BINS * DEC_N];
    if (!h->cosT || !h->sinT) {
        DecFail(h, "DEC table alloc failed");
        return h;
    }
    const float pi2_N = 2.0f * 3.14159265358979323846f / (float)DEC_N;
    for (int k = 0; k < DEC_BINS; k++)
        for (int n = 0; n < DEC_N; n++) {
            float ph = pi2_N * (float)(k * n);
            h->cosT[k * DEC_N + n] = cosf(ph);
            h->sinT[k * DEC_N + n] = sinf(ph);
        }
    for (int n = 0; n < DEC_N; n++) {
        float hann = 0.5f - 0.5f * cosf(pi2_N * (float)n);
        h->win[n] = sqrtf(hann > 0.0f ? hann : 0.0f);
    }
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "dec");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::string p(onnxPath);
        std::wstring w(p.begin(), p.end());
        h->sess = new Ort::Session(*h->env, w.c_str(), *h->opts);
        Ort::AllocatorWithDefaultOptions alloc;
        h->inFeatName = h->sess->GetInputNameAllocated(0, alloc).get();
        h->inH01Name = h->sess->GetInputNameAllocated(1, alloc).get();
        h->inH02Name = h->sess->GetInputNameAllocated(2, alloc).get();
        h->outMaskName = h->sess->GetOutputNameAllocated(0, alloc).get();
        h->outH01Name = h->sess->GetOutputNameAllocated(1, alloc).get();
        h->outH02Name = h->sess->GetOutputNameAllocated(2, alloc).get();
    } catch (const Ort::Exception& ex) {
        DecFail(h, ex.what());
    } catch (const std::exception& ex) {
        DecFail(h, ex.what());
    }
    return h;
}

int DecUsable(DecHandle* h) {
    return (h && h->sess && h->cosT && h->sinT) ? 1 : 0;
}

// Windowed forward DFT of one frame into re/im (bins 0..160).
static void DecForward(const DecHandle* h, const float* frame,
                       float* re, float* im) {
    for (int k = 0; k < DEC_BINS; k++) {
        const float* c = h->cosT + k * DEC_N;
        const float* s = h->sinT + k * DEC_N;
        float r = 0.0f, imv = 0.0f;
        for (int n = 0; n < DEC_N; n++) {
            float xw = frame[n] * h->win[n];
            r += xw * c[n];
            imv -= xw * s[n];
        }
        re[k] = r;
        im[k] = imv;
    }
}

int DecProcess(DecHandle* h, const float* cleaned, const float* ref,
               float* out, int n) {
    if (!DecUsable(h) || !cleaned || !ref || !out || n != DEC_HOP) return 0;
    h->lastError[0] = 0;

    // Assemble windowed frames (previous hop + new hop).
    float frame[DEC_N], frameRef[DEC_N];
    memcpy(frame, h->inOv, sizeof(h->inOv));
    memcpy(frame + DEC_HOP, cleaned, (size_t)DEC_HOP * sizeof(float));
    memcpy(frameRef, h->refOv, sizeof(h->refOv));
    memcpy(frameRef + DEC_HOP, ref, (size_t)DEC_HOP * sizeof(float));
    memcpy(h->inOv, cleaned, sizeof(h->inOv));
    memcpy(h->refOv, ref, sizeof(h->refOv));

    float reM[DEC_BINS], imM[DEC_BINS], reF[DEC_BINS], imF[DEC_BINS];
    DecForward(h, frame, reM, imM);
    DecForward(h, frameRef, reF, imF);

    float feat[2 * DEC_BINS];
    for (int k = 0; k < DEC_BINS; k++) {
        float pm = reM[k] * reM[k] + imM[k] * imM[k];
        float pf = reF[k] * reF[k] + imF[k] * imF[k];
        if (pm < 1e-12f) pm = 1e-12f;
        if (pf < 1e-12f) pf = 1e-12f;
        feat[k] = log10f(pm) / 20.0f;
        feat[DEC_BINS + k] = log10f(pf) / 20.0f;
    }

    try {
        std::vector<int64_t> featShape = { 1, 1, 2 * DEC_BINS };
        std::vector<int64_t> stShape = { 1, 1, DEC_HIDDEN };
        Ort::Value tFeat = Ort::Value::CreateTensor<float>(
            *h->mem, feat, 2 * DEC_BINS, featShape.data(), featShape.size());
        Ort::Value tH01 = Ort::Value::CreateTensor<float>(
            *h->mem, h->h01, DEC_HIDDEN, stShape.data(), stShape.size());
        Ort::Value tH02 = Ort::Value::CreateTensor<float>(
            *h->mem, h->h02, DEC_HIDDEN, stShape.data(), stShape.size());
        const char* inNames[] = {
            h->inFeatName.c_str(), h->inH01Name.c_str(), h->inH02Name.c_str()
        };
        const char* outNames[] = {
            h->outMaskName.c_str(), h->outH01Name.c_str(), h->outH02Name.c_str()
        };
        Ort::Value inputs[] = { std::move(tFeat), std::move(tH01), std::move(tH02) };
        auto outs = h->sess->Run(Ort::RunOptions{ nullptr },
                                 inNames, inputs, 3, outNames, 3);
        auto ti0 = outs[0].GetTensorTypeAndShapeInfo();
        auto ti1 = outs[1].GetTensorTypeAndShapeInfo();
        auto ti2 = outs[2].GetTensorTypeAndShapeInfo();
        if (!h->ioChecked) {
            if (ti0.GetElementCount() != DEC_BINS ||
                ti1.GetElementCount() != DEC_HIDDEN ||
                ti2.GetElementCount() != DEC_HIDDEN) {
                DecFail(h, "DEC model I/O shape mismatch");
                return 0;
            }
            h->ioChecked = true;
        }
        const float* mask = outs[0].GetTensorData<float>();
        // Fail-open: a garbage mask must pass audio through, never blast.
        for (int k = 0; k < DEC_BINS; k++) {
            float m = mask[k];
            if (!(m >= 0.0f) || !(m <= 10.0f)) {
                DecFail(h, "DEC mask out of range, bypassing frame");
                return 0;
            }
        }
        memcpy(h->h01, outs[1].GetTensorData<float>(), sizeof(h->h01));
        memcpy(h->h02, outs[2].GetTensorData<float>(), sizeof(h->h02));

        // Masked inverse DFT (conjugate symmetry) + window + overlap-add.
        float enh[DEC_N];
        for (int i = 0; i < DEC_N; i++) {
            float acc = mask[0] * reM[0];
            float nyq = mask[DEC_BINS - 1] * reM[DEC_BINS - 1];
            acc += ((i & 1) ? -nyq : nyq);
            for (int k = 1; k < DEC_BINS - 1; k++) {
                float yr = mask[k] * reM[k];
                float yi = mask[k] * imM[k];
                const float* c = h->cosT + k * DEC_N;
                const float* s = h->sinT + k * DEC_N;
                acc += 2.0f * (yr * c[i] - yi * s[i]);
            }
            enh[i] = acc * (1.0f / (float)DEC_N) * h->win[i];
        }
        for (int i = 0; i < DEC_HOP; i++) {
            out[i] = h->olaCarry[i] + enh[i];
            h->olaCarry[i] = enh[DEC_HOP + i];
        }
        return 1;
    } catch (const Ort::Exception& ex) {
        DecFail(h, ex.what());
        return 0;
    }
}

void DecReset(DecHandle* h) {
    if (!h) return;
    memset(h->h01, 0, sizeof(h->h01));
    memset(h->h02, 0, sizeof(h->h02));
    memset(h->inOv, 0, sizeof(h->inOv));
    memset(h->refOv, 0, sizeof(h->refOv));
    memset(h->olaCarry, 0, sizeof(h->olaCarry));
    h->lastError[0] = 0;
}

void DecDestroy(DecHandle* h) {
    if (!h) return;
    delete h->sess;
    delete h->mem;
    delete h->opts;
    delete h->env;
    delete[] h->cosT;
    delete[] h->sinT;
    delete h;
}

const char* DecLastError(DecHandle* h) {
    if (!h) return "null handle";
    if (!h->sess) return h->lastError[0] ? h->lastError : "model not loaded";
    return h->lastError[0] ? h->lastError : "ok";
}
