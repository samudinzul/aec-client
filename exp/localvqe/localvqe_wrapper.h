#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LocalVqeHandle LocalVqeHandle;

// LocalVQE v1.4-AEC echo cancellation (localai-org/LocalVQE, Apache-2.0:
// streaming CPU-tuned derivative of DeepVQE, 203 K params, 0.83 ms/hop).
// Echo-ONLY: removes the far-end echo, passes voice/noise/room through.
// Needs a synchronized mic + far-end reference every call.
//
// Model files (see exp/get_models.sh):
//   onnxPath:    exp/models/LocalVQEAECResidualMask.onnx (soniqo portable
//                export of the residual mask) — the neural half.
//   frontendPath: exp/models/LocalVQEAECFrontend.json (controller weights,
//                sqrt-Hann window, DSP geometry) — the host-DSP half.
//
// The ONNX file is NOT a waveform model: the host must run GCC-PHAT
// prealignment, a partitioned-block frequency-domain Kalman filter, the
// 2,742-weight controller, sqrt-Hann STFT + overlap-add around the mask
// (reference: soniqo/speech-core OnnxLocalVQEEchoCanceller).
//
// v0: validates both files exist and the JSON parses far enough to read
// geometry; Process is mic passthrough until the host DSP is ported (V1).
// Always returns a handle; LocalVqeReady() == 0 means stay off (fail-open).
LocalVqeHandle* LocalVqeNew(const char* onnxPath, const char* frontendPath);

// 1 when the stage can run (v0: always 0 with the reason in LastError).
int LocalVqeReady(const LocalVqeHandle* h);

// Process n samples: mic + ref in, exactly n cleaned samples out.
// Latches pass-through on error — never mute. Single-threaded use.
int LocalVqeProcess(LocalVqeHandle* h, const float* mic, const float* ref,
                    float* out, int n);

// Zero filter/controller state, clear the error latch and RMS followers.
void LocalVqeReset(LocalVqeHandle* h);

void LocalVqeDestroy(LocalVqeHandle* h);

const char* LocalVqeLastError(const LocalVqeHandle* h);

// Live diagnostics. Null-safe (zeroes on null).
struct LocalVqeStats {
    int ready;             // LocalVqeReady() at last query
    int neural;            // 1 = V1 host-DSP + mask engaged, 0 = v0 bypass
    float inRms, refRms;   // last Process call inputs
    float outRms;          // last Process call output
    int dropped;           // FIFO overflow samples dropped
};
void LocalVqeStatsGet(const LocalVqeHandle* h, LocalVqeStats* s);

// Debug: print file status + last stats to stdout.
void LocalVqeDump(const LocalVqeHandle* h);

#ifdef __cplusplus
}
#endif
