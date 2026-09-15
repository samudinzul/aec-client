#include "localvqe_wrapper.h"
#include "localvqe_api.h"
#include <vector>
#include <cstring>

struct LocalVqeHandle {
    localvqe_ctx_t ctx = 0;
    std::vector<int16_t> micAccum;
    std::vector<int16_t> refAccum;
    std::vector<int16_t> outAccum;
};

static const int LOCALVQE_HOP = 256;

extern "C" {

LocalVqeHandle* LocalVqeNew(const char* modelPath) {
    auto* h = new LocalVqeHandle();
    h->ctx = localvqe_new(modelPath);
    if (h->ctx == 0) {
        delete h;
        return nullptr;
    }

    // Residual noise gate: cleans quiet residual at or below -45 dBFS.
    // Preserves typical speech (-30 to -10 dBFS).
    localvqe_set_noise_gate(h->ctx, 1, -45.0f);

    h->micAccum.reserve(LOCALVQE_HOP * 4);
    h->refAccum.reserve(LOCALVQE_HOP * 4);
    h->outAccum.reserve(LOCALVQE_HOP * 4);
    return h;
}

void LocalVqeProcess(LocalVqeHandle* h, const int16_t* mic, const int16_t* ref,
                     int16_t* out, int hopSamples) {
    if (!h || h->ctx == 0) return;

    h->micAccum.insert(h->micAccum.end(), mic, mic + hopSamples);
    h->refAccum.insert(h->refAccum.end(), ref, ref + hopSamples);

    while (h->micAccum.size() >= (size_t)LOCALVQE_HOP &&
           h->refAccum.size() >= (size_t)LOCALVQE_HOP) {

        int16_t outFrame[LOCALVQE_HOP];

        int ret = localvqe_process_frame_s16(
            h->ctx,
            h->micAccum.data(),
            h->refAccum.data(),
            LOCALVQE_HOP,
            outFrame);

        if (ret == 0) {
            h->outAccum.insert(h->outAccum.end(), outFrame, outFrame + LOCALVQE_HOP);
        } else {
            h->outAccum.insert(h->outAccum.end(),
                               h->micAccum.begin(),
                               h->micAccum.begin() + LOCALVQE_HOP);
        }

        h->micAccum.erase(h->micAccum.begin(), h->micAccum.begin() + LOCALVQE_HOP);
        h->refAccum.erase(h->refAccum.begin(), h->refAccum.begin() + LOCALVQE_HOP);
    }

    for (int i = 0; i < hopSamples; i++) {
        if (!h->outAccum.empty()) {
            out[i] = h->outAccum.front();
            h->outAccum.erase(h->outAccum.begin());
        } else {
            out[i] = 0;
        }
    }
}

void LocalVqeReset(LocalVqeHandle* h) {
    if (!h || h->ctx == 0) return;
    h->micAccum.clear();
    h->refAccum.clear();
    h->outAccum.clear();
    localvqe_reset(h->ctx);
}

void LocalVqeDestroy(LocalVqeHandle* h) {
    if (!h) return;
    if (h->ctx) localvqe_free(h->ctx);
    delete h;
}

const char* LocalVqeLastError(LocalVqeHandle* h) {
    if (!h || h->ctx == 0) return "No context";
    return localvqe_last_error(h->ctx);
}

void LocalVqeSetNoiseGate(LocalVqeHandle* h, int enabled, float threshold_dbfs) {
    if (!h || h->ctx == 0) return;
    localvqe_set_noise_gate(h->ctx, enabled, threshold_dbfs);
}

} // extern "C"
