// ============================================================
//  GTCRN wrapper — NKF "Dry voice" enhancement stage
//  (Xiaobin-Rong/gtcrn streaming ONNX export, MIT, ~535 KB)
//
//  Ultra-light 16 kHz speech enhancement (ShuffleNetV2 + SFE +
//  TRA + 2x DPGRNN): drier mic with less room reverb on top of
//  echo cancellation. RTF ~0.07 on a desktop CPU core.
//
//  Streaming contract (mirrors gtcrn_stream.py / the ONNX export):
//    - STFT n_fft=512, hop=256, sqrt(periodic hann), 16 kHz
//    - per hop: spec (1,257,1,2) in -> enhanced spec out, with
//      conv/tra/inter caches carried across calls
//    - analysis: frame * w  -> r2c; synthesis: c2r / N * w -> OLA
//      (hann OLA with hop=N/2 sums to 1: no envelope division)
//    - center=True framing: frame j covers [jH-H, jH+H); the pad
//      half of frame 0 is discarded; 512 zero samples prefilled on
//      the output side keep the API exactly 1:1 from sample one
//      (~32 ms startup latency, then constant).
//
//  No link-time deps beyond the already-linked ONNX Runtime and
//  the pocketfft header NKF/DTLN already use for FFTs.
// ============================================================
#include "gtcrn_wrapper.h"

#include "onnxruntime_cxx_api.h"
#include "pocketfft_hdronly.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr int kNfft  = GTCRN_WIN;              // 512
constexpr int kHop   = GTCRN_HOP;              // 256
constexpr int kBins  = kNfft / 2 + 1;          // 257
constexpr int kMixN  = kBins * 2;              // (1,257,1,2) floats
constexpr int kConvN = 2 * 1 * 16 * 16 * 33;   // 16896
constexpr int kTraN  = 2 * 3 * 1 * 1 * 16;     // 96
constexpr int kIntN  = 2 * 1 * 33 * 16;        // 1056
constexpr int kPrefill = 512;                  // 1:1 priming (frame-0 gap)
constexpr int kRingCap  = 4096;                // >> steady-state ~768
constexpr int kQCap     = 1024;                // >= 512 max call + 255

}  // namespace

struct GtcrnHandle {
    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::string inNames[4], outNames[4];

    float w[kNfft];                  // sqrt(periodic hann)
    float prevHop[kHop];             // previous hop (zeros at start)
    float q[kQCap];                  // input awaiting hop framing
    int qn = 0;
    float ola[kNfft];                // pending overlap-add region
    int frameIdx = 0;                // next frame to run

    float ring[kRingCap];            // output FIFO (prefilled)
    int rHead = 0, rCount = 0;

    float mix[kMixN];                // model input / enh output
    float conv[kConvN], tra[kTraN], inter[kIntN];
    float frameTd[kNfft];            // synthesis scratch
    double anaX[kNfft];              // analysis scratch (double)
    double td[kNfft];                // synthesis time-domain scratch
    std::complex<double> spec[kBins];

    // Hoisted FFT descriptors + ONNX shape vectors (no per-hop heap)
    std::vector<size_t>    fftShape;
    std::vector<size_t>    fftAxes;
    std::vector<ptrdiff_t> fftStrideIn;
    std::vector<ptrdiff_t> fftStrideOut;
    int64_t mixShape[4] = { 1, kBins, 1, 2 };
    int64_t convShape[5] = { 2, 1, 16, 16, 33 };
    int64_t traShape[5]  = { 2, 3, 1, 1, 16 };
    int64_t intShape[4]  = { 2, 1, 33, 16 };

    bool failed = false;
    char lastError[256] = {0};
};

static void GtFail(GtcrnHandle* h, const char* msg) {
    if (!h) return;
    snprintf(h->lastError, sizeof(h->lastError), "%s", msg);
}

static bool FileExists(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

static void RingReset(GtcrnHandle* h) {
    memset(h->ring, 0, sizeof(h->ring));
    h->rHead = 0;
    h->rCount = kPrefill;
}

static void RingPush(GtcrnHandle* h, float v) {
    if (h->rCount >= kRingCap) {
        h->failed = true;
        GtFail(h, "dry-voice output FIFO overflow");
        return;
    }
    h->ring[(h->rHead + h->rCount) % kRingCap] = v;
    h->rCount++;
}

// One hop through analysis -> model -> synthesis -> OLA -> emit.
static bool GtRunFrame(GtcrnHandle* h) {
    try {
        // Analysis: frame = prevHop || first hop of q, windowed rfft.
        for (int i = 0; i < kHop; i++) {
            h->anaX[i]        = (double)h->prevHop[i] * (double)h->w[i];
            h->anaX[kHop + i] = (double)h->q[i]       * (double)h->w[kHop + i];
        }
        pocketfft::r2c(h->fftShape, h->fftStrideIn, h->fftStrideOut,
                       h->fftAxes, pocketfft::FORWARD,
                       h->anaX, h->spec, 1.0);
        for (int f = 0; f < kBins; f++) {
            h->mix[f * 2]     = (float)h->spec[f].real();
            h->mix[f * 2 + 1] = (float)h->spec[f].imag();
        }

        // Model: one hop, caches in/out (same buffers, copy back).
        Ort::Value tMix = Ort::Value::CreateTensor<float>(
            *h->mem, h->mix, kMixN, h->mixShape, 4);
        Ort::Value tConv = Ort::Value::CreateTensor<float>(
            *h->mem, h->conv, kConvN, h->convShape, 5);
        Ort::Value tTra = Ort::Value::CreateTensor<float>(
            *h->mem, h->tra, kTraN, h->traShape, 5);
        Ort::Value tInt = Ort::Value::CreateTensor<float>(
            *h->mem, h->inter, kIntN, h->intShape, 4);
        const char* inN[] = {
            h->inNames[0].c_str(), h->inNames[1].c_str(),
            h->inNames[2].c_str(), h->inNames[3].c_str()
        };
        const char* outN[] = {
            h->outNames[0].c_str(), h->outNames[1].c_str(),
            h->outNames[2].c_str(), h->outNames[3].c_str()
        };
        Ort::Value inputs[] = { std::move(tMix), std::move(tConv),
                                std::move(tTra), std::move(tInt) };
        auto outs = h->sess->Run(Ort::RunOptions{ nullptr },
                                 inN, inputs, 4, outN, 4);
        float* enh = outs[0].GetTensorMutableData<float>();
        float* cO  = outs[1].GetTensorMutableData<float>();
        float* tO  = outs[2].GetTensorMutableData<float>();
        float* iO  = outs[3].GetTensorMutableData<float>();
        memcpy(h->conv, cO, sizeof(h->conv));
        memcpy(h->tra,  tO, sizeof(h->tra));
        memcpy(h->inter, iO, sizeof(h->inter));

        // Synthesis: enhanced bins -> irfft (unnorm / N) * w -> OLA.
        for (int f = 0; f < kBins; f++)
            h->spec[f] = std::complex<double>((double)enh[f * 2],
                                              (double)enh[f * 2 + 1]);
        pocketfft::c2r(h->fftShape, h->fftStrideOut, h->fftStrideIn,
                       h->fftAxes, pocketfft::BACKWARD,
                       h->spec, h->td, 1.0);
        for (int i = 0; i < kNfft; i++)
            h->frameTd[i] = (float)(h->td[i] / (double)kNfft) * h->w[i];
        for (int i = 0; i < kNfft; i++)
            h->ola[i] += h->frameTd[i];

        // Emit completed region; frame 0's pad half is discarded.
        if (h->frameIdx >= 1) {
            for (int i = 0; i < kHop; i++) RingPush(h, h->ola[i]);
        }
        memmove(h->ola, h->ola + kHop, kHop * sizeof(float));
        memset(h->ola + kHop, 0, kHop * sizeof(float));
        h->frameIdx++;

        // Next frame: this hop becomes prev.
        memcpy(h->prevHop, h->q, kHop * sizeof(float));
        memmove(h->q, h->q + kHop, (size_t)(h->qn - kHop) * sizeof(float));
        h->qn -= kHop;
        return true;
    } catch (const Ort::Exception& ex) {
        GtFail(h, ex.what());
        return false;
    } catch (const std::exception& ex) {
        GtFail(h, ex.what());
        return false;
    }
}

extern "C" {

GtcrnHandle* GtcrnNew(const char* onnxPath) {
    GtcrnHandle* h = new (std::nothrow) GtcrnHandle();
    if (!h) return nullptr;
    if (!onnxPath || !FileExists(onnxPath)) {
        GtFail(h, "GTCRN model not found in models/ (Dry voice stays off)");
        return h;  // non-null but unusable; GtcrnReady() == 0
    }
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "gtcrn");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::string p(onnxPath);
        std::wstring w(p.begin(), p.end());
        h->sess = new Ort::Session(*h->env, w.c_str(), *h->opts);
        Ort::AllocatorWithDefaultOptions alloc;
        for (int i = 0; i < 4; i++) {
            h->inNames[i]  = h->sess->GetInputNameAllocated((size_t)i, alloc).get();
            h->outNames[i] = h->sess->GetOutputNameAllocated((size_t)i, alloc).get();
        }
    } catch (const Ort::Exception& ex) {
        GtFail(h, ex.what());
    } catch (const std::exception& ex) {
        GtFail(h, ex.what());
    }
    if (!h->sess) return h;  // unusable, caller checks Ready

    h->fftShape.assign(1, (size_t)kNfft);
    h->fftAxes.assign(1, (size_t)0);
    h->fftStrideIn.assign(1, (ptrdiff_t)sizeof(double));
    h->fftStrideOut.assign(1, (ptrdiff_t)sizeof(std::complex<double>));

    // sqrt(periodic hann): analysis * synthesis = hann, hop N/2 OLA = 1.
    for (int i = 0; i < kNfft; i++) {
        const float c = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979323846f *
                                           (float)i / (float)kNfft);
        h->w[i] = sqrtf(c > 0.0f ? c : 0.0f);
    }
    memset(h->conv,  0, sizeof(h->conv));
    memset(h->tra,   0, sizeof(h->tra));
    memset(h->inter, 0, sizeof(h->inter));
    RingReset(h);
    return h;
}

int GtcrnReady(const GtcrnHandle* h) {
    return (h && h->sess) ? 1 : 0;
}

int GtcrnProcess(GtcrnHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    if (!h->sess) return 0;
    if (h->failed) {  // latched pass-through
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    // Absorb input, run every completed hop (each consumes kHop).
    if (h->qn + n > kQCap) {  // cannot happen with n <= 512, defend anyway
        h->failed = true;
        GtFail(h, "dry-voice input overflow");
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    memcpy(h->q + h->qn, in, (size_t)n * sizeof(float));
    h->qn += n;
    while (h->qn >= kHop && !h->failed) {
        if (!GtRunFrame(h)) h->failed = true;
    }
    if (h->failed) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    // Emit exactly n; ring is prefilled so this never runs dry in
    // practice — pad zeros defensively rather than desync.
    for (int i = 0; i < n; i++) {
        if (h->rCount > 0) {
            out[i] = h->ring[h->rHead];
            h->rHead = (h->rHead + 1) % kRingCap;
            h->rCount--;
        } else {
            out[i] = 0.0f;
        }
    }
    return 1;
}

void GtcrnReset(GtcrnHandle* h) {
    if (!h) return;
    memset(h->prevHop, 0, sizeof(h->prevHop));
    h->qn = 0;
    memset(h->ola, 0, sizeof(h->ola));
    h->frameIdx = 0;
    memset(h->conv,  0, sizeof(h->conv));
    memset(h->tra,   0, sizeof(h->tra));
    memset(h->inter, 0, sizeof(h->inter));
    RingReset(h);
    h->failed = false;
    h->lastError[0] = 0;
}

void GtcrnDestroy(GtcrnHandle* h) {
    if (!h) return;
    delete h->sess;
    delete h->mem;
    delete h->opts;
    delete h->env;
    delete h;
}

const char* GtcrnLastError(const GtcrnHandle* h) {
    if (!h) return "null handle";
    if (!h->sess) return h->lastError[0] ? h->lastError : "model not loaded";
    return h->lastError[0] ? h->lastError : "ok";
}

}  // extern "C"
