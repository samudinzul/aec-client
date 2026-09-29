#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DtlnNsHandle DtlnNsHandle;

// DTLN noise reduction (networkedaudio/Realtime_AudioDenoise_EchoCancellation,
// MIT — the same DTLN architecture as the AEC pair, minus the loud-playback
// feed): model_1 [mag(257), states(512)] -> [mask(257), states(512)],
// model_2 [est(512), states] -> [block(512), states]. 512-sample block,
// 128-sample shift, 16 kHz, hann OLA — exactly the AEC wrapper's DSP.
//
//   onnxPath: e.g. "models/dtln_ns_128"
// Returns a handle; DtlnNsReady() == 0 means the stage must stay off
// (missing/broken model or no tensorflowlite_c.dll — fail-open).
DtlnNsHandle* DtlnNsNew(const char* onnxPath);

// 1 when the stage can run.
int DtlnNsReady(const DtlnNsHandle* h);

// Process n samples: out receives exactly n samples (delayed version of
// in once primed). On any inference error the handle latches pass-through
// (out == in) until DtlnNsReset — fail-open, never mute.
// Realtime-safe: no heap allocation after New, single-threaded use.
int DtlnNsProcess(DtlnNsHandle* h, const float* in, float* out, int n);

// Zero STFT/model caches + buffers, clear the error latch.
void DtlnNsReset(DtlnNsHandle* h);

void DtlnNsDestroy(DtlnNsHandle* h);

const char* DtnsLastError(const DtlnNsHandle* h);

#ifdef __cplusplus
}
#endif