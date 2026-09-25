#include "nkf_wrapper.h"
#include "NKFImpl.h"
#include "wpe.h"
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
static const int   TDC_EAGER      = 1024;   // pre-lock cadence: ~64 ms of audio
static const int   TDC_PERIOD     = 8192;   // re-estimate every ~0.512 s of audio
static const int   TDC_LOCK_GRACE = 48000;  // 3 s: engage with best guess if no lock
static const float TDC_MIN_PEAK   = 0.35f;  // NCC needed to accept a small lag step
static const float TDC_JUMP_PEAK  = 0.55f;  // NCC needed to accept a jump (> 5 ms)
static const int   TDC_JUMP       = 80;     // lag steps beyond this count as a jump
static const float TDC_MIN_MEAN_E = 1e-5f;  // mean-square floor (don't chase silence)

// Exposure pipeline: after (re)lock the engine runs in SHADOW — mic
// stays on the wire, guard monitors internally, so cold-start spikes
// are never heard. Shadow ends -> FADE blocks crossfade mic->NKF on
// the same block timeline (no holes, no replays), then live.
static const int   TDC_SHADOW     = 16;     // 512 ms internal warm-up
static const int   TDC_FADE       = 8;      // 256 ms crossfade into NKF

// Self-monitor loop detection: with "Listen to myself" or a Discord
// mic test on SPEAKERS, ref carries our own recent output back around
// (mic -> NKF -> Discord -> speakers -> loopback). NKF's live Kalman
// adaptation inside that feedback loop can ring/blow; DTLN (fixed
// weights) and AEC3 (leakage guards) tolerate it. Correlate ref
// against our own output history — a strong delayed match means a
// loop is present -> hold exposure on mic (shadow) until it clears.
static const int   LOOP_WIN      = 1024;    // correlation window
static const int   LOOP_DMAX     = 4800;    // 300 ms search (playback chain)
static const int   LOOP_DEC      = 4;       // box-decimate x4 (cheap NCC)
static const int   LOOP_PERIOD   = 2048;    // check every ~128 ms of audio
static const float LOOP_MIN_PEAK = 0.45f;   // NCC: delayed copy, not coincidence
static const int   LOOP_ON       = 2;       // hits needed to engage (hysteresis)
static const int   LOOP_QUIET    = 16;      // silent detections (~2 s) -> release

// ---- Divergence guard ---------------------------------------------------
// Second line of defence: if output energy runs far above BOTH inputs,
// reset the filter and drop back to shadow (mic on the wire). After too
// many resets, fail open (always mic) for the rest of the session —
// clean-but-echoed beats blown-out.
static const int   GUARD_HOT_BLOCKS  = 3;     // ~100 ms hot -> reset+shadow
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
    // FIFO heads — consume by offset, compact once per Process
    // (never erase(begin) per sample/block on the audio thread).
    size_t micHead = 0, outHead = 0, nsInHead = 0;

    // Per-block scratch (hoisted: no heap inside NkfProcess's loop)
    float micBlock[NKF_BLOCK_SHIFT];
    float refBlock[NKF_BLOCK_SHIFT];
    float outBlock[NKF_BLOCK_SHIFT];
    float emitBlock[NKF_BLOCK_SHIFT];

    size_t total = 0;               // mic+ref samples pushed (lockstep)
    size_t micConsumed = 0;         // abs index of micAccum[0]
    std::vector<float> micWin;      // last TDC_WIN mic samples (estimator)
    int alignDelay = 0;             // current ref->mic lag (samples)
    int samplesSinceTdc = 0;        // estimate as soon as data allows
    bool tdcLocked = false;
    // TDC / loop NCC prefix scratch (hoisted — used by DetectLoop/EstimateDelay)
    double tdcPref[TDC_WIN + TDC_DMAX + 1];
    float  loopRd[(LOOP_WIN) / LOOP_DEC];
    float  loopOd[(LOOP_WIN + LOOP_DMAX) / LOOP_DEC];
    double loopPref[(LOOP_WIN + LOOP_DMAX) / LOOP_DEC + 1];

    float micEnv = 0, refEnv = 0, outEnv = 0;  // EMA of per-block mean-square
    int hotBlocks = 0;
    int resets = 0;
    bool giveUp = false;            // too many resets: always mic this session
    int shadowBlocks = TDC_SHADOW;  // blocks still emitting mic (engine warms)
    int fadePos = TDC_FADE;         // crossfade position (TDC_FADE = complete)

    // Self-monitor loop detector state.
    std::vector<float> outHist;     // our emitted output (what speakers get)
    int samplesSinceLoop = 0;
    int loopConf = 0;               // consecutive-ish hit score (0..4)
    int loopQuiet = 0;              // silent detections in a row

    // Post stage: WebRTC NS (nsEnabled checkbox) on top of NKF
    // (AEC3/residual cancellation retired in v1.9 — NKF is strictly
    // linear and that tradeoff was accepted).
    rtc::scoped_refptr<webrtc::AudioProcessing> nsApm;
    std::vector<int16_t> nsInAccum;   // NKF output awaiting the post stage
    std::vector<float> nsMicFloat;    // 10 ms frame scratch
    std::vector<float> nsOutFloat;    // 10 ms frame scratch

    // Optional streaming WPE dereverb stage (null = off): sits AFTER
    // NS (chain NS -> WPE), before outHist/outAccum, so VAD gate,
    // meters, gain and the loop detector all see the dereverbed
    // signal. Fail-open like GTCRN was: a bad Process call just
    // passes audio through internally.
    WpeHandle* wpe = nullptr;
    std::vector<float> wpeOut;        // dereverb-stage output scratch
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
    double* pref = h->tdcPref;
    pref[0] = 0.0;
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

// Box-decimated NCC of ref's tail against our own output history's
// tail: ref[n] ~ out[n - L] with L in [0, LOOP_DMAX] = a self-monitor
// loop is on. Confirms with LOOP_ON hits, releases after LOOP_QUIET
// consecutive silent windows (so a finished mic test re-exposes NKF).
static void NkfDetectLoop(NkfHandle* h) {
    h->samplesSinceLoop = 0;
    const int W = LOOP_WIN, DMAX = LOOP_DMAX, D = LOOP_DEC;
    const int Wd = W / D;
    const int OdN = (W + DMAX) / D;
    const int off = DMAX / D;         // out-bin lag 0 aligns tail-to-tail
    if ((int)h->refHist.size() < W ||
        (int)h->outHist.size() < W + DMAX)
        return;

    float* Rf = h->refHist.data() + (h->refHist.size() - W);
    float* Of = h->outHist.data() + (h->outHist.size() - (W + DMAX));

    float* Rd = h->loopRd;
    float* Od = h->loopOd;
    for (int k = 0; k < Wd; k++)
        Rd[k] = (Rf[4*k] + Rf[4*k+1] + Rf[4*k+2] + Rf[4*k+3]) * 0.25f;
    for (int j = 0; j < OdN; j++)
        Od[j] = (Of[4*j] + Of[4*j+1] + Of[4*j+2] + Of[4*j+3]) * 0.25f;

    double eR = 0, eO = 0;
    for (int k = 0; k < Wd; k++) eR += (double)Rd[k] * Rd[k];
    for (int j = 0; j < OdN; j++) eO += (double)Od[j] * Od[j];
    if (eR / Wd < TDC_MIN_MEAN_E || eO / OdN < TDC_MIN_MEAN_E) {
        // Too quiet to judge — hold, but release after sustained quiet
        // so NKF re-exposes once the self-test is over.
        if (++h->loopQuiet >= LOOP_QUIET) {
            h->loopConf = 0;
            h->loopQuiet = 0;
        }
        return;
    }
    h->loopQuiet = 0;

    double* pref = h->loopPref;
    pref[0] = 0.0;
    for (int j = 0; j < OdN; j++)
        pref[j + 1] = pref[j] + (double)Od[j] * Od[j];

    double best = -2.0;
    for (int Ld = 0; Ld <= off; Ld++) {
        const int s = off - Ld;       // out-bin start paired with Rd[0]
        double num = 0;
        for (int k = 0; k < Wd; k++) num += (double)Rd[k] * Od[s + k];
        const double eSeg = pref[s + Wd] - pref[s];
        if (eSeg <= 0.0) continue;
        const double sc = num / std::sqrt(eR * eSeg);
        if (sc > best) best = sc;
    }

    if (best >= (double)LOOP_MIN_PEAK) {
        if (h->loopConf < 4) h->loopConf++;
    } else if (h->loopConf > 0) {
        h->loopConf--;
    }
}

// Ship finished post-APM samples (float, ±1) to the wire: optional
// WPE dereverb first, then outHist (loop detector tap — what the
// speakers actually get) and the clamped int16 outAccum.
static void NkfEmit(NkfHandle* h, const float* v, int n) {
    if (h->wpe) {
        h->wpeOut.resize((size_t)n);
        if (WpeProcess(h->wpe, v, h->wpeOut.data(), n) > 0)
            v = h->wpeOut.data();
        // else: bad args (cannot happen here) — ship v unchanged
    }
    for (int i = 0; i < n; i++) {
        h->outHist.push_back(v[i]);
        float s = v[i] * 32768.0f;
        if (s >  32767.0f) s =  32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        h->outAccum.push_back((int16_t)s);
    }
}

// Drain raw output through the post stage (optional WebRTC NS) into
// outAccum; NkfEmit then applies WPE (chain NS -> WPE). Ordering is
// preserved: leftovers (< 1 frame) wait.
static void NkfDrainPost(NkfHandle* h) {
    webrtc::StreamConfig sc(NS_SAMPLE_RATE, 1);  // mono
    while (h->nsInAccum.size() - h->nsInHead >= (size_t)NS_FRAME_SIZE) {
        const int16_t* src = h->nsInAccum.data() + h->nsInHead;
        for (int i = 0; i < NS_FRAME_SIZE; i++)
            h->nsMicFloat[i] = src[i] / 32768.0f;
        float* micPtr = h->nsMicFloat.data();
        float* outPtr = h->nsOutFloat.data();
        if (h->nsApm->ProcessStream(&micPtr, sc, sc, &outPtr) != 0) {
            // Fail-open: on any APM error ship the raw frame. Ignoring the
            // error would replay the previous out buffer forever (harsh
            // looping full-scale noise — the "NKF overflows" report).
            for (int i = 0; i < NS_FRAME_SIZE; i++)
                h->nsOutFloat[i] = h->nsMicFloat[i];
        }
        NkfEmit(h, h->nsOutFloat.data(), NS_FRAME_SIZE);
        h->nsInHead += NS_FRAME_SIZE;
    }
    if (h->nsInHead) {
        h->nsInAccum.erase(h->nsInAccum.begin(),
                           h->nsInAccum.begin() + (ptrdiff_t)h->nsInHead);
        h->nsInHead = 0;
    }
}

static void NkfDrainOut(NkfHandle* h, int16_t* out, int frameSize) {
    const size_t avail = h->outAccum.size() - h->outHead;
    for (int i = 0; i < frameSize; i++) {
        if ((size_t)i < avail) {
            out[i] = h->outAccum[h->outHead + i];
        } else {
            out[i] = 0;
        }
    }
    const size_t consume = ((size_t)frameSize < avail) ? (size_t)frameSize : avail;
    h->outHead += consume;
    if (h->outHead) {
        h->outAccum.erase(h->outAccum.begin(),
                          h->outAccum.begin() + (ptrdiff_t)h->outHead);
        h->outHead = 0;
    }
}

extern "C" {

NkfHandle* NkfNew(const char* modelPath, bool nsEnabled,
                  bool wpeEnabled) {
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
    h->nsInAccum.reserve(NKF_BLOCK_SHIFT * 4);
    h->outHist.reserve(LOOP_WIN + LOOP_DMAX);
    // Post stage: WebRTC NS tied to the checkbox (no AEC3 — v1.9
    // retired the residual pass), HPF like the standalone AEC3
    // wrapper, no AGC.
    h->nsApm = webrtc::AudioProcessingBuilder().Create();
    if (h->nsApm) {
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled     = false;
        config.echo_canceller.mobile_mode = false;
        config.noise_suppression.enabled  = nsEnabled;
        config.noise_suppression.level    =
            webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        config.high_pass_filter.enabled   = true;
        config.gain_controller1.enabled   = false;
        config.gain_controller2.enabled   = false;
        h->nsApm->ApplyConfig(config);
        h->nsMicFloat.assign(NS_FRAME_SIZE, 0.0f);
        h->nsOutFloat.assign(NS_FRAME_SIZE, 0.0f);
    }
    // Optional WPE dereverb: toggleable, default on in the UI. A
    // null handle (allocation failure) just leaves the stage off —
    // never kills NKF.
    if (wpeEnabled) h->wpe = WpeNew(NS_SAMPLE_RATE);
    if (h->wpe) h->wpeOut.reserve(NKF_BLOCK_SHIFT);
    return h;
}

void NkfSetNearSpeech(NkfHandle* h, int speaking) {
    if (!h || !h->wpe) return;
    WpeSetSpeech(h->wpe, speaking);
}

void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize) {
    // Fail-open: dead handle ships mic, never silence.
    if (!h || !h->engine) {
        if (out && mic && frameSize > 0)
            memcpy(out, mic, (size_t)frameSize * sizeof(int16_t));
        return;
    }

    // int16 -> float: mic into the block accumulator, ref into the
    // absolute-addressed history, mic also into the estimator window.
    // Pushed unconditionally so ALL phases (warm-up, shadow, fade,
    // live) emit from one continuous block timeline — engagement can
    // neither drop a hole nor replay already-heard samples.
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

    if (!h->giveUp) {
        h->samplesSinceTdc += frameSize;
        const int tdcNeed = h->tdcLocked ? TDC_PERIOD : TDC_EAGER;
        if (h->samplesSinceTdc >= tdcNeed) NkfEstimateDelay(h);
        if (!h->tdcLocked && h->total >= (size_t)TDC_LOCK_GRACE)
            h->tdcLocked = true;  // engage anyway; TDC keeps correcting

        h->samplesSinceLoop += frameSize;
        if (h->samplesSinceLoop >= LOOP_PERIOD) NkfDetectLoop(h);
    }

    while (h->micAccum.size() - h->micHead >= (size_t)NKF_BLOCK_SHIFT) {
        // Aligned pairing: mic [micConsumed, +SHIFT) with ref SHIFT-delay
        // samples earlier — the TDC slice NKF needs. Positions < 0 are
        // stream warmup: feed zeros. (Pre-lock the engine isn't called;
        // the slice just stays ready.)
        const long long needStart = (long long)h->micConsumed - h->alignDelay;
        const long long front =
            (long long)h->total - (long long)h->refHist.size();
        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            h->micBlock[i] = h->micAccum[h->micHead + i];
            const long long p = needStart + i;
            if (p >= front && (size_t)(p - front) < h->refHist.size())
                h->refBlock[i] = h->refHist[(size_t)(p - front)];
            else
                h->refBlock[i] = 0.0f;
        }

        // Engine runs only once locked and before fail-open; its output
        // reaches the wire only past shadow, through the fade.
        bool processed = false;
        if (h->tdcLocked && !h->giveUp) {
            h->engine->ProcessBlock(h->micBlock, h->refBlock,
                                    h->outBlock);
            processed = true;
            if (NkfGuard(h, h->micBlock, h->refBlock,
                         h->outBlock)) {
                // Divergence: reset the filter and drop back to shadow.
                // The wire keeps carrying mic — internal spikes, even
                // here, are never exposed.
                h->engine->Reset();
                h->micEnv = h->refEnv = h->outEnv = 0;
                h->hotBlocks = 0;
                h->shadowBlocks = TDC_SHADOW;
                h->fadePos = TDC_FADE;
                if (++h->resets >= GUARD_MAX_RESETS) h->giveUp = true;
                processed = false;
            }
        }

        // Self-monitor loop detected while exposed: yank back to shadow
        // NOW — NKF's live adaptation inside a feedback loop is what
        // rings before the guard can trip.
        if (h->loopConf >= LOOP_ON && h->shadowBlocks == 0 && processed) {
            h->shadowBlocks = TDC_SHADOW;
            h->fadePos = TDC_FADE;
        }

        // Exposure mix: g=0 -> mic, g=1 -> full NKF.
        const bool shadowed = processed && h->shadowBlocks > 0;
        float g = 0.0f;
        if (processed && !shadowed) {
            if (h->fadePos < TDC_FADE) {
                h->fadePos++;
                g = (float)h->fadePos / (float)TDC_FADE;
            } else {
                g = 1.0f;
            }
        }

        // No-APM edge (builder Create failed): emit the block straight
        // through (WPE stage still applies inside NkfEmit).
        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            float v = h->micBlock[i];
            if (g > 0.0f) v += (h->outBlock[i] - v) * g;
            if (h->nsApm) {
                float s = v * 32768.0f;
                if (s >  32767.0f) s =  32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                h->nsInAccum.push_back((int16_t)s);
            } else {
                h->emitBlock[i] = v;
            }
        }
        if (h->nsApm) NkfDrainPost(h);
        else NkfEmit(h, h->emitBlock, NKF_BLOCK_SHIFT);

        // Hold shadow while a self-monitor loop is active; release the
        // countdown only when the loop is gone (then fade normally).
        if (shadowed && h->loopConf < LOOP_ON &&
            --h->shadowBlocks == 0)
            h->fadePos = 0;
        h->micHead += NKF_BLOCK_SHIFT;
        h->micConsumed += NKF_BLOCK_SHIFT;
    }
    // Compact micAccum once after the block loop (not per block).
    if (h->micHead) {
        h->micAccum.erase(h->micAccum.begin(),
                          h->micAccum.begin() + (ptrdiff_t)h->micHead);
        h->micHead = 0;
    }

    NkfTrimRef(h);
    {
        const size_t cap = (size_t)(LOOP_WIN + LOOP_DMAX);
        if (h->outHist.size() > cap)
            h->outHist.erase(h->outHist.begin(),
                             h->outHist.begin() + (h->outHist.size() - cap));
    }
    NkfDrainOut(h, out, frameSize);
}

void NkfReset(NkfHandle* h) {
    if (!h) return;
    h->micAccum.clear();
    h->refHist.clear();
    h->outAccum.clear();
    h->nsInAccum.clear();
    h->micWin.clear();
    h->outHist.clear();
    h->micHead = 0;
    h->outHead = 0;
    h->nsInHead = 0;
    h->total = 0;
    h->micConsumed = 0;
    h->alignDelay = 0;
    h->samplesSinceTdc = 0;
    h->tdcLocked = false;
    h->micEnv = h->refEnv = h->outEnv = 0;
    h->hotBlocks = 0;
    h->resets = 0;
    h->giveUp = false;
    h->shadowBlocks = TDC_SHADOW;
    h->fadePos = TDC_FADE;
    h->samplesSinceLoop = 0;
    h->loopConf = 0;
    h->loopQuiet = 0;
    if (h->wpe) WpeReset(h->wpe);
    if (h->engine) h->engine->Reset();
}

void NkfDestroy(NkfHandle* h) {
    if (!h) return;
    WpeDestroy(h->wpe);
    delete h->engine;
    delete h;
}

} // extern "C"
