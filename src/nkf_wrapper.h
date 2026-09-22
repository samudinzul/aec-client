#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkfHandle NkfHandle;

// Create NKF-AEC engine. modelPath = path to nkf.onnx
// nsEnabled = extra WebRTC noise-suppression pass (Moderate) on the
// NKF output. DTLN already removes noise itself, so it gets no flag.
NkfHandle* NkfNew(const char* modelPath, bool nsEnabled);

// Process one frame of audio.
//   mic:   int16 mic input
//   ref:   int16 speaker reference input
//   out:   int16 cleaned output
//   frameSize: samples per frame (e.g. 160 at 16kHz)
//   NOTE: This must only be called with sample rate 16000.
//   Internally performs TDC (ref->mic delay alignment by cross-
//   correlation, required by upstream NKF) and runs a divergence
//   guard: on runaway output the filter is reset; after too many
//   resets the session fails open to mic passthrough.
void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize);

// Reset all internal state (buffers, TDC lag, guard counters).
// Engines are recreated on Stop/Start anyway; provided for completeness.
void NkfReset(NkfHandle* h);

// Destroy the engine
void NkfDestroy(NkfHandle* h);

#ifdef __cplusplus
}
#endif