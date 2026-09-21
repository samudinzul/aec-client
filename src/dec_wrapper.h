#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DecHandle DecHandle;

// Streaming Microsoft AEC-Challenge ICASSP 2022 DEC baseline
// (MIT) via ONNX Runtime (already linked). Post-engine residual
// echo cleanup: engine output + far-end reference in, polished
// output out. 16 kHz mono float32, 160 samples (10 ms hop) per
// call — matches the app's 16 kHz engine frame exactly.
//
// Model: 20 ms sqrt-Hann window, 320-point DFT (161 bins),
// log-power mic+far features, 2-layer GRU (states carried
// internally), magnitude mask + overlap-add. Adds ~10 ms of
// constant algorithmic latency.
//
// Returns 1 on success, 0 on bypass (model missing/broken,
// bad frame size, non-finite mask) — caller leaves audio
// untouched, so the feature always fails open.
DecHandle* DecNew(const char* onnxPath);

// cleaned/ref: 16 kHz float32 mono in [-1,1]; out: same format.
// n must be 160. out may alias cleaned. No allocation after New,
// single-threaded use.
int DecProcess(DecHandle* h, const float* cleaned, const float* ref,
               float* out, int n);

// Non-null and session ready.
int DecUsable(DecHandle* h);

// Zero GRU states + overlap buffers. Call on Start/Stop (audio idle).
void DecReset(DecHandle* h);

void DecDestroy(DecHandle* h);

// Human-readable error from the last failed New/Process. Never nullptr.
const char* DecLastError(DecHandle* h);

#ifdef __cplusplus
}
#endif
