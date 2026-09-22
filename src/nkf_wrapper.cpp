#include "nkf_wrapper.h"
#include "NKFImpl.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "api/scoped_refptr.h"
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================
//  NKF block parameters — must match NKFImpl.h
//  Updated to 512 to match the 1024-sample block in the model
// ============================================================
static const int NKF_BLOCK_SHIFT = 512;

// NKF runs at 16 kHz fixed: the NS-only pass eats 10 ms frames.
static const int NS_SAMPLE_RATE = 16000;
static const int NS_FRAME_SIZE = NS_SAMPLE_RATE / 100;

// ---- TDC (time delay compensation) -------------------------------------
// NKF-AEC is a *linear* canceller; upstream (fjiang9/NKF-AEC) states
// delay compensation is necessary when the far-end/mic delay is
// significant. Real-time WASAPI loopback delay is large and drifts, so
// the Kalman filter diverges until output runs away ("blowout"). Fix:
// estimate the ref->mic lag by normalized cross-correlation and feed
// NKF a delay-aligned reference — the alignment the paper's "-a" flag
// (GCC-PHAT) provides offline, done continuously here.
static const int   TDC_DMAX       = 1600;   // 100 ms search range @16 kHz
static const int   TDC_WIN        = 1024;   // 64 ms correlation window
static const int   TDC_PERIOD     = 8192;   // re-estimate every ~0.512 s of audio
static const float TDC_MIN_PEAK   = 0.35f;  // NCC needed to accept a small lag step
static const float TDC_JUMP_PEAK  = 0.55f;  // NCC needed to accept a jump (> 5 ms)
static const int   TDC_JUMP       = 80;     // lag steps beyond this count as a jump
static const float TDC_MIN_MEAN_E = 1e-5f;  // mean-square floor (don't chase silence)

// ---- Divergence guard ---------------------------------------------------
// Second line of defence: if output energy runs far above BOTH inputs
// for a few hundred ms, reset the filter. After too many resets, fail
// open (mic passthrough) for the rest of the session — clean-but-echoed
// beats blown-out.
static const int   GUARD_HOT_BLOCKS  = 8;     // ~256 ms hot -> reset
static const float GUARD_EMA         = 0.125f;
static const float GUARD_HOT_MEAN_E  = 0.05f; // output mean-square floor
static const float GUARD_RATIO       = 4.0f;  // out must exceed inputs by 6 dB
static const float GUARD_CLIP_MEAN_E = 0.5f;  // near-full-scale single block
static const int   GUARD_MAX_RESETS  = 6;

struct NkfHandle {
    NKFImpl* engine = nullptr;

    // mic awaiting 512-sample blocks; ref kept as absolute-addressed
    // history so the TDC can read a delayed slice of it.
    std::vector<float> micAccum;
    std::vector<float> refHist;     // raw ref, abs [total - size(), total)
    std::vector<int16_t> outAccum;

    size_t total = 0;               // mic+ref samples pushed (lockstep)
    size_t micConsumed = 0;         // abs index of micAccum[0]
    std::vector<float> micWin;      // last TDC_WIN mic samples (estimator)
    int alignDelay = 0;             // current ref->mic lag (samples)
    int samplesSinceTdc = TDC_PERIOD;  // estimate on first eligible call
    bool tdcLocked = false;

    float micEnv = 0, refEnv = 0, outEnv = 0;  // EMA of per-block mean-square
    int hotBlocks = 0;
    int resets = 0;
    bool giveUp = false;            // too many resets: passthrough this session

    // Noise-reduction stage: WebRTC NS only (no echo canceller, no
    // reverse stream) applied to the NKF output when nsEnabled.
    bool nsEnabled = false;
    rtc::scoped_refptr<webrtc::AudioProcessing> nsApm;
    std::vector<int16_t> nsInAccum;   // raw NKF output awaiting NS
    std::vector<float> nsMicFloat;    // 10 ms frame scratch
    std::vector<float> nsOutFloat;    // 10 ms frame scratch
};

// Keep the last TDC_WIN+TDC_DMAX ref samples (correlation window plus
// the oldest lag the block reader may still need). Prefix-only trim
// keeps refHist contiguous, so front = total - size() always.
static void NkfTrimRef(NkfHandle* h) {
    const size_t cap = (size_t)(TDC_WIN + TDC_DMAX);
    if (h->refHist.size() > cap)
        h->refHist.erase(h->refHist.begin(),
                         h->refHist.begin() + (h->refHist.size() - cap));
}

// Normalized cross-correlation delay estimate: lag d maximizes
//   sum_n mic[total-WIN+n] * ref[total-WIN+n-d]
// (R is indexed so R[n + DMAX - d] = ref at the mic sample's time minus d).
static void NkfEstimateDelay(NkfHandle* h) {
    h->samplesSinceTdc = 0;
    if (h->total < (size_t)(TDC_WIN + TDC_DMAX)) return;
    if (h->micWin.size() < (size_t)TDC_WIN) return;
    if (h->refHist.size() < (size_t)(TDC_WIN + TDC_DMAX)) return;

    const float* M = h->micWin.data();
    const long long front = (long long)h->total - (long long)h->refHist.size();
    const long long refLo = (long long)h->total - TDC_WIN - TDC_DMAX;
    if (refLo < front) return;
    const float* R = h->refHist.data() + (refLo - front);

    double micE = 0;
    for (int n = 0; n < TDC_WIN; n++)
        micE += (double)M[n] * M[n];
    if (micE / TDC_WIN < TDC_MIN_MEAN_E) return;

    const int RLEN = TDC_WIN + TDC_DMAX;
    std::vector<double> pref(RLEN + 1, 0.0);
    for (int i = 0; i < RLEN; i++)
        pref[i + 1] = pref[i] + (double)R[i] * R[i];
    if (pref[RLEN] / RLEN < TDC_MIN_MEAN_E) return;

    double best = -2.0;
    int bestD = h->alignDelay;
    for (int d = 0; d <= TDC_DMAX; d++) {
        const int off = TDC_DMAX - d;
        double num = 0;
        for (int n = 0; n < TDC_WIN; n++)
            num += (double)M[n] * R[n + off];
        const double refE = pref[off + TDC_WIN] - pref[off];
        if (refE <= 0.0) continue;
        const double sc = num / std::sqrt(micE * refE);
        if (sc > best) { best = sc; bestD = d; }
    }

    // Small tracking steps accept a moderate peak; a big lag jump needs
    // stronger evidence so noise can't teleport the alignment.
    int diff = bestD - h->alignDelay;
    if (diff < 0) diff = -diff;
    const bool jump = diff > TDC_JUMP;
    if (best >= (jump ? (double)TDC_JUMP_PEAK : (double)TDC_MIN_PEAK)) {
        h->alignDelay = bestD;
        h->tdcLocked = true;
    }
}

// Returns true when the filter should be reset (divergence detected).
static bool NkfGuard(NkfHandle* h, const float* micB,
                     const float* refB, const float* outB) {
    double m = 0, r = 0, o = 0;
    for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
        m += (double)micB[i] * micB[i];
        r += (double)refB[i] * refB[i];
        o += (double)outB[i] * outB[i];
    }
    m /= NKF_BLOCK_SHIFT; r /= NKF_BLOCK_SHIFT; o /= NKF_BLOCK_SHIFT;
    h->micEnv += GUARD_EMA * ((float)m - h->micEnv);
    h->refEnv += GUARD_EMA * ((float)r - h->refEnv);
    h->outEnv += GUARD_EMA * ((float)o - h->outEnv);

    // Output loud and far above BOTH inputs: energy the inputs never
    // had = the filter is adding it. (Clean pass-through keeps
    // outEnv ~ micEnv, so legit near-end speech never trips this.)
    bool hot = h->outEnv > GUARD_HOT_MEAN_E &&
               h->outEnv > GUARD_RATIO * h->micEnv &&
               h->outEnv > GUARD_RATIO * h->refEnv;
    // Single-block emergency: near-full-scale output while inputs moderate.
    if (o > GUARD_CLIP_MEAN_E && o > GUARD_RATIO * std::max(m, r))
        hot = true;

    if (hot) h->hotBlocks++;
    else if (h->hotBlocks > 0) h->hotBlocks--;
    return h->hotBlocks >= GUARD_HOT_BLOCKS;
}

// Drain raw output into outAccum (NS applied when enabled).
static void NkfDrainNs(NkfHandle* h) {
    webrtc::StreamConfig sc(NS_SAMPLE_RATE, 1);  // mono
    while (h->nsInAccum.size() >= (size_t)NS_FRAME_SIZE) {
        for (int i = 0; i < NS_FRAME_SIZE; i++)
            h->nsMicFloat[i] = h->nsInAccum[i] / 32768.0f;
        float* micPtr = h->nsMicFloat.data();
        float* outPtr = h->nsOutFloat.data();
        if (h->nsApm->ProcessStream(&micPtr, sc, sc, &outPtr) != 0) {
            // Fail-open: on any APM error ship the raw frame. Ignoring the
            // error would replay the previous out buffer forever (harsh
            // looping full-scale noise — the "NKF overflows" report).
            for (int i = 0; i < NS_FRAME_SIZE; i++)
                h->nsOutFloat[i] = h->nsMicFloat[i];
        }
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

static void NkfDrainOut(NkfHandle* h, int16_t* out, int frameSize) {
    for (int i = 0; i < frameSize; i++) {
        if (!h->outAccum.empty()) {
            out[i] = h->outAccum.front();
            h->outAccum.erase(h->outAccum.begin());
        } else {
            out[i] = 0;
        }
    }
}

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
    h->refHist.reserve(TDC_WIN + TDC_DMAX + NKF_BLOCK_SHIFT * 4);
    h->micWin.reserve(TDC_WIN);
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

void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize) {
    if (!h || !h->engine) return;

    // Fail-open session: mic passthrough (still NS'd), engine idle.
    if (h->giveUp) {
        for (int i = 0; i < frameSize; i++) {
            if (h->nsEnabled && h->nsApm) h->nsInAccum.push_back(mic[i]);
            else                          h->outAccum.push_back(mic[i]);
        }
        if (h->nsEnabled && h->nsApm) NkfDrainNs(h);
        NkfDrainOut(h, out, frameSize);
        return;
    }

    // int16 -> float: mic into the block accumulator, ref into the
    // absolute-addressed history, mic also into the estimator window.
    for (int i = 0; i < frameSize; i++) {
        const float s = mic[i] / 32768.0f;
        h->micAccum.push_back(s);
        h->micWin.push_back(s);
        h->refHist.push_back(ref[i] / 32768.0f);
    }
    h->total += (size_t)frameSize;
    if (h->micWin.size() > (size_t)TDC_WIN)
        h->micWin.erase(h->micWin.begin(),
                        h->micWin.begin() + (h->micWin.size() - TDC_WIN));

    h->samplesSinceTdc += frameSize;
    if (h->samplesSinceTdc >= TDC_PERIOD) NkfEstimateDelay(h);

    std::vector<float> micBlock(NKF_BLOCK_SHIFT);
    std::vector<float> refBlock(NKF_BLOCK_SHIFT);
    std::vector<float> outBlock(NKF_BLOCK_SHIFT);

    while (h->micAccum.size() >= (size_t)NKF_BLOCK_SHIFT) {
        // Aligned pairing: mic [micConsumed, +SHIFT) with ref SHIFT-delay
        // samples earlier — the TDC slice NKF needs. Positions < 0 are
        // stream warmup: feed zeros.
        const long long needStart = (long long)h->micConsumed - h->alignDelay;
        const long long front =
            (long long)h->total - (long long)h->refHist.size();
        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            micBlock[i] = h->micAccum[i];
            const long long p = needStart + i;
            if (p >= front && (size_t)(p - front) < h->refHist.size())
                refBlock[i] = h->refHist[(size_t)(p - front)];
            else
                refBlock[i] = 0.0f;
        }

        h->engine->ProcessBlock(micBlock.data(), refBlock.data(),
                                outBlock.data());

        if (NkfGuard(h, micBlock.data(), refBlock.data(), outBlock.data())) {
            // Divergence: purge blown samples, reset the filter, drop the
            // pending backlog (a few ms of audio — fine during recovery).
            h->outAccum.clear();
            h->nsInAccum.clear();
            h->micAccum.clear();
            h->micConsumed = h->total;
            h->engine->Reset();
            h->micEnv = h->refEnv = h->outEnv = 0;
            h->hotBlocks = 0;
            if (++h->resets >= GUARD_MAX_RESETS) {
                h->giveUp = true;
                h->micAccum.clear();
                h->micConsumed = h->total;
            }
            break;
        }

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
        h->micAccum.erase(h->micAccum.begin(),
                          h->micAccum.begin() + NKF_BLOCK_SHIFT);
        h->micConsumed += NKF_BLOCK_SHIFT;
    }

    NkfTrimRef(h);
    NkfDrainOut(h, out, frameSize);
}

void NkfReset(NkfHandle* h) {
    if (!h) return;
    h->micAccum.clear();
    h->refHist.clear();
    h->outAccum.clear();
    h->nsInAccum.clear();
    h->micWin.clear();
    h->total = 0;
    h->micConsumed = 0;
    h->alignDelay = 0;
    h->samplesSinceTdc = TDC_PERIOD;
    h->tdcLocked = false;
    h->micEnv = h->refEnv = h->outEnv = 0;
    h->hotBlocks = 0;
    h->resets = 0;
    h->giveUp = false;
    if (h->engine) h->engine->Reset();
}

void NkfDestroy(NkfHandle* h) {
    if (!h) return;
    delete h->engine;
    delete h;
}

} // extern "C"
