// ============================================================
//  WPE wrapper — NKF "Dereverb (WPE)" post-filter (v1.9.0)
//
//  Streaming single-channel Weighted Prediction Error
//  dereverberation: replaces the retired GTCRN "Dry voice" model
//  with a model-free late-reverb canceller (also drops the ONNX
//  file from the release). Pure pocketfft + per-bin recursive
//  weighted least squares — no ONNX, no new dependencies (same
//  FFT header NKF/DTLN already use).
//
//  Algorithm (per bin, 257 bins @ 16 kHz):
//    - STFT n_fft=512, hop=128, sqrt(periodic hann)
//    - regressors u = {Y[n-D-i]}, D = 2 frames, T = 5 taps
//      (history = the OBSERVED mixture, classic WPE)
//    - per-frame weight  w = 1 / max(|Y|², max|u|², floor) —
//      inverse-power cost, every covariance/rhs entry then stays
//      O(1) whatever the level (no slow blow-up, no underflow)
//    - RLS with forgetting lambda=0.995, solve (R + load*I) g = r
//      with relative diagonal loading (1e-2 of mean trace)
//    - output Z = Y - g^H u, hard-bounded: |pred| <= 0.5|Y|
//      (worst case -6 dB on a sustained bin — pure tones are
//      mathematically perfectly predictable, the cap is what
//      keeps voice from being eaten) and |Z| <= 1.5|Y|
//      (never adds gain). Unsolvable bin -> that bin resets and
//      passes through this frame.
//
//  Streaming (1:1, mirrors GTCRN's proven ring/FIFO pattern):
//    - input FIFO pre-padded with 384 zeros; a frame = q[0..512);
//      each frame consumes 128. The pad makes the first three
//      emitted hops pure pre-stream zeros, so the first REAL hop
//      already has full COLA (4 frames, hann sum = 2 at hop N/4,
//      synthesis carries x0.5) — no ramp-in, no edge dropout.
//    - output ring prefilled with 128 zeros: C - emitted <= 127
//      always holds, so pops never run dry (pads defensively).
//    - latency: real sample 0 emerges after 512 consumed = 32 ms,
//      then constant (same ballpark as GTCRN's ~32 ms).
//
//  Fail-open: any internal latch copies input to output until
//  WpeReset — the stage can mute nothing, ever (release theme).
// ============================================================
#include "wpe.h"

#include "pocketfft_hdronly.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr int kNfft    = 512;
constexpr int kHop     = 128;
constexpr int kBins    = kNfft / 2 + 1;          // 257
constexpr int kDelay   = 2;                      // prediction delay (frames)
constexpr int kTaps    = 5;                      // LP taps per bin
constexpr int kHist    = kDelay + kTaps - 1;     // 6 stored spectra
constexpr int kPad     = 384;                    // input prefill (COLA align)
constexpr int kPrefill = 128;                    // output priming (1:1)
constexpr int kRingCap = 4096;                   // >> steady-state ~256
constexpr int kQCap    = 2048;                   // >= 511 + max call (512)

constexpr double kForget   = 0.995;              // RLS forgetting
constexpr double kLoad     = 0.01;               // rel. diagonal loading
constexpr double kPowFloor = 1e-6;               // weight floor (unit scale)
constexpr double kPredCap  = 0.5;                // |pred| <= 0.5|Y|
constexpr double kOutCap   = 1.5;                // |Z|    <= 1.5|Y|

}  // namespace

struct WpeHandle {
    float w[kNfft];                     // sqrt(periodic hann)
    float q[kQCap];                     // input awaiting frame cut (padded)
    int qn = 0;
    float ola[kNfft];                   // pending overlap-add region

    float ring[kRingCap];               // output FIFO (prefilled)
    int rHead = 0, rCount = 0;

    // Observed-mixture history, per bin: [bin][0] = previous frame.
    std::complex<double> hist[kBins][kHist];
    int histN = 0;                      // valid frames, saturates at kHist

    // Per-bin RLS state (double: complex 5x5 solves need the headroom).
    std::complex<double> R[kBins][kTaps][kTaps];
    std::complex<double> rp[kBins][kTaps];
    std::complex<double> g[kBins][kTaps];

    double anaX[kNfft];                 // analysis scratch (double)
    double td[kNfft];                   // synthesis time-domain scratch
    std::complex<double> spec[kBins];

    // Hoisted FFT descriptors (no per-hop heap)
    std::vector<size_t>    fftShape;
    std::vector<size_t>    fftAxes;
    std::vector<ptrdiff_t> fftStrideIn;
    std::vector<ptrdiff_t> fftStrideOut;

    bool failed = false;                // latched pass-through
};

static void RingReset(WpeHandle* h) {
    memset(h->ring, 0, sizeof(h->ring));
    h->rHead = 0;
    h->rCount = kPrefill;
}

static void RingPush(WpeHandle* h, float v) {
    if (h->rCount >= kRingCap) {        // cannot happen; defend
        h->failed = true;
        return;
    }
    h->ring[(h->rHead + h->rCount) % kRingCap] = v;
    h->rCount++;
}

static void WpeBinInit(WpeHandle* h, int k) {
    for (int i = 0; i < kTaps; i++) {
        for (int j = 0; j < kTaps; j++)
            h->R[k][i][j] = (i == j) ? 1.0 : 0.0;
        h->rp[k][i] = 0.0;
        h->g[k][i]  = 0.0;
    }
}

// Solve (R + load*I) g = rp for one bin: complex Gaussian elimination
// with partial pivoting (kTaps = 5, ~257 bins x 125 Hz — peanuts).
static bool WpeSolve(WpeHandle* h, int k) {
    std::complex<double> A[kTaps][kTaps];
    std::complex<double> b[kTaps];
    double trace = 0.0;
    for (int i = 0; i < kTaps; i++) {
        trace += h->R[k][i][i].real();
        for (int j = 0; j < kTaps; j++) A[i][j] = h->R[k][i][j];
        b[i] = h->rp[k][i];
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
        for (int c = r + 1; c < kTaps; c++) s -= A[r][c] * h->g[k][c];
        const std::complex<double> v = s / A[r][r];
        if (!(std::isfinite(v.real()) && std::isfinite(v.imag())))
            return false;
        h->g[k][r] = v;
    }
    for (int i = 0; i < kTaps; i++)
        if (!(std::isfinite(h->g[k][i].real()) &&
              std::isfinite(h->g[k][i].imag())))
            return false;
    return true;
}

// One hop: analysis -> per-bin weighted LP -> synthesis -> OLA ->
// emit first hop -> consume hop from the FIFO.
static bool WpeRunFrame(WpeHandle* h) {
    for (int i = 0; i < kNfft; i++)
        h->anaX[i] = (double)h->q[i] * (double)h->w[i];
    pocketfft::r2c(h->fftShape, h->fftStrideIn, h->fftStrideOut,
                   h->fftAxes, pocketfft::FORWARD,
                   h->anaX, h->spec, 1.0);

    const bool warm = h->histN >= kHist;
    for (int k = 0; k < kBins; k++) {
        std::complex<double> y = h->spec[k];
        if (!(std::isfinite(y.real()) && std::isfinite(y.imag())))
            y = std::complex<double>(0.0, 0.0);

        if (!warm) {                       // not enough history: pass through
            h->spec[k] = y;
            h->hist[k][0] = y;
            continue;
        }

        const std::complex<double>* u = h->hist[k] + (kDelay - 1);
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
        std::complex<double> pred(0.0, 0.0);
        for (int i = 0; i < kTaps; i++) pred += std::conj(h->g[k][i]) * u[i];

        // Voice guards: bounded subtraction, bounded output gain.
        const double py = std::sqrt(ay);
        const double pm = std::abs(pred);
        if (pm > kPredCap * py) pred *= (kPredCap * py) / pm;
        std::complex<double> z = y - pred;
        const double pz = std::abs(z);
        if (pz > kOutCap * py) z *= (kOutCap * py) / pz;
        h->spec[k] = z;

        // RLS update from THIS frame's observed data, then re-solve.
        for (int i = 0; i < kTaps; i++) {
            for (int j = 0; j < kTaps; j++)
                h->R[k][i][j] = kForget * h->R[k][i][j] +
                                w * u[i] * std::conj(u[j]);
            h->rp[k][i] = kForget * h->rp[k][i] + w * u[i] * std::conj(y);
        }
        if (!WpeSolve(h, k))               // this bin: reset, z stands
            WpeBinInit(h, k);

        // History stores the observed mixture (classic WPE regressors),
        // not the enhanced bin: shift this bin's row, insert y.
        memmove(h->hist[k] + 1, h->hist[k],
                (kHist - 1) * sizeof(std::complex<double>));
        h->hist[k][0] = y;
    }
    if (h->histN < kHist) h->histN++;

    // Synthesis: c2r / N * window * 0.5 (hann OLA at hop N/4 = 2).
    pocketfft::c2r(h->fftShape, h->fftStrideOut, h->fftStrideIn,
                   h->fftAxes, pocketfft::BACKWARD,
                   h->spec, h->td, 1.0);
    for (int i = 0; i < kNfft; i++)
        h->ola[i] += (float)(h->td[i] / (double)kNfft) * h->w[i] * 0.5f;

    // Emit completed first hop; every emitted hop is fully covered
    // (the 384-zero input pad guarantees it from frame 0 on).
    for (int i = 0; i < kHop; i++) RingPush(h, h->ola[i]);
    memmove(h->ola, h->ola + kHop, (kNfft - kHop) * sizeof(float));
    memset(h->ola + (kNfft - kHop), 0, kHop * sizeof(float));

    // Next frame: slide the input FIFO by one hop.
    memmove(h->q, h->q + kHop, (size_t)(h->qn - kHop) * sizeof(float));
    h->qn -= kHop;
    return true;
}

extern "C" {

WpeHandle* WpeNew(int sampleRate) {
    if (sampleRate != 16000) return nullptr;     // NKF runs 16 kHz fixed
    auto* h = new (std::nothrow) WpeHandle();
    if (!h) return nullptr;

    h->fftShape.assign(1, (size_t)kNfft);
    h->fftAxes.assign(1, (size_t)0);
    h->fftStrideIn.assign(1, (ptrdiff_t)sizeof(double));
    h->fftStrideOut.assign(1, (ptrdiff_t)sizeof(std::complex<double>));

    // sqrt(periodic hann): analysis * synthesis = hann, hop N/4 OLA = 2
    // (synthesis carries the 0.5).
    for (int i = 0; i < kNfft; i++) {
        const float c = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979323846f *
                                           (float)i / (float)kNfft);
        h->w[i] = sqrtf(c > 0.0f ? c : 0.0f);
    }
    memset(h->q, 0, sizeof(h->q));
    h->qn = kPad;                                  // stream-alignment pad
    memset(h->hist, 0, sizeof(h->hist));
    for (int k = 0; k < kBins; k++) WpeBinInit(h, k);
    memset(h->ola, 0, sizeof(h->ola));
    RingReset(h);
    return h;
}

int WpeProcess(WpeHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    if (h->failed) {                               // latched pass-through
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    if (h->qn + n > kQCap) {                       // cannot happen; defend
        h->failed = true;
        memcpy(out, in, (size_t)n * sizeof(float));
        return 1;
    }
    memcpy(h->q + h->qn, in, (size_t)n * sizeof(float));
    h->qn += n;
    while (h->qn >= kNfft && !h->failed) {
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
            h->rHead = (h->rHead + 1) % kRingCap;
            h->rCount--;
        } else {
            out[i] = 0.0f;
        }
    }
    return 1;
}

void WpeReset(WpeHandle* h) {
    if (!h) return;
    memset(h->q, 0, sizeof(h->q));
    h->qn = kPad;
    memset(h->ola, 0, sizeof(h->ola));
    memset(h->hist, 0, sizeof(h->hist));
    h->histN = 0;
    for (int k = 0; k < kBins; k++) WpeBinInit(h, k);
    RingReset(h);
    h->failed = false;
}

void WpeDestroy(WpeHandle* h) {
    delete h;
}

}  // extern "C"
