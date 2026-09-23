#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtcrnHandle GtcrnHandle;

#define GTCRN_HOP 256   // hop size @ 16 kHz = 16 ms
#define GTCRN_WIN 512   // STFT window = 32 ms

// Streaming GTCRN speech enhancement (Xiaobin-Rong/gtcrn, MIT).
// 16 kHz mono float32 in, same count out: internally buffers 256-sample
// hops, runs the model every hop, overlap-adds back. When active it
// adds ~32 ms algorithmic latency. ONNX already linked for NKF/DTLN.
//
//   onnxPath: e.g. "models/gtcrn_stream.onnx"
// Returns a handle; GtcrnReady() == 0 means the stage must stay off
// (missing/broken model — fail-open, the caller continues without it).
GtcrnHandle* GtcrnNew(const char* onnxPath);

// 1 when the model loaded and the stage can run.
int GtcrnReady(const GtcrnHandle* h);

// Process n samples: out receives exactly n samples (delayed version
// of in once primed). On any inference error the handle latches
// pass-through (out == in) until GtcrnReset — fail-open, never mute.
// Realtime-safe: no heap allocation after New, single-threaded use.
int GtcrnProcess(GtcrnHandle* h, const float* in, float* out, int n);

// Zero STFT/model caches + buffers, clear the error latch.
void GtcrnReset(GtcrnHandle* h);

void GtcrnDestroy(GtcrnHandle* h);

// Human-readable error from the last failed New/Process. Never nullptr.
const char* GtcrnLastError(const GtcrnHandle* h);

#ifdef __cplusplus
}
#endif
