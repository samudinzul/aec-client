#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LocalVqeHandle LocalVqeHandle;

// LocalVQE v1.4-AEC echo cancellation (localai-org/LocalVQE, Apache-2.0;
// portable ONNX export by soniqo). Echo-ONLY engine: removes the far-end
// echo, passes voice/noise/room through. Needs a synchronized mic +
// far-end reference every call (a silent ref preserves mic speech).
//
// Three files (see exp/get_models.sh):
//   onnxPath:     LocalVQEAECResidualMask.onnx (recurrent residual mask)
//   frontendPath: LocalVQEAECFrontend.json (2,742 controller weights +
//                 512-tap sqrt-Hann window + DSP geometry)
//   configPath:   bundle config.json (provenance: run id + GGUF hash;
//                 rejected unless it matches v1.4.r005 exactly)
//
// Pipeline per 256-sample hop (ports soniqo/speech-core
// onnx_localvqe_echo_canceller.cpp flow verbatim):
//   frontend (partitioned-block FD Kalman + controller, 2x128 blocks)
//   -> residual/echo-estimate spectra via the 512/256 codec (bins 1..256)
//   -> mask Run (7 carried states) -> ISTFT overlap-add.
//
// Always returns a handle (it carries the error string);
// LocalVqeReady() == 0 means the engine must stay off (missing files,
// bundle mismatch, ORT failure — fail-closed at Start, like a missing
// DTLN pair). 16 kHz mono float only.
LocalVqeHandle* LocalVqeNew(const char* onnxPath, const char* frontendPath,
                            const char* configPath);

// 1 when the engine can run.
int LocalVqeReady(const LocalVqeHandle* h);

// Process n samples: mic + ref in, exactly n cleaned samples out.
// A mid-run failure latches to mic passthrough until LocalVqeReset —
// fail-open, never mute. Realtime-safe: no heap allocation after New,
// single-threaded use.
int LocalVqeProcess(LocalVqeHandle* h, const float* mic, const float* ref,
                    float* out, int n);

// Zero filter/controller/codec/model state, clear the error latch.
void LocalVqeReset(LocalVqeHandle* h);

void LocalVqeDestroy(LocalVqeHandle* h);

const char* LocalVqeLastError(const LocalVqeHandle* h);

// Live diagnostics. Null-safe (zeroes on null).
struct LocalVqeStats {
    int ready;            // LocalVqeReady() at last query
    int neural;           // 1 = mask Run engaged, 0 = bypass/latched
    int delaySamples;     // frontend bulk-delay estimate
    float delayConf;      // ...and its confidence
    float inRms, refRms;  // last Process call inputs
    float outRms;         // last Process call output
    int dropped;          // FIFO overflow samples dropped
};
void LocalVqeStatsGet(const LocalVqeHandle* h, LocalVqeStats* s);

// Debug: print bundle status + IO table + last stats to stdout.
void LocalVqeDump(const LocalVqeHandle* h);

#ifdef __cplusplus
}
#endif
