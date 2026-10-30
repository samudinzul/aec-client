#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtcrnHandle GtcrnHandle;

// GTCRN noise suppression (Xiaobin-Rong/gtcrn, MIT: grouped temporal
// convolutional recurrent network, ~48 K params, 33 MMACs/s) via the
// official streaming graph (stream/onnx_models/gtcrn_simple.onnx) on the
// vendored ONNX Runtime — same runtime the NKF engine uses, linked the
// same way (never a new DLL).
//
// Mic-only NS: no reference signal needed (unlike AEC engines).
// 16 kHz mono float, 512-point STFT / 256 hop inside; the wrapper's FIFO
// accepts any n and emits exactly n.
//
// Always returns a handle (it carries the error string); GtcrnReady() == 0
// means the stage must stay off (missing model, IO mismatch, ORT failure —
// fail-open, like a missing DTLN-NS pair).
//
// v0: session loads + IO table is captured; Process is passthrough until
// the V1 inference path is validated against probe.py output.
GtcrnHandle* GtcrnNew(const char* modelPath);

// 1 when the stage can run.
int GtcrnReady(const GtcrnHandle* h);

// Process n samples: out receives exactly n samples. On any inference
// error the handle latches pass-through until GtcrnReset — fail-open,
// never mute. Realtime-safe: no heap allocation after New,
// single-threaded use.
int GtcrnProcess(GtcrnHandle* h, const float* in, float* out, int n);

// Zero caches + FIFOs, clear the error latch and RMS followers.
void GtcrnReset(GtcrnHandle* h);

void GtcrnDestroy(GtcrnHandle* h);

const char* GtcrnLastError(const GtcrnHandle* h);

// Live diagnostics. Null-safe (zeroes on null).
struct GtcrnStats {
    int ready;            // GtcrnReady() at last query
    int neural;           // 1 = V1 inference path engaged, 0 = v0 bypass
    float inRms, outRms;  // last Process call
    int dropped;          // FIFO overflow samples dropped
};
void GtcrnStatsGet(const GtcrnHandle* h, GtcrnStats* s);

// Debug: print session IO table + last stats to stdout.
void GtcrnDump(const GtcrnHandle* h);

#ifdef __cplusplus
}
#endif
