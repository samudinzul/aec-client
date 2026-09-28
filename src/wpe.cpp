// ============================================================
//  WPE wrapper — streaming dereverb post-filter (v1.9.0 core,
//  48 kHz support added for v2.0.0)
//
//  Streaming single-channel Weighted Prediction Error
//  dereverberation: a model-free late-reverb canceller (no ONNX,
//  no new dependencies — same pocketfft header NKF/DTLN use).
//  Stacked after any engine as the "Dereverb (WPE)" stage.
//
//  Rate-derived frame geometry (same milliseconds at both rates,
//  so the algorithm behaves identically wherever it runs):
//    n = sr/32 (512 @16k, 1536 @48k)  -> 32 ms frames
//    hop = n/4 (128 @16k, 384 @48k)   ->  8 ms hops
//    pad = 3*hop, prefill = hop, bins = n/2+1 (257 / 769)
//
//  Algorithm (per bin):
//    - STFT n_fft=n, hop=n/4, sqrt(periodic hann)
//    - regressors u = {Y[n-D-i]}, D = 2 frames, T = 5 taps
//      (history = the OBSERVED mixture, classic WPE)
//    - per-frame weight  w = 1 / max(|Y|², max|u|², floor) —
//      inverse-power cost, every covariance/rhs entry then stays
//      O(1) whatever the level (no slow blow-up, no underflow)
//    - RLS with forgetting lambda=0.995, solve (R + load*I) g = r
//      with relative diagonal loading (1e-2 of mean trace)
//    - output Z = Y - g^H u, hard-bounded: |pred| <= cap|Y|
//      and |Z| <= 1.5|Y| (never adds gain). The cap is what keeps
//      voice from being eaten — vowels are quasi-stationary, hence
//      perfectly predictable, and an uncapped predictor would
//      subtract the direct path itself. Measured on synthetic
//      syllabic vowels: cap 0.5 -> -5.3 dB voice (measured), cap
//      0.25 -> about -2.5 dB (bounded worst case). So the cap is
//      speech-adaptive via WpeSetSpeech: TIGHT (0.25) while the
//      near-end person talks, WIDE (0.5) between speech where only
//      reverb/echo tails remain and aggressive subtraction is the
//      whole point. Unsolvable bin -> that bin resets and passes
//      through this frame.
//
//  Streaming (1:1, mirrors GTCRN's proven ring/FIFO pattern):
//    - input FIFO pre-padded with 3*hop zeros; a frame = q[0..n);
//      each frame consumes hop. The pad makes the first three
//      emitted hops pure pre-stream zeros, so the first REAL hop
//      already has full COLA (4 frames, hann sum = 2 at hop N/4,
//      synthesis carries x0.5) — no ramp-in, no edge dropout.
//    - output ring prefilled with `prefill` zeros: never runs dry.
//    - latency: real sample 0 emerges after n consumed = 32 ms,
//      then constant.
//
//  Fail-open: any internal latch copies input to output until
//  WpeReset — the stage can mute nothing, ever (release theme).
// ============================================================
#include "wpe.h"

#include "pocketfft_hdronly.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr int kDelay   = 2;                      // prediction delay (frames)
constexpr int kTaps    = 5;                      // LP taps per bin
constexpr int kHist    = kDelay + kTaps - 1;     // 6 stored spectra

constexpr double kForget   = 0.995;              // RLS forgetting
constexpr double kLoad     = 0.01;               // rel. diagonal loading
constexpr double kPowFloor = 1e-6;               // weight floor (unit scale)
constexpr double kPredCapSpeech = 0.25;          // |pred| <= 0.25|Y| (voice)
constexpr double kPredCapOther  = 0.5;           // |pred| <= 0.5|Y| (tails)
constexpr double kOutCap   = 1.5;                // |Z|    <= 1.5|Y|

}  // namespace

struct WpeHandle {
    // Rate-derived geometry (see header comment).
    int n = 0;                                   // FFT frame length
    int hop = 0;                                 // hop = n/4
    int bins = 0;                                // n/2+1
    int pad = 0;                                 // 3*hop input prefill
    int prefill = 0;                             // hop output priming
    int ringCap = 0;                             // 8*n
    int qCap = 0;                                // 4*n

    std::vector<float> w;                        // sqrt(periodic hann), n
    std::vector<float> q;                        // input FIFO, qCap
    int qn = 0;
    std::vector<float> ola;                      // overlap-add region, n

    std::vector<float> ring;                     // output FIFO (prefilled)
    int rHead = 0, rCount = 0;

    // Observed-mixture history, flat [bin*kHist]: [0] = last frame.
    std::vector<std::complex<double>> hist;
    int histN = 0;                               // valid frames, saturates

    // Per-bin RLS state (double: complex 5x5 solves need the headroom).
    std::vector<std::complex<double>> R;         // bins*kTaps*kTaps
    std::vector<std::complex<double>> rp;        // bins*kTaps
    std::vector<std::complex<double>> g;         // bins*kTaps

    std::vector<double> anaX;                    // analysis scratch, n
    std::vector<double> td;                      // synthesis scratch, n
    std::vector<std::complex<double>> spec;      // bins

    // Hoisted FFT descriptors (no per-hop heap)
    std::vector<size_t>    fftShape;
    std::vector<size_t>    fftAxes;
    std::vector<ptrdiff_t> fftStrideIn;
    std::vector<ptrdiff_t> fftStrideOut;

    bool failed = false;                         // latched pass-through
    int speech = 1;                              // near-end talking (tight cap)
};

static void RingReset(WpeHandle* h) {
    std::fill(h->ring.begin(), h->ring.end(), 0.0f);
    h->rHead = 0;
    h->rCount = h->prefill;
}

static void RingPush(WpeHandle* h, float v) {
    if (h->rCount >= h->ringCap) {               // cannot happen; defend
        h->failed = true;
        return;
    }
    h->ring[(h->rHead + h->rCount) % h->ringCap] = v;
    h->rCount++;
}

static void WpeBinInit(WpeHandle* h, int k) {
    std::complex<double>* Rk = h->R.data() + (size_t)k * kTaps * kTaps;
    std::complex<double>* rpk = h->rp.data() + (size_t)k * kTaps;
    std::complex<double>* gk  = h->g.data() + (size_t)k * kTaps;
    for (int i = 0; i < kTaps; i++) {
        for (int j = 0; j < kTaps; j++)
            Rk[i * kTaps + j] = (i == j) ? 1.0 : 0.0;
        rpk[i] = 0.0;
        gk[i]  = 0.0;
    }
}

// Solve (R + load*I) g = rp for one bin: complex Gaussian elimination
// with partial pivoting (kTaps = 5, ~769 bins / 125 Hz — peanuts).
static bool WpeSolve(WpeHandle* h, int k) {
    const std::complex<double>* Rk = h->R.data() + (size_t)k * kTaps * kTaps;
    const std::complex<double>* rpk = h->rp.data() + (size_t)k * kTaps;
    std::complex<double>* gk = h->g.data() + (size_t)k * kTaps;

    std::complex<double> A[kTaps][kTaps];
    std::complex<double> b[kTaps];
    double trace = 0.0;
    for (int i = 0; i < kTaps; i++) {
        trace += Rk[i * kTaps + i].real();
        for (int j = 0; j < kTaps; j++) A[i][j] = Rk[i * kTaps + j];
        b[i] = rpk[i];
    }
    const double load = kLoad * (trace / (double)kTaps);
    if (!(load > 0.0)) return false;              // poisoned state
    for (int i = 0; i < kTaps; i++) A[i][i] += load;

    for (int c = 0; c < kTaps; c++) {
        int piv = c;
        double best = std::abs(A[c][c]);
        for (int r = c + 1; r < kTaps; r++) {
            const double m = std::abs(A[r][c]);
            if (m > best) { best = m; piv = r; }
        }
        if (!(best > 0.0)) return false;
        if (piv != c) {
            for (int j = c; j < kTaps; j++) std::swap(A[c][j], A[piv][j]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < kTaps; r++) {
            const std::complex<double> f = A[r][c] / A[c][c];
            if (f == 0.0) continue;
            for (int j = c; j < kTaps; j++) A[r][j] -= f * A[c][j];
            b[r] -= f * b[c];
        }
    }
    for (int r = kTaps - 1; r >= 0; r--) {
        std::complex<double> s = b[r];
        for (int c = r + 1; c < kTaps; c++) s -= A[r][c] * gk[c];
        const std::complex<double> v = s / A[r][r];
        if (!(std::isfinite(v.real()) && std::isfinite(v.imag())))
            return false;
        gk[r] = v;
    }
    for (int i = 0; i < kTaps; i++)
        if (!(std::isfinite(gk[i].real()) && std::isfinite(gk[i].imag())))
            return false;
    return true;
}

// One hop: analysis -> per-bin weighted LP -> synthesis -> OLA ->
// emit first hop -> consume hop from the FIFO.
static bool WpeRunFrame(WpeHandle* h) {
    const int n = h->n, hop = h->hop, bins = h->bins;
    for (int i = 0; i < n; i++)
        h->anaX[i] = (double)h->q[i] * (double)h->w[i];
    pocketfft::r2c(h->fftShape, h->fftStrideIn, h->fftStrideOut,
                   h->fftAxes, pocketfft::FORWARD,
                   h->anaX.data(), h->spec.data(), 1.0);

    const bool warm = h->histN >= kHist;
    for (int k = 0; k < bins; k++) {
        std::complex<double> y = h->spec[k];
        if (!(std::isfinite(y.real()) && std::isfinite(y.imag())))
            y = std::complex<double>(0.0, 0.0);

        if (!warm) {                       // not enough history: pass through
            h->spec[k] = y;
            h->hist[(size_t)k * kHist] = y;
            continue;
        }

        const std::complex<double>* hk = h->hist.data() + (size_t)k * kHist;
        const std::complex<double>* u = hk + (kDelay - 1);
        double umax2 = 0.0;
        for (int i = 0; i < kTaps; i++) {
            const double m = std::norm(u[i]);
            if (m > umax2) umax2 = m;
        }
        const double ay = std::norm(y);
        double den = ay > umax2 ? ay : umax2;
        if (den < kPowFloor) den = kPowFloor;
        const double w = 1.0 / den;

        // A-priori prediction with the current filter.
        const std::complex<double>* gk = h->g.data() + (size_t)k * kTaps;
        std::complex<double> pred(0.0, 0.0);
        for (int i = 0; i < kTaps; i++) pred += std::conj(gk[i]) * u[i];

        // Voice guards: bounded subtraction, bounded output gain.
        // Cap tightens while the near-end person talks (protects
        // voice level), widens between speech (eat the tails).
        const double cap = h->speech ? kPredCapSpeech : kPredCapOther;
        const double py = std::sqrt(ay);
        const double pm = std::abs(pred);
        if (pm > cap * py) pred *= (cap * py) / pm;
        std::complex<double> z = y - pred;
        const double pz = std::abs(z);
        if (pz > kOutCap * py) z *= (kOutCap * py) / pz;
        h->spec[k] = z;

        // RLS update from THIS frame's observed data, then re-solve.
        std::complex<double>* Rk =
            h->R.data() + (size_t)k * kTaps * kTaps;
        std::complex<double>* rpk = h->rp.data() + (size_t)k * kTaps;
        for (int i = 0; i < kTaps; i++) {
            for (int j = 0; j < kTaps; j++)
                Rk[i * kTaps + j] = kForget * Rk[i * kTaps + j] +
                                    w * u[i] * std::conj(u[j]);
            rpk[i] = kForget * rpk[i] + w * u[i] * std::conj(y);
        }
        if (!WpeSolve(h, k))               // this bin: reset, z stands
            WpeBinInit(h, k);

        // History stores the observed mixture (classic WPE regressors),
        // not the enhanced bin: shift this bin's row, insert y.
        std::complex<double>* hs = h->hist.data() + (size_t)k * kHist;
        memmove(hs + 1, hs, (kHist - 1) * sizeof(std::complex<double>));
        hs[0] = y;
    }
    if (h->histN < kHist) h->histN++;

    // Synthesis: c2r / N * window * 0.5 (hann OLA at hop N/4 = 2).
    pocketfft::c2r(h->fftShape, h->fftStrideOut, h->fftStrideIn,
                   h->fftAxes, pocketfft::BACKWARD,
                   h->spec.data(), h->td.data(), 1.0);
    for (int i = 0; i < n; i++)
        h->ola[i] += (float)(h->td[i] / (double)n) * h->w[i] * 0.5f;

    // Emit completed first hop; every emitted hop is fully covered
    // (the 3*hop zero input pad guarantees it from frame 0 on).
    for (int i = 0; i < hop; i++) RingPush(h, h->ola[i]);
    memmove(h->ola.data(), h->ola.data() + hop,
            (size_t)(n - hop) * sizeof(float));
    std::fill(h->ola.begin() + (n - hop), h->ola.end(), 0.0f);

    // Next frame: slide the input FIFO by one hop.
    memmove(h->q.data(), h->q.data() + hop,
            (size_t)(h->qn - hop) * sizeof(float));
    h->qn -= hop;
    return true;
}

extern "C" {

WpeHandle* WpeNew(int sampleRate) {
    int n = 0;
    if (sampleRate == 16000)       n = 512;    // 32 ms
    else if (sampleRate == 48000)  n = 1536;   // 32 ms (v2.0: 48 kHz)
    else return nullptr;

    auto* h = new (std::nothrow) WpeHandle();
    if (!h) return nullptr;

    h->n = n;
    h->hop = n / 4;
    h->bins = n / 2 + 1;
    h->pad = 3 * h->hop;
    h->prefill = h->hop;
    h->ringCap = 8 * n;
    h->qCap = 4 * n;

    h->w.assign((size_t)n, 0.0f);
    h->q.assign((size_t)h->qCap, 0.0f);
    h->ola.assign((size_t)n, 0.0f);
    h->ring.assign((size_t)h->ringCap, 0.0f);
    h->hist.assign((size_t)h->bins * kHist, std::complex<double>(0.0, 0.0));
    h->R.assign((size_t)h->bins * kTaps * kTaps,
                std::complex<double>(0.0, 0.0));
    h->rp.assign((size_t)h->bins * kTaps, std::complex<double>(0.0, 0.0));
    h->g.assign((size_t)h->bins * kTaps, std::complex<double>(0.0, 0.0));
    h->anaX.assign((size_t)n, 0.0);
    h->td.assign((size_t)n, 0.0);
    h->spec.assign((size_t)h->bins, std::complex<double>(0.0, 0.0));

    h->fftShape.assign(1, (size_t)n);
    h->fftAxes.assign(1, (size_t)0);
    h->fftStrideIn.assign(1, (ptrdiff_t)sizeof(double));
    h->fftStrideOut.assign(1, (ptrdiff_t)sizeof(std::complex<double>));

    // sqrt(periodic hann): analysis * synthesis = hann, hop N/4 OLA = 2
    // (synthesis carries the 0.5).
    for (int i = 0; i < n; i++) {
        const float c = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979323846f *
                                           (float)i / (float)n);
        h->w[i] = sqrtf(c > 0.0f ? c : 0.0f);
    }
    h->qn = h->pad;                              // stream-alignment pad
    for (int k = 0; k < h->bins; k++) WpeBinInit(h, k);
    RingReset(h);
    return h;
}

int WpeProcess(WpeHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    if (h->failed) {                               // latched pass-through
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    if (h->qn + n > h->qCap) {                     // cannot happen; defend
        h->failed = true;
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    memcpy(h->q.data() + h->qn, in, (size_t)n * sizeof(float));
    h->qn += n;
    while (h->qn >= h->n && !h->failed) {
        if (!WpeRunFrame(h)) h->failed = true;
    }
    if (h->failed) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    // Emit exactly n; the ring is prefilled so this never runs dry in
    // practice — pad zeros defensively rather than desync.
    for (int i = 0; i < n; i++) {
        if (h->rCount > 0) {
            out[i] = h->ring[h->rHead];
            h->rHead = (h->rHead + 1) % h->ringCap;
            h->rCount--;
        } else {
            out[i] = 0.0f;
        }
    }
    return 1;
}

void WpeSetSpeech(WpeHandle* h, int speaking) {
    if (!h) return;
    h->speech = speaking ? 1 : 0;
}

void WpeReset(WpeHandle* h) {
    if (!h) return;
    std::fill(h->q.begin(), h->q.end(), 0.0f);
    h->qn = h->pad;
    std::fill(h->ola.begin(), h->ola.end(), 0.0f);
    std::fill(h->hist.begin(), h->hist.end(),
              std::complex<double>(0.0, 0.0));
    h->histN = 0;
    for (int k = 0; k < h->bins; k++) WpeBinInit(h, k);
    RingReset(h);
    h->failed = false;
}

void WpeDestroy(WpeHandle* h) {
    delete h;
}

}  // extern "C"
