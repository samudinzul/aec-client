#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle
typedef struct Aec3Handle Aec3Handle;

// Create AEC3 engine. frameSize = samples per 10 ms frame.
Aec3Handle* Aec3New(int sampleRate, int frameSize);

// Process one frame: mic + ref -> out
void Aec3CancelEcho(Aec3Handle* h, const int16_t* mic, const int16_t* ref,
                    int16_t* out, int frameSize);

// Destroy engine
void Aec3Destroy(Aec3Handle* h);

#ifdef __cplusplus
}
#endif