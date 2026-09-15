#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LocalVqeHandle LocalVqeHandle;

LocalVqeHandle* LocalVqeNew(const char* modelPath);
void LocalVqeProcess(LocalVqeHandle* h, const int16_t* mic, const int16_t* ref,
                     int16_t* out, int hopSamples);
void LocalVqeReset(LocalVqeHandle* h);
void LocalVqeDestroy(LocalVqeHandle* h);
const char* LocalVqeLastError(LocalVqeHandle* h);
void LocalVqeSetNoiseGate(LocalVqeHandle* h, int enabled, float threshold_dbfs);

#ifdef __cplusplus
}
#endif
