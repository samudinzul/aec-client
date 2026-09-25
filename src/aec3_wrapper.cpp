#include "aec3_wrapper.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "api/scoped_refptr.h"

#include <vector>
#include <cstring>
#include <cmath>

// ============================================================
//  AEC3 wrapper for MSYS2's webrtc-audio-processing-1 package.
//  Uses the older webrtc::AudioProcessing API where:
//    1. Create the APM via AudioProcessingBuilder().Create()
//    2. Then apply config via apm->ApplyConfig(config)
// ============================================================

struct Aec3Handle {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    int sampleRate;
    int frameSize;
    std::vector<float> refFloat;
    std::vector<float> micFloat;
    std::vector<float> outFloat;
};

extern "C" {

Aec3Handle* Aec3New(int sampleRate, int frameSize, bool nsEnabled) {
    auto h = new Aec3Handle();
    h->sampleRate = sampleRate;
    h->frameSize  = frameSize;

    // 1. Create the APM instance (no args in this older API)
    h->apm = webrtc::AudioProcessingBuilder().Create();
    if (!h->apm) { delete h; return nullptr; }

    // 2. Build the config and apply it
    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled     = true;
    config.echo_canceller.mobile_mode = false;
    config.noise_suppression.enabled  = nsEnabled;
    config.noise_suppression.level    =
        webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    config.high_pass_filter.enabled   = true;
    config.gain_controller1.enabled   = false;
    config.gain_controller2.enabled   = false;

    h->apm->ApplyConfig(config);

    h->refFloat.assign(frameSize, 0.0f);
    h->micFloat.assign(frameSize, 0.0f);
    h->outFloat.assign(frameSize, 0.0f);

    return h;
}

void Aec3CancelEcho(Aec3Handle* h, const int16_t* mic, const int16_t* ref,
                    int16_t* out, int frameSize) {
    // Fail-open: unusable handle or mismatched frame ships mic, never
    // silence (a dead handle must not cut the voice mid-call).
    if (!h || !h->apm || frameSize != h->frameSize) {
        if (out && mic && frameSize > 0)
            memcpy(out, mic, (size_t)frameSize * sizeof(int16_t));
        return;
    }

    // int16 -> float [-1, 1]
    for (int i = 0; i < frameSize; i++) {
        h->refFloat[i] = ref[i] / 32768.0f;
        h->micFloat[i] = mic[i] / 32768.0f;
    }

    webrtc::StreamConfig sc(h->sampleRate, 1);  // mono

    // 1. Feed reference (far-end / speaker output)
    float* refPtr = h->refFloat.data();
    h->apm->ProcessReverseStream(&refPtr, sc, sc, &refPtr);

    // 2. Process microphone (near-end)
    float* micPtr = h->micFloat.data();
    float* outPtr = h->outFloat.data();
    h->apm->ProcessStream(&micPtr, sc, sc, &outPtr);

    // float -> int16 with clamp
    for (int i = 0; i < frameSize; i++) {
        float v = h->outFloat[i] * 32768.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }
}

void Aec3Destroy(Aec3Handle* h) {
    delete h;
}

} // extern "C"
