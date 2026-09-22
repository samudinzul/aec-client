#include "nkf_wrapper.h"
#include "NKFImpl.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "api/scoped_refptr.h"
#include <vector>
#include <cstring>
#include <cmath>

// ============================================================
//  NKF block parameters — must match NKFImpl.h
//  Updated to 512 to match the 1024-sample block in the model
// ============================================================
static const int NKF_BLOCK_SHIFT = 512;

// NKF runs at 16 kHz fixed: the NS-only pass eats 10 ms frames.
static const int NS_SAMPLE_RATE = 16000;
static const int NS_FRAME_SIZE = NS_SAMPLE_RATE / 100;

struct NkfHandle {
    NKFImpl* engine = nullptr;

    // Accumulation buffers
    std::vector<float> micAccum;
    std::vector<float> refAccum;
    std::vector<int16_t> outAccum;

    // Noise-reduction stage: WebRTC NS only (no echo canceller, no
    // reverse stream) applied to the NKF output when nsEnabled.
    bool nsEnabled = false;
    rtc::scoped_refptr<webrtc::AudioProcessing> nsApm;
    std::vector<int16_t> nsInAccum;   // raw NKF output awaiting NS
    std::vector<float> nsMicFloat;    // 10 ms frame scratch
    std::vector<float> nsOutFloat;    // 10 ms frame scratch
};

extern "C" {

NkfHandle* NkfNew(const char* modelPath, bool nsEnabled) {
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
    h->nsEnabled = nsEnabled;
    if (nsEnabled) {
        // NS-only pass: echo canceller off (NKF already killed the echo),
        // Moderate suppression, no reverse stream needed.
        h->nsApm = webrtc::AudioProcessingBuilder().Create();
        if (!h->nsApm) {
            h->nsEnabled = false;  // fail-open: ship NKF output unfiltered
        } else {
            webrtc::AudioProcessing::Config config;
            config.echo_canceller.enabled   = false;
            config.noise_suppression.enabled = true;
            config.noise_suppression.level   =
                webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
            config.high_pass_filter.enabled = false;
            config.gain_controller1.enabled = false;
            config.gain_controller2.enabled = false;
            h->nsApm->ApplyConfig(config);
            h->nsMicFloat.assign(NS_FRAME_SIZE, 0.0f);
            h->nsOutFloat.assign(NS_FRAME_SIZE, 0.0f);
        }
    }
    return h;
}

// Drain raw NKF output through the NS-only pass into outAccum.
// Ordering is preserved: leftovers (< 1 frame) wait for the next call.
static void NkfDrainNs(NkfHandle* h) {
    webrtc::StreamConfig sc(NS_SAMPLE_RATE, 1);  // mono
    while (h->nsInAccum.size() >= (size_t)NS_FRAME_SIZE) {
        for (int i = 0; i < NS_FRAME_SIZE; i++)
            h->nsMicFloat[i] = h->nsInAccum[i] / 32768.0f;
        float* micPtr = h->nsMicFloat.data();
        float* outPtr = h->nsOutFloat.data();
        h->nsApm->ProcessStream(&micPtr, sc, sc, &outPtr);
        for (int i = 0; i < NS_FRAME_SIZE; i++) {
            float v = h->nsOutFloat[i] * 32768.0f;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            h->outAccum.push_back((int16_t)v);
        }
        h->nsInAccum.erase(h->nsInAccum.begin(),
                            h->nsInAccum.begin() + NS_FRAME_SIZE);
    }
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
            if (h->nsEnabled && h->nsApm)
                h->nsInAccum.push_back((int16_t)v);
            else
                h->outAccum.push_back((int16_t)v);
        }
        if (h->nsEnabled && h->nsApm) NkfDrainNs(h);
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
    h->nsInAccum.clear();
    h->engine->Reset();
}

void NkfDestroy(NkfHandle* h) {
    if (!h) return;
    delete h->engine;
    delete h;
}

} // extern "C"