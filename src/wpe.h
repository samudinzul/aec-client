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
void WpeReset(WpeHandle* h);
void WpeDestroy(WpeHandle* h);

#ifdef __cplusplus
}
#endif
