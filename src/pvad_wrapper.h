#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PvadHandle PvadHandle;

// Personalized voice gate: Silero decides WHEN (timing),
// ECAPA embedding decides WHO (identity). Verify runs on a worker
// thread — never on the audio thread (~90 ms per 2 s window).
//
// Model: vedk00/ecapa-voxceleb-speaker-embedding-onnx (Apache-2.0,
// mirror of speechbrain/spkrec-ecapa-voxceleb). Input features
// [1,frames,80] log-mel + feature_lens -> embedding [1,192].
// Frontend: 80-bin log-mel, 25 ms / 10 ms @16 kHz (kaldi-style),
// utterance mean-norm.
// Threshold default 0.60 (cosine). Fail-open: no voiceprint, no
// verification yet, or inference error all read as "owner" — the
// gate never mutes on identity uncertainty, only on measured mismatch.
//
// Voiceprint never leaves the machine (models/voiceprint.bin,
// untracked). Thread-safe via internal mutex; audio thread only
// memcpys under lock, inference happens on worker/UI threads.
#define PVAD_EMB 192
#define PVAD_DEFAULT_THRESHOLD 0.60f

PvadHandle* PvadNew(const char* modelPath);
void PvadDestroy(PvadHandle* h);
const char* PvadLastError(PvadHandle* h);

// Enrollment: Begin, Feed 16 kHz float mono (any chunking), Finish
// embeds the accumulation (needs >= 1 s, caps at 10 s). Returns 1 if
// a voiceprint is ready afterwards.
void PvadBeginEnroll(PvadHandle* h);
void PvadFeedEnroll(PvadHandle* h, const float* pcm, int n);
int PvadFinishEnroll(PvadHandle* h);
int PvadHasVoiceprint(PvadHandle* h);
// Forget the enrolled voiceprint (keeps the handle usable).
void PvadClearVoiceprint(PvadHandle* h);

// Verify a window (recomm. 1-2 s of speech): cosine vs enrolled.
// Returns < -1.0 when no voiceprint exists (fail-open signal).
float PvadVerify(PvadHandle* h, const float* pcm, int n);

int PvadSave(PvadHandle* h, const char* path);
int PvadLoad(PvadHandle* h, const char* path);
void PvadSetThreshold(PvadHandle* h, float t);
float PvadThreshold(PvadHandle* h);

#ifdef __cplusplus
}
#endif
