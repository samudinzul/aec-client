#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DtlnHandle DtlnHandle;

// Create DTLN-AEC engine.
//   modelPrefix: path prefix without suffix, e.g. "models/dtln_aec_128"
//     Backend probe order:
//       1. TFLite: "<prefix>_1.tflite" + "<prefix>_2.tflite" via
//          tensorflowlite_c.dll loaded at runtime (no link-time dep).
//       2. ONNX:   "<prefix>_1.onnx" + "<prefix>_2.onnx" via ONNX Runtime
//          (already linked for NKF).
//     Returns nullptr on failure; use DtlnLastError() for details.
//     NOTE: 16 kHz only. Block 512, shift 128 (257-bin FFT).
DtlnHandle* DtlnNew(const char* modelPrefix);

// Process audio.
//   mic/ref: int16 input frames, out: int16 cleaned output.
//   frameSize: samples per call (e.g. 160 at 16 kHz / 10 ms).
//   Internal 128-sample shift is bridged with accumulators, same pattern
//   as NkfProcess / LocalVqeProcess.
void DtlnProcess(DtlnHandle* h, const int16_t* mic, const int16_t* ref,
                 int16_t* out, int frameSize);

// Reset internal state (call after Stop/Start).
void DtlnReset(DtlnHandle* h);

// Destroy the engine.
void DtlnDestroy(DtlnHandle* h);

// Human-readable error from the last failed New/Process. Never nullptr.
const char* DtlnLastError(DtlnHandle* h);

// Which backend got selected: 0=none, 1=tflite, 2=onnx.
int DtlnBackend(DtlnHandle* h);

#ifdef __cplusplus
}
#endif
