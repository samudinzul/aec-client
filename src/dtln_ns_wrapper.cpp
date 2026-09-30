// ============================================================
//  DTLN noise reduction wrapper — networkedaudio port of
//  breizhn/DTLN denoise (MIT). Same 512-block / 128-shift / 257-bin
//  DSP as the AEC pair, minus the loud-playback feed:
//      model_1: [mag(257), states(512)] -> [mask(257), states(512)]
//      model_2: [est(512), states]      -> [block(512), states]
//
//  Backends (probed in order):
//    1. TFLite pair (*_1.tflite + *_2.tflite) via tensorflowlite_c.dll
//       loaded at runtime — no link-time dependency, so this file
//       always compiles (MinGW has no TFLite package).
//    2. ONNX pair (*_1.onnx + *_2.onnx) via ONNX Runtime (already
//       linked for NKF/DTLN).
// ============================================================
#include "dtln_ns_wrapper.h"

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
#include <new>

#ifndef DTNS_PI
#define DTNS_PI 3.14159265358979323846
#endif

#define DTNS_BLOCK_LEN   512
#define DTNS_BLOCK_SHIFT 128
#define DTNS_BINS        257  // rfft(512)

enum DtnsBackendKind { DTNS_NONE = 0, DTNS_TFLITE = 1, DTNS_ONNX = 2 };

// ---------- Minimal TFLite C API declarations (opaque handles) ----------
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
typedef TfLiteStatus (*FnCopyTo)(TfLiteTensor*, void*, size_t);
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

struct DtlnNsHandle {
    int backend = DTNS_NONE;
    char lastError[256] = { 0 };

    // DSP state
    float micBuf[DTNS_BLOCK_LEN] = { 0 };
    float q[2048];            // input awaiting hop framing
    int qn = 0;
    float oring[4096];       // output FIFO (prefilled with zeros)
    int rHead = 0, rCount = 0;
    float outBuf[DTNS_BLOCK_LEN] = { 0 };
    std::vector<int16_t> micAccum;
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
    int tfM1Mask = 0, tfM1StateIn = 1, tfM1MaskOut = 0, tfM1StateOut = 1;
    int tfM2Est = 0, tfM2StateIn = 1, tfM2Out = 0, tfM2StateOut = 1;

    // ONNX backend
    Ort::Env* env = nullptr;
    Ort::SessionOptions* opts = nullptr;
    Ort::Session* sess1 = nullptr;
    Ort::Session* sess2 = nullptr;
    Ort::MemoryInfo* mem = nullptr;
    std::vector<std::string> in1Names, out1Names, in2Names, out2Names;
    std::vector<const char*> in1Ptr, out1Ptr, in2Ptr, out2Ptr;
    int onnxM1Mask = 0, onnxM1State = 1;
    int onnxM2Est = 0, onnxM2State = 1;
    int64_t feat257[3] = { 1, 1, DTNS_BINS };
    int64_t feat512[3] = { 1, 1, DTNS_BLOCK_LEN };

    // Live diagnostics
    int dropped = 0;
    float lastInRms = 0, lastOutRms = 0;
    int lastInN = 0;
};

static void DtnsSetError(DtlnNsHandle* h, const char* msg) {
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

// Classify a 2-input DTLN stage: in[0]=feat, in[1]=states.
// Prefer official indices when sizes match (state >= feat, or equal tie).
static void ClassifyDtns2(size_t* sizes, int n, int* stateIn, int* featIn) {
    if (n == 2 && sizes[1] >= sizes[0]) { *stateIn = 1; *featIn = 0; return; }
    int s = 0;
    for (int i = 1; i < n; i++)
        if (sizes[i] > sizes[s]) s = i;
    if (n == 2 && sizes[0] == sizes[1]) s = 1;
    *stateIn = s;
    *featIn = s ? 0 : (n > 1 ? 1 : 0);
}

static void ClassifyDtnsOut(size_t s0, size_t s1, int* primaryOut, int* stateOut) {
    if (s0 <= s1) { *primaryOut = 0; *stateOut = 1; }
    else { *primaryOut = 1; *stateOut = 0; }
}

static void ClassifyTfliteM1(DtlnNsHandle* h) {
    int n = h->tf.inputCount(h->tfInterp1);
    if (n < 2) return;
    size_t sizes[8] = { 0 };
    for (int i = 0; i < n && i < 8; i++)
        sizes[i] = h->tf.byteSize(h->tf.getInput(h->tfInterp1, i));
    int s = 0, mask = 0;
    ClassifyDtns2(sizes, n, &s, &mask);
    h->tfM1StateIn = s;
    h->tfM1Mask = mask;
    h->states1.assign(sizes[s] / sizeof(float), 0.0f);
    int no = h->tf.outputCount(h->tfInterp1);
    if (no >= 2) {
        size_t s0 = h->tf.byteSize(h->tf.getOutput(h->tfInterp1, 0));
        size_t s1 = h->tf.byteSize(h->tf.getOutput(h->tfInterp1, 1));
        ClassifyDtnsOut(s0, s1, &h->tfM1MaskOut, &h->tfM1StateOut);
    }
}

static void ClassifyTfliteM2(DtlnNsHandle* h) {
    int n = h->tf.inputCount(h->tfInterp2);
    if (n < 2) return;
    size_t sizes[8] = { 0 };
    for (int i = 0; i < n && i < 8; i++)
        sizes[i] = h->tf.byteSize(h->tf.getInput(h->tfInterp2, i));
    int s = 0, est = 0;
    ClassifyDtns2(sizes, n, &s, &est);
    h->tfM2StateIn = s;
    h->tfM2Est = est;
    h->states2.assign(sizes[s] / sizeof(float), 0.0f);
    int no = h->tf.outputCount(h->tfInterp2);
    if (no >= 2) {
        size_t s0 = h->tf.byteSize(h->tf.getOutput(h->tfInterp2, 0));
        size_t s1 = h->tf.byteSize(h->tf.getOutput(h->tfInterp2, 1));
        ClassifyDtnsOut(s0, s1, &h->tfM2Out, &h->tfM2StateOut);
    }
}

static bool TryTflite(DtlnNsHandle* h, const std::string& prefix) {
    std::string p1 = prefix + "_1.tflite";
    std::string p2 = prefix + "_2.tflite";
    if (!FileExists(p1) || !FileExists(p2)) return false;
    char err[256] = { 0 };
    if (!LoadTfliteApi(h->tf, err, sizeof(err))) {
        DtnsSetError(h, err); return false;
    }
    h->tfModel1 = h->tf.modelFromFile(p1.c_str());
    h->tfModel2 = h->tf.modelFromFile(p2.c_str());
    if (!h->tfModel1 || !h->tfModel2) {
        DtnsSetError(h, "TFLite: failed to load NS model pair"); return false;
    }
    TfLiteInterpreterOptions* o = h->tf.optsCreate();
    if (o) h->tf.optsThreads(o, 1);
    h->tfInterp1 = h->tf.interpCreate(h->tfModel1, o);
    h->tfInterp2 = h->tf.interpCreate(h->tfModel2, o);
    if (!h->tfInterp1 || !h->tfInterp2) {
        DtnsSetError(h, "TFLite: failed to create interpreters"); return false;
    }
    if (h->tf.allocate(h->tfInterp1) != kTfLiteOk ||
        h->tf.allocate(h->tfInterp2) != kTfLiteOk) {
        DtnsSetError(h, "TFLite: AllocateTensors failed"); return false;
    }
    ClassifyTfliteM1(h);
    ClassifyTfliteM2(h);
    return true;
}

static bool RunTfliteFirst(DtlnNsHandle* h, const float micMag[DTNS_BINS],
                            float mask[DTNS_BINS]) {
    TfLiteTensor* inMask = h->tf.getInput(h->tfInterp1, h->tfM1Mask);
    TfLiteTensor* inSt   = h->tf.getInput(h->tfInterp1, h->tfM1StateIn);
    if (!inMask || !inSt) return false;
    if (h->tf.copyFrom(inMask, micMag, DTNS_BINS * sizeof(float)) != kTfLiteOk)
        return false;
    if (!h->states1.empty() &&
        h->tf.copyFrom(inSt, h->states1.data(),
                        h->states1.size() * sizeof(float)) != kTfLiteOk)
        return false;
    try {
        if (h->tf.invoke(h->tfInterp1) != kTfLiteOk) return false;
    } catch (...) {
        DtnsSetError(h, "TFLite: invoke raised an exception"); return false;
    }
    TfLiteTensor* mo = h->tf.getOutput(h->tfInterp1, h->tfM1MaskOut);
    TfLiteTensor* so = h->tf.getOutput(h->tfInterp1, h->tfM1StateOut);
    if (!mo || !so) return false;
    size_t mc = h->tf.byteSize(mo) / sizeof(float);
    if (mc >= DTNS_BINS)
        h->tf.copyTo(mo, mask, DTNS_BINS * sizeof(float));
    size_t sc = h->tf.byteSize(so) / sizeof(float);
    if (sc == h->states1.size())
        h->tf.copyTo(so, h->states1.data(), sc * sizeof(float));
    return true;
}

static bool RunTfliteSecond(DtlnNsHandle* h, const float est[DTNS_BLOCK_LEN],
                            float out[DTNS_BLOCK_LEN]) {
    TfLiteTensor* inEst = h->tf.getInput(h->tfInterp2, h->tfM2Est);
    TfLiteTensor* inSt  = h->tf.getInput(h->tfInterp2, h->tfM2StateIn);
    if (!inEst || !inSt) return false;
    if (h->tf.copyFrom(inEst, est, DTNS_BLOCK_LEN * sizeof(float)) != kTfLiteOk)
        return false;
    if (!h->states2.empty() &&
        h->tf.copyFrom(inSt, h->states2.data(),
                        h->states2.size() * sizeof(float)) != kTfLiteOk)
        return false;
    try {
        if (h->tf.invoke(h->tfInterp2) != kTfLiteOk) return false;
    } catch (...) {
        DtnsSetError(h, "TFLite: invoke raised an exception"); return false;
    }
    TfLiteTensor* bo = h->tf.getOutput(h->tfInterp2, h->tfM2Out);
    TfLiteTensor* so = h->tf.getOutput(h->tfInterp2, h->tfM2StateOut);
    if (!bo || !so) return false;
    size_t bc = h->tf.byteSize(bo) / sizeof(float);
    if (bc >= DTNS_BLOCK_LEN)
        h->tf.copyTo(bo, out, DTNS_BLOCK_LEN * sizeof(float));
    size_t sc = h->tf.byteSize(so) / sizeof(float);
    if (sc == h->states2.size())
        h->tf.copyTo(so, h->states2.data(), sc * sizeof(float));
    return true;
}

// ---------------- ONNX backend ----------------
static size_t OnnxElemCount(const Ort::Session* s, bool isInput, size_t idx) {
    try {
        Ort::TypeInfo ti = isInput ? s->GetInputTypeInfo(idx) : s->GetOutputTypeInfo(idx);
        auto tsi = ti.GetTensorTypeAndShapeInfo();
        size_t n = 1;
        for (auto d : tsi.GetShape()) n *= (d > 0 ? (size_t)d : 1);
        return n;
    } catch (...) { return 0; }
}

static bool RunOnnxFirst(DtlnNsHandle* h, const float micMag[DTNS_BINS],
                         float mask[DTNS_BINS]) {
    try {
        std::vector<int64_t> mixShape{ 1, 1, DTNS_BINS, 2 };
        std::vector<int64_t> stateShape{ 1, 1, (int64_t)h->states1.size() };
        if (h->states1.empty()) stateShape[2] = 1;
        Ort::Value tMix = Ort::Value::CreateTensor<float>(
            *h->mem, const_cast<float*>(micMag), DTNS_BINS * 2,
            mixShape.data(), mixShape.size());
        Ort::Value tSt;
        if (h->states1.empty()) {
            float z = 0;
            stateShape[2] = 1;
            tSt = Ort::Value::CreateTensor<float>(*h->mem, &z, 1,
                                                  stateShape.data(), stateShape.size());
        } else {
            tSt = Ort::Value::CreateTensor<float>(*h->mem, h->states1.data(),
                                                  h->states1.size(),
                                                  stateShape.data(), stateShape.size());
        }
        const char* inN[] = { h->in1Ptr[0], h->in1Ptr[1] };
        Ort::Value inputs[] = { std::move(tMix), std::move(tSt) };
        auto outs = h->sess1->Run(Ort::RunOptions{ nullptr },
                                  inN, inputs, 2,
                                  h->out1Ptr.data(), h->out1Ptr.size());
        float* mp = outs[0].GetTensorMutableData<float>();
        memcpy(mask, mp, DTNS_BINS * sizeof(float));
        if (outs.size() >= 2) {
            float* sp = outs[1].GetTensorMutableData<float>();
            size_t c = outs[1].GetTensorTypeAndShapeInfo().GetElementCount();
            if (c == h->states1.size())
                memcpy(h->states1.data(), sp, c * sizeof(float));
        }
        return true;
    } catch (...) { return false; }
}

static bool RunOnnxSecond(DtlnNsHandle* h, const float est[DTNS_BLOCK_LEN],
                          float out[DTNS_BLOCK_LEN]) {
    try {
        std::vector<int64_t> estShape{ 1, 1, DTNS_BLOCK_LEN };
        std::vector<int64_t> stateShape{ 1, 1, (int64_t)h->states2.size() };
        if (h->states2.empty()) stateShape[2] = 1;
        Ort::Value tEst = Ort::Value::CreateTensor<float>(
            *h->mem, const_cast<float*>(est), DTNS_BLOCK_LEN,
            estShape.data(), estShape.size());
        Ort::Value tSt;
        if (h->states2.empty()) {
            float z = 0;
            stateShape[2] = 1;
            tSt = Ort::Value::CreateTensor<float>(*h->mem, &z, 1,
                                                  stateShape.data(), stateShape.size());
        } else {
            tSt = Ort::Value::CreateTensor<float>(*h->mem, h->states2.data(),
                                                  h->states2.size(),
                                                  stateShape.data(), stateShape.size());
        }
        const char* inN[] = { h->in2Ptr[0], h->in2Ptr[1] };
        Ort::Value inputs[] = { std::move(tEst), std::move(tSt) };
        auto outs = h->sess2->Run(Ort::RunOptions{ nullptr },
                                  inN, inputs, 2,
                                  h->out2Ptr.data(), h->out2Ptr.size());
        float* op = outs[0].GetTensorMutableData<float>();
        memcpy(out, op, DTNS_BLOCK_LEN * sizeof(float));
        if (outs.size() >= 2) {
            float* sp = outs[1].GetTensorMutableData<float>();
            size_t c = outs[1].GetTensorTypeAndShapeInfo().GetElementCount();
            if (c == h->states2.size())
                memcpy(h->states2.data(), sp, c * sizeof(float));
        }
        return true;
    } catch (...) { return false; }
}

static bool TryOnnx(DtlnNsHandle* h, const std::string& prefix) {
    std::string p1 = prefix + "_1.onnx";
    std::string p2 = prefix + "_2.onnx";
    if (!FileExists(p1) || !FileExists(p2)) return false;
    try {
        h->env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "dtln-ns");
        h->opts = new Ort::SessionOptions();
        h->opts->SetIntraOpNumThreads(1);
        h->opts->SetInterOpNumThreads(1);
        h->opts->SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        h->mem = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        std::wstring w1(p1.begin(), p1.end()), w2(p2.begin(), p2.end());
        h->sess1 = new Ort::Session(*h->env, w1.c_str(), *h->opts);
        h->sess2 = new Ort::Session(*h->env, w2.c_str(), *h->opts);
        Ort::AllocatorWithDefaultOptions alloc;
        for (int i = 0; i < (int)h->sess1->GetInputCount(); i++) {
            h->in1Names.push_back(h->sess1->GetInputNameAllocated(
                (size_t)i, alloc).get());
            h->out1Names.push_back(h->sess1->GetOutputNameAllocated(
                (size_t)i, alloc).get());
        }
        for (int i = 0; i < (int)h->sess2->GetInputCount(); i++) {
            h->in2Names.push_back(h->sess2->GetInputNameAllocated(
                (size_t)i, alloc).get());
            h->out2Names.push_back(h->sess2->GetOutputNameAllocated(
                (size_t)i, alloc).get());
        }
        for (auto& s : h->in1Names) h->in1Ptr.push_back(s.c_str());
        for (auto& s : h->out1Names) h->out1Ptr.push_back(s.c_str());
        for (auto& s : h->in2Names) h->in2Ptr.push_back(s.c_str());
        for (auto& s : h->out2Names) h->out2Ptr.push_back(s.c_str());
        size_t s0 = h->sess1->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementCount();
        size_t s1 = h->sess1->GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetElementCount();
        if (s1 >= s0) { h->onnxM1Mask = 0; h->onnxM1State = 1; }
        else          { h->onnxM1Mask = 1; h->onnxM1State = 0; }
        h->states1.assign(s1 ? s1 : 1, 0.0f);
        s0 = h->sess2->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementCount();
        s1 = h->sess2->GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetElementCount();
        if (s1 >= s0) { h->onnxM2Est = 0; h->onnxM2State = 1; }
        else          { h->onnxM2Est = 1; h->onnxM2State = 0; }
        h->states2.assign(s1 ? s1 : 1, 0.0f);
        return true;
    } catch (...) { return false; }
}

// ---------------- DSP ----------------
static void DtlnNsProcessShift(DtlnNsHandle* h, const float micNew[DTNS_BLOCK_SHIFT]) {
    memmove(h->micBuf, h->micBuf + DTNS_BLOCK_SHIFT,
            (DTNS_BLOCK_LEN - DTNS_BLOCK_SHIFT) * sizeof(float));
    memcpy(h->micBuf + DTNS_BLOCK_LEN - DTNS_BLOCK_SHIFT, micNew,
           DTNS_BLOCK_SHIFT * sizeof(float));

    double micIn[DTNS_BLOCK_LEN];
    for (int i = 0; i < DTNS_BLOCK_LEN; i++) micIn[i] = h->micBuf[i];
    std::vector<std::complex<double>> micSpec(DTNS_BLOCK_LEN);
    std::vector<size_t> shape{ (size_t)DTNS_BLOCK_LEN };
    std::vector<size_t> axes{ 0 };
    std::vector<ptrdiff_t> strideIn{ sizeof(double) };
    std::vector<ptrdiff_t> strideOut{ sizeof(std::complex<double>) };
    pocketfft::r2c(shape, strideIn, strideOut, axes, pocketfft::FORWARD,
                   micIn, micSpec.data(), 1.0);

    float micMag[DTNS_BINS];
    for (int i = 0; i < DTNS_BINS; i++)
        micMag[i] = (float)std::abs(micSpec[i]);

    float mask[DTNS_BINS];
    for (int i = 0; i < DTNS_BINS; i++) mask[i] = 1.0f;
    if (h->backend == DTNS_TFLITE) RunTfliteFirst(h, micMag, mask);
    else if (h->backend == DTNS_ONNX) RunOnnxFirst(h, micMag, mask);

    std::vector<std::complex<double>> estSpec(DTNS_BLOCK_LEN);
    for (int i = 0; i < DTNS_BINS; i++)
        estSpec[i] = micSpec[i] * (double)mask[i];
    double estTd[DTNS_BLOCK_LEN] = { 0 };
    pocketfft::c2r(shape, strideOut, strideIn, axes, pocketfft::BACKWARD,
                   estSpec.data(), estTd, 1.0);
    float estBlock[DTNS_BLOCK_LEN];
    for (int i = 0; i < DTNS_BLOCK_LEN; i++)
        estBlock[i] = (float)(estTd[i] / DTNS_BLOCK_LEN);

    float outBlock[DTNS_BLOCK_LEN];
    memcpy(outBlock, estBlock, sizeof(outBlock));
    if (h->backend == DTNS_TFLITE) RunTfliteSecond(h, estBlock, outBlock);
    else if (h->backend == DTNS_ONNX) RunOnnxSecond(h, estBlock, outBlock);

    for (int i = 0; i < DTNS_BLOCK_LEN; i++) h->outBuf[i] += outBlock[i];
    for (int i = 0; i < DTNS_BLOCK_SHIFT; i++) {
        if (h->rCount < 4096) {
            h->oring[(h->rHead + h->rCount) % 4096] = h->outBuf[i];
            h->rCount++;
        } else {
            h->dropped++;  // ring full — processed audio lost
        }
    }
    memmove(h->outBuf, h->outBuf + DTNS_BLOCK_SHIFT,
            (DTNS_BLOCK_LEN - DTNS_BLOCK_SHIFT) * sizeof(float));
    memset(h->outBuf + DTNS_BLOCK_LEN - DTNS_BLOCK_SHIFT, 0,
           DTNS_BLOCK_SHIFT * sizeof(float));
}

// ---------------- C API ----------------
extern "C" {

DtlnNsHandle* DtlnNsNew(const char* onnxPath) {
    DtlnNsHandle* h = new (std::nothrow) DtlnNsHandle();
    if (!h) return nullptr;
    if (!onnxPath) return h;
    // Check the actual pair files, not the bare prefix — "models/dtln_ns_128"
    // is a directory prefix, not a file, so FileExists() on it is always
    // false and the stage silently stayed off.
    std::string p1 = std::string(onnxPath) + "_1.tflite";
    std::string p2 = std::string(onnxPath) + "_2.tflite";
    std::string q1 = std::string(onnxPath) + "_1.onnx";
    std::string q2 = std::string(onnxPath) + "_2.onnx";
    if (!((FileExists(p1) && FileExists(p2)) ||
          (FileExists(q1) && FileExists(q2)))) {
        DtnsSetError(h, "dtln_ns model pair not found in models/ (NS stays off)");
        return h;
    }
    if (TryTflite(h, onnxPath)) {
        h->backend = DTNS_TFLITE;
        h->rCount = 256;  // 16 ms priming — covers the first shift's OLA
                          // warmup (frame 0's pad half is discarded)
                          // while keeping latency low. A full 4096 ring
                          // made the stage feel sluggish (256 ms).
        return h;
    }
    if (TryOnnx(h, onnxPath)) {
        h->backend = DTNS_ONNX;
        h->rCount = 256;
        return h;
    }
    DtnsSetError(h, "dtln_ns: neither TFLite nor ONNX pair loaded");
    return h;
}

int DtlnNsReady(const DtlnNsHandle* h) {
    return (h && h->backend != DTNS_NONE) ? 1 : 0;
}

int DtlnNsProcess(DtlnNsHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    double si = 0, so = 0;
    for (int i = 0; i < n; i++) si += (double)in[i] * in[i];
    if (h->backend == DTNS_NONE) { memcpy(out, in, (size_t)n * sizeof(float)); return 1; }
    if (h->qn + n > 2048) { memcpy(out, in, (size_t)n * sizeof(float)); return 1; }
    memcpy(h->q + h->qn, in, (size_t)n * sizeof(float));
    h->qn += n;
    while (h->qn >= DTNS_BLOCK_SHIFT)
        DtlnNsProcessShift(h, h->q + h->qn - DTNS_BLOCK_SHIFT);
    for (int i = 0; i < n; i++) {
        if (h->rCount > 0) {
            out[i] = h->oring[h->rHead];
            h->rHead = (h->rHead + 1) % 4096;
            h->rCount--;
        } else {
            out[i] = in[i];  // priming: pass through until the OLA warms up
        }
        so += (double)out[i] * out[i];
    }
    h->lastInRms  = (float)sqrt(si / n);
    h->lastOutRms = (float)sqrt(so / n);
    h->lastInN    = n;
    return 1;
}

void DtlnNsReset(DtlnNsHandle* h) {
    if (!h) return;
    memset(h->micBuf, 0, sizeof(h->micBuf));
    memset(h->outBuf, 0, sizeof(h->outBuf));
    h->qn = 0;
    memset(h->oring, 0, sizeof(h->oring));
    h->rHead = h->rCount = 0;
    h->dropped = 0;
    h->lastInRms = h->lastOutRms = 0;
    h->lastInN = 0;
    memset(h->states1.data(), 0, h->states1.size() * sizeof(float));
    memset(h->states2.data(), 0, h->states2.size() * sizeof(float));
    h->lastError[0] = 0;
}

void DtlnNsDestroy(DtlnNsHandle* h) {
    if (!h) return;
    if (h->backend == DTNS_TFLITE) {
        if (h->tfInterp2) h->tf.interpDelete(h->tfInterp2);
        if (h->tfInterp1) h->tf.interpDelete(h->tfInterp1);
        if (h->tfModel2)  h->tf.modelDelete(h->tfModel2);
        if (h->tfModel1)  h->tf.modelDelete(h->tfModel1);
        if (h->tf.dll)     FreeLibrary(h->tf.dll);
    }
    if (h->backend == DTNS_ONNX) {
        delete h->sess2; delete h->sess1;
        delete h->mem; delete h->opts; delete h->env;
    }
    delete h;
}

const char* DtnsLastError(const DtlnNsHandle* h) {
    if (!h) return "null handle";
    return h->lastError[0] ? h->lastError : "ok";
}

void DtlnNsStatsGet(const DtlnNsHandle* h, DtlnNsStats* s) {
    if (!s) return;
    if (!h) { memset(s, 0, sizeof(*s)); return; }
    s->backend  = h->backend;
    s->rCount   = h->rCount;
    s->inRms    = h->lastInRms;
    s->outRms   = h->lastOutRms;
    s->dropped  = h->dropped;
}

void DtlnNsDump(const DtlnNsHandle* h) {
    if (!h) { printf("DTLN-NS: off (null)\n"); return; }
    printf("DTLN-NS: backend=%d rCount=%d dropped=%d last inRms=%.4f outRms=%.4f n=%d err=%s\n",
           h->backend, h->rCount, h->dropped, h->lastInRms, h->lastOutRms,
           h->lastInN, h->lastError[0] ? h->lastError : "ok");
}

}  // extern "C"
