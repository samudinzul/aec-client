#include "localvqe_wrapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// v0: file validation + passthrough. The V1 host DSP (GCC-PHAT prealign,
// partitioned-block FD Kalman, 2,742-weight controller from
// LocalVQEAECFrontend.json, sqrt-Hann STFT/OLA around the residual mask)
// is a port of soniqo/speech-core — landed here when validated, not before.

struct LocalVqeHandle {
    std::string error;
    long onnxBytes = 0;
    long frontendBytes = 0;
    bool ready = false;
    float inRms = 0.0f, refRms = 0.0f, outRms = 0.0f;
    int dropped = 0;
};

static long FileSize(const char* path) {
    if (!path || !*path) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

LocalVqeHandle* LocalVqeNew(const char* onnxPath,
                            const char* frontendPath) {
    LocalVqeHandle* h = new LocalVqeHandle();
    h->onnxBytes = FileSize(onnxPath);
    h->frontendBytes = FileSize(frontendPath);
    if (h->onnxBytes < 0) {
        h->error =
            "localvqe: mask ONNX missing (run exp/get_models.sh)";
        return h;
    }
    if (h->frontendBytes < 0) {
        h->error =
            "localvqe: frontend JSON missing (run exp/get_models.sh)";
        return h;
    }
    // v0 stops here: files validated, host DSP not ported yet.
    h->error =
        "localvqe v0: host DSP (Kalman frontend) not ported yet — "
        "passthrough only. Next: vendor soniqo/speech-core or port the "
        "frontend (see exp/README).";
    h->ready = false;
    return h;
}

int LocalVqeReady(const LocalVqeHandle* h) {
    return (h && h->ready) ? 1 : 0;
}

int LocalVqeProcess(LocalVqeHandle* h, const float* mic, const float* ref,
                    float* out, int n) {
    if (!h || n <= 0) return 0;
    if (!mic || !out) return 0;
    // v0: mic passthrough (a silent ref preserves mic speech — same rule
    // as the upstream runtime: never subtract a mix, never mute).
    double si = 0.0, sr = 0.0, so = 0.0;
    for (int i = 0; i < n; i++) {
        out[i] = mic[i];
        si += (double)mic[i] * mic[i];
        if (ref) sr += (double)ref[i] * ref[i];
        so += (double)out[i] * out[i];
    }
    h->inRms = (float)sqrt(si / n);
    h->refRms = ref ? (float)sqrt(sr / n) : 0.0f;
    h->outRms = (float)sqrt(so / n);
    return 0;  // v0 bypass
}

void LocalVqeReset(LocalVqeHandle* h) {
    if (!h) return;
    h->inRms = h->refRms = h->outRms = 0.0f;
    h->dropped = 0;
}

void LocalVqeDestroy(LocalVqeHandle* h) { delete h; }

const char* LocalVqeLastError(const LocalVqeHandle* h) {
    return (h && !h->error.empty()) ? h->error.c_str() : "";
}

void LocalVqeStatsGet(const LocalVqeHandle* h, LocalVqeStats* s) {
    if (!s) return;
    if (!h) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->ready = h->ready ? 1 : 0;
    s->neural = 0;  // v0 bypass; V1 sets 1 when mask engages
    s->inRms = h->inRms;
    s->refRms = h->refRms;
    s->outRms = h->outRms;
    s->dropped = h->dropped;
}

void LocalVqeDump(const LocalVqeHandle* h) {
    if (!h) {
        printf("localvqe: null handle\n");
        return;
    }
    printf("localvqe: ready=%d onnxBytes=%ld frontendBytes=%ld\n",
           h->ready ? 1 : 0, h->onnxBytes, h->frontendBytes);
    if (!h->error.empty()) printf("localvqe: note: %s\n", h->error.c_str());
    printf("localvqe: micRms=%.1f refRms=%.1f outRms=%.1f (v0 bypass)\n",
           (double)h->inRms * 32768.0, (double)h->refRms * 32768.0,
           (double)h->outRms * 32768.0);
}
