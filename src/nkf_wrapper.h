#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkfHandle NkfHandle;

// Create NKF-AEC engine. modelPath = path to nkf.onnx.
// v2.0: the in-engine WebRTC NS/WPE stages are retired — dereverb and
// feedback suppression run as the global WPE/notch stages stacked
// after every engine (see wpe.h / notch.h).
NkfHandle* NkfNew(const char* modelPath);

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
//   myself on speakers) holds exposure on mic while a feedback loop is
//   present AND the ref->mic delay is not a confident cross-correlation
//   lock; the first loop confirm then holds an 8 s convergence window
//   (the engine freezes while it sits in an active loop), and after too
//   many guard trips the session fails open to permanent mic passthrough.
void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize);

// Telemetry snapshot for the UI (benign race: audio thread writes, UI
// thread reads — same pattern as the speech gate status).
typedef struct NkfState {
    int lagSamples;    // current ref->mic alignment (samples @16 kHz)
    int confident;     // 1 = lag from a real cross-correlation peak
    int locked;        // 1 = lag accepted (real peak or 3 s grace)
    int exposed;       // 1 = NKF output on the wire, 0 = mic shadow
    int loopActive;    // 1 = self-monitor loop currently detected
    int guardResets;   // divergence resets this session
    int giveUp;        // 1 = failed open (permanent mic passthrough)
} NkfState;
void NkfGetState(NkfHandle* h, NkfState* s);

// Reset all internal state (buffers, TDC lag, guard counters).
// Engines are recreated on Stop/Start anyway; provided for completeness.
void NkfReset(NkfHandle* h);

// Destroy the engine
void NkfDestroy(NkfHandle* h);

#ifdef __cplusplus
}
#endif
