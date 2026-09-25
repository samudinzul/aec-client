#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WpeHandle WpeHandle;

// Streaming single-channel WPE dereverb (NKF post-filter).
//   sampleRate: must be 16000; anything else returns nullptr (stage off).
//   WpeProcess: exactly n in -> n out (fixed ~32 ms algorithmic latency,
//   zeros during the first window). Fail-open: a bad call or internal
//   error latches pass-through, never kills the engine.
WpeHandle* WpeNew(int sampleRate);
int  WpeProcess(WpeHandle* h, const float* in, float* out, int n);
// Near-end speech flag (audio thread, one frame stale is fine):
// while the person is talking the predictor bound tightens so voice
// level survives; between speech it stays wide to eat reverb/echo
// tails. Default = speech-safe (tight bound).
void WpeSetSpeech(WpeHandle* h, int speaking);
void WpeReset(WpeHandle* h);
void WpeDestroy(WpeHandle* h);

#ifdef __cplusplus
}
#endif
