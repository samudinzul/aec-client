#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkfHandle NkfHandle;

// Create NKF-AEC engine. modelPath = path to nkf.onnx
// nsEnabled = WebRTC noise suppression (Moderate) stacked after the
// NKF block output. wpeEnabled = the streaming WPE dereverb
// post-filter ("Dereverb (WPE)" checkbox, default on): model-free
// late-reverb cancellation, NS -> WPE order, ~32 ms latency, hard
// bounded — voice cut at most ~2.5 dB while NkfSetNearSpeech says
// the person is talking, up to 6 dB between speech (tails).
// DTLN already removes noise itself, so it gets no flags.
NkfHandle* NkfNew(const char* modelPath, bool nsEnabled,
                  bool wpeEnabled);

// Near-end speech flag for the WPE stage (audio thread, call once per
// frame BEFORE NkfProcess; one-frame staleness is intentional — the
// decision comes from the previous frame's VAD). While the person is
// talking, WPE runs its tight predictor bound so voice level survives;
// between speech it widens to eat reverb/echo tails. No-op when the
// WPE stage is disabled.
void NkfSetNearSpeech(NkfHandle* h, int speaking);

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
//   loop detector (ref vs our own output — Discord mic test / Listen to
//   myself on speakers) holds exposure on mic while a feedback loop
//   is present; after too many guard trips the session fails open to
//   permanent mic passthrough. Output then runs the optional NS and
//   the optional WPE dereverb stage (when enabled in NkfNew) before
//   it reaches the wire.
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
