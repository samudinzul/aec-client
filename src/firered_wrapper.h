#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FireRedHandle FireRedHandle;

// Streaming FireRed Stream-VAD detector (FireRedTeam/FireRedVAD,
// Apache-2.0; ONNX export via leospark/FireRedVAD-Engineering, MIT).
//
// Contract (proven parity vs golden_stream.safetensors):
//   frontend: kaldi fbank, 80 bins, 25 ms window / 10 ms hop @16 kHz,
//     povey window, DC removal, 0.97 preemphasis, log floor 1e-10,
//     global CMVN from firered_cmvn.bin (80 means + 80 istd, f32LE,
//     derived from the official cmvn.ark via kaldiio — see wrapper).
//   model: input [1,1,80] + 8x [1,128,19] caches -> prob + 8 new caches.
//     NOTE: the export emits probabilities directly — do NOT sigmoid
//     (the reference recipe's extra sigmoid double-squashes silence
//     0.03 -> 0.51; found during parity, meanabs 5.4e-5 without it).
//   cadence: one prob per 10 ms frame (finer than Silero's 32 ms).
//   cost: ~0.07 ms/frame on CPU.
//
// FireRedNew(modelPath, cmvnPath): e.g. "models/model_with_caches.onnx",
//   "models/firered_cmvn.bin". The .onnx.data sidecar must sit next to
//   the model (ORT loads external data automatically).
//   Returns nullptr on failure; use FireRedLastError() for details.
// FireRedPush(h, pcm16k, n, prob): feed 16 kHz float mono; returns 1 and
//   sets *prob every time a 160-sample hop completes, else 0.
//   Realtime-safe: no allocation after New, single-threaded.
// FireRedReset(h): zero caches + frontend buffers (call on Start/Stop).
#define FIRERED_HOP 160

FireRedHandle* FireRedNew(const char* modelPath, const char* cmvnPath);
int FireRedPush(FireRedHandle* h, const float* pcm, int n, float* prob);
void FireRedReset(FireRedHandle* h);
void FireRedDestroy(FireRedHandle* h);
const char* FireRedLastError(FireRedHandle* h);

#ifdef __cplusplus
}
#endif
