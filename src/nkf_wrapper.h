#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkfHandle NkfHandle;

// Create NKF-AEC engine. modelPath = path to nkf.onnx
// nsEnabled = WebRTC noise suppression (Moderate) on top of the
// residual stage; the residual AEC3 pass itself is always on.
// DTLN already removes noise itself, so it gets no flag.
NkfHandle* NkfNew(const char* modelPath, bool nsEnabled);

// Process one frame of audio.
//   mic:   int16 mic input
//   ref:   int16 speaker reference input
//   out:   int16 cleaned output
//   frameSize: samples per frame (e.g. 160 at 16kHz)
//   NOTE: This must only be called with sample rate 16000.
//   Internally performs TDC (ref->mic delay alignment by cross-
//   correlation, required by upstream NKF) and runs a divergence
//   guard. Exposure is staged on one continuous block timeline:
//   warm-up (lag unknown) and a post-lock shadow phase carry mic on
//   the wire while the engine runs and is monitored only; then a
//   256 ms crossfade brings NKF in — cold-start spikes are never
//   heard, and there are no engagement holes or replays. Guard trips
//   drop back to shadow rather than exposing a reset; a self-monitor
//   loop detector (ref vs our output — Discord mic test / Listen to
//   myself on speakers) holds exposure on mic while a feedback loop
//   is present; after too many guard trips the session fails open to
//   permanent mic passthrough. Output then runs a residual WebRTC
//   AEC3 stage (always on — NKF is strictly linear, AEC3's nonlinear
//   suppressor eats the echoey leftovers; also active on shadow /
//   fail-up passthrough) plus optional NS.
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