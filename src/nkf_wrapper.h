#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkfHandle NkfHandle;

// Create NKF-AEC engine. modelPath = path to nkf.onnx
// nsEnabled = WebRTC noise suppression (Moderate) on top of the
// residual stage. NS is skipped automatically when dryModelPath is
// given — the GTCRN stage denoises itself, stacking both only
// over-suppresses.
// dryModelPath = GTCRN "Dry voice" model ("models/gtcrn_stream.onnx"),
// or nullptr to skip the stage; a missing/broken file just leaves the
// stage off (fail-open), the engine still starts.
// residualAec = the post-NKF WebRTC AEC3 pass (on by default). It
// aggressively eats the echoey leftovers NKF can't (strictly linear
// canceller), but its multi-band suppressor can sound processed /
// robotic on some setups while speakers play — pass false to hear
// raw NKF output (echo may return).
// DTLN already removes noise itself, so it gets no flag.
NkfHandle* NkfNew(const char* modelPath, bool nsEnabled,
                  const char* dryModelPath, bool residualAec);

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
//   permanent mic passthrough. Output then runs the optional residual
//   WebRTC AEC3 stage (on by default — NKF is strictly linear, AEC3's
//   nonlinear suppressor eats the echoey leftovers; can be toggled off
//   when it sounds too processed; also active on shadow / fail-up
//   passthrough) plus optional NS, then the optional GTCRN
//   "Dry voice" stage (room-reverb reduction, when the model was
//   given to NkfNew) before it reaches the wire.
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