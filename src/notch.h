#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NotchHandle NotchHandle;

// Streaming LMS adaptive notch filter ("Feedback suppression" stage,
// stacked after WPE): two cascaded second-order notches whose center
// frequencies track narrowband howling/ringing loops.
//   sampleRate: 16000 or 48000 (notch bandwidth stays ~60 Hz at both
//   rates); anything else returns nullptr (stage off).
//   NotchProcess: in-place, exactly n samples. Fail-open: internal
//   state is bounded (clamped frequency, slew-limited adaptation) —
//   the stage can never mute or blow up the stream.
NotchHandle* NotchNew(int sampleRate);
// Near-end speech flag: while the person talks, frequency adaptation
// FREEZES (voice harmonics must never capture the notch); filtering
// keeps applying. Between speech, above the silence floor, the LMS
// tracks howl. Default = talking (frozen) until told otherwise.
void NotchSetSpeech(NotchHandle* h, int speaking);
void NotchProcess(NotchHandle* h, int16_t* buf, int n);
void NotchReset(NotchHandle* h);
void NotchDestroy(NotchHandle* h);
// Current center frequency of section idx (0/1), Hz. For tests/telemetry.
double NotchFreq(NotchHandle* h, int idx);
// 1 once the section has latched onto a tone and its cut is actually
// applied; 0 while it still runs as an exact bypass (no howl found).
int NotchEngaged(NotchHandle* h, int idx);

#ifdef __cplusplus
}
#endif
