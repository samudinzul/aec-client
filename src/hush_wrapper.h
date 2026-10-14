#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HushHandle HushHandle;

// Hush noise suppression (pulp-vision/Hush, Apache-2.0: DeepFilterNet3
// fork with a background-speaker auxiliary loss) via its prebuilt
// weya_nc shared library (bundled ML runtime, like tensorflowlite_c
// — resolved at runtime with LoadLibrary, never linked).
//
//   modelPath: e.g. "models/hush_16k_onnx.tar.gz" (the upstream
//   advanced_dfnet16k_model_best_onnx.tar.gz bundle, renamed: the
//   release asserts ban any staged filename containing "dfn").
// Always returns a handle (it carries the error string);
// HushReady() == 0 means the stage must stay off (DLL, bundle, or
// session missing — fail-open, like a missing DTLN-NS pair).
HushHandle* HushNew(const char* modelPath);

// 1 when the stage can run.
int HushReady(const HushHandle* h);

// Native Hush frame length in samples (160 @ 16 kHz); 0 when not ready.
int HushFrameLen(const HushHandle* h);

// Process n samples: out receives exactly n samples. Shortfalls
// (n not a multiple of the frame length — cannot happen on the
// 16 kHz desktop path, where every frame IS one Hush frame) hold
// the last value instead of stepping to zero (a step clicks).
// Realtime-safe: no heap allocation after New, single-threaded use.
int HushProcess(HushHandle* h, const float* in, float* out, int n);

// Zero session state + FIFOs, clear RMS followers.
void HushReset(HushHandle* h);

void HushDestroy(HushHandle* h);

const char* HushLastError(const HushHandle* h);

// Live diagnostics for the NS stage. Null-safe (zeroes on null).
struct HushStats {
    int backend;            // 0=none, 3=hush
    int frameLen;           // native frame, 0 when not ready
    float inRms, outRms;    // last Process call
    int dropped;            // FIFO overflow samples dropped
};
void HushStatsGet(const HushHandle* h, HushStats* s);

#ifdef __cplusplus
}
#endif
