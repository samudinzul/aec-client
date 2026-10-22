#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NovasrHandle NovasrHandle;

// NovaSR super-resolution tail (YatharthS/NovaSR, Apache-2.0: 52 KB conv
// net, 16 kHz in -> 48 kHz out, baked to exp/novasr/novasr.onnx with
// torch/ORT parity 1.000000) on the vendored ONNX Runtime — same runtime
// the NKF engine uses, linked the same way (never a new DLL).
//
// Streaming chunked inference with overlap-discard: the graph is
// feedforward but symmetric (non-causal, ~+/-20-sample lookahead), so
// each STEP emits from a HALO-padded window and the edges are
// discarded (chunked-vs-whole equivalence 0.999998). Costs a fixed
// 8 ms algorithmic delay (HALO @ 16 kHz). First call primes with
// edge replication (no mute, ever).
//
// 16 kHz float in, 48 kHz float out; the caller owns both rates
// (chain stays 16 kHz, output device runs 48 kHz — see exp/main.cpp).
// Emits exactly 3*n samples per call (after priming).
//
// Always returns a handle (it carries the error string); NovasrReady()
// == 0 means the stage must stay off (missing model, IO mismatch, ORT
// failure — fail-closed at Start, like LocalVQE/GTCRN).
// Per-frame inference failure falls back to pure linear upsampling
// (fail-open, never mute) and latches the error until NovasrReset.
NovasrHandle* NovasrNew(const char* modelPath);

// 1 when the stage can run.
int NovasrReady(const NovasrHandle* h);

// Process n samples: out receives exactly 3*n samples. Returns 1 on the
// neural path, 0 on linear fallback. Realtime-safe: no heap allocation
// after New, single-threaded use.
int NovasrProcess(NovasrHandle* h, const float* in, float* out, int n);

// Clear the input FIFO + prime, the error latch and the ms follower.
void NovasrReset(NovasrHandle* h);

void NovasrDestroy(NovasrHandle* h);

const char* NovasrLastError(const NovasrHandle* h);

// Live diagnostics. Null-safe.
struct NovasrStats {
    int ready;            // NovasrReady() at last query
    int neural;           // 1 = last Process ran the graph
    float inRms, outRms;  // last Process call
    float ms;             // inference ms EMA (this thread only)
};
void NovasrStatsGet(const NovasrHandle* h, NovasrStats* s);

// Debug: print session IO table + last stats to stdout.
void NovasrDump(const NovasrHandle* h);

#ifdef __cplusplus
}
#endif
