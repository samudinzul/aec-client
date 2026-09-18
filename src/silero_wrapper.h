#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SileroHandle SileroHandle;

#define SILERO_CHUNK 512    // 32 ms @ 16 kHz of NEW audio per inference
#define SILERO_CONTEXT 64   // context samples prepended (upstream recipe)
#define SILERO_WINDOW (SILERO_CHUNK + SILERO_CONTEXT)  // 576 model input

// Streaming Silero VAD (snakers4/silero-vad, MIT) via ONNX Runtime
// (already linked for NKF/DTLN). 16 kHz mono float32 in, speech
// probability out. LSTM state is carried across chunks internally.
//
//   onnxPath: e.g. "models/silero_vad.onnx"
// Returns nullptr on failure; use SileroLastError() for details.
SileroHandle* SileroNew(const char* onnxPath);

// Push any number of 16 kHz mono float32 samples ([-1,1]). Internally
// accumulates 512-sample chunks; each completed chunk runs inference
// (< 1 ms) and its probability is written to *prob.
// Returns 1 + sets *prob when a chunk completed, else 0.
// Realtime-safe: no allocation after New, single-threaded use.
int SileroPush(SileroHandle* h, const float* pcm, int n, float* prob);

// Zero LSTM state + drop partial chunk. Call on Start/Stop/engine change.
void SileroReset(SileroHandle* h);

void SileroDestroy(SileroHandle* h);

// Human-readable error from the last failed New/Push. Never nullptr.
const char* SileroLastError(SileroHandle* h);

#ifdef __cplusplus
}
#endif
