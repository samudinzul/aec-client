// ============================================================
//  Silero VAD wrapper — neural voice activity detection
//  (snakers4/silero-vad v6.2.1, MIT; ~2.2 MB, ~309K params)
//
//  Streaming use: 512-sample (32 ms @ 16 kHz) chunks, float32 mono.
//  LSTM state [2,1,128] is carried across chunks inside the handle;
//  SileroReset() zeroes it between streams. Inference is < 1 ms on a
//  single CPU thread, so Push runs on the audio thread directly —
//  no worker, no ring, 32 ms decision cadence.
//
//  No link-time deps beyond the already-linked ONNX Runtime.
// ============================================================
#include "silero_wrapper.h"

#include "onnxruntime_cxx_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

struct SileroHandle {
    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::string inWaveName, inStateName, inSrName;
    std::string outProbName, outStateName;
    float state[2 * 1 * 128] = { 0 };
    float context[SILERO_CONTEXT] = { 0 };  // last 64 samples of previous audio
    float accum[SILERO_CHUNK] = { 0 };
    int accumUsed = 0;
    int64_t sampleRate = 16000;
    char lastError[256] = { 0 };
};

static void SileroFail(SileroHandle* h, const char* msg) {
    if (!h) return;
    snprintf(h->lastError, sizeof(h->lastError), "%s", msg);
}

static bool FileExists(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

SileroHandle* SileroNew(const char* onnxPath) {
    SileroHandle* h = new (std::nothrow) SileroHandle();
    if (!h) return nullptr;
    if (!onnxPath || !FileExists(onnxPath)) {
        SileroFail(h, "Silero model not found in models/ (see README)");
        return h;  // non-null but unusable; Push will refuse
    }
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "silero");
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
        h->inWaveName = h->sess->GetInputNameAllocated(0, alloc).get();
        h->inStateName = h->sess->GetInputNameAllocated(1, alloc).get();
        h->inSrName = h->sess->GetInputNameAllocated(2, alloc).get();
        h->outProbName = h->sess->GetOutputNameAllocated(0, alloc).get();
        h->outStateName = h->sess->GetOutputNameAllocated(1, alloc).get();
    } catch (const Ort::Exception& ex) {
        SileroFail(h, ex.what());
    } catch (const std::exception& ex) {
        SileroFail(h, ex.what());
    }
    return h;
}

static int SileroRunChunk(SileroHandle* h, float* prob) {
    // Upstream recipe: 64 context samples + 512 new = 576 model input.
    float window[SILERO_WINDOW];
    memcpy(window, h->context, sizeof(h->context));
    memcpy(window + SILERO_CONTEXT, h->accum, sizeof(h->accum));
    memcpy(h->context, h->accum + SILERO_CHUNK - SILERO_CONTEXT,
           sizeof(h->context));
    try {
        std::vector<int64_t> waveShape = { 1, SILERO_WINDOW };
        std::vector<int64_t> stateShape = { 2, 1, 128 };
        std::vector<int64_t> srShape = { 1 };
        Ort::Value tWave = Ort::Value::CreateTensor<float>(
            *h->mem, window, SILERO_WINDOW, waveShape.data(), waveShape.size());
        Ort::Value tState = Ort::Value::CreateTensor<float>(
            *h->mem, h->state, 2 * 1 * 128, stateShape.data(), stateShape.size());
        Ort::Value tSr = Ort::Value::CreateTensor<int64_t>(
            *h->mem, &h->sampleRate, 1, srShape.data(), srShape.size());
        const char* inNames[] = {
            h->inWaveName.c_str(), h->inStateName.c_str(), h->inSrName.c_str()
        };
        const char* outNames[] = { h->outProbName.c_str(), h->outStateName.c_str() };
        Ort::Value inputs[] = { std::move(tWave), std::move(tState), std::move(tSr) };
        auto out = h->sess->Run(Ort::RunOptions{ nullptr },
                                inNames, inputs, 3, outNames, 2);
        float* p = out[0].GetTensorMutableData<float>();
        float* sN = out[1].GetTensorMutableData<float>();
        if (!(p[0] >= 0.0f) || !(p[0] <= 1.0f)) {
            SileroFail(h, "VAD returned out-of-range probability");
            return 0;
        }
        *prob = p[0];
        memcpy(h->state, sN, sizeof(h->state));
        return 1;
    } catch (const Ort::Exception& ex) {
        SileroFail(h, ex.what());
        return 0;
    }
}

int SileroPush(SileroHandle* h, const float* pcm, int n, float* prob) {
    if (!h || !h->sess || !pcm || !prob || n <= 0) return 0;
    h->lastError[0] = 0;
    int wrote = 0;
    float last = 0.0f;
    int off = 0;
    while (off < n) {
        int room = SILERO_CHUNK - h->accumUsed;
        int take = n - off < room ? n - off : room;
        memcpy(h->accum + h->accumUsed, pcm + off, (size_t)take * sizeof(float));
        h->accumUsed += take;
        off += take;
        if (h->accumUsed == SILERO_CHUNK) {
            h->accumUsed = 0;
            if (SileroRunChunk(h, &last)) wrote = 1;
            else return wrote;  // error latched; keep going next call
        }
    }
    if (wrote) *prob = last;
    return wrote;
}

void SileroReset(SileroHandle* h) {
    if (!h) return;
    memset(h->state, 0, sizeof(h->state));
    memset(h->context, 0, sizeof(h->context));
    memset(h->accum, 0, sizeof(h->accum));
    h->accumUsed = 0;
    h->lastError[0] = 0;
}

void SileroDestroy(SileroHandle* h) {
    if (!h) return;
    delete h->sess;
    delete h->mem;
    delete h->opts;
    delete h->env;
    delete h;
}

const char* SileroLastError(SileroHandle* h) {
    if (!h) return "null handle";
    if (!h->sess) return h->lastError[0] ? h->lastError : "model not loaded";
    return h->lastError[0] ? h->lastError : "ok";
}
