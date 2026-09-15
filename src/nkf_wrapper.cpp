#include "nkf_wrapper.h"
#include "NKFImpl.h"
#include <vector>
#include <cstring>
#include <cmath>

// ============================================================
//  NKF block parameters — must match NKFImpl.h
//  Updated to 512 to match the 1024-sample block in the model
// ============================================================
static const int NKF_BLOCK_SHIFT = 512;

struct NkfHandle {
    NKFImpl* engine = nullptr;

    // Accumulation buffers
    std::vector<float> micAccum;
    std::vector<float> refAccum;
    std::vector<int16_t> outAccum;
};

extern "C" {

NkfHandle* NkfNew(const char* modelPath) {
    auto* h = new NkfHandle();
    try {
        h->engine = new NKFImpl(modelPath);
    } catch (...) {
        delete h;
        return nullptr;
    }
    h->micAccum.reserve(NKF_BLOCK_SHIFT * 4);
    h->refAccum.reserve(NKF_BLOCK_SHIFT * 4);
    h->outAccum.reserve(NKF_BLOCK_SHIFT * 4);
    return h;
}

void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize) {
    if (!h || !h->engine) return;

    // Convert int16 -> float and append to accumulators
    for (int i = 0; i < frameSize; i++) {
        h->micAccum.push_back(mic[i] / 32768.0f);
        h->refAccum.push_back(ref[i] / 32768.0f);
    }

    // Process as many NKF_BLOCK_SHIFT-sample blocks as we can
    std::vector<float> micBlock(NKF_BLOCK_SHIFT);
    std::vector<float> refBlock(NKF_BLOCK_SHIFT);
    std::vector<float> outBlock(NKF_BLOCK_SHIFT);

    while (h->micAccum.size() >= (size_t)NKF_BLOCK_SHIFT &&
           h->refAccum.size() >= (size_t)NKF_BLOCK_SHIFT) {
        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            micBlock[i] = h->micAccum[i];
            refBlock[i] = h->refAccum[i];
        }
        h->engine->ProcessBlock(micBlock.data(), refBlock.data(), outBlock.data());
        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            float v = outBlock[i] * 32768.0f;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            h->outAccum.push_back((int16_t)v);
        }
        h->micAccum.erase(h->micAccum.begin(), h->micAccum.begin() + NKF_BLOCK_SHIFT);
        h->refAccum.erase(h->refAccum.begin(), h->refAccum.begin() + NKF_BLOCK_SHIFT);
    }

    // Output as many samples as requested
    for (int i = 0; i < frameSize; i++) {
        if (!h->outAccum.empty()) {
            out[i] = h->outAccum.front();
            h->outAccum.erase(h->outAccum.begin());
        } else {
            out[i] = 0;
        }
    }
}

void NkfReset(NkfHandle* h) {
    if (!h || !h->engine) return;
    h->micAccum.clear();
    h->refAccum.clear();
    h->outAccum.clear();
    h->engine->Reset();
}

void NkfDestroy(NkfHandle* h) {
    if (!h) return;
    delete h->engine;
    delete h;
}

} // extern "C"