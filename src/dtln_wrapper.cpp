// ============================================================
//  DTLN-AEC wrapper — Dual-signal Transformation LSTM Network
//  (Westhausen & Meyer, ICASSP 2021, MIT license)
//
//  Pipeline mirrors breizhn/DTLN-aec run_aec.py:
//    block 512, shift 128, 16 kHz only
//    model_1: [mic_mag(257), states, lpb_mag(257)] -> [mask(257), states]
//    model_2: [estimated_td(512), states, lpb_td(512)] -> [out(512), states]
//  with overlap-add on a 512-sample output buffer.
//
//  Backends (probed in order):
//    1. TFLite pair (*_1.tflite + *_2.tflite) via tensorflowlite_c.dll
//       loaded at runtime — no link-time dependency, so this file
//       always compiles (MinGW has no TFLite package).
//    2. ONNX pair (*_1.onnx + *_2.onnx) via ONNX Runtime (already
//       linked for NKF). Convert with the script in docs/ or the
//       breizhn/DTLN convert_weights_to_onnx.py pattern.
// ============================================================
#include "dtln_wrapper.h"

#include "pocketfft_hdronly.h"
#include "onnxruntime_cxx_api.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <complex>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#ifndef DTLN_PI
#define DTLN_PI 3.14159265358979323846
#endif

#define DTLN_BLOCK_LEN   512
#define DTLN_BLOCK_SHIFT 128
#define DTLN_BINS        257  // rfft(512)

enum DtlnBackendKind { DTLN_NONE = 0, DTLN_TFLITE = 1, DTLN_ONNX = 2 };

// ---------- Minimal TFLite C API declarations (opaque handles) ----------
// Only pointers cross the DLL boundary, so no struct layout is needed.
struct TfLiteModel;
struct TfLiteInterpreterOptions;
struct TfLiteInterpreter;
struct TfLiteTensor;
enum TfLiteStatus { kTfLiteOk = 0, kTfLiteError = 1 };

typedef TfLiteModel* (*FnModelFromFile)(const char*);
typedef void (*FnModelDelete)(TfLiteModel*);
typedef TfLiteInterpreterOptions* (*FnOptsCreate)(void);
typedef void (*FnOptsDelete)(TfLiteInterpreterOptions*);
typedef void (*FnOptsThreads)(TfLiteInterpreterOptions*, int32_t);
typedef TfLiteInterpreter* (*FnInterpCreate)(const TfLiteModel*,
                                             const TfLiteInterpreterOptions*);
typedef void (*FnInterpDelete)(TfLiteInterpreter*);
typedef TfLiteStatus (*FnAllocate)(TfLiteInterpreter*);
typedef int32_t (*FnInputCount)(const TfLiteInterpreter*);
typedef int32_t (*FnOutputCount)(const TfLiteInterpreter*);
typedef TfLiteTensor* (*FnGetInput)(TfLiteInterpreter*, int32_t);
typedef TfLiteTensor* (*FnGetOutput)(TfLiteInterpreter*, int32_t);
typedef size_t (*FnByteSize)(const TfLiteTensor*);
typedef TfLiteStatus (*FnCopyFrom)(TfLiteTensor*, const void*, size_t);
typedef TfLiteStatus (*FnCopyTo)(const TfLiteTensor*, void*, size_t);
typedef TfLiteStatus (*FnInvoke)(TfLiteInterpreter*);

struct TfliteApi {
    HMODULE dll = nullptr;
    FnModelFromFile modelFromFile = nullptr;
    FnModelDelete modelDelete = nullptr;
    FnOptsCreate optsCreate = nullptr;
    FnOptsDelete optsDelete = nullptr;
    FnOptsThreads optsThreads = nullptr;
    FnInterpCreate interpCreate = nullptr;
    FnInterpDelete interpDelete = nullptr;
    FnAllocate allocate = nullptr;
    FnInputCount inputCount = nullptr;
    FnOutputCount outputCount = nullptr;
    FnGetInput getInput = nullptr;
    FnGetOutput getOutput = nullptr;
    FnByteSize byteSize = nullptr;
    FnCopyFrom copyFrom = nullptr;
    FnCopyTo copyTo = nullptr;
    FnInvoke invoke = nullptr;
    bool ok = false;
};

struct DtlnHandle {
    int backend = DTLN_NONE;
    char lastError[256] = { 0 };

    // DSP state
    float micBuf[DTLN_BLOCK_LEN] = { 0 };
    float lpbBuf[DTLN_BLOCK_LEN] = { 0 };
    float outBuf[DTLN_BLOCK_LEN] = { 0 };
    std::vector<int16_t> micAccum;
    std::vector<int16_t> refAccum;
    std::vector<int16_t> outAccum;

    // LSTM states (both backends)
    std::vector<float> states1;
    std::vector<float> states2;

    // TFLite backend
    TfliteApi tf;
    TfLiteModel* tfModel1 = nullptr;
    TfLiteModel* tfModel2 = nullptr;
    TfLiteInterpreter* tfInterp1 = nullptr;
    TfLiteInterpreter* tfInterp2 = nullptr;
    int tfM1Mask = 0, tfM1StateIn = 1, tfM1Lpb = 2, tfM1MaskOut = 0, tfM1StateOut = 1;
    int tfM2Est = 0, tfM2StateIn = 1, tfM2Lpb = 2, tfM2Out = 0, tfM2StateOut = 1;

    // ONNX backend
    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess1 = nullptr;
    Ort::Session* sess2 = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::vector<std::string> in1Names, out1Names, in2Names, out2Names;
    std::vector<const char*> in1Ptr, out1Ptr, in2Ptr, out2Ptr;
    // role map: index of mic-feat / lpb-feat / states per model
    int onnxM1Mic = 0, onnxM1State = 1, onnxM1Lpb = 2;
    int onnxM2Est = 0, onnxM2State = 1, onnxM2Lpb = 2;
    int64_t feat257[3] = { 1, 1, DTLN_BINS };
    int64_t feat512[3] = { 1, 1, DTLN_BLOCK_LEN };
};

static void DtlnSetError(DtlnHandle* h, const char* msg) {
    if (!h || !msg) return;
    strncpy(h->lastError, msg, sizeof(h->lastError) - 1);
    h->lastError[sizeof(h->lastError) - 1] = '\0';
}

static bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

// ---------------- TFLite dynamic loader ----------------
static bool LoadTfliteApi(TfliteApi& api, char* errBuf, size_t errSize) {
    const char* candidates[] = { "tensorflowlite_c.dll", "libtensorflowlite_c.dll" };
    for (const char* name : candidates) {
        api.dll = LoadLibraryA(name);
        if (api.dll) break;
    }
    if (!api.dll) {
        snprintf(errBuf, errSize,
                 "tensorflowlite_c.dll not found (tried tensorflowlite_c.dll, libtensorflowlite_c.dll)");
        return false;
    }
#define RESOLVE(field, sym)                                  \
    api.field = reinterpret_cast<decltype(api.field)>(       \
        GetProcAddress(api.dll, sym));                       \
    if (!api.field) {                                        \
        snprintf(errBuf, errSize, "tensorflowlite_c.dll missing symbol: %s", sym); \
        FreeLibrary(api.dll); api.dll = nullptr;             \
        return false;                                        \
    }
    RESOLVE(modelFromFile, "TfLiteModelCreateFromFile");
    RESOLVE(modelDelete, "TfLiteModelDelete");
    RESOLVE(optsCreate, "TfLiteInterpreterOptionsCreate");
    RESOLVE(optsDelete, "TfLiteInterpreterOptionsDelete");
    RESOLVE(optsThreads, "TfLiteInterpreterOptionsSetNumThreads");
    RESOLVE(interpCreate, "TfLiteInterpreterCreate");
    RESOLVE(interpDelete, "TfLiteInterpreterDelete");
    RESOLVE(allocate, "TfLiteInterpreterAllocateTensors");
    RESOLVE(inputCount, "TfLiteInterpreterGetInputTensorCount");
    RESOLVE(outputCount, "TfLiteInterpreterGetOutputTensorCount");
    RESOLVE(getInput, "TfLiteInterpreterGetInputTensor");
    RESOLVE(getOutput, "TfLiteInterpreterGetOutputTensor");
    RESOLVE(byteSize, "TfLiteTensorByteSize");
    RESOLVE(copyFrom, "TfLiteTensorCopyFromBuffer");
    RESOLVE(copyTo, "TfLiteTensorCopyToBuffer");
    RESOLVE(invoke, "TfLiteInterpreterInvoke");
#undef RESOLVE
    api.ok = true;
    return true;
}

// Classify a 3-input DTLN stage against upstream layout
// (breizhn/DTLN-aec run_aec.py): in[0]=mic/est, in[1]=states, in[2]=lpb;
// out[0]=mask/block, out[1]=states.
// "State = largest input" breaks on dtln_aec_128 stage 2, where state is
// [1,2,128,2] = 512 floats — the SAME size as est/lpb (512) — so the tie
// picked index 0 and wrote est into the state tensor (full-scale blowup).
// Prefer official indices when sizes match (state >= feat, or equal tie).
static void ClassifyDtln3(size_t* sizes, int n, int* stateIn, int* aIn, int* bIn) {
    if (n == 3 && sizes[0] == sizes[2] && sizes[1] >= sizes[0]) {
        *stateIn = 1;
        *aIn = 0;
        *bIn = 2;
        return;
    }
    int s = 0;
    for (int i = 1; i < n; i++)
        if (sizes[i] > sizes[s]) s = i;
    if (n == 3 && sizes[0] == sizes[1] && sizes[1] == sizes[2]) s = 1;
    *stateIn = s;
    int f[8], nf = 0;
    for (int i = 0; i < n && nf < 8; i++)
        if (i != s) f[nf++] = i;
    if (nf >= 2) { *aIn = f[0]; *bIn = f[nf - 1]; }
    else if (nf == 1) { *aIn = f[0]; *bIn = f[0]; }
    else { *aIn = 0; *bIn = 0; }
}

static void ClassifyDtlnOut(size_t s0, size_t s1, int* primaryOut, int* stateOut) {
    // Official order when sizes allow (or tie): out[0]=primary, out[1]=state.
    if (s0 <= s1) { *primaryOut = 0; *stateOut = 1; }
    else { *primaryOut = 1; *stateOut = 0; }
}

// Classify TFLite model_1 inputs: two 257-float feats + states.
static void ClassifyTfliteM1(DtlnHandle* h) {
    int n = h->tf.inputCount(h->tfInterp1);
    if (n < 2) return;
    size_t sizes[8] = { 0 };
    for (int i = 0; i < n && i < 8; i++)
        sizes[i] = h->tf.byteSize(h->tf.getInput(h->tfInterp1, i));
    int s = 0, mic = 0, lpb = 0;
    ClassifyDtln3(sizes, n, &s, &mic, &lpb);
    h->tfM1StateIn = s;
    h->tfM1Mask = mic;
    h->tfM1Lpb = lpb;
    h->states1.assign(sizes[s] / sizeof(float), 0.0f);
    int no = h->tf.outputCount(h->tfInterp1);
    if (no >= 2) {
        size_t s0 = h->tf.byteSize(h->tf.getOutput(h->tfInterp1, 0));
        size_t s1 = h->tf.byteSize(h->tf.getOutput(h->tfInterp1, 1));
        ClassifyDtlnOut(s0, s1, &h->tfM1MaskOut, &h->tfM1StateOut);
    }
}

// Classify TFLite model_2 inputs: two 512-float feats + states.
// For dtln_aec_128, all three inputs are 512 floats (2048 bytes) — must
// use official in[1]=state, not "largest" (which ties to in[0]).
static void ClassifyTfliteM2(DtlnHandle* h) {
    int n = h->tf.inputCount(h->tfInterp2);
    if (n < 2) return;
    size_t sizes[8] = { 0 };
    for (int i = 0; i < n && i < 8; i++)
        sizes[i] = h->tf.byteSize(h->tf.getInput(h->tfInterp2, i));
    int s = 0, est = 0, lpb = 0;
    ClassifyDtln3(sizes, n, &s, &est, &lpb);
    h->tfM2StateIn = s;
    h->tfM2Est = est;
    h->tfM2Lpb = lpb;
    h->states2.assign(sizes[s] / sizeof(float), 0.0f);
    int no = h->tf.outputCount(h->tfInterp2);
    if (no >= 2) {
        size_t s0 = h->tf.byteSize(h->tf.getOutput(h->tfInterp2, 0));
        size_t s1 = h->tf.byteSize(h->tf.getOutput(h->tfInterp2, 1));
        ClassifyDtlnOut(s0, s1, &h->tfM2Out, &h->tfM2StateOut);
    }
}

static bool TryTflite(DtlnHandle* h, const std::string& prefix) {
    std::string p1 = prefix + "_1.tflite";
    std::string p2 = prefix + "_2.tflite";
    if (!FileExists(p1) || !FileExists(p2)) return false;
    char err[256] = { 0 };
    if (!LoadTfliteApi(h->tf, err, sizeof(err))) {
        DtlnSetError(h, err);
        return false;
    }
    h->tfModel1 = h->tf.modelFromFile(p1.c_str());
    h->tfModel2 = h->tf.modelFromFile(p2.c_str());
    if (!h->tfModel1 || !h->tfModel2) {
        DtlnSetError(h, "TFLite: failed to load model pair");
        return false;
    }
    auto* opts = h->tf.optsCreate();
    if (opts) h->tf.optsThreads(opts, 1);
    h->tfInterp1 = h->tf.interpCreate(h->tfModel1, opts);
    h->tfInterp2 = h->tf.interpCreate(h->tfModel2, opts);
    if (opts) h->tf.optsDelete(opts);
    if (!h->tfInterp1 || !h->tfInterp2) {
        DtlnSetError(h, "TFLite: failed to create interpreters");
        return false;
    }
    if (h->tf.allocate(h->tfInterp1) != kTfLiteOk ||
        h->tf.allocate(h->tfInterp2) != kTfLiteOk) {
        DtlnSetError(h, "TFLite: AllocateTensors failed");
        return false;
    }
    ClassifyTfliteM1(h);
    ClassifyTfliteM2(h);
    h->backend = DTLN_TFLITE;
    return true;
}

// ---------------- ONNX backend ----------------
static size_t OnnxInputCount(Ort::Session* s) {
    return s ? s->GetInputCount() : 0;
}
static size_t OnnxElemCount(Ort::Session* s, bool isInput, size_t i) {
    try {
        Ort::TypeInfo ti = isInput ? s->GetInputTypeInfo(i) : s->GetOutputTypeInfo(i);
        auto tsi = ti.GetTensorTypeAndShapeInfo();
        size_t n = 1;
        for (auto d : tsi.GetShape()) n *= (d > 0 ? (size_t)d : 1);
        return n;
    } catch (...) { return 0; }
}

static bool TryOnnx(DtlnHandle* h, const std::string& prefix) {
    std::string p1 = prefix + "_1.onnx";
    std::string p2 = prefix + "_2.onnx";
    if (!FileExists(p1) || !FileExists(p2)) return false;
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "dtln");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::wstring w1(p1.begin(), p1.end()), w2(p2.begin(), p2.end());
        h->sess1 = new Ort::Session(*h->env, w1.c_str(), *h->opts);
        h->sess2 = new Ort::Session(*h->env, w2.c_str(), *h->opts);

        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < OnnxInputCount(h->sess1); i++) {
            h->in1Names.push_back(h->sess1->GetInputNameAllocated(i, alloc).get());
        }
        for (size_t i = 0; i < h->sess1->GetOutputCount(); i++) {
            h->out1Names.push_back(h->sess1->GetOutputNameAllocated(i, alloc).get());
        }
        for (size_t i = 0; i < OnnxInputCount(h->sess2); i++) {
            h->in2Names.push_back(h->sess2->GetInputNameAllocated(i, alloc).get());
        }
        for (size_t i = 0; i < h->sess2->GetOutputCount(); i++) {
            h->out2Names.push_back(h->sess2->GetOutputNameAllocated(i, alloc).get());
        }
        for (auto& s : h->in1Names) h->in1Ptr.push_back(s.c_str());
        for (auto& s : h->out1Names) h->out1Ptr.push_back(s.c_str());
        for (auto& s : h->in2Names) h->in2Ptr.push_back(s.c_str());
        for (auto& s : h->out2Names) h->out2Ptr.push_back(s.c_str());

        // Role map model_1/2: official DTLN-aec layout
        // in[0]=mic/est, in[1]=states, in[2]=lpb (same rule as TFLite).
        {
            size_t n = h->in1Ptr.size();
            size_t sizes[8] = { 0 };
            for (size_t i = 0; i < n && i < 8; i++)
                sizes[i] = OnnxElemCount(h->sess1, true, i);
            int s = 0, mic = 0, lpb = 0;
            ClassifyDtln3(sizes, (int)n, &s, &mic, &lpb);
            h->onnxM1State = s;
            h->onnxM1Mic = mic;
            h->onnxM1Lpb = lpb;
            h->states1.assign(sizes[s] ? sizes[s] : 1, 0.0f);
        }
        {
            size_t n = h->in2Ptr.size();
            size_t sizes[8] = { 0 };
            for (size_t i = 0; i < n && i < 8; i++)
                sizes[i] = OnnxElemCount(h->sess2, true, i);
            int s = 0, est = 0, lpb = 0;
            ClassifyDtln3(sizes, (int)n, &s, &est, &lpb);
            h->onnxM2State = s;
            h->onnxM2Est = est;
            h->onnxM2Lpb = lpb;
            h->states2.assign(sizes[s] ? sizes[s] : 1, 0.0f);
        }
        h->backend = DTLN_ONNX;
        return true;
    } catch (const std::exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "ONNX: %s", e.what());
        DtlnSetError(h, buf);
        return false;
    } catch (...) {
        DtlnSetError(h, "ONNX: failed to load model pair");
        return false;
    }
}

// ---------------- Inference: one 128-sample shift ----------------
static bool RunTflitePair(DtlnHandle* h, const float micMag[DTLN_BINS],
                          const float lpbMag[DTLN_BINS], float mask[DTLN_BINS]) {
    auto& tf = h->tf;
    if (tf.copyFrom(tf.getInput(h->tfInterp1, h->tfM1Mask), micMag,
                    DTLN_BINS * sizeof(float)) != kTfLiteOk) return false;
    if (tf.copyFrom(tf.getInput(h->tfInterp1, h->tfM1Lpb), lpbMag,
                    DTLN_BINS * sizeof(float)) != kTfLiteOk) return false;
    if (!h->states1.empty() &&
        tf.copyFrom(tf.getInput(h->tfInterp1, h->tfM1StateIn), h->states1.data(),
                    h->states1.size() * sizeof(float)) != kTfLiteOk) return false;
    if (tf.invoke(h->tfInterp1) != kTfLiteOk) return false;
    if (tf.copyTo(tf.getOutput(h->tfInterp1, h->tfM1MaskOut), mask,
                  DTLN_BINS * sizeof(float)) != kTfLiteOk) return false;
    if (!h->states1.empty())
        tf.copyTo(tf.getOutput(h->tfInterp1, h->tfM1StateOut), h->states1.data(),
                  h->states1.size() * sizeof(float));
    return true;
}

static bool RunTfliteSecond(DtlnHandle* h, const float est[DTLN_BLOCK_LEN],
                            const float lpb[DTLN_BLOCK_LEN],
                            float out[DTLN_BLOCK_LEN]) {
    auto& tf = h->tf;
    if (tf.copyFrom(tf.getInput(h->tfInterp2, h->tfM2Est), est,
                    DTLN_BLOCK_LEN * sizeof(float)) != kTfLiteOk) return false;
    if (tf.copyFrom(tf.getInput(h->tfInterp2, h->tfM2Lpb), lpb,
                    DTLN_BLOCK_LEN * sizeof(float)) != kTfLiteOk) return false;
    if (!h->states2.empty() &&
        tf.copyFrom(tf.getInput(h->tfInterp2, h->tfM2StateIn), h->states2.data(),
                    h->states2.size() * sizeof(float)) != kTfLiteOk) return false;
    if (tf.invoke(h->tfInterp2) != kTfLiteOk) return false;
    if (tf.copyTo(tf.getOutput(h->tfInterp2, h->tfM2Out), out,
                  DTLN_BLOCK_LEN * sizeof(float)) != kTfLiteOk) return false;
    if (!h->states2.empty())
        tf.copyTo(tf.getOutput(h->tfInterp2, h->tfM2StateOut), h->states2.data(),
                  h->states2.size() * sizeof(float));
    return true;
}

static bool RunOnnxFirst(DtlnHandle* h, const float micMag[DTLN_BINS],
                         const float lpbMag[DTLN_BINS], float mask[DTLN_BINS]) {
    try {
        size_t n = h->in1Ptr.size();
        std::vector<Ort::Value> ins;
        ins.reserve(n);
        for (size_t i = 0; i < n; i++) {
            const float* src = h->states1.data();
            const int64_t* dims = nullptr;
            size_t count = h->states1.size();
            size_t c = OnnxElemCount(h->sess1, true, i);
            std::vector<int64_t> stateDims = { 1, 1, (int64_t)h->states1.size() };
            if ((int)i == h->onnxM1State) {
                if (count == 0) {
                    float z = 0;
                    stateDims[2] = 1;
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, &z, 1, stateDims.data(), 3));
                } else {
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, const_cast<float*>(src), count,
                        stateDims.data(), 3));
                }
            } else if (c == DTLN_BINS) {
                src = ((int)i == h->onnxM1Lpb) ? lpbMag : micMag;
                dims = h->feat257;
                count = DTLN_BINS;
                ins.push_back(Ort::Value::CreateTensor<float>(
                    *h->mem, const_cast<float*>(src), count, dims, 3));
            } else {
                if (count == 0) { float z = 0; stateDims[2] = 1;
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, &z, 1, stateDims.data(), 3));
                } else {
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, const_cast<float*>(src), count,
                        stateDims.data(), 3));
                }
            }
        }
        auto outs = h->sess1->Run(Ort::RunOptions{ nullptr },
                                  h->in1Ptr.data(), ins.data(), ins.size(),
                                  h->out1Ptr.data(), h->out1Ptr.size());
        // mask = official out[0]; states = out[1] (official layout; size
        // heuristic only when c0 > c1 would mean unexpected order).
        size_t oi = 0;
        if (outs.size() >= 2) {
            size_t c0 = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            size_t c1 = outs[1].GetTensorTypeAndShapeInfo().GetElementCount();
            oi = (c0 <= c1) ? 0 : 1;
        }
        float* mp = outs[oi].GetTensorMutableData<float>();
        memcpy(mask, mp, DTLN_BINS * sizeof(float));
        if (outs.size() >= 2) {
            size_t si = 1 - oi;
            float* sp = outs[si].GetTensorMutableData<float>();
            size_t c = outs[si].GetTensorTypeAndShapeInfo().GetElementCount();
            if (c == h->states1.size()) memcpy(h->states1.data(), sp, c * sizeof(float));
        }
        return true;
    } catch (...) { return false; }
}

static bool RunOnnxSecond(DtlnHandle* h, const float est[DTLN_BLOCK_LEN],
                          const float lpb[DTLN_BLOCK_LEN],
                          float out[DTLN_BLOCK_LEN]) {
    try {
        size_t n = h->in2Ptr.size();
        std::vector<Ort::Value> ins;
        ins.reserve(n);
        for (size_t i = 0; i < n; i++) {
            size_t c = OnnxElemCount(h->sess2, true, i);
            std::vector<int64_t> stateDims = { 1, 1, (int64_t)h->states2.size() };
            // dtln_aec_128: est/state/lpb are all 512 elems — must use the
            // classified indices, not "count == 512" (that maps state → est).
            if ((int)i == h->onnxM2State) {
                size_t count = h->states2.size() ? h->states2.size() : 1;
                if (h->states2.empty()) {
                    float z = 0;
                    stateDims[2] = 1;
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, &z, 1, stateDims.data(), 3));
                } else {
                    ins.push_back(Ort::Value::CreateTensor<float>(
                        *h->mem, h->states2.data(), count,
                        stateDims.data(), 3));
                }
            } else if (c == DTLN_BLOCK_LEN) {
                const float* src = ((int)i == h->onnxM2Lpb) ? lpb : est;
                ins.push_back(Ort::Value::CreateTensor<float>(
                    *h->mem, const_cast<float*>(src), DTLN_BLOCK_LEN,
                    h->feat512, 3));
            } else {
                size_t count = h->states2.size() ? h->states2.size() : 1;
                ins.push_back(Ort::Value::CreateTensor<float>(
                    *h->mem, h->states2.data(), count,
                    stateDims.data(), 3));
            }
        }
        auto outs = h->sess2->Run(Ort::RunOptions{ nullptr },
                                  h->in2Ptr.data(), ins.data(), ins.size(),
                                  h->out2Ptr.data(), h->out2Ptr.size());
        size_t oi = 0;
        if (outs.size() >= 2) {
            size_t c0 = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            size_t c1 = outs[1].GetTensorTypeAndShapeInfo().GetElementCount();
            // out block = 512-count output
            oi = (c0 == DTLN_BLOCK_LEN) ? 0 : 1;
            (void)c1;
        }
        float* op = outs[oi].GetTensorMutableData<float>();
        memcpy(out, op, DTLN_BLOCK_LEN * sizeof(float));
        if (outs.size() >= 2) {
            size_t si = 1 - oi;
            float* sp = outs[si].GetTensorMutableData<float>();
            size_t c = outs[si].GetTensorTypeAndShapeInfo().GetElementCount();
            if (c == h->states2.size()) memcpy(h->states2.data(), sp, c * sizeof(float));
        }
        return true;
    } catch (...) { return false; }
}

static void DtlnProcessShift(DtlnHandle* h, const float micNew[DTLN_BLOCK_SHIFT],
                             const float lpbNew[DTLN_BLOCK_SHIFT]) {
    memmove(h->micBuf, h->micBuf + DTLN_BLOCK_SHIFT,
            (DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT) * sizeof(float));
    memmove(h->lpbBuf, h->lpbBuf + DTLN_BLOCK_SHIFT,
            (DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT) * sizeof(float));
    memcpy(h->micBuf + DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT, micNew,
           DTLN_BLOCK_SHIFT * sizeof(float));
    memcpy(h->lpbBuf + DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT, lpbNew,
           DTLN_BLOCK_SHIFT * sizeof(float));

    // FFT both (pocketfft, same helper as NKF)
    double micIn[DTLN_BLOCK_LEN], lpbIn[DTLN_BLOCK_LEN];
    for (int i = 0; i < DTLN_BLOCK_LEN; i++) {
        micIn[i] = h->micBuf[i];
        lpbIn[i] = h->lpbBuf[i];
    }
    std::vector<std::complex<double>> micSpec(DTLN_BLOCK_LEN), lpbSpec(DTLN_BLOCK_LEN);
    std::vector<size_t> shape{ (size_t)DTLN_BLOCK_LEN };
    std::vector<size_t> axes{ 0 };
    std::vector<ptrdiff_t> strideIn{ sizeof(double) };
    std::vector<ptrdiff_t> strideOut{ sizeof(std::complex<double>) };
    pocketfft::r2c(shape, strideIn, strideOut, axes, pocketfft::FORWARD,
                   micIn, micSpec.data(), 1.0);
    pocketfft::r2c(shape, strideIn, strideOut, axes, pocketfft::FORWARD,
                   lpbIn, lpbSpec.data(), 1.0);

    float micMag[DTLN_BINS], lpbMag[DTLN_BINS];
    for (int i = 0; i < DTLN_BINS; i++) {
        micMag[i] = (float)std::abs(micSpec[i]);
        lpbMag[i] = (float)std::abs(lpbSpec[i]);
    }

    float mask[DTLN_BINS];
    for (int i = 0; i < DTLN_BINS; i++) mask[i] = 1.0f;
    bool ok = false;
    if (h->backend == DTLN_TFLITE) ok = RunTflitePair(h, micMag, lpbMag, mask);
    else if (h->backend == DTLN_ONNX) ok = RunOnnxFirst(h, micMag, lpbMag, mask);
    if (!ok) { /* passthrough mask keeps mic audible on inference failure */ }

    // estimated = irfft(mic_spec * mask)
    std::vector<std::complex<double>> estSpec(DTLN_BLOCK_LEN);
    for (int i = 0; i < DTLN_BINS; i++)
        estSpec[i] = micSpec[i] * (double)mask[i];
    double estTd[DTLN_BLOCK_LEN] = { 0 };
    pocketfft::c2r(shape, strideOut, strideIn, axes, pocketfft::BACKWARD,
                   estSpec.data(), estTd, 1.0);
    float estBlock[DTLN_BLOCK_LEN];
    for (int i = 0; i < DTLN_BLOCK_LEN; i++)
        estBlock[i] = (float)(estTd[i] / DTLN_BLOCK_LEN);

    float outBlock[DTLN_BLOCK_LEN];
    memcpy(outBlock, estBlock, sizeof(outBlock));
    if (h->backend == DTLN_TFLITE) RunTfliteSecond(h, estBlock, h->lpbBuf, outBlock);
    else if (h->backend == DTLN_ONNX) RunOnnxSecond(h, estBlock, h->lpbBuf, outBlock);

    // Overlap-add
    memmove(h->outBuf, h->outBuf + DTLN_BLOCK_SHIFT,
            (DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT) * sizeof(float));
    memset(h->outBuf + DTLN_BLOCK_LEN - DTLN_BLOCK_SHIFT, 0,
           DTLN_BLOCK_SHIFT * sizeof(float));
    for (int i = 0; i < DTLN_BLOCK_LEN; i++) h->outBuf[i] += outBlock[i];
}

// ---------------- C API ----------------
extern "C" {

DtlnHandle* DtlnNew(const char* modelPrefix) {
    auto* h = new DtlnHandle();
    h->micAccum.reserve(DTLN_BLOCK_SHIFT * 4);
    h->refAccum.reserve(DTLN_BLOCK_SHIFT * 4);
    h->outAccum.reserve(DTLN_BLOCK_SHIFT * 4);
    std::string prefix = (modelPrefix && *modelPrefix) ? modelPrefix : "models/dtln_aec_128";

    char tfErr[256] = { 0 };
    // Probe TFLite first (preferred: ships upstream weights verbatim)
    {
        std::string p1 = prefix + "_1.tflite";
        std::string p2 = prefix + "_2.tflite";
        if (FileExists(p1) && FileExists(p2)) {
            if (TryTflite(h, prefix)) return h;
            // remember TFLite error, still try ONNX below
            strncpy(tfErr, h->lastError, sizeof(tfErr) - 1);
        }
    }
    if (TryOnnx(h, prefix)) return h;

    if (tfErr[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf), "DTLN: no backend (TFLite: %.120s; ONNX pair missing)",
                 tfErr);
        DtlnSetError(h, buf);
    } else if (!h->lastError[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "DTLN: model pair not found for prefix '%s' "
                 "(need *_1.tflite+*_2.tflite and tensorflowlite_c.dll, "
                 "or *_1.onnx+*_2.onnx)", prefix.c_str());
        DtlnSetError(h, buf);
    }
    delete h;
    return nullptr;
}

void DtlnProcess(DtlnHandle* h, const int16_t* mic, const int16_t* ref,
                 int16_t* out, int frameSize) {
    if (!h || h->backend == DTLN_NONE) {
        if (out && frameSize > 0) memset(out, 0, (size_t)frameSize * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < frameSize; i++) {
        h->micAccum.push_back(mic[i]);
        h->refAccum.push_back(ref[i]);
    }
    float micF[DTLN_BLOCK_SHIFT], refF[DTLN_BLOCK_SHIFT];
    while (h->micAccum.size() >= (size_t)DTLN_BLOCK_SHIFT &&
           h->refAccum.size() >= (size_t)DTLN_BLOCK_SHIFT) {
        for (int i = 0; i < DTLN_BLOCK_SHIFT; i++) {
            micF[i] = h->micAccum[i] / 32768.0f;
            refF[i] = h->refAccum[i] / 32768.0f;
        }
        h->micAccum.erase(h->micAccum.begin(), h->micAccum.begin() + DTLN_BLOCK_SHIFT);
        h->refAccum.erase(h->refAccum.begin(), h->refAccum.begin() + DTLN_BLOCK_SHIFT);
        DtlnProcessShift(h, micF, refF);
        for (int i = 0; i < DTLN_BLOCK_SHIFT; i++) {
            float v = h->outBuf[i] * 32768.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            h->outAccum.push_back((int16_t)v);
        }
        // Note: outBuf head is consumed logically here; the physical
        // shift happens inside DtlnProcessShift's overlap-add step
        // on the next block. Mark consumed with zeros to avoid
        // re-emitting on underflow paths.
        memset(h->outBuf, 0, DTLN_BLOCK_SHIFT * sizeof(float));
    }
    for (int i = 0; i < frameSize; i++) {
        if (!h->outAccum.empty()) {
            out[i] = h->outAccum.front();
            h->outAccum.erase(h->outAccum.begin());
        } else {
            out[i] = 0;
        }
    }
}

void DtlnReset(DtlnHandle* h) {
    if (!h) return;
    memset(h->micBuf, 0, sizeof(h->micBuf));
    memset(h->lpbBuf, 0, sizeof(h->lpbBuf));
    memset(h->outBuf, 0, sizeof(h->outBuf));
    h->micAccum.clear();
    h->refAccum.clear();
    h->outAccum.clear();
    std::fill(h->states1.begin(), h->states1.end(), 0.0f);
    std::fill(h->states2.begin(), h->states2.end(), 0.0f);
}

void DtlnDestroy(DtlnHandle* h) {
    if (!h) return;
    if (h->tfInterp1) h->tf.interpDelete(h->tfInterp1);
    if (h->tfInterp2) h->tf.interpDelete(h->tfInterp2);
    if (h->tfModel1) h->tf.modelDelete(h->tfModel1);
    if (h->tfModel2) h->tf.modelDelete(h->tfModel2);
    if (h->tf.dll) FreeLibrary(h->tf.dll);
    delete h->sess1;
    delete h->sess2;
    delete h->opts;
    delete h->mem;
    delete h->env;
    delete h;
}

const char* DtlnLastError(DtlnHandle* h) {
    if (!h || !h->lastError[0]) return "No DTLN context";
    return h->lastError;
}

int DtlnBackend(DtlnHandle* h) {
    if (!h) return 0;
    return h->backend;
}

} // extern "C"
