#include "nkf_wrapper.h"
#include "NKFImpl.h"
#include "aec_log.h"
#include "pocketfft_hdronly.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <complex>

// Transition-only phase log (nkf-phase.log in the working directory):
// pins where a crash or exception happened on the audio thread. Written
// only on state changes — never per block — so it is safe there.
#define NkfPhase AecPhase
#define NKF_T(h) ((double)(h)->total / 16000.0)

// ============================================================
//  NKF block parameters — must match NKFImpl.h
//  Updated to 512 to match the 1024-sample block in the model
// ============================================================
static const int NKF_BLOCK_SHIFT = 512;

// ---- TDC (time delay compensation) -------------------------------------
// NKF-AEC is a *linear* canceller; upstream (fjiang9/NKF-AEC) states
// delay compensation is necessary when the far-end/mic delay is
// significant. Real-time WASAPI loopback delay is large and drifts, so
// the Kalman filter diverges until output runs away ("blowout"). Fix:
// estimate the ref->mic lag by normalized cross-correlation and feed
// NKF a delay-aligned reference — the alignment the paper's "-a" flag
// (GCC-PHAT) provides offline, done continuously here.
static const int   TDC_DMAX       = 12800;  // 800 ms search range @16 kHz
                                            // (was 4800/300 ms: the mic-test
                                            // round trip — app + Discord +
                                            // speaker + loopback capture —
                                            // can exceed 300 ms, and NKF's
                                            // 128 ms filter span cannot
                                            // cancel an unaligned delay)
static const int   TDC_WIN        = 1024;   // 64 ms correlation window
static const int   TDC_DEC        = 4;      // box-decimate x4: coarse pass
static const int   TDC_EAGER      = 1024;   // pre-lock cadence: ~64 ms of audio
static const int   TDC_PERIOD     = 8192;   // re-estimate every ~0.512 s of audio
static const int   TDC_LOCK_GRACE = 48000;  // 3 s: engage with best guess if no lock
static const float TDC_MIN_PEAK   = 0.35f;  // NCC needed to accept a small lag step
static const float TDC_JUMP_PEAK  = 0.55f;  // NCC needed to accept a jump (> 5 ms)
static const int   TDC_JUMP       = 80;     // lag steps beyond this count as a jump
static const int   TDC_SLEW       = 512;    // max applied delay change per
                                            // re-estimate (32 ms): a real
                                            // acoustic path drifts slowly —
                                            // teleporting thousands of
                                            // samples between 0.5 s updates
                                            // is always a periodicity
                                            // artifact (tonal ref). The 800 ms
                                            // range stays available for
                                            // genuine long round trips,
                                            // reached over a few updates.
static const double TDC_TIE_EPS   = 0.02;   // coarse-pass near-tie margin:
                                            // only a CLEARLY better peak
                                            // displaces the current delay.
                                            // The sweep ascends, so near-ties
                                            // keep the smaller lag — the true
                                            // path is the first strong peak,
                                            // its multiples are echoes of it.
static const int   TDC_FINE       = 8;      // raw-sample refine range around coarse peak
static const float TDC_MIN_MEAN_E = 1e-5f;  // mean-square floor (don't chase silence)
// The stock model is level-sensitive: fed float signals at ~0.15 rms it
// amplifies (guard-trip runaway in a feedback loop), fed ~1/16 of that it
// cancels 20-30 dB and stays stable. The wrapper works in int16/32768
// (voice ~0.1-0.2), so scale into the engine's sweet spot and back:
// guard, mix and emit all stay in the real domain.
static const float ENG_IN_SCALE   = 1.0f / 16.0f;
static const float ENG_OUT_SCALE  = 16.0f;

// Exposure pipeline: after (re)lock the engine runs in SHADOW — mic
// stays on the wire, guard monitors internally, so cold-start spikes
// are never heard. Shadow ends -> FADE blocks crossfade mic->NKF on
// the same block timeline (no holes, no replays), then live.
static const int   TDC_SHADOW     = 16;     // 512 ms internal warm-up
static const int   TDC_FADE       = 8;      // 256 ms crossfade into NKF

// Self-monitor loop detection: with "Listen to myself" or a Discord
// mic test on SPEAKERS, ref carries our own output back around (app ->
// Discord -> speakers -> loopback) and the speaker couples back to the
// mic through the air. A loop must NEVER un-expose NKF and must NEVER
// freeze it either — raw mic on the wire or a stale filter are both
// howl fuel (the frozen policy sustained full-scale ringing at
// coupling >= 1.2 in the closed-loop test). Since the level-scaled
// engine, the Kalman adapts straight through the loop: it learns the
// pickup path, loop gain drops below 1 and the ringing dies (held to
// coupling 1.8, zero guard resets). The detector stays as telemetry
// (UI + phase log); guard trips remain the backstop.
static const int   LOOP_WIN      = 1024;    // correlation window
static const int   LOOP_DMAX     = 12800;   // same 800 ms round trip as TDC
static const int   LOOP_DEC      = 4;       // box-decimate x4 (cheap NCC)
static const int   LOOP_PERIOD   = 2048;    // check every ~128 ms of audio
static const float LOOP_MIN_PEAK = 0.45f;   // NCC: delayed copy, not coincidence
static const int   LOOP_ON       = 2;       // hits needed to engage (hysteresis)
static const int   LOOP_QUIET    = 16;      // silent detections (~2 s) -> release

// ---- Aggressive-on-loop policy (mirrors web/nkf.py, proven there) ----
// A confirmed loop leans on everything downstream of the Kalman core —
// never the core itself (no freeze, no un-expose: field-verified howl
// fuel). Wire trim (-6 dB, multiplicative with the backstop) that
// ESCALATES to -12/-18 dB on gapless loud sustain (marginal stability
// at high coupling howls forever at -6; bursty voice resets the run,
// so mic tests never escalate). Single-window backstop attack while
// looped, eager TDC cadence, and a no-cancel watchdog (loud both legs
// with wire-as-loud-as-mic past grace + ~5 s trims like a loop — keyed
// on cancellation evidence, skipped failed-open). All self-release.
static const float LOOP_TRIM      = 0.5f;
static const float LOOP_TRIM_DEEP = 0.25f;    // -12 dB after ~3 s sustained loud
static const float LOOP_TRIM_FLOOR = 0.125f;   // -18 dB after ~6 s; never deeper
static const double LOOP_TAU_ATK  = 0.35;
static const double LOOP_TAU_REL  = 0.15;
static const int   LOOP_LOUD_BLOCKS = 96;     // ~3 s continuous per step
static const size_t NOCANCEL_HOT_AFTER = 80000;  // ~5 s loud, uncancelled
static const double NOCANCEL_ECHO_RATIO = 0.25;  // ref within 6 dB of mic

// ---- Tonal census + tracking notch (was: howl backstop trim) ---------
// A feedback howl is ONE tone owning the wire for seconds; voice and
// music spread across the spectrum. Measure per-frame spectral
// concentration of the pre-notch wire (512-pt Hann, best 3-bin sum
// over total) and arm only after a hold no speech pattern sustains,
// so voice can't false-trigger. This protects cases the engine
// cannot: broken/absent ref (loop detector blind), failed-open
// engine, coupling past what cancellation can pull under 1.
// The actuator is a tracking RBJ notch (mirrors web/nkf.py, proven
// there first) instead of the retired broadband trim: stable tones
// die like before, slow sweeps are followed instead of escaping,
// and voice outside one band survives untouched. Release needs
// PROOF (wire-vs-mic depth <= BS_HEAL_D on non-tonal audio) or a
// fully toneless window — releasing on mere silence would let the
// howl regrow and cycle. Depth ramps in/out (no clicks); ramp 0 is
// an exact bypass.
static const int    BS_BIN_RUN  = 40;      // same-bin (±1) run marking a
                                             // STABLE tone (~1.3 s): a howl
                                             // sits on one bin for seconds
                                             // (fixed loop delay); voice pitch
                                             // moves and consonants break the
                                             // run. Sustained sung vowels can
                                             // still trip it — correctly, they
                                             // ARE sustained tones — and heal
                                             // the moment phonation changes.
static const int    BS_FRAMES   = 64;      // frames per window (64x512 = 2.048 s)
static const int    BS_RUN      = 2;       // held windows -> arm (~4 s)
static const int    BS_HEAL_RUN = 1;       // qualifying windows -> release.
                                             // Deliberately asymmetric with
                                             // BS_RUN: field logs showed heal
                                             // alternating with run forever
                                             // (tonal-stable-ish wire), so 2
                                             // consecutive never arrived and
                                             // voiced speech stayed trimmed.
                                             // A single window still has to
                                             // qualify (loud mic + proven
                                             // cancellation), and a true
                                             // stable howl never qualifies —
                                             // it holds.
static const double BS_TONAL    = 0.33;  // per-frame 3-bin power fraction
static const int    BS_HITS     = 38;      // >=~60% of 64 frames tonal
static const double BS_MIC_MS   = 8.4e-5;  // mic mean-square floor (RMS ~300)
static const double BS_HEAL_D   = -1.0;    // depth (dB) proving cancellation
static const double NOTCH_Q_BW  = 60.0;    // notch bandwidth, Hz
static const double NOTCH_SLEW  = 150.0;   // max center move per block, Hz
static const int    NOTCH_RAMP_BLOCKS = 8; // engage/disengage ramp (~256 ms)
static const double NOTCH_FMIN  = 50.0;
static const double NOTCH_FMAX  = 7500.0;

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
    size_t micHead = 0, outHead = 0;

    // Per-block scratch (hoisted: no heap inside NkfProcess's loop)
    float micBlock[NKF_BLOCK_SHIFT];
    float refBlock[NKF_BLOCK_SHIFT];
    float outBlock[NKF_BLOCK_SHIFT];
    float emitBlock[NKF_BLOCK_SHIFT];
    float engMic[NKF_BLOCK_SHIFT];  // engine-domain copies (ENG_IN_SCALE)
    float engRef[NKF_BLOCK_SHIFT];

    size_t total = 0;               // mic+ref samples pushed (lockstep)
    size_t micConsumed = 0;         // abs index of micAccum[0]
    std::vector<float> micWin;      // last TDC_WIN mic samples (estimator)
    int alignDelay = 0;             // current ref->mic lag (samples)
    int samplesSinceTdc = 0;        // estimate as soon as data allows
    bool tdcLocked = false;
    bool tdcConfident = false;      // lag from a real NCC peak (not 3 s grace)
    // TDC / loop NCC prefix scratch (hoisted: used by DetectLoop/EstimateDelay)
    double tdcPref[TDC_WIN + TDC_DMAX + 1];
    float  micD[TDC_WIN / TDC_DEC];                       // coarse-pass scratch
    float  refD[(TDC_WIN + TDC_DMAX) / TDC_DEC];
    double refDPref[(TDC_WIN + TDC_DMAX) / TDC_DEC + 1];
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
    // Last TDC measurement even when rejected — grace-lock log explains
    // WHY no peak was accepted (out of range vs below threshold).
    float lastTdcSc = -2.0f;
    int   lastTdcD  = 0;
    // Loop-depth accumulator: engine mic vs engine out energy while the
    // self-monitor loop is engaged — 10log10(out/mic) tells whether
    // cancellation actually HOLDS during the loop (howl fuel check).
    double depMic = 0.0, depOut = 0.0;
    int    depSamples = 0;
    // Howl backstop state (tonal-hold detector + wire trim). bsHits/
    // bsFrames = current window's frame census; bsRun/bsHeal = the
    // attack/release hold counters fed by NkfBackstopWindow.
    int   bsHits = 0, bsFrames = 0;
    int   bsRun = 0, bsHeal = 0;
    int   bsBinLast = -1000000, bsBinRun = 0;  // tonal-center stability
    int   bsBinBest = 0, bsBinDom = -1;        // window max run + its bin
    // Tracking notch (replaces the broadband trim — mirror web/nkf.py).
    bool  notchArmed = false;
    float notchRamp = 0.0f;
    double notchF0 = 0.0;
    double notchB[3] = { 1.0, 0.0, 0.0 };
    double notchA[2] = { 0.0, 0.0 };
    double notchS1 = 0.0, notchS2 = 0.0;
    double notchDb = 0.0;
    // Aggressive-on-loop brakes (mirror web/nkf.py).
    float loopGain = 1.0f;
    int   loopLoudRun = 0;
    size_t nocancelHotSamples = 0;
    bool  nocancelHot = false;
    bool  lastWindowCancelled = false;
    bool  bsEnabled = true;             // NKF_BACKSTOP=0 disables the notch
    bool  anfEnabled = true;            // NKF_DISABLE_ANF=1 forces bypass
                                        // (A/B testing the actuator)
    // Frame-analysis scratch (512-pt Hann + r2c), built in NkfNew.
    std::vector<double> bsIn, bsHann;
    std::vector<std::complex<double>> bsSpec;
    std::vector<size_t> bsShape, bsAxes;
    std::vector<ptrdiff_t> bsStrideIn, bsStrideOut;
    // Phase-log transitions (crash diagnostics; see NkfPhase).
    bool engRanOnce = false;
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
// (R is indexed so R[n + dMax - d] = ref at the mic sample's time minus d).
// Coarse-to-fine: a x4 box-decimated pass sweeps the available range
// (up to 800 ms, ~0.7 M MACs worst case), then a full-rate pass
// refines ±TDC_FINE samples around the coarse peak (~17 k MACs).
static void NkfEstimateDelay(NkfHandle* h) {
    h->samplesSinceTdc = 0;
    if (h->micWin.size() < (size_t)TDC_WIN) return;
    if (h->refHist.size() < (size_t)(TDC_WIN + 512)) return;

    // Sweep as deep as ref history allows: full 800 ms once complete,
    // a shorter range in the first seconds — a typical <300 ms round
    // trip locks as soon as its data exists instead of waiting for the
    // whole buffer to fill.
    int dMax = (int)h->refHist.size() - TDC_WIN;
    if (dMax > TDC_DMAX) dMax = TDC_DMAX;
    dMax &= ~(TDC_DEC - 1);         // keep the x4 decimation aligned
    if (dMax < 512) return;         // need >= 32 ms of lag range

    const float* M = h->micWin.data();
    const long long front = (long long)h->total - (long long)h->refHist.size();
    const long long refLo = (long long)h->total - TDC_WIN - dMax;
    if (refLo < front) return;
    const float* R = h->refHist.data() + (refLo - front);

    // ---- Coarse pass: box-decimated x4 over the available range ---------
    const int WD = TDC_WIN / TDC_DEC;                 // 256
    const int DB = dMax / TDC_DEC;
    const int RD = DB + WD;                           // (dMax+WIN)/4
    float* Md = h->micD;
    float* Rd = h->refD;
    for (int k = 0; k < WD; k++)
        Md[k] = (M[4*k] + M[4*k+1] + M[4*k+2] + M[4*k+3]) * 0.25f;
    for (int j = 0; j < RD; j++)
        Rd[j] = (R[4*j] + R[4*j+1] + R[4*j+2] + R[4*j+3]) * 0.25f;

    double micEd = 0;
    for (int k = 0; k < WD; k++) micEd += (double)Md[k] * Md[k];
    if (micEd / WD < TDC_MIN_MEAN_E) return;

    double* dpref = h->refDPref;
    dpref[0] = 0.0;
    for (int j = 0; j < RD; j++)
        dpref[j + 1] = dpref[j] + (double)Rd[j] * Rd[j];
    if (dpref[RD] / RD < TDC_MIN_MEAN_E) return;

    double cBest = -2.0;
    int cBestDb = 0;
    for (int db = 0; db <= DB; db++) {
        const int off = DB - db;
        double num = 0;
        for (int k = 0; k < WD; k++)
            num += (double)Md[k] * Rd[off + k];
        const double refEd = dpref[off + WD] - dpref[off];
        if (refEd <= 0.0) continue;
        const double sc = num / std::sqrt(micEd * refEd);
        if (sc > cBest + TDC_TIE_EPS) { cBest = sc; cBestDb = db; }
    }

    // ---- Fine pass: full-rate NCC around the coarse peak ---------------
    double micE = 0;
    for (int n = 0; n < TDC_WIN; n++)
        micE += (double)M[n] * M[n];
    if (micE / TDC_WIN < TDC_MIN_MEAN_E) return;

    const int RLEN = TDC_WIN + dMax;
    double* pref = h->tdcPref;
    pref[0] = 0.0;
    for (int i = 0; i < RLEN; i++)
        pref[i + 1] = pref[i] + (double)R[i] * R[i];
    if (pref[RLEN] / RLEN < TDC_MIN_MEAN_E) return;

    int dLo = cBestDb * TDC_DEC - TDC_FINE;
    if (dLo < 0) dLo = 0;
    int dHi = cBestDb * TDC_DEC + TDC_FINE;
    if (dHi > dMax) dHi = dMax;

    double best = -2.0;
    int bestD = h->alignDelay;
    for (int d = dLo; d <= dHi; d++) {
        const int off = dMax - d;
        double num = 0;
        for (int n = 0; n < TDC_WIN; n++)
            num += (double)M[n] * R[n + off];
        const double refE = pref[off + TDC_WIN] - pref[off];
        if (refE <= 0.0) continue;
        const double sc = num / std::sqrt(micE * refE);
        if (sc > best) { best = sc; bestD = d; }
    }
    h->lastTdcD = bestD;            // remembered even when rejected —
    h->lastTdcSc = (float)best;     // grace-lock log explains why

    // Small tracking steps accept a moderate peak; a big lag jump needs
    // stronger evidence so noise can't teleport the alignment.
    int diff = bestD - h->alignDelay;
    if (diff < 0) diff = -diff;
    const bool jump = diff > TDC_JUMP;
    if (best >= (jump ? (double)TDC_JUMP_PEAK : (double)TDC_MIN_PEAK)) {
        const bool wasLocked = h->tdcLocked;
        const int prevD = h->alignDelay;
        // Slew-rate limit (see TDC_SLEW): walk toward far estimates
        // instead of teleporting. The jump-peak gate above still judges
        // the true candidate; only the applied motion is limited.
        int target = bestD;
        const int step = target - prevD;
        if (step > TDC_SLEW) target = prevD + TDC_SLEW;
        else if (step < -TDC_SLEW) target = prevD - TDC_SLEW;
        h->alignDelay = target;
        h->tdcLocked = true;
        if (!h->tdcConfident)
            NkfPhase("t=%.2f TDC lock%s d=%d sc=%.3f", NKF_T(h),
                     wasLocked ? " (update)" : "", target, best);
        else if (target - prevD >= 32 || prevD - target >= 32)
            NkfPhase("t=%.2f TDC drift d=%d->%d sc=%.3f", NKF_T(h),
                     prevD, target, best);
        h->tdcConfident = true;   // real peak — safe to stay live in a loop
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
    // mic/ref legs are followed by the block loop (live even when the
    // engine is skipped); only the out leg updates here with the trip.
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
// tail: ref[n] ~ out[n - L] with L in [0, available range] = a
// self-monitor loop is on. Confirms with LOOP_ON hits, releases after
// LOOP_QUIET consecutive silent windows (mic test over -> telemetry
// clears; no exposure policy hangs off this any more).
static void NkfDetectLoop(NkfHandle* h) {
    h->samplesSinceLoop = 0;
    const int W = LOOP_WIN, D = LOOP_DEC;
    const int Wd = W / D;
    if ((int)h->refHist.size() < W || (int)h->outHist.size() < W + 512)
        return;

    // Partial range, like the TDC: full 800 ms once the buffer is
    // complete, shorter early — a typical <300 ms loop is caught in
    // the first half second instead of waiting for the whole buffer.
    int dMax = (int)h->outHist.size() - W;
    if (dMax > LOOP_DMAX) dMax = LOOP_DMAX;
    dMax &= ~(D - 1);
    if (dMax < 512) return;

    const int OdN = dMax / D + Wd;   // (W+dMax)/D
    const int off = dMax / D;        // out-bin lag 0 aligns tail-to-tail

    float* Rf = h->refHist.data() + (h->refHist.size() - W);
    float* Of = h->outHist.data() + (h->outHist.size() - (W + dMax));

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
        if (h->loopConf == LOOP_ON)
            NkfPhase("t=%.2f loop engaged sc=%.3f", NKF_T(h), best);
    } else if (h->loopConf > 0) {
        if (h->loopConf == LOOP_ON)
            NkfPhase("t=%.2f loop released sc=%.3f", NKF_T(h), best);
        h->loopConf--;
    }
}

// One 512-sample frame of the wire: is it a single dominant tone?
// Mean out, Hann, r2c, then best 3-bin power sum vs total — a howl
// frame lands ~0.45+, a voiced frame spreads over harmonics (<0.33).
// bestBin receives the center bin of the winning window (-1 on
// silence) so callers can tell a FIXED tone (howl: same bin for
// seconds) from a MOVING one (voice pitch wanders bin to bin).
static bool NkfFrameTonal(NkfHandle* h, const float* v, int* bestBin) {
    double mean = 0.0;
    for (int i = 0; i < NKF_BLOCK_SHIFT; i++) mean += (double)v[i];
    mean /= (double)NKF_BLOCK_SHIFT;
    for (int i = 0; i < NKF_BLOCK_SHIFT; i++)
        h->bsIn[i] = (v[i] - mean) * h->bsHann[i];
    pocketfft::r2c(h->bsShape, h->bsStrideIn, h->bsStrideOut,
                   h->bsAxes, pocketfft::FORWARD,
                   h->bsIn.data(), h->bsSpec.data(), 1.0);
    // powers[1..255]; index 0 (DC) excluded from numerator and total.
    auto powAt = [&](int k) -> double {
        if (k < 1 || k > NKF_BLOCK_SHIFT / 2) return 0.0;
        return std::norm(h->bsSpec[(size_t)k]);
    };
    double tot = 0.0, best3 = 0.0;
    int bestK = -1;
    for (int k = 1; k <= NKF_BLOCK_SHIFT / 2; k++) tot += powAt(k);
    if (!(tot > 1e-30)) { if (bestBin) *bestBin = -1; return false; }
    for (int k = 1; k <= NKF_BLOCK_SHIFT / 2; k++) {
        const double s = powAt(k - 1) + powAt(k) + powAt(k + 1);
        if (s > best3) { best3 = s; bestK = k; }
    }
    if (bestBin) *bestBin = bestK;
    return best3 / tot >= BS_TONAL;
}

// Window boundary (64 frames): depth log + notch arm/disarm.
// NOTE: name kept (callers, tests); the actuator is the tracking
// notch now (mirror web/nkf.py), not the retired broadband trim.
// Engagement/heal conditions are unchanged.
static void NkfNotchRetune(NkfHandle* h) {
    const double w0 = 2.0 * 3.14159265358979323846 * h->notchF0 / 16000.0;
    double q = h->notchF0 / NOTCH_Q_BW;
    if (q < 1.0) q = 1.0;
    if (q > 20.0) q = 20.0;
    const double alpha = sin(w0) / (2.0 * q);
    const double c = cos(w0);
    const double a0 = 1.0 + alpha;
    h->notchB[0] = 1.0 / a0;
    h->notchB[1] = -2.0 * c / a0;
    h->notchB[2] = 1.0 / a0;
    h->notchA[0] = -2.0 * c / a0;
    h->notchA[1] = (1.0 - alpha) / a0;
}

static void NkfBackstopWindow(NkfHandle* h) {
    const int frames = h->bsFrames;
    const double micMs = h->depMic / (double)h->depSamples;
    const double outMs = h->depOut / (double)h->depSamples;
    const double depth =
        10.0 * log10((h->depOut + 1e-12) / (h->depMic + 1e-12));
    // Cancellation evidence for the no-cancel watchdog (pure
    // measurement — independent of the tonal logic below).
    h->lastWindowCancelled = depth <= BS_HEAL_D;

    if (h->loopConf >= LOOP_ON && h->depMic > 0.0)
        NkfPhase("t=%.2f loop depth=%.1f dB", NKF_T(h), depth);

    // Release when the howl is gone and real audio is present at the
    // mic. The old test also required the wire to be loud (outMs >=
    // BS_OUT_MS), but the backstop's entire job is to keep the wire
    // quiet -- so that clause could never be satisfied while the trim
    // was deep, and the backstop held -84 dB forever, muting the
    // near-end voice. The mic floor alone proves real audio is here;
    // depth <= BS_HEAL_D still guards against releasing while the
    // engine is amplifying (positive depth).
    const bool floors = micMs >= BS_MIC_MS && depth <= BS_HEAL_D;
    const bool tonal = frames > 0 && h->bsHits >= BS_HITS;
    // Stable center: the dominant bin held (±1) for BS_BIN_RUN+
    // blocks this window. A howl parks; voiced speech wanders, so a
    // tonal-but-moving wire is voice, not feedback.
    const bool stable = h->bsBinBest >= BS_BIN_RUN;
    if (h->bsHits >= BS_HITS / 2 || h->notchArmed)
        NkfPhase("t=%.2f bs-watch hits=%d/%d dom=%d stable=%d micMs=%.2e outMs=%.2e d=%.1f "
                 "run=%d heal=%d", NKF_T(h), h->bsHits, frames, h->bsBinDom,
                 stable ? 1 : 0, micMs, outMs,
                 depth, h->bsRun, h->bsHeal);
    // Arm needs two consecutive CENTER-STABLE tonal windows: a
    // merely-tonal window with a wandering center falls into the
    // heal/hold branches below (both reset the run counter), so it
    // can never accrue an arm. Net effect, verified on the synth:
    // fixed-center howls arm, wandering voices never do. The notch
    // then TRACKS (steered per block below), so slow sweeps are
    // followed where the retired trim could never hold them.
    if (tonal && stable) {
        h->bsHeal = 0;
        if (h->bsRun < 1000) h->bsRun++;
        // Looped howls arm after ONE window: a tonal-stable wire
        // while the loop detector is engaged is the howl signature —
        // waiting costs howl seconds. Heal still demands proof.
        const int needRun =
            (h->loopConf >= LOOP_ON) ? 1 : BS_RUN;
        if (h->bsRun >= needRun && h->bsEnabled && h->anfEnabled) {
            if (!h->notchArmed) {
                h->notchArmed = true;
                double f = (double)h->bsBinDom * 16000.0 / 512.0;
                if (f < NOTCH_FMIN) f = NOTCH_FMIN;
                if (f > NOTCH_FMAX) f = NOTCH_FMAX;
                h->notchF0 = f;
                NkfNotchRetune(h);
                NkfPhase("t=%.2f notch ATTACK depth=%.1f dB "
                         "tonal=%d/%d f0=%.0f Hz",
                         NKF_T(h), depth, h->bsHits, frames, f);
            }
        }
    // Heal accrues on non-tonal windows (classic case) AND on
    // tonal-but-unstable ones (voiced speech: pitch wanders bin to
    // bin, so no 40-block run forms) — plus any fully toneless
    // window (nothing to notch). A fixed-center howl keeps
    // `stable` true and can never heal.
    } else if (((!tonal || !stable) && floors && depth <= BS_HEAL_D)
               || (frames > 0 && h->bsHits == 0)) {
        h->bsRun = 0;
        if (h->bsHeal < 1000) h->bsHeal++;
        if (h->bsHeal >= BS_HEAL_RUN && h->notchArmed) {
            h->notchArmed = false;
            h->bsRun = h->bsHeal = 0;
            NkfPhase("t=%.2f notch release (tone gone, depth=%.1f dB)",
                     NKF_T(h), depth);
        }
    } else {
        h->bsRun = 0;
        h->bsHeal = 0;
    }

    h->depMic = h->depOut = 0.0;
    h->depSamples = 0;
    h->bsHits = h->bsFrames = 0;
    h->bsBinBest = 0;
    h->bsBinDom = -1;
}

// Ship finished post-NKF samples (float, ±1) to the wire: outHist
// (loop detector tap — what the speakers actually get) and the clamped
// int16 outAccum. v2.0: WebRTC NS retired — DTLN-NS runs as the
// global post stage stacked after every engine.
static void NkfEmit(NkfHandle* h, const float* v, int n) {
    for (int i = 0; i < n; i++) {
        h->outHist.push_back(v[i]);
        float s = v[i] * 32768.0f;
        if (s >  32767.0f) s =  32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        h->outAccum.push_back((int16_t)s);
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

NkfHandle* NkfNew(const char* modelPath) {
    auto* h = new NkfHandle();
    try {
        h->engine = new NKFImpl(modelPath);
    } catch (...) {
        NkfPhase("model load FAILED (%s)", modelPath ? modelPath : "?");
        delete h;
        return nullptr;
    }
    NkfPhase("model loaded (%s)", modelPath ? modelPath : "?");
    h->micAccum.reserve(NKF_BLOCK_SHIFT * 4);
    h->refHist.reserve(TDC_WIN + TDC_DMAX + NKF_BLOCK_SHIFT * 4);
    h->micWin.reserve(TDC_WIN);
    h->outAccum.reserve(NKF_BLOCK_SHIFT * 4);
    h->outHist.reserve(LOOP_WIN + LOOP_DMAX);
    // Howl backstop: 512-pt Hann frame analysis, one frame per block.
    const char* bs = getenv("NKF_BACKSTOP");
    h->bsEnabled = !(bs && bs[0] == '0');
    if (!h->bsEnabled) NkfPhase("notch disabled (NKF_BACKSTOP=0)");
    const char* anf = getenv("NKF_DISABLE_ANF");
    h->anfEnabled = !(anf && anf[0] == '1');
    if (!h->anfEnabled) NkfPhase("notch bypassed (NKF_DISABLE_ANF=1)");
    if (getenv("NKF_FORCE_GIVEUP")) {
        h->giveUp = true;
        NkfPhase("forced fail-open (NKF_FORCE_GIVEUP)");
    }
    h->bsIn.assign((size_t)NKF_BLOCK_SHIFT, 0.0);
    h->bsSpec.assign((size_t)(NKF_BLOCK_SHIFT / 2 + 1),
                     std::complex<double>(0.0, 0.0));
    h->bsHann.assign((size_t)NKF_BLOCK_SHIFT, 0.0);
    for (int i = 0; i < NKF_BLOCK_SHIFT; i++)
        h->bsHann[i] = 0.5 - 0.5 * cos(3.14159265358979323846 * 2.0 *
                                       (double)i / (double)NKF_BLOCK_SHIFT);
    h->bsShape.assign(1, (size_t)NKF_BLOCK_SHIFT);
    h->bsAxes.assign(1, (size_t)0);
    h->bsStrideIn.assign(1, (ptrdiff_t)sizeof(double));
    h->bsStrideOut.assign(1, (ptrdiff_t)sizeof(std::complex<double>));
    return h;
}

static void NkfProcessImpl(NkfHandle* h, const int16_t* mic,
                           const int16_t* ref, int16_t* out, int frameSize) {
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
        // Eager cadence until the lag is a *confident* peak — a grace
        // lock (or no lock) keeps searching fast so a loop can exit
        // shadow as soon as the delay is trustworthy. Eager too while
        // looped: loop delay may be shifting, and alignment is the
        // cheapest stabilization available.
        const bool loopedNow = h->loopConf >= LOOP_ON;
        const int tdcNeed =
            (!loopedNow && h->tdcLocked && h->tdcConfident) ? TDC_PERIOD
                                                           : TDC_EAGER;
        if (h->samplesSinceTdc >= tdcNeed) NkfEstimateDelay(h);
        if (!h->tdcLocked && h->total >= (size_t)TDC_LOCK_GRACE) {
            h->tdcLocked = true;  // engage anyway; TDC keeps correcting
            NkfPhase("t=%.2f TDC grace lock (no peak; last d=%d sc=%.3f)",
                     NKF_T(h), h->lastTdcD, (double)h->lastTdcSc);
        }
    }
    // Loop detection runs even after fail-open: it reads only ref/out
    // histories (no engine), and the UI must show a live loop while
    // it persists.
    h->samplesSinceLoop += frameSize;
    if (h->samplesSinceLoop >= LOOP_PERIOD) NkfDetectLoop(h);

    while (h->micAccum.size() - h->micHead >= (size_t)NKF_BLOCK_SHIFT) {
        // Aligned pairing: mic [micConsumed, +SHIFT) with ref
        // alignDelay samples earlier — the TDC slice NKF needs.
        // Positions < 0 are stream warmup: feed zeros. (Pre-lock the
        // engine isn't called; the slice just stays ready.)
        const long long needStart =
            (long long)h->micConsumed - h->alignDelay;
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

        // Level followers run on EVERY block (engine or not) so the
        // no-cancel watchdog sees live audio even failed-open. (The
        // guard keeps only its out-leg update; trip logic unchanged.)
        {
            double m = 0, r = 0;
            for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                m += (double)h->micBlock[i] * h->micBlock[i];
                r += (double)h->refBlock[i] * h->refBlock[i];
            }
            m /= NKF_BLOCK_SHIFT; r /= NKF_BLOCK_SHIFT;
            h->micEnv += GUARD_EMA * ((float)m - h->micEnv);
            h->refEnv += GUARD_EMA * ((float)r - h->refEnv);
            // No-cancel watchdog: loud both legs, echo-significant
            // (ref within 6 dB of mic — loud talk over quiet music has
            // nothing to fix), no proven cancellation lately.
            const bool echoBig =
                h->refEnv >= BS_MIC_MS
                && h->refEnv >= (float)(NOCANCEL_ECHO_RATIO * h->micEnv);
            if (h->micEnv < BS_MIC_MS || !echoBig
                    || h->lastWindowCancelled) {
                h->nocancelHotSamples = 0;
            } else {
                h->nocancelHotSamples += NKF_BLOCK_SHIFT;
            }
        }

        // Engine runs only once locked and before fail-open; its output
        // reaches the wire only past shadow, through the fade.
        bool processed = false;
        if (h->tdcLocked && !h->giveUp) {
            try {
                // Closed mic<->loudspeaker loop: keep ADAPTING. The old
                // freeze policy (fixed filter while looped) sustained a
                // howl — the frozen filter never learned the loop path
                // (closed-loop test: howl at coupling >= 1.2). Since the
                // level-scaled engine, in-loop adaptation converges and
                // *stabilises* the loop (held to coupling 1.8, 0 guard
                // resets): cancelling the pickup keeps loop gain < 1,
                // which is the only real way to stop the ringing —
                // raw mic or a stale filter is exactly the fuel.
                if (!h->engRanOnce) {
                    h->engRanOnce = true;
                    NkfPhase("t=%.2f first ProcessBlock", NKF_T(h));
                }
                for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                    h->engMic[i] = h->micBlock[i] * ENG_IN_SCALE;
                    h->engRef[i] = h->refBlock[i] * ENG_IN_SCALE;
                }
                h->engine->ProcessBlock(h->engMic, h->engRef, h->outBlock);
                for (int i = 0; i < NKF_BLOCK_SHIFT; i++)
                    h->outBlock[i] *= ENG_OUT_SCALE;
            } catch (...) {
                // Ort::Exception (or worse) from the engine: fail open
                // instead of taking the app down with it.
                NkfPhase("t=%.2f EXCEPTION in ProcessBlock -> fail open",
                         NKF_T(h));
                h->giveUp = true;
                h->engine->Reset();
                h->shadowBlocks = TDC_SHADOW;
                h->fadePos = TDC_FADE;
                continue;
            }
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
                NkfPhase("t=%.2f guard reset #%d%s", NKF_T(h), h->resets,
                         h->giveUp ? " -> give up (fail open)" : "");
                processed = false;
            }
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

        for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
            float v = h->micBlock[i];
            if (g > 0.0f) v += (h->outBlock[i] - v) * g;
            h->emitBlock[i] = v;
        }

        // Aggressive-on-loop brakes (mirror web/nkf.py). Escalation
        // run on pre-gain wire energy — post-trim energy would
        // sawtooth against its own trim. Any quiet-ish block
        // restarts it: bursty voice never climbs, gapless sustain
        // (howls don't pause; speech does) does.
        const bool looped = h->loopConf >= LOOP_ON;
        const bool nocancel =
            h->total > (size_t)TDC_LOCK_GRACE && !h->giveUp
            && h->nocancelHotSamples >= NOCANCEL_HOT_AFTER;
        h->nocancelHot = nocancel;
        {
            double ms = 0;
            for (int i = 0; i < NKF_BLOCK_SHIFT; i++)
                ms += (double)h->emitBlock[i] * h->emitBlock[i];
            ms /= NKF_BLOCK_SHIFT;
            if ((looped || nocancel) && ms >= BS_MIC_MS)
                h->loopLoudRun++;
            else
                h->loopLoudRun = 0;
        }
        float loopTarget = 1.0f;
        if (looped || nocancel) {
            const int step = h->loopLoudRun / LOOP_LOUD_BLOCKS;
            loopTarget = (step >= 2) ? LOOP_TRIM_FLOOR
                       : (step >= 1) ? LOOP_TRIM_DEEP : LOOP_TRIM;
        }

        // Tonal census on the PRE-notch wire: sense the threat, not
        // our own suppression (post-notch analysis would go blind the
        // moment the notch works, oscillating armed state on a
        // sustained tone). One analysis serves steering below and the
        // stability runs — no extra FFT.
        {
            int bb = -1;
            if (NkfFrameTonal(h, h->emitBlock, &bb)) {
                h->bsHits++;
                // Tonal-center stability run: same bin (±1) keeps
                // counting, a moved center restarts it. Voice pitch
                // wanders bin to bin; a howl parks on one for seconds.
                // (Non-tonal blocks take the else branch below, which
                // also restarts the run.)
                if (bb >= h->bsBinLast - 1 && bb <= h->bsBinLast + 1) {
                    h->bsBinRun++;
                } else {
                    h->bsBinLast = bb;
                    h->bsBinRun = 1;
                }
                if (h->bsBinRun > h->bsBinBest) {
                    h->bsBinBest = h->bsBinRun;
                    h->bsBinDom = h->bsBinLast;
                }
            } else {
                h->bsBinLast = -1000000;
                h->bsBinRun = 0;
                bb = -1;
            }
            // Tracking notch (replaces the broadband trim — mirror
            // web/nkf.py): steer toward this block's center
            // (slew-limited: sweeps tracked, jumps absorbed), depth
            // ramps in/out (no clicks), exact bypass at ramp 0.
            if (h->notchArmed && h->anfEnabled && bb > 0) {
                double target = (double)bb * 16000.0 / 512.0;
                if (target < NOTCH_FMIN) target = NOTCH_FMIN;
                if (target > NOTCH_FMAX) target = NOTCH_FMAX;
                double step = target - h->notchF0;
                if (step > NOTCH_SLEW) step = NOTCH_SLEW;
                else if (step < -NOTCH_SLEW) step = -NOTCH_SLEW;
                if (step != 0.0) {
                    h->notchF0 += step;
                    NkfNotchRetune(h);
                }
            }
            const float wantRamp =
                (h->notchArmed && h->anfEnabled) ? 1.0f : 0.0f;
            const float rampStep = 1.0f / (float)NOTCH_RAMP_BLOCKS;
            if (h->notchRamp < wantRamp) {
                h->notchRamp += rampStep;
                if (h->notchRamp > wantRamp) h->notchRamp = wantRamp;
            } else if (h->notchRamp > wantRamp) {
                h->notchRamp -= rampStep;
                if (h->notchRamp < wantRamp) h->notchRamp = wantRamp;
            }
            if (h->notchRamp > 0.0f) {
                double inE = 0.0;
                for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                    const double v = h->emitBlock[i];
                    inE += v * v;
                }
                const double b0 = h->notchB[0], b1 = h->notchB[1],
                             b2 = h->notchB[2];
                const double a1 = h->notchA[0], a2 = h->notchA[1];
                double s1 = h->notchS1, s2 = h->notchS2;
                for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                    const double x = h->emitBlock[i];
                    const double yn = b0 * x + s1;
                    s1 = b1 * x - a1 * yn + s2;
                    s2 = b2 * x - a2 * yn;
                    h->emitBlock[i] = (float)(x + (yn - x)
                                              * (double)h->notchRamp);
                }
                h->notchS1 = s1;
                h->notchS2 = s2;
                double outE = 0.0;
                for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                    const double v = h->emitBlock[i];
                    outE += v * v;
                }
                // Guard the silence floor: log10(0) is -inf, and -inf
                // arithmetic NaN-poisons the smoothed telemetry (and
                // anything downstream reading it) permanently.
                const double inst =
                    (inE > 1e-12)
                    ? 10.0 * log10(outE / (inE + 1e-12)) : 0.0;
                h->notchDb += 0.25 * (inst - h->notchDb);
            } else {
                h->notchDb += 0.25 * (0.0 - h->notchDb);
            }
        }
        // Loop trim: multiplicative with the notch, own smoothing.
        // Starves the loop of gain; the engine keeps adapting under it.
        h->loopGain += (float)((loopTarget - h->loopGain) *
                               ((loopTarget < h->loopGain) ? LOOP_TAU_ATK
                                                           : LOOP_TAU_REL));
        if (h->loopGain != 1.0f)
            for (int i = 0; i < NKF_BLOCK_SHIFT; i++)
                h->emitBlock[i] *= h->loopGain;

        // Window accumulators on the WIRE vs mic: the howl fuel check
        // now sees what the loop actually carries (shadow and fail-open
        // included — emit == mic there, i.e. 0 dB, uncancelled). Every
        // 64th frame closes the window and runs arm/release + depth log.
        {
            double sm = 0, so = 0;
            for (int i = 0; i < NKF_BLOCK_SHIFT; i++) {
                sm += (double)h->micBlock[i] * h->micBlock[i];
                so += (double)h->emitBlock[i] * h->emitBlock[i];
            }
            h->depMic += sm;
            h->depOut += so;
            h->depSamples += NKF_BLOCK_SHIFT;
            if (++h->bsFrames >= BS_FRAMES) NkfBackstopWindow(h);
        }

        NkfEmit(h, h->emitBlock, NKF_BLOCK_SHIFT);

        // Warm-up (or post-guard) shadow drains unconditionally. It used
        // to be held by an active loop until the delay was confident —
        // but pinning NKF on raw mic while a feedback loop runs strips
        // the wire of all cancellation and the howl grows on exactly
        // that fuel (no post stage can hold a near-field loop alone;
        // DTLN/AEC3 never un-expose either). Live-in-loop is safe now:
        // the engine adapts straight through the loop (see above),
        // cancelling the pickup so loop gain stays below 1.
        if (shadowed && --h->shadowBlocks == 0)
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

void NkfProcess(NkfHandle* h, const int16_t* mic, const int16_t* ref,
                int16_t* out, int frameSize) {
    // Fail-open: dead handle ships mic, never silence.
    if (!h || !h->engine) {
        if (out && mic && frameSize > 0)
            memcpy(out, mic, (size_t)frameSize * sizeof(int16_t));
        return;
    }
    // Last-resort catch: an exception anywhere in the pipeline must not
    // take the app down — log the phase and fail open to mic.
    try {
        NkfProcessImpl(h, mic, ref, out, frameSize);
    } catch (...) {
        NkfPhase("t=%.2f EXCEPTION in NkfProcess -> fail open",
                 h->total ? (double)h->total / 16000.0 : 0.0);
        h->giveUp = true;
        if (out && mic && frameSize > 0)
            memcpy(out, mic, (size_t)frameSize * sizeof(int16_t));
    }
}

void NkfReset(NkfHandle* h) {
    if (!h) return;
    h->micAccum.clear();
    h->refHist.clear();
    h->outAccum.clear();
    h->micWin.clear();
    h->outHist.clear();
    h->micHead = 0;
    h->outHead = 0;
    h->total = 0;
    h->micConsumed = 0;
    h->alignDelay = 0;
    h->samplesSinceTdc = 0;
    h->tdcLocked = false;
    h->tdcConfident = false;
    h->micEnv = h->refEnv = h->outEnv = 0;
    h->hotBlocks = 0;
    h->resets = 0;
    h->giveUp = false;
    h->shadowBlocks = TDC_SHADOW;
    h->fadePos = TDC_FADE;
    h->samplesSinceLoop = 0;
    h->loopConf = 0;
    h->loopQuiet = 0;
    h->lastTdcSc = -2.0f;
    h->lastTdcD = 0;
    h->depMic = h->depOut = 0.0;
    h->depSamples = 0;
    h->loopGain = 1.0f;
    h->loopLoudRun = 0;
    h->nocancelHotSamples = 0;
    h->nocancelHot = false;
    h->lastWindowCancelled = false;
    h->notchArmed = false;
    h->notchRamp = 0.0f;
    h->notchF0 = 0.0;
    h->notchB[0] = 1.0; h->notchB[1] = 0.0; h->notchB[2] = 0.0;
    h->notchA[0] = 0.0; h->notchA[1] = 0.0;
    h->notchS1 = h->notchS2 = 0.0;
    h->notchDb = 0.0;
    h->bsHits = h->bsFrames = 0;
    h->bsRun = h->bsHeal = 0;
    // NOTE: bsBin* intentionally survive reset (tonal-center memory is
    // session-lifetime — mirrors web/nkf.py).
    h->engRanOnce = false;
    if (h->engine) h->engine->Reset();
}

void NkfDestroy(NkfHandle* h) {
    if (!h) return;
    delete h->engine;
    delete h;
}

void NkfGetState(NkfHandle* h, NkfState* s) {
    if (!s) return;
    s->lagSamples = 0;
    s->confident = 0;
    s->locked = 0;
    s->exposed = 0;
    s->loopActive = 0;
    s->guardResets = 0;
    s->giveUp = 0;
    s->loopDb = 0.0f;
    s->resDb = 0.0f;
    s->nocancelHot = 0;
    s->notchFreq = 0.0f;
    s->notchDb = 0.0f;
    if (!h) return;
    s->lagSamples = h->alignDelay;
    s->confident = h->tdcConfident ? 1 : 0;
    s->locked = h->tdcLocked ? 1 : 0;
    s->exposed = (h->tdcLocked && !h->giveUp && h->shadowBlocks == 0 &&
                  h->fadePos >= TDC_FADE) ? 1 : 0;
    s->loopActive = h->loopConf >= LOOP_ON ? 1 : 0;
    s->guardResets = h->resets;
    s->giveUp = h->giveUp ? 1 : 0;
    s->loopDb = (float)(20.0 * log10((double)h->loopGain + 1e-9));
    s->resDb = h->engine ? (float)h->engine->ResDb() : 0.0f;
    s->nocancelHot = h->nocancelHot ? 1 : 0;
    s->notchFreq = (h->notchRamp > 0.0f) ? (float)h->notchF0 : 0.0f;
    s->notchDb = (float)h->notchDb;
}

} // extern "C"
