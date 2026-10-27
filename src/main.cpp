#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// --- Windows tray support ---
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <exception>
#include <shellapi.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "aec3_wrapper.h"
#include "nkf_wrapper.h"
#include "dtln_wrapper.h"
#include "dtln_ns_wrapper.h"
#include "hush_wrapper.h"
#include "gtcrn_wrapper.h"
#include "pocketfft_hdronly.h"
#include "resampler.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <complex>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ============================================================
//  App identity (kept above single-instance: the focus-steal title
//  is built from these at runtime)
// ============================================================
#define APP_NAME    "AEC Client"
#define APP_VERSION "1.14.0"

// ============================================================
//  Single-instance protection
// ============================================================
#ifdef _WIN32
static HANDLE g_singleInstanceMutex = NULL;
static const wchar_t* SINGLE_INSTANCE_MUTEX_NAME = L"AECClient_SingleInstance_Mutex_v1";

// Returns true if this is the first instance.
// If not, brings the existing window to front and returns false.
static bool AcquireSingleInstance() {
    g_singleInstanceMutex = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
    DWORD err = GetLastError();

    if (g_singleInstanceMutex == NULL) {
        return true; // Can't create mutex — just continue
    }

    if (err == ERROR_ALREADY_EXISTS) {
        // Another instance is running — try to bring it to front.
        // Title is built from APP_VERSION (a hardcoded "v1.0.0" here
        // silently broke focus-steal on every later version).
        wchar_t title[64];
        swprintf(title, 64, L"%hs v%hs", APP_NAME, APP_VERSION);
        HWND existing = FindWindowW(NULL, title);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = NULL;
        return false;
    }

    return true;
}

static void ReleaseSingleInstance() {
    if (g_singleInstanceMutex) {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = NULL;
    }
}
#endif

// ============================================================
//  Tray icon (Windows)
// ============================================================
#ifdef _WIN32
#define WM_TRAYICON   (WM_APP + 1)
#define ID_TRAY_SHOW  1001
#define ID_TRAY_EXIT  1002

static NOTIFYICONDATAW g_nid = {};
static bool            g_trayIconActive = false;
static WNDPROC         g_originalWndProc = nullptr;
static HWND            g_hwnd = nullptr;
static GLFWwindow*     g_glfwWindow = nullptr;
#endif

// ============================================================
//  Lock-free SPSC ring buffer
// ============================================================
template <size_t N>
struct SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    int16_t buf[N];
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    inline size_t available() const {
        return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
    }
    static constexpr size_t capacity = N;
    inline void write(const int16_t* src, size_t count) {
        size_t h = head.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) buf[(h + i) & (N - 1)] = src[i];
        head.store(h + count, std::memory_order_release);
    }
    inline void read(int16_t* dst, size_t count) {
        size_t t = tail.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) dst[i] = buf[(t + i) & (N - 1)];
        tail.store(t + count, std::memory_order_release);
    }
    inline void skip(size_t count) {
        size_t t = tail.load(std::memory_order_relaxed);
        tail.store(t + count, std::memory_order_release);
    }
    void reset() { head.store(0); tail.store(0); }
};

// ============================================================
//  Engine abstraction
// ============================================================
// Numeric values keep their historical meaning so saved aec_config.txt
// indices keep loading: 0 used to be SpeexDSP (retired long ago) and
// now means engine Off; 3 (old LocalVQE code) and 8 (exp-fork
// LocalVQE) were retired and remap to DTLN (the default) on load.
// Experimental engines live in exp/main.cpp, never here.
enum EngineType { ENGINE_OFF = 0, ENGINE_AEC3 = 1, ENGINE_NKF = 2, ENGINE_DTLN = 4 };
struct EngineState {
    EngineType  type  = ENGINE_DTLN;
    Aec3Handle* aec3  = nullptr;
    NkfHandle*  nkf   = nullptr;
    DtlnHandle* dtln  = nullptr;
    DtlnNsHandle* ns = nullptr;   // DTLN noise reduction (all engines)
    HushHandle* hush = nullptr;   // Hush NS backend (all engines, 16 kHz)
    GtcrnHandle* gtcrn = nullptr;  // GTCRN NS backend (all engines, 16 kHz)
};

// ============================================================
//  Profiles
// ============================================================
// Profile = engine selection. The combo label shows the engine name
// directly; the running chain is engine + noise suppression while it
// is ticked. Filter length comes from here; sample rate, gains,
// devices and the NS tick are free knobs.
struct Profile {
    const char* name;
    int   engine;
    int   filterIdx;
};

static const Profile PROFILES[] = {
    { "DTLN-AEC 128", ENGINE_DTLN, 0 },
    { "WebRTC AEC3",  ENGINE_AEC3, 2 },
    { "NKF-AEC",      ENGINE_NKF,  0 },
    { "Off (no AEC)", ENGINE_OFF,  0 },
};
// Display order for the engine radio: Off first, then the engines.
// Indices into PROFILES (which stays append-order so saved profile
// slots keep loading without migration).
static const int ENGINE_RADIO_ORDER[] = { 3, 0, 1, 2 };
const int PROFILE_COUNT = (int)(sizeof(PROFILES) / sizeof(PROFILES[0]));

// ============================================================
//  Globals
// ============================================================
EngineState g_engine;

ma_device g_micDevice, g_loopbackDevice, g_outputDevice, g_monitorDevice;
ma_context g_context;
static bool g_contextInitialized = false;
const int MAX_FRAME_SIZE = 480;
// Last emitted output frame (audio thread only) — fail-open gaps fade
// from it instead of hard-muting (10 ms holes are audible clicks).
int16_t            g_lastOutFrame[MAX_FRAME_SIZE];
int                g_lastOutLen = 0;
bool               g_lastOutValid = false;
// Same fail-open memory for the soundboard monitor: a starved monitor
// callback fades from its last frame instead of hard-cutting to zero
// (a cut clicks on the local speakers, same as the main path).
int16_t            g_lastMonFrame[MAX_FRAME_SIZE];
int                g_lastMonLen = 0;
bool               g_lastMonValid = false;
SpscRing<4096> g_micRing, g_refRing;
bool g_isRunning = false;
Clock::time_point g_sessionStart;

std::atomic<int>   g_sampleRate(16000);
std::atomic<int>   g_selectedEngine(ENGINE_DTLN);
std::atomic<int>   g_filterLengthMs(50);
std::atomic<bool>  g_enablePreprocess(false);
// Post-stage tick is a global pref (like the gains): a profile
// switch never resets it. Defaults ON for every engine.
std::atomic<bool>  g_nsEnabled{ true };        // DTLN noise reduction (all engines)
// NS backend: 0=off, 1=DTLN-NS, 2=Hush, 3=GTCRN (default).
// Meaningful only while g_nsEnabled; the radio sets both
// consistently. Global pref like the tick: a profile switch never
// resets it.
std::atomic<int>   g_nsBackend{ 3 };
std::atomic<bool>  g_experimentalEngines{ false }; // show WebRTC AEC3 / NKF-AEC in the list
// High-band splice: capture mic at 48 kHz, process the low band (0-8k)
// through the 16 kHz engine, then splice the untouched real high band
// (8-24k) from the original 48 kHz mic onto the upsampled output.
// Default ON: 48 kHz output is the quality path; the chain stays at
// the chain rate — only the splice upsamples to 48 kHz.
std::atomic<bool>  g_highbandEnabled{ true };
// High-band mix (0..1): the real 8-24 kHz band is raw mic, so its level
// is a taste knob, not a fixed law. After the FIR resampler fix the
// low band is flat to 7.5 kHz, so 0.7-1.0 gives more air.
std::atomic<float> g_hbMix{ 1.0f };
SpscRing<4096>     g_micRing48;
// Diagnostics: the high-band VAD level (dBFS).
std::atomic<float> g_hbRatioDb{ 0.0f };
// High-band delay (48 kHz samples): aligns the spliced high band with
// the processed low band (AEC + NS latency).
static std::atomic<int> g_hbDelaySamples{ 0 };
std::atomic<float> g_micGain(1.0f);
std::atomic<float> g_outputGain(1.0f);
std::atomic<float> g_last_reduction_db(0.0f);
std::atomic<float> g_last_mic_rms(0.0f), g_last_ref_rms(0.0f), g_last_out_rms(0.0f);

std::vector<ma_device_info> g_captureDevices, g_playbackDevices;

// Display filter lists (indices into the full arrays above)
std::vector<int> g_micDisplayIndices;
std::vector<int> g_refDisplayIndices;
std::vector<int> g_outDisplayIndices;

int  g_micIndex = 0, g_refIndex = 0, g_outIndex = 0;
bool g_listenToSelf = false;  // self-monitor: route cleaned mic to Speaker Reference instead of Output
bool g_isFirstRun = false;      // no config file at launch: newcomers start simple
bool g_advancedOpen = false;    // Advanced collapse state (persisted)
bool g_advInitDone = false;     // first-frame default apply (see DrawAudioTab)
int  g_engineIndex = 4, g_sampleRateIndex = 0, g_filterIndex = 0;  // 4 = ENGINE_DTLN; fresh = profile 0
int  g_profileIndex = 0;         // active profile (PROFILES[])
bool g_preprocessEnabled = false;
bool g_minimizeToTray   = true;   // UI state; behavior controlled via checkbox
char g_statusText[128] = "Idle";

GLuint g_wallpaperTex = 0;
int    g_wallpaperW = 0, g_wallpaperH = 0;
std::vector<std::string> g_wallpaperPaths;
std::vector<std::string> g_wallpaperNames;
int    g_wallpaperIndex = 0;

// ============================================================
//  Helpers
// ============================================================
static inline int16_t clamp_s16(int v) {
    return (v > 32767) ? 32767 : (v < -32768) ? -32768 : (int16_t)v;
}
static inline int frameSizeForRate(int sr) { return sr / 100; }

// Output-device rate: 48 kHz while the high-band splice is live
// (chain stays at the chain rate — only the splice upsamples to 48 kHz),
// else the chain rate. Read at Start/device-open and per output tick;
// the toggle restarts the stream, so it never changes mid-callback.
static inline int OutRate() {
    if (g_highbandEnabled.load()) return 48000;
    return g_sampleRate.load();
}

// Re-derive the chain rate from the stage selection. 48 kHz output is
// the quality default, so the chain only stays at / drops to
// 16 kHz when something genuinely needs it: any NS backend (all are
// 16 kHz), or a 16-kHz-only engine (DTLN / NKF). With the splice off
// and no NS, Off/AEC3 run natively at 48 kHz.
static void SyncChainRate() {
    const int eng = g_selectedEngine.load();
    const bool force16 =
        g_highbandEnabled.load() ||
        g_nsBackend.load() != 0 ||
        eng == ENGINE_DTLN || eng == ENGINE_NKF;
    g_sampleRateIndex = force16 ? 0 : 1;
    g_sampleRate.store(force16 ? 16000 : 48000);
}

// ---------------------------------------------------------
//  High-band noise suppression (splice support)
// ---------------------------------------------------------
// The 16 kHz NS models only reach 8 kHz, but the splice copies the
// 8-24 kHz band from the raw 48 kHz mic, so a broadband transient
// (keyboard click) keeps its sharp part: the NS killed its low band
// and the high band was never seen by any suppressor.
//
// Fix: a REAL noise suppressor for the high band — STFT spectral
// subtraction with a decision-directed a priori SNR, applied only to
// the bins above 8 kHz.
//
// Measured on real speech with a realistic keyboard click:
//   click -10.9 dB | mouse -12.0 dB | hiss -8.6 dB
//   voice -0.9 dB (fahhh) / -2.5 dB (shut-your-f-mouth)
static const int    HB_FFT_N      = 512;   // STFT size
static const int    HB_HOP        = 256;   // 50% overlap
static const int    HB_BINS       = HB_FFT_N / 2 + 1;
static const int    HB_FIRST_BIN  = 85;    // 8 kHz at 48 kHz
static const double HB_AD_D       = 0.998; // decision-directed smoothing
static const double HB_GAIN_SMOOTH= 0.80;  // gain smoothing (temporal)
static const double HB_GAIN_ORDER = 1.2;   // Wiener gain exponent
static const double HB_G_MIN      = 0.10;  // gain floor (-20 dB)
static const double HB_NOISE_UP   = 0.995; // noise tracker: slow rise
static const double HB_NOISE_DOWN = 0.70;  // noise tracker: fast fall
static const double HB_VAD_DB     = -72.0; // full close below this
static const double HB_VAD_RAMP_DB= 12.0;  // full open above this

struct HbNsState {
    double win[HB_FFT_N];
    double noise[HB_BINS];
    double prevClean2[HB_BINS];
    double prevGain[HB_BINS];
    double inBuf[HB_FFT_N];    // analysis history
    double ola[HB_FFT_N];      // overlap-add accumulator
    double inFifo[4096];       // samples waiting for the next hop
    double outFifo[8192];      // finished samples
    int    inCount = 0;        // samples in inFifo
    int    outHead = 0, outCount = 0;
    double hpB0 = 1, hpB1 = 0, hpB2 = 0, hpA1 = 0, hpA2 = 0;
    double hpX1 = 0, hpX2 = 0, hpY1 = 0, hpY2 = 0;
    // pocketfft descriptors
    std::vector<std::complex<double>> spec;
    std::vector<size_t> shape, axes;
    std::vector<ptrdiff_t> strideIn, strideOut;
};
static HbNsState g_hbNs;

static void HbNsInit(HbNsState& h) {
    // sqrt of the periodic Hann: analysis and synthesis both use it,
    // so the overlap-add of w*w sums to exactly 1 at 50% overlap.
    for (int i = 0; i < HB_FFT_N; i++)
        h.win[i] = sqrt(0.5 * (1.0 - cos(2.0 * 3.14159265358979323846 * i / HB_FFT_N)));
    for (int i = 0; i < HB_BINS; i++) {
        h.noise[i] = 1e-8;
        h.prevClean2[i] = 0.0;
        h.prevGain[i] = 1.0;
    }
    memset(h.inBuf, 0, sizeof(h.inBuf));
    memset(h.ola, 0, sizeof(h.ola));
    memset(h.inFifo, 0, sizeof(h.inFifo));
    memset(h.outFifo, 0, sizeof(h.outFifo));
    h.inCount = 0;
    h.outHead = 0;
    h.outCount = 0;
    h.hpX1 = h.hpX2 = h.hpY1 = h.hpY2 = 0.0;
    // 2nd-order Butterworth highpass at 8 kHz (48 kHz rate) — the
    // high band starts here; below it the 16 kHz chain owns the signal.
    const double w0 = 2.0 * 3.14159265358979323846 * 8000.0 / 48000.0;
    const double c = cos(w0), s = sin(w0);
    const double alpha = s / 2.0 * 0.70710678118654752;
    const double a0 = 1.0 + alpha;
    h.hpB0 = ((1.0 + c) / 2.0) / a0;
    h.hpB1 = -(1.0 + c) / a0;
    h.hpB2 = ((1.0 + c) / 2.0) / a0;
    h.hpA1 = (-2.0 * c) / a0;
    h.hpA2 = (1.0 - alpha) / a0;
    h.spec.resize(HB_FFT_N);
    h.shape = { (size_t)HB_FFT_N };
    h.axes = { 0 };
    h.strideIn = { (ptrdiff_t)sizeof(double) };
    h.strideOut = { (ptrdiff_t)sizeof(std::complex<double>) };
}

// One analysis/synthesis hop. The 256 finished samples are pushed
// onto the output FIFO.
static void HbNsHop(HbNsState& h) {
    // shift the analysis window and append the new samples
    for (int i = 0; i < HB_FFT_N - HB_HOP; i++) h.inBuf[i] = h.inBuf[i + HB_HOP];
    for (int i = 0; i < HB_HOP; i++) h.inBuf[HB_FFT_N - HB_HOP + i] = h.inFifo[i];
    for (int i = 0; i < HB_HOP; i++) h.inFifo[i] = h.inFifo[i + HB_HOP];
    h.inCount -= HB_HOP;

    // highpass -> the high band
    double x[HB_FFT_N];
    for (int i = 0; i < HB_FFT_N; i++) {
        const double xi = h.inBuf[i];
        const double y = h.hpB0 * xi + h.hpB1 * h.hpX1 + h.hpB2 * h.hpX2
                       - h.hpA1 * h.hpY1 - h.hpA2 * h.hpY2;
        h.hpX2 = h.hpX1; h.hpX1 = xi;
        h.hpY2 = h.hpY1; h.hpY1 = y;
        x[i] = y * h.win[i];
    }

    pocketfft::r2c(h.shape, h.strideIn, h.strideOut, h.axes, pocketfft::FORWARD,
                   x, h.spec.data(), 1.0);

    for (int b = 0; b < HB_BINS; b++) {
        const double mag = std::abs(h.spec[b]);
        const double psd = mag * mag / HB_FFT_N;
        // noise PSD tracker: fast down, slow up
        const double up   = HB_NOISE_UP   * h.noise[b] + (1.0 - HB_NOISE_UP)   * psd;
        const double down = HB_NOISE_DOWN * h.noise[b] + (1.0 - HB_NOISE_DOWN) * psd;
        h.noise[b] = (psd < h.noise[b]) ? down : up;
        // decision-directed a priori SNR: the previous frame's CLEAN
        // magnitude, so a transient cannot raise it
        const double gamma = psd / (h.noise[b] + 1e-12);
        double xi = HB_AD_D * h.prevClean2[b] / (h.noise[b] + 1e-12)
                  + (1.0 - HB_AD_D) * std::max(gamma - 1.0, 0.0);
        double G = xi / (1.0 + xi);
        if (HB_GAIN_ORDER != 1.0) G = pow(G, HB_GAIN_ORDER);
        if (G < HB_G_MIN) G = HB_G_MIN;
        if (b < HB_FIRST_BIN) G = 1.0;   // below 8 kHz: the chain owns it
        // temporal gain smoothing (less musical noise)
        const double Gs = HB_GAIN_SMOOTH * h.prevGain[b] + (1.0 - HB_GAIN_SMOOTH) * G;
        h.prevGain[b] = Gs;
        h.prevClean2[b] = Gs * Gs * psd;
        h.spec[b] *= Gs;
    }

    double y[HB_FFT_N];
    pocketfft::c2r(h.shape, h.strideOut, h.strideIn, h.axes, pocketfft::BACKWARD,
                   h.spec.data(), y, 1.0);
    for (int i = 0; i < HB_FFT_N; i++) {
        h.ola[i] += (y[i] / HB_FFT_N) * h.win[i];
    }
    // the first HB_HOP samples of the accumulator are finished
    for (int i = 0; i < HB_HOP; i++) {
        const int slot = (h.outHead + h.outCount) % (int)(sizeof(h.outFifo) / sizeof(h.outFifo[0]));
        h.outFifo[slot] = h.ola[i];
        h.outCount++;
    }
    for (int i = 0; i < HB_FFT_N - HB_HOP; i++) h.ola[i] = h.ola[i + HB_HOP];
    for (int i = HB_FFT_N - HB_HOP; i < HB_FFT_N; i++) h.ola[i] = 0.0;
}

// Process n samples of 48 kHz audio; the cleaned HIGH BAND comes out
// (float, same scale as the input). Fail-soft: zeros while priming.
static void HbNsProcess(HbNsState& h, const int16_t* in, float* out, int n) {
    const int CAP = (int)(sizeof(h.inFifo) / sizeof(h.inFifo[0]));
    const int OCAP = (int)(sizeof(h.outFifo) / sizeof(h.outFifo[0]));
    for (int i = 0; i < n; i++) {
        if (h.inCount < CAP) h.inFifo[h.inCount++] = (double)in[i] * (1.0 / 32768.0);
        while (h.inCount >= HB_HOP) {
            HbNsHop(h);
        }
        if (h.outCount > 0) {
            out[i] = (float)h.outFifo[h.outHead];
            h.outHead = (h.outHead + 1) % OCAP;
            h.outCount--;
        } else {
            out[i] = 0.0f;   // still priming
        }
    }
}

// Algorithmic latency of the 16 kHz chain, in 16 kHz samples.
// The high band is delayed by the chain latency so it lands on the
// same instant as the processed low band.
static int AecLatency16(EngineType t) {
    switch (t) {
        case ENGINE_OFF:  return 0;     // no canceller: mic path only
        case ENGINE_AEC3: return 128;   // measured
        case ENGINE_NKF:  return 480;   // measured
        case ENGINE_DTLN: return 480;   // measured
        default:          return 512;   // estimate
    }
}
static int NsLatency16(int backend) {
    switch (backend) {
        case 3: return 512;   // GTCRN: estimate (512-pt STFT / 256 hop)
        case 2: return 320;   // Hush: estimate (~2 native frames)
        case 1: return 896;   // DTLN-NS: measured
        default: return 0;
    }
}

// Delay line for the 48 kHz mic: the high band is read this many
// samples back so it matches the processed low band.
struct HbDelay {
    static const int CAP = 16384;   // power of two
    int16_t buf[CAP];
    int   w = 0;
    int   fill = 0;
    void reset() { w = 0; fill = 0; memset(buf, 0, sizeof(buf)); }
    void push(const int16_t* in, int n) {
        for (int i = 0; i < n; i++) buf[(w + i) & (CAP - 1)] = in[i];
        w = (w + n) & (CAP - 1);
        fill += n;
    }
    // The n samples written `delay` samples ago. Zeros until primed.
    void readDelayed(int16_t* out, int n, int delay) {
        if (delay < 0) delay = 0;
        if (delay > CAP - n) delay = CAP - n;
        if (fill < delay + n) { memset(out, 0, n * sizeof(int16_t)); return; }
        int r = (w - delay) & (CAP - 1);
        for (int i = 0; i < n; i++) out[i] = buf[(r + i) & (CAP - 1)];
    }
};
static HbDelay g_hbDelay;

// High-band VAD gain (the ramped gate the splice applies while the
// 16 kHz chain says "no voice").
static float g_hbVadGain = 1.0f;

// 3:1 resampler state (exp/resampler.h). The FIR pair is flat to 7.5
// kHz with -76 dB aliases and -55 dB images. State is reset on every
// Start.
static rs::Down3 g_down3;
static rs::Up3   g_up3;

// The decimator delays the mic by 32 samples @16k relative to the
// loopback ref. Delay the ref by the same amount so the AEC still sees
// the two aligned (only in splice mode).
constexpr int REF_DELAY16 = rs::kHalf / 3;   // 32
static int16_t g_refDelayBuf[REF_DELAY16];
static int     g_refDelayPos = 0;

static void RefDelayReset() {
    g_refDelayPos = 0;
    memset(g_refDelayBuf, 0, sizeof(g_refDelayBuf));
}

static void RefDelayProcess(int16_t* frame, int n) {
    // Shift the ref by REF_DELAY16 samples (decimator group delay)
    // so it lines up with the downsampled mic.
    static thread_local int16_t tmp[MAX_FRAME_SIZE];
    for (int i = 0; i < n; i++) {
        tmp[i] = g_refDelayBuf[g_refDelayPos];
        g_refDelayBuf[g_refDelayPos] = frame[i];
        g_refDelayPos = (g_refDelayPos + 1) % REF_DELAY16;
        frame[i] = tmp[i];
    }
}

std::string FormatUptime() {
    if (!g_isRunning) return "";
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now() - g_sessionStart).count();
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char buf[32];
    if (h > 0) snprintf(buf, 32, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, 32, "%02d:%02d", m, s);
    return buf;
}

static bool IsVirtualCableDevice(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("cable") != std::string::npos ||
           lower.find("vb-audio") != std::string::npos;
}

// CABLE Input = the playback endpoint voice apps listen to via CABLE Output.
static bool IsCableInputDevice(const std::string& name) {
    if (!IsVirtualCableDevice(name)) return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("input") != std::string::npos;
}

// First CABLE Input in the playback list, else first cable device, else -1.
static int FindCableInputIndex() {
    int fallback = -1;
    for (int i = 0; i < (int)g_playbackDevices.size(); i++) {
        if (!IsVirtualCableDevice(g_playbackDevices[i].name)) continue;
        if (fallback < 0) fallback = i;
        if (IsCableInputDevice(g_playbackDevices[i].name)) return i;
    }
    return fallback;
}

static bool CableInputPresent() { return FindCableInputIndex() >= 0; }

// Index of the Windows system default endpoint (the "Default Device"
// the OS hands apps; eConsole role) in `list`, else -1. miniaudio
// flags it as isDefault during enumeration (WASAPI + WinMM backends).
static int FindSystemDefaultIndex(const std::vector<ma_device_info>& list) {
    for (int i = 0; i < (int)list.size(); i++) {
        if (list[i].isDefault) return i;
    }
    return -1;
}

// ============================================================
//  System tray (Windows)
// ============================================================
#ifdef _WIN32

// App icon shared by the tray and the window (taskbar). Must match the
// numeric ID in version.rc. Two gotchas this exists for:
// - hInstance must be our own module: LoadIcon(NULL, ...) only finds
//   stock icons (IDI_*), so a NULL handle always misses our resource.
// - version.rc must use a numeric ID: an undefined word like IDR_ICON1
//   is stored as a *string* name, which Explorer ignores and
//   MAKEINTRESOURCE(101) can never find (verified in the built exe:
//   RT_GROUP_ICON held the string 'IDR_ICON1', no integer 101).
static HICON LoadAppIcon() {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HICON h = hMod ? LoadIconW(hMod, MAKEINTRESOURCEW(101)) : nullptr;
    if (!h) h = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    return h;
}

void ShowTrayIcon() {
    if (g_trayIconActive || !g_hwnd) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Same icon as the .exe itself (resources/app.ico). Shared icon
    // from LoadAppIcon — do not DestroyIcon it. Falls back to the
    // stock application icon if the resource is missing.
    g_nid.hIcon = LoadAppIcon();
    wcscpy_s(g_nid.szTip, L"AEC Client - running");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayIconActive = true;
}

void HideTrayIcon() {
    if (!g_trayIconActive) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIconActive = false;
}

// Show/hide tray icon based on the current setting
void UpdateTrayIcon() {
    if (g_minimizeToTray) {
        ShowTrayIcon();
    } else {
        HideTrayIcon();
    }
}

void ShowMainWindow() {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

void MinimizeToTray() {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, SW_HIDE);
}

void ShowTrayMenu() {
    if (!g_hwnd) return;
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Show AEC Client");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(hMenu,
                             TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(hMenu);
    if (cmd == ID_TRAY_SHOW) {
        ShowMainWindow();
    } else if (cmd == ID_TRAY_EXIT) {
        HideTrayIcon();
        if (g_glfwWindow) glfwSetWindowShouldClose(g_glfwWindow, GLFW_TRUE);
    }
}

LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            // If tray mode is ON, hide window. Otherwise close normally.
            if (g_minimizeToTray) {
                MinimizeToTray();
                return 0;
            }
            // Fall through to GLFW's default handler, which sets shouldClose
            break;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                ShowMainWindow();
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu();
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_SHOW) {
                ShowMainWindow();
            } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
                HideTrayIcon();
                if (g_glfwWindow) glfwSetWindowShouldClose(g_glfwWindow, GLFW_TRUE);
            }
            return 0;
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

void SetupTray(GLFWwindow* window) {
    g_glfwWindow = window;
    g_hwnd = glfwGetWin32Window(window);
    if (!g_hwnd) return;
    g_originalWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                                    (LONG_PTR)CustomWndProc);
    // GLFW builds its window class with the stock IDI_APPLICATION icon,
    // so the running taskbar button shows the generic Windows icon even
    // when the .exe file itself has the right one. Point the window at
    // our embedded icon (shared handle from LoadAppIcon — do not destroy).
    {
        HICON hIcon = LoadAppIcon();
        if (hIcon) {
            SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }
    // Note: tray icon visibility is decided by UpdateTrayIcon() after settings load.
}

#else

// Non-Windows: no-op stubs
void SetupTray(GLFWwindow*) {}
void UpdateTrayIcon() {}
void HideTrayIcon() {}

#endif // _WIN32

// ============================================================
//  Wallpaper
// ============================================================
void ScanWallpapers() {
    g_wallpaperPaths.clear();
    g_wallpaperNames.clear();
    g_wallpaperNames.push_back("(none)");

    std::vector<std::string> candidates = { "wallpapers", "../wallpapers", "C:/aec/wallpapers" };
    for (const auto& dir : candidates) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
        for (auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                g_wallpaperPaths.push_back(e.path().string());
                g_wallpaperNames.push_back(e.path().filename().string());
            }
        }
        if (!g_wallpaperPaths.empty()) break;
    }
}

bool LoadWallpaperByIndex(int idx) {
    if (idx == 0 || idx - 1 >= (int)g_wallpaperPaths.size()) {
        if (g_wallpaperTex != 0) { glDeleteTextures(1, &g_wallpaperTex); g_wallpaperTex = 0; }
        g_wallpaperW = g_wallpaperH = 0;
        return true;
    }
    const std::string& path = g_wallpaperPaths[idx - 1];
    int w, h, ch;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return false;

    if (g_wallpaperTex != 0) glDeleteTextures(1, &g_wallpaperTex);
    glGenTextures(1, &g_wallpaperTex);
    glBindTexture(GL_TEXTURE_2D, g_wallpaperTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    g_wallpaperW = w; g_wallpaperH = h;
    return true;
}

void RenderWallpaper(int vpW, int vpH) {
    if (g_wallpaperTex == 0) return;
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, vpW, vpH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_wallpaperTex);
    glColor4f(1, 1, 1, 1);
    float imgAspect = (float)g_wallpaperW / (float)g_wallpaperH;
    float winAspect = (float)vpW / (float)vpH;
    float x0, y0, x1, y1;
    if (imgAspect > winAspect) {
        float newW = vpH * imgAspect;
        x0 = (vpW - newW) * 0.5f; x1 = x0 + newW; y0 = 0; y1 = (float)vpH;
    } else {
        float newH = vpW / imgAspect;
        y0 = (vpH - newH) * 0.5f; y1 = y0 + newH; x0 = 0; x1 = (float)vpW;
    }
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex2f(x0, y0);
    glTexCoord2f(1,0); glVertex2f(x1, y0);
    glTexCoord2f(1,1); glVertex2f(x1, y1);
    glTexCoord2f(0,1); glVertex2f(x0, y1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

// ============================================================
//  Soundboard
// ============================================================
// Sounds are decoded once at scan/launch into mono PCM and played
// back by resampling linearly into the output callback, so a pad
// click never touches the disk mid-call and never allocates on the
// audio thread.
const int MAX_SOUNDS        = 32;
const int MAX_SOUND_SECONDS = 60;      // longer files are truncated
const int MAX_SOUND_CHUNK   = 4096;

struct Sound {
    std::string        name;        // filename without extension
    std::string        path;
    std::vector<float> pcm;         // mono, -1..1
    int                sampleRate = 0;
};

std::vector<Sound> g_sounds;
std::atomic<float> g_soundboardGain(1.0f);

// Voice state is owned by the audio thread alone. The UI only ever
// writes the atomic arrays below, so the output callback takes no
// lock and never blocks on a click. One sound at a time: clicking a
// pad takes over from whatever is still sounding.
struct SoundVoice {
    const Sound* sound = nullptr;
    double       pos   = 0.0;   // fractional read position (source frames)
    bool         active = false;
};
SoundVoice g_soundVoice;

// UI -> audio: 0 = nothing, 1 = play, 2 = stop. One byte per pad.
std::atomic<uint8_t> g_soundRequest[MAX_SOUNDS];
// audio -> UI: is this pad currently sounding (drives the button colour).
std::atomic<bool> g_soundPlaying[MAX_SOUNDS];
// UI -> audio: panic stop. Kills every voice in one go, for long
// files where hunting for the pad that is still green is a nuisance.
std::atomic<bool> g_soundStopAll{ false };

// Local monitoring: a second playback device gets a copy of the
// soundboard mix so it is audible on the speakers while voice apps
// still hear it through VB-CABLE. Its own callback drains a ring fed
// from the output callback, so the mic/AEC path is untouched.
SpscRing<4096>    g_monitorRing;
// Written by the UI thread (Start/StopMonitor), read by the audio
// thread in FeedMonitor — atomic so the callback never sees a torn
// flag. The device itself is only created/destroyed on the UI thread.
std::atomic<bool> g_monitorStarted{ false };
int               g_monitorIndex   = -1;      // -1 = follow "Your speakers"
std::atomic<bool>  g_monitorEnabled{ true };
std::atomic<float> g_monitorGain(1.0f);

// Decode one file to mono f32 at its native rate. Kept at native rate
// (instead of the engine rate) so the same scan stays valid across a
// 16/48 kHz switch — MixSoundboard resamples per voice.
static bool LoadSoundFile(const std::string& path, Sound& out) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS)
        return false;
    if (dec.outputSampleRate == 0) {
        ma_decoder_uninit(&dec);
        return false;
    }
    out.sampleRate = (int)dec.outputSampleRate;

    const ma_uint64 capFrames =
        (ma_uint64)((double)MAX_SOUND_SECONDS * (double)dec.outputSampleRate);
    float chunk[MAX_SOUND_CHUNK];
    ma_uint64 total = 0;
    out.pcm.clear();
    while (total < capFrames) {
        ma_uint64 want = capFrames - total;
        if (want > MAX_SOUND_CHUNK) want = MAX_SOUND_CHUNK;
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, chunk, want, &got) != MA_SUCCESS ||
            got == 0)
            break;
        out.pcm.insert(out.pcm.end(), chunk, chunk + got);
        total += got;
    }
    ma_decoder_uninit(&dec);
    return !out.pcm.empty();
}

// Rescan sounds/. Only called while the AEC is stopped: the voices
// hold pointers into g_sounds, so the list must not move under them.
void ScanSounds() {
    g_soundVoice = SoundVoice();
    for (auto& r : g_soundRequest) r.store(0);
    for (auto& p : g_soundPlaying) p.store(false);
    g_soundStopAll.store(false);
    g_sounds.clear();

    std::vector<std::string> candidates = { "sounds", "../sounds", "C:/aec/sounds" };
    std::vector<std::string> files;
    for (const auto& dir : candidates) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
        for (auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg")
                files.push_back(e.path().string());
        }
        if (!files.empty()) break;
    }
    std::sort(files.begin(), files.end());   // stable pad order

    for (const auto& path : files) {
        if ((int)g_sounds.size() >= MAX_SOUNDS) break;
        Sound s;
        s.path = path;
        s.name = fs::path(path).stem().string();
        if (s.name.empty()) s.name = fs::path(path).filename().string();
        if (LoadSoundFile(path, s)) g_sounds.push_back(std::move(s));
    }
}

// Audio-thread only. Fills pOut with fs frames of mono soundboard
// audio scaled to -1..1, applying queued play/stop requests first.
// Linear interpolation is plenty for SFX and keeps this allocation-
// free with no resampler state to reset.
static void MixSoundboard(float* pOut, int fs, int outRate) {
    memset(pOut, 0, fs * sizeof(float));
    const int nSounds = (int)g_sounds.size();
    const int nPads   = nSounds < MAX_SOUNDS ? nSounds : MAX_SOUNDS;

    // Panic stop first, so it also swallows any trigger still queued
    // from a click a moment ago — "stop all" should always win.
    if (g_soundStopAll.exchange(false)) {
        g_soundVoice.active = false;
        for (int s = 0; s < nPads; s++) g_soundRequest[s].store(0);
    }

    for (int s = 0; s < nPads; s++) {
        uint8_t req = g_soundRequest[s].exchange(0);
        if (req == 0) continue;
        if (req == 2) {   // stop this pad
            if (g_soundVoice.active && g_soundVoice.sound == &g_sounds[s])
                g_soundVoice.active = false;
            continue;
        }
        // Play: one sound at a time — the new click takes over from
        // whatever is still sounding, so pads never stack up.
        g_soundVoice.sound  = &g_sounds[s];
        g_soundVoice.pos    = 0.0;
        g_soundVoice.active = true;
    }

    if (g_soundVoice.active) {
        const std::vector<float>& pcm = g_soundVoice.sound->pcm;
        const double step =
            (double)g_soundVoice.sound->sampleRate / (double)outRate;
        for (int i = 0; i < fs; i++) {
            if (g_soundVoice.pos >= (double)pcm.size()) {
                g_soundVoice.active = false;
                break;
            }
            const size_t i0 = (size_t)g_soundVoice.pos;
            const float  frac = (float)(g_soundVoice.pos - (double)i0);
            const float  s0 = pcm[i0];
            const float  s1 = (i0 + 1 < pcm.size()) ? pcm[i0 + 1] : s0;
            pOut[i] = s0 + (s1 - s0) * frac;
            g_soundVoice.pos += step;
        }
    }

    for (int s = 0; s < nPads; s++)
        g_soundPlaying[s].store(g_soundVoice.active &&
                                g_soundVoice.sound == &g_sounds[s]);
}

// ============================================================
//  Settings
// ============================================================
// Post-stage pref sync: NS runs on every engine, so this just
    // restores the global default. Kept as a function because the
    // call sites (profile apply, settings load, reset) sync here.
static void SyncStagePrefsToEngine();

void SaveSettings() {
    std::ofstream f("aec_config.txt");
    if (!f.is_open()) return;
    f << g_micIndex << "\n" << g_refIndex << "\n" << g_outIndex << "\n"
      << g_engineIndex << "\n" << g_sampleRateIndex << "\n" << g_filterIndex << "\n"
      << 0 << "\n"  // retired: WPE flag (was preprocess/DFN)
      << (int)(g_micGain.load() * 100) << "\n"
      << (int)(g_outputGain.load() * 100) << "\n"
      << g_wallpaperIndex << "\n"
      << g_profileIndex << "\n"
       << (g_minimizeToTray ? 1 : 0) << "\n"
       << 0 << "\n"  // retired: voice gate enable (kept for file alignment)
       << (g_listenToSelf ? 1 : 0) << "\n"
       << (g_advancedOpen ? 1 : 0) << "\n"
       << 0 << "\n"  // retired: voice detector (kept for file alignment)
 << 0 << "\n"  // retired: notch flag (was custom-profile)
        << (g_nsEnabled.load() ? 1 : 0) << "\n"    // DTLN noise reduction flag
<< 0 << "\n"  // retired: calibrated gate open threshold
        << 0 << "\n"  // retired: calibrated gate close threshold
        << 0 << "\n"  // retired: show-legacy-engines toggle
        << (g_experimentalEngines.load() ? 1 : 0) << "\n"  // experimental engines flag
       << 0 << "\n"  // retired: nuked DEC-toggle slot
       << 11 << "\n"  // config gen (11: auto 48 kHz migration; 10: high-band mix slot 32)
      << 0 << "\n"  // retired: NKF WPE dereverb (kept for file alignment)
      << (int)(g_soundboardGain.load() * 100) << "\n"   // soundboard gain (%)
      << (g_monitorEnabled.load() ? 1 : 0) << "\n"      // soundboard monitor on/off
      << (int)(g_monitorGain.load() * 100) << "\n"      // monitor gain (%)
      << g_monitorIndex << "\n"                        // monitor device (-1 = follow)
      << g_nsBackend.load() << "\n"                   // slot 30: NS backend (0=off,1=DTLN-NS,2=Hush,3=GTCRN)
      << (g_highbandEnabled.load() ? 1 : 0) << "\n"    // slot 31: high-band splice (default ON)
      << (int)lrintf(g_hbMix.load() * 100.0f) << "\n"; // slot 32: high-band mix (%)
}

void LoadSettings() {
    std::ifstream f("aec_config.txt");
    g_isFirstRun = !f.is_open();
    g_advancedOpen = !g_isFirstRun;  // newcomers start simple; regulars keep full view
    if (!f.is_open()) return;
    int mg, og, wpeRaw = 1;
    if (f >> g_micIndex >> g_refIndex >> g_outIndex
          >> g_engineIndex >> g_sampleRateIndex >> g_filterIndex
          >> wpeRaw >> mg >> og) {
        g_micGain.store(mg / 100.0f);
        g_outputGain.store(1.0f);  // single visible level: output stage fixed
        // Rates are {16000, 48000}: old index 1 (32 kHz, removed) and any
        // out-of-range index land on 48 kHz — no stranded configs.
        if (g_sampleRateIndex == 0) { g_sampleRate.store(16000); }
        else { g_sampleRateIndex = 1; g_sampleRate.store(48000); }
        // Retired engine 3 (old LocalVQE code): remap to the default
        // (DTLN) so old configs never point at a deleted engine. Value
        // 0 used to be SpeexDSP (retired long ago) and now means Off.
        if (g_engineIndex == 3) {
            g_engineIndex = ENGINE_DTLN;
            g_sampleRateIndex = 0;
            g_sampleRate.store(16000);
        }
        if (g_engineIndex != ENGINE_OFF && g_engineIndex != ENGINE_AEC3 &&
            g_engineIndex != ENGINE_NKF && g_engineIndex != ENGINE_DTLN)
            g_engineIndex = ENGINE_DTLN;
        g_selectedEngine.store(g_engineIndex);
        g_preprocessEnabled = false;
        g_enablePreprocess.store(false);
        int wp = -1;
        if (f >> wp) g_wallpaperIndex = wp;
        // Slot 11: gen<5 = preset index, gen5+ = profile index.
        int pi = -1;
        if (f >> pi) g_profileIndex = pi;
        int pgen = 1;  // preset/profile layout generation (value read at end of file)

        // New field — default true if missing (backward compat with old config)
        int mt = 1;
        if (f >> mt) g_minimizeToTray = (mt != 0);

        // (Retired field: voice gate enable, always 0. Read to keep
        // old and new config files positionally aligned.)
        int ve = 0;
        if (f >> ve) { (void)ve; }

        // New field — self-monitor defaults OFF if missing
        int ls = 0;
        if (f >> ls) g_listenToSelf = (ls != 0);

        // New field — Advanced open state (defaults set above)
        int ao = g_advancedOpen ? 1 : 0;
        if (f >> ao) g_advancedOpen = (ao != 0);

        // (Retired field: voice detector slot, always 0. Read to keep
        // old and new config files positionally aligned.)
        int vd_skip = 0;
        if (f >> vd_skip) { (void)vd_skip; }
        // Slot 17: notch flag (gen6); older files hold the retired
        // custom-profile flag here — read positionally, decided after pgen.
        int notchRaw = 1;
        if (f >> notchRaw) { (void)notchRaw; }
        // Slot 18: DTLN noise reduction flag (gen7+); older files don't
        // carry it — default ON (global pref).
        int nsRaw = 1;
        if (f >> nsRaw) { (void)nsRaw; }
        // (Retired field: calibrated gate open threshold. Read to keep
        // old and new config files positionally aligned.)
        float vo = 0.0f;
        if (f >> vo) { (void)vo; }
        // (Retired field: calibrated gate close threshold. Kept aligned.)
        float vc = 0.0f;
        if (f >> vc) { (void)vc; }
        // (Retired field: show-legacy-engines toggle, always 0. Kept aligned.)
        int le_skip = 0;
        if (f >> le_skip) { (void)le_skip; }
        // Slot 19: experimental engines flag (gen8+); older files don't
        // carry it — default OFF (DTLN only), per the release notes.
        int expRaw = 0;
        if (f >> expRaw) { (void)expRaw; }
        // (Retired field: nuked DEC-toggle slot, always 0. Read to keep
        // old and new config files positionally aligned.)
        int dc_skip = 0;
        if (f >> dc_skip) { (void)dc_skip; }
        // New field — preset layout generation (missing = pre-cut layout
        // that still had "High Quality" at index 2).
        if (f >> pgen) { (void)pgen; }
        // (Retired field: NKF WPE dereverb slot, always 0. Read to keep
        // old and new config files positionally aligned.)
        if (f >> le_skip) { (void)le_skip; }
        // New field — soundboard gain (default 100% when missing).
        int sbRaw = 100;
        if (f >> sbRaw) g_soundboardGain.store(sbRaw / 100.0f);
        // New fields — soundboard monitor. Defaults: on, 100%, and
        // follow the "Your speakers" selection (-1).
        int monOn = 1;
        if (f >> monOn) g_monitorEnabled.store(monOn != 0);
        int monGain = 100;
        if (f >> monGain) g_monitorGain.store(monGain / 100.0f);
        int monIdx = -1;
        if (f >> monIdx) g_monitorIndex = monIdx;
        // Slot 30: NS backend (no pgen gate — old files simply miss
        // it; validated at apply time below, -1 = absent).
        int nsBackendRaw = -1;
        if (f >> nsBackendRaw) { (void)nsBackendRaw; }
        // Slot 31: high-band splice (default ON when missing).
        int hbRaw = 1;
        if (f >> hbRaw) g_highbandEnabled.store(hbRaw != 0);
        // Slot 32: high-band mix (default 100% when missing).
        int hbMixRaw = 100;
        if (f >> hbMixRaw) {
            if (hbMixRaw < 0) hbMixRaw = 0;
            if (hbMixRaw > 100) hbMixRaw = 100;
            g_hbMix.store(hbMixRaw / 100.0f);
        }
        // "High Quality" was cut: generation-1 indices slide down
        // (2→Discord, 3→2, 4→3, 5→4). Running engine/rate are
        // separate fields, unaffected. Anything else lands on Discord.
        if (pgen == 1) {
            if (g_profileIndex == 2) g_profileIndex = 1;  // HQ ≈ Discord
            else if (g_profileIndex > 2 && g_profileIndex <= 5) g_profileIndex -= 1;
        }
        // "Low CPU" was cut (it duplicated the NKF Discord): pre-cut
        // layouts slide again (3→Discord which is the same NKF engine,
        // 4→Noisy Room). Current files skip both remaps.
        if (pgen <= 2) {
            if (g_profileIndex == 3) g_profileIndex = 1;  // Low CPU ≈ Discord (both NKF)
            else if (g_profileIndex == 4) g_profileIndex = 3;
        }
        if (pgen >= 5) {
            // Gen5+: slot 11 is already a profile index (0..2).
            if (g_profileIndex < 0 || g_profileIndex >= PROFILE_COUNT)
                g_profileIndex = 0;
        } else {
            // Gen<5: 5-slot preset layout (Manual, Discord, Echo-Heavy,
            // Noisy Room, Low CPU) -> 3 profiles.
            if (g_profileIndex < 0 || g_profileIndex >= 5) g_profileIndex = 1;
            static const int kPresetToProfile[5] = { 0, 0, 1, 0, 2 };
            // Manual settings (old index 0): closest profile for the
            // user's own engine; named presets map to their chain.
            if (g_profileIndex == 0) {
                g_profileIndex = (g_engineIndex == ENGINE_AEC3) ? 1
                               : (g_engineIndex == ENGINE_NKF)  ? 2 : 0;
            } else {
                g_profileIndex = kPresetToProfile[g_profileIndex];
            }
        }
        if (g_filterIndex < 0 || g_filterIndex > 4) g_filterIndex = 0;
        static const int rates[]   = { 16000, 48000 };
        static const int filters[] = { 30, 50, 80, 120, 200 };
        // Engine and filter always follow the profile (slot values above
        // were only positional placeholders).
        const Profile& p = PROFILES[g_profileIndex];
        g_engineIndex = p.engine;
        g_filterIndex = p.filterIdx;
        // Post-stage prefs are engine-scoped — re-sync after load so a
        // toggle left on for another engine never leaks into this one.
        SyncStagePrefsToEngine();
        // Sample rate is a free knob: keep the persisted value; only the
        // 16-kHz-only engines force it back down.
        if (g_engineIndex == ENGINE_NKF || g_engineIndex == ENGINE_DTLN) {
            g_sampleRateIndex = 0;
            g_sampleRate.store(rates[0]);
        } else {
            g_sampleRate.store(rates[g_sampleRateIndex == 0 ? 0 : 1]);
        }
        // SyncChainRate() moves to the quality default 48 kHz output unless
        // NS or a 16-kHz-only engine genuinely forces 16.
        SyncChainRate();
        // (Retired slots: gen6 WPE + notch live toggles. Read
        // positionally above; the stages no longer exist, so the
        // values are ignored and NS is the only post stage.)
        (void)wpeRaw;
        (void)notchRaw;
        if (pgen >= 7) g_nsEnabled.store(nsRaw != 0);
        else           g_nsEnabled.store(true);
        // Slot 30 applies here (after the tick is final): a stored
        // 0/1/2/3 wins, anything else derives from the tick so old
        // configs keep sounding exactly as before.
        if (nsBackendRaw >= 0 && nsBackendRaw <= 3)
            g_nsBackend.store(nsBackendRaw);
        else
            g_nsBackend.store(g_nsEnabled.load() ? 3 : 0);
        // Experimental engines (WebRTC AEC3 / NKF-AEC) are hidden by
        // default — only shown when the user ticks the Appearance
        // checkbox. Older files default OFF (DTLN only).
        if (pgen >= 8) g_experimentalEngines.store(expRaw != 0);
        else           g_experimentalEngines.store(false);
        // If the persisted engine is hidden (NKF-AEC without the
        // experimental flag), fall back to DTLN so the selection can't
        // be left on an engine that isn't in the list. WebRTC AEC3 is
        // a regular choice now and needs no fallback.
        if (!g_experimentalEngines.load() &&
            g_engineIndex == ENGINE_NKF) {
            g_engineIndex = ENGINE_DTLN;
            g_selectedEngine.store(ENGINE_DTLN);
            g_profileIndex = 0;
            SyncStagePrefsToEngine();  // prefs follow the new (visible) engine
        }
        g_filterLengthMs.store(filters[g_filterIndex]);
        g_selectedEngine.store(g_engineIndex);
    }
}

// Forward: stopping live/owned audio before reconfiguring (defined below).
void StopAEC();
void BuildDisplayIndices();  // defined below, used by ResetToDefaults

void ResetToDefaults() {
    if (g_isRunning) StopAEC();  // never reconfigure live devices
    // Devices: Windows system default — same as a fresh install
    // (mic/speakers), not whatever sits first in enumeration order.
    int defMic = FindSystemDefaultIndex(g_captureDevices);
    int defRef = FindSystemDefaultIndex(g_playbackDevices);
    g_micIndex = (defMic >= 0) ? defMic : 0;
    g_refIndex = (defRef >= 0) ? defRef : 0;
    // Send-cleaned-sound-to lands on CABLE Input (the routing target
    // voice apps listen to); without one, the system default output.
    int cable = FindCableInputIndex();
    g_outIndex = (cable >= 0) ? cable
                : ((defRef >= 0) ? defRef : 0);
    BuildDisplayIndices();  // keep selections inside the filtered lists
    g_engineIndex = 4; g_sampleRateIndex = 0; g_filterIndex = 0;  // profile 0 (DTLN @16 kHz)
    g_profileIndex = 0;
    g_preprocessEnabled = false;
    // Post-stage pref: set the engine first, then sync (NS runs
    // on every engine, so this only restores the global default).
    SyncStagePrefsToEngine();
    g_nsBackend.store(3);  // GTCRN default backend
    // Fresh default is 48 kHz output (splice ON) at full high band.
    g_highbandEnabled.store(true);
    g_hbMix.store(1.0f);
    g_minimizeToTray = true;
    g_listenToSelf = false;
    g_micGain.store(1.0f);
    g_outputGain.store(1.0f);
    g_soundboardGain.store(1.0f);
    g_monitorEnabled.store(true);
    g_monitorGain.store(1.0f);
    g_monitorIndex = -1;
    g_advancedOpen = false;  // reset lands simple: Advanced collapsed
    g_advInitDone = false;  // re-apply the persisted default next frame
    g_sampleRate.store(16000);
    g_selectedEngine.store(ENGINE_DTLN);
    g_filterLengthMs.store(30);
    g_enablePreprocess.store(false);
    g_wallpaperIndex = 0;
    LoadWallpaperByIndex(0);
    UpdateTrayIcon();
    SaveSettings();
    snprintf(g_statusText, 128, "Defaults restored - press Start");
}

// Apply a profile: engine and filter length follow. Sample rate,
// gains, devices and the NS tick are untouched (NS runs on every
// engine, so there is nothing engine-scoped left to sync).
    static void SyncStagePrefsToEngine() {
        // NS runs on every engine: restore the global default.
        g_nsEnabled.store(true);
    }

void ApplyProfile(int idx) {
    if (idx < 0 || idx >= PROFILE_COUNT) return;
    const Profile& p = PROFILES[idx];

    static const int filters[] = { 30, 50, 80, 120, 200 };

    g_engineIndex      = p.engine;
    g_filterIndex      = p.filterIdx;
    g_preprocessEnabled = false;
    g_selectedEngine.store(p.engine);
    g_filterLengthMs.store(filters[p.filterIdx]);
    g_enablePreprocess.store(false);
    g_profileIndex = idx;
    // Rate is a free knob, but 16-kHz-only engines force it down.
    if (p.engine == ENGINE_NKF || p.engine == ENGINE_DTLN) {
        g_sampleRateIndex = 0;
        g_sampleRate.store(16000);
    }
    // Post-stage pref sync: NS runs on every engine, so switching
    // engines only restores the global default (never leaks a stale
    // toggle into a chain, which once made Start look broken).
    SyncStagePrefsToEngine();
    // Re-derive the rate from the new engine plus the current NS
    // choice (16 kHz-only engines and any NS backend force 16, an
    // all-off Off/AEC3 chain goes to 48).
    SyncChainRate();
}

// ============================================================
//  Engine init
// ============================================================
// High-pass filter for the voice path: 2nd-order Butterworth
// (RBJ cookbook), 80 Hz, Q = 1/sqrt(2). Cuts what denoisers leave
// in the basement — mains hum, room rumble, mic thumps, DC offset —
// without touching voice (male fundamentals start ~85 Hz, and the
// 12 dB/oct slope barely kisses them). Always on, like the limiter:
// hygiene, not a knob. Mirrored in web/chain.py (same fc/Q, same
// Direct Form I) — pinned by test_parity --constants (HPF_FC/HPF_Q).
// Declared ahead of ReinitEngine (which re-anchors it every Start).
static const double HPF_FC = 80.0;
static const double HPF_Q  = 0.7071;
struct HpfState {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
};
static void HpfInit(HpfState& h, int sampleRate) {
    const double w0 = 2.0 * 3.14159265358979323846 * HPF_FC / (double)sampleRate;
    const double c = cos(w0);
    const double alpha = sin(w0) / (2.0 * HPF_Q);
    const double a0 = 1.0 + alpha;
    h.b0 = (1.0 + c) / 2.0 / a0;
    h.b1 = -(1.0 + c) / a0;
    h.b2 = (1.0 + c) / 2.0 / a0;
    h.a1 = -2.0 * c / a0;
    h.a2 = (1.0 - alpha) / a0;
    h.x1 = h.x2 = h.y1 = h.y2 = 0.0;
}
// In-place int16 frame filter (float internals, clamped). Runs after
// the NS post-stage, before the meters — meters show what Discord
// hears, hum included out.
static void HpfProcess(HpfState& h, int16_t* frame, int n) {
    for (int i = 0; i < n; i++) {
        const double x = (double)frame[i] * (1.0 / 32768.0);
        const double y = h.b0 * x + h.b1 * h.x1 + h.b2 * h.x2
                       - h.a1 * h.y1 - h.a2 * h.y2;
        h.x2 = h.x1;
        h.x1 = x;
        h.y2 = h.y1;
        h.y1 = y;
        frame[i] = clamp_s16((int)lrint(y * 32768.0));
    }
}
static HpfState g_hpf;  // voice-path instance; (re)init in ReinitEngine

// Gentle downward expander: the floor silencer. DTLN-NS is light by
// design and Hush still breathes — either way a sensitive meter shows
// residual floor flutter. Fixed thresholds with hysteresis (NOT an
// adaptive floor: a follower that chases sustained energy mutes held
// vowels and humming — measured muting a steady tone. Fixed levels
// can only fail open, never mute voice). Opens above OPEN_RMS,
// closes below CLOSE_RMS after HANG_MS hangover (syllables never
// trip it), 2 ms attack / 5 ms release ramps (no clicks, crisp
// onsets). Always on, like the HPF — hygiene, not a knob. Mirrored
// in web/chain.py; pinned by test_parity --constants.
static const double EXPAND_OPEN_RMS = 150.0;    // ~-47 dBFS
static const double EXPAND_CLOSE_RMS = 100.0;   // ~-50 dBFS
static const int EXPAND_HANG_MS = 200;
struct ExpState {
    double gain = 1.0;     // current smoothed multiplier
    int hang = 0;          // close-hangover frames remaining
    bool open = true;      // gate state (starts open: fail-open)
};
static void ExpanderProcess(ExpState& e, int16_t* frame, int n, int sr) {
    double ms = 0.0;
    for (int i = 0; i < n; i++) {
        const double x = (double)frame[i] * (1.0 / 32768.0);
        ms += x * x;
    }
    ms /= (n ? n : 1);
    const double openE = (EXPAND_OPEN_RMS / 32768.0) * (EXPAND_OPEN_RMS / 32768.0);
    const double closeE = (EXPAND_CLOSE_RMS / 32768.0) * (EXPAND_CLOSE_RMS / 32768.0);
    const int hangFrames =
        (int)(EXPAND_HANG_MS / 1000.0 * (double)sr / (double)(n ? n : 1));
    if (ms > openE) { e.open = true; e.hang = hangFrames > 0 ? hangFrames : 1; }
    else if (ms < closeE) {
        if (e.hang > 0) e.hang--;
        else e.open = false;
    }
    // Between thresholds: hold (hysteresis — no chatter).
    const double want = e.open ? 1.0 : 0.0;
    // Asymmetric slew: 2 ms attack (onsets stay crisp), 5 ms release.
    const double upStep = 1.0 / (0.002 * (double)sr);
    const double dnStep = 1.0 / (0.005 * (double)sr);
    for (int i = 0; i < n; i++) {
        if (e.gain < want) { e.gain += upStep; if (e.gain > want) e.gain = want; }
        else if (e.gain > want) { e.gain -= dnStep; if (e.gain < want) e.gain = want; }
        const double x = (double)frame[i] * (1.0 / 32768.0);
        frame[i] = clamp_s16((int)lrint(x * e.gain * 32768.0));
    }
}
static ExpState g_expst;  // voice-path instance; reset in ReinitEngine

// Gentle presence shelf: the anti-phone stage. The chain is 16 kHz
// (models are 16 kHz by design), and DTLN-NS shaves a dB or two off
// the top octave — together they read as "mobile call" even when the
// spectrum looks full on export (upsampling empty highs stays empty).
// This is an in-band RBJ high shelf, +4 dB at 5 kHz, S = 1: lifts
// consonant clarity / air inside 0-8 kHz without touching lows,
// without hallucination, zero lookahead. Always on, like the HPF —
// hygiene, not a knob. Mirrored in web/chain.py; pinned by
// test_parity --constants (PRES_FC/PRES_GAIN_DB).
static const double PRES_FC = 5000.0;
static const double PRES_GAIN_DB = 4.0;
struct PresState {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
};
static void PresInit(PresState& p, int sampleRate) {
    const double A = pow(10.0, PRES_GAIN_DB / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * PRES_FC / (double)sampleRate;
    const double c = cos(w0);
    const double s = sin(w0);
    const double alpha = s / 2.0 * 1.4142135623730951;  // sqrt(2), S = 1
    const double beta = 2.0 * sqrt(A) * alpha;
    const double a0 = (A + 1.0) - (A - 1.0) * c + beta;
    p.b0 = A * ((A + 1.0) + (A - 1.0) * c + beta) / a0;
    p.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0;
    p.b2 = A * ((A + 1.0) + (A - 1.0) * c - beta) / a0;
    p.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0;
    p.a2 = ((A + 1.0) - (A - 1.0) * c - beta) / a0;
    p.x1 = p.x2 = p.y1 = p.y2 = 0.0;
}
static void PresProcess(PresState& p, int16_t* frame, int n) {
    for (int i = 0; i < n; i++) {
        const double x = (double)frame[i] * (1.0 / 32768.0);
        const double y = p.b0 * x + p.b1 * p.x1 + p.b2 * p.x2
                       - p.a1 * p.y1 - p.a2 * p.y2;
        p.x2 = p.x1;
        p.x1 = x;
        p.y2 = p.y1;
        p.y1 = y;
        frame[i] = clamp_s16((int)lrint(y * 32768.0));
    }
}
static PresState g_pres;  // voice-path instance; (re)init in ReinitEngine

void ReinitEngine() {
    if (g_engine.aec3)  { Aec3Destroy(g_engine.aec3);   g_engine.aec3  = nullptr; }
    if (g_engine.nkf)   { NkfDestroy(g_engine.nkf);   g_engine.nkf   = nullptr; }
    if (g_engine.dtln)  { DtlnDestroy(g_engine.dtln);  g_engine.dtln  = nullptr; }
    if (g_engine.ns)   { DtlnNsDestroy(g_engine.ns);   g_engine.ns   = nullptr; }
    if (g_engine.hush) { HushDestroy(g_engine.hush); g_engine.hush = nullptr; }
    if (g_engine.gtcrn) { GtcrnDestroy(g_engine.gtcrn); g_engine.gtcrn = nullptr; }

    EngineType eng = (EngineType)g_selectedEngine.load();

    // NKF and DTLN run 16 kHz only (their models are 16 kHz by design).
    if (eng == ENGINE_NKF || eng == ENGINE_DTLN) {
        g_sampleRate.store(16000);
        g_sampleRateIndex = 0;
    }

    int sr = g_sampleRate.load();
    int fs = frameSizeForRate(sr);

    if (eng == ENGINE_AEC3) {
        g_engine.aec3 = Aec3New(sr, fs);
        g_engine.type = ENGINE_AEC3;
    } else if (eng == ENGINE_NKF) {
        g_engine.nkf = NkfNew("models/nkf.onnx");
        g_engine.type = ENGINE_NKF;
    } else if (eng == ENGINE_DTLN) {
        g_engine.dtln = DtlnNew("models/dtln_aec_128");
        g_engine.type = ENGINE_DTLN;
    } else if (eng == ENGINE_OFF) {
        // Engine Off: no echo canceller — the mic passes straight to
        // the picked NS backend (denoise-only mode). Nothing to create;
        // the output pump's fallthrough ships mic, and Start needs no
        // fail-closed check for it.
        g_engine.type = ENGINE_OFF;
    }

    // Noise suppression post-stage: runs after the engine on every
    // path. DTLN-NS is the reference; Hush is an alternative; GTCRN
    // (experimental NS) is mic-only and runs at 16 kHz. Only one is
    // created.
    if (g_nsBackend.load() == 3)
        g_engine.gtcrn = GtcrnNew("models/gtcrn_simple.onnx");
    else if (g_nsBackend.load() == 2)
        g_engine.hush = HushNew("models/hush_16k_onnx.tar.gz");
    else if (g_nsBackend.load() == 1)
        g_engine.ns = DtlnNsNew("models/dtln_ns_128");
    // backend 0 (Off): no post-stage — the engine output goes straight
    // to the limiter.
    // Voice-path high-pass follows the sample rate (engines that force
    // 16 kHz re-anchor it here, every Start).
    HpfInit(g_hpf, g_sampleRate.load());
    g_expst = ExpState();  // fresh floor/gain/hangover every Start
    PresInit(g_pres, g_sampleRate.load());
}

// Soft limiter for the final gain stage: linear to -3 dBFS, tanh knee
// to full scale above it. Hard clamp stays as last-resort safety.
static inline int16_t SoftLimit(float v) {
    static const float kKnee = 0.7079f;  // -3 dBFS
    float a = fabsf(v);
    if (a <= kKnee) return clamp_s16((int)(v * 32768.0f));
    // Map [knee, inf) -> [knee, 1) with tanh, then clamp.
    float y = kKnee + (1.0f - kKnee) * tanhf((a - kKnee) / (1.0f - kKnee));
    if (v < 0.0f) y = -y;
    return clamp_s16((int)(y * 32768.0f));
}

// Fail-open gap: emit the last good frame fading to silence instead of
// a hard mute (10 ms of zeros is an audible click on every starve).
// The fade lands at ~0, so a following gap may hard-zero inaudibly.
static void EmitFading(int16_t* out, ma_uint32 frameCount) {
    if (g_lastOutValid && g_lastOutLen > 0) {
        for (ma_uint32 i = 0; i < frameCount; i++) {
            int16_t s = (i < (ma_uint32)g_lastOutLen) ? g_lastOutFrame[i] : 0;
            float fade = 1.0f - (float)i / (float)(frameCount ? frameCount : 1);
            out[i] = clamp_s16((int)((float)s * fade));
        }
    } else {
        memset(out, 0, frameCount * sizeof(int16_t));
    }
    g_lastOutLen = 0;
    g_lastOutValid = false;
}

// ============================================================
//  Audio callbacks
// ============================================================
void mic_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    const int16_t* in = (const int16_t*)pInput;
    if (g_highbandEnabled.load()) {
        // Capture is 48 kHz here: store the raw high band, then
        // downsample 3:1 through the linear-phase FIR for the 16 kHz
        // engine (flat to 7.5 kHz, aliases below -76 dB). The phase
        // carries over between callbacks, so a frame count that is not
        // a multiple of 3 cannot shift the output grid.
        g_micRing48.write(in, frameCount);
        static int16_t ds[2048];
        const int dsLen = g_down3.process(in, (int)frameCount, ds, 2048);
        if (dsLen > 0) g_micRing.write(ds, dsLen);
    } else {
        g_micRing.write(in, frameCount);
    }
}
void loopback_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    g_refRing.write((const int16_t*)pInput, frameCount);
}
// Defined with the monitor device, further down.
static void FeedMonitor(const float* sb, int fs);

void output_callback(ma_device*, void* pOutput, const void*, ma_uint32 frameCount) {
    int16_t* out = (int16_t*)pOutput;
    const bool hb = g_highbandEnabled.load();
    const int outRate = hb ? 48000 : g_sampleRate.load();
    const int fsOut = frameSizeForRate(outRate);
    if ((int)frameCount != fsOut) { EmitFading(out, frameCount); return; }
    int fs = frameSizeForRate(g_sampleRate.load());

    const size_t DRIFT_TARGET = (size_t)fs * 2;
    const size_t DRIFT_THRESHOLD = (size_t)fs / 16;
    if (g_micRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_micRing.skip(g_micRing.available() - DRIFT_TARGET);
    if (g_highbandEnabled.load() &&
        g_micRing48.available() > (DRIFT_TARGET + DRIFT_THRESHOLD) * 3)
        g_micRing48.skip(g_micRing48.available() - DRIFT_TARGET * 3);
    if (g_refRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_refRing.skip(g_refRing.available() - DRIFT_TARGET);

    // Loopback starves while the speakers are idle (no frames flow),
    // so never stall the mic path on it: missing ref reads as digital
    // silence, which is exactly what "nothing playing" means to the
    // AEC. (CABLE-output sessions looked dead until sound played on
    // the speakers; monitor sessions never starved because they always
    // drive the speakers — same bug, masked.)
    if (g_micRing.available() < (size_t)fs) {
        // Mic starve is fail-open for the voice path, but the
        // soundboard must keep going through it — a pad click should
        // not glitch just because the mic hiccuped. Silence here
        // still falls back to the fade so the gap stays inaudible.
        float sbStarve[MAX_FRAME_SIZE];
        MixSoundboard(sbStarve, fsOut, outRate);
        FeedMonitor(sbStarve, fsOut);
        const float sbGainStarve = g_soundboardGain.load() * g_outputGain.load();
        bool anySb = false;
        for (int i = 0; i < fsOut; i++) {
            out[i] = SoftLimit(sbStarve[i] * sbGainStarve);
            if (sbStarve[i] != 0.0f) anySb = true;
        }
        if (anySb) {
            memcpy(g_lastOutFrame, out, fsOut * sizeof(int16_t));
            g_lastOutLen = fsOut;
            g_lastOutValid = true;
        } else {
            EmitFading(out, frameCount);
        }
        return;
    }

    int16_t micFrame[MAX_FRAME_SIZE];
    int16_t refFrame[MAX_FRAME_SIZE];
    int16_t cleanedFrame[MAX_FRAME_SIZE];
    float   fltBuf[MAX_FRAME_SIZE];
    memset(cleanedFrame, 0, sizeof(cleanedFrame));

    g_micRing.read(micFrame, fs);
    if (g_refRing.available() < (size_t)fs)
        memset(refFrame, 0, fs * sizeof(int16_t));  // speakers idle: silent ref
    else
        g_refRing.read(refFrame, fs);
    // In splice mode the mic reaches the engine through our FIR (32
    // samples @16k of delay) while the loopback reference reaches it
    // through miniaudio's resampler (~1 sample). Delay the reference
    // to match, or the AEC sees a ~2 ms offset.
    if (g_highbandEnabled.load())
        RefDelayProcess(refFrame, fs);

    if (g_engine.type == ENGINE_AEC3 && g_engine.aec3)
        Aec3CancelEcho(g_engine.aec3, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_NKF && g_engine.nkf)
        NkfProcess(g_engine.nkf, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_DTLN && g_engine.dtln)
        DtlnProcess(g_engine.dtln, micFrame, refFrame, cleanedFrame, fs);
    else
        memcpy(cleanedFrame, micFrame, fs * sizeof(int16_t));



    // Noise-suppression post-stage: runs on the engine output of every
    // path, 16 kHz only (the models are 16 kHz; other rates skip the
    // stage). Fail-open: a null or failed stage passes the frame
    // straight through — it never mutes the near-end voice. Meters
    // below show post-stage output, i.e. what Discord hears. Only the
    // radio-picked backend was created in ReinitEngine, so at most one
    // of these runs; backend 0 (Off) runs none.
    GtcrnHandle* gtcrn = (g_nsBackend.load() == 3) ? g_engine.gtcrn : nullptr;
    if (gtcrn && g_sampleRate.load() == 16000) {
        for (int i = 0; i < fs; i++)
            fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
        if (GtcrnProcess(gtcrn, fltBuf, fltBuf, fs) != 1) {
            for (int i = 0; i < fs; i++)
                fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
        }
        for (int i = 0; i < fs; i++)
            cleanedFrame[i] = clamp_s16((int)lrintf(fltBuf[i] * 32768.0f));
    } else {
        HushHandle* hush = (g_nsBackend.load() == 2) ? g_engine.hush : nullptr;
        if (hush && g_sampleRate.load() == 16000) {
            for (int i = 0; i < fs; i++)
                fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
            if (HushProcess(hush, fltBuf, fltBuf, fs) != 1) {
                for (int i = 0; i < fs; i++)
                    fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
            }
            for (int i = 0; i < fs; i++)
                cleanedFrame[i] = clamp_s16((int)lrintf(fltBuf[i] * 32768.0f));
        } else if (g_nsBackend.load() == 1 && g_engine.ns &&
                   g_sampleRate.load() == 16000) {
            for (int i = 0; i < fs; i++)
                fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
            // Fail-open: any NS failure (exception, bad tensor, model
            // mismatch) leaves the frame untouched instead of taking the
            // app down mid-call.
            if (DtlnNsProcess(g_engine.ns, fltBuf, fltBuf, fs) != 1) {
                // NS failed this frame — leave it untouched and keep going.
                // (The diagnostics line under the checkbox will show the
                // last error string from DtnsLastError.)
                for (int i = 0; i < fs; i++)
                    fltBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
            }
            for (int i = 0; i < fs; i++)
                cleanedFrame[i] = clamp_s16((int)lrintf(fltBuf[i] * 32768.0f));
        }
    }

    // Voice-path high-pass (80 Hz): strips hum/rumble/DC the stages
    // above leave behind, on every path including engine-Off. Runs
    // before the meters so they read the shipped signal.
    HpfProcess(g_hpf, cleanedFrame, fs);

    // Gentle downward expander just after the high-pass (same reason:
    // meters read the shipped signal, floor flutter included out).
    ExpanderProcess(g_expst, cleanedFrame, fs, g_sampleRate.load());

    // Presence shelf just after the expander (same reason: meters read
    // the shipped signal, brightness included).
    PresProcess(g_pres, cleanedFrame, fs);

    float rms_mic = 0, rms_ref = 0, rms_out = 0;
    for (int i = 0; i < fs; i++) {
        rms_mic += (float)micFrame[i]     * micFrame[i];
        rms_ref += (float)refFrame[i]     * refFrame[i];
        rms_out += (float)cleanedFrame[i] * cleanedFrame[i];
    }
    rms_mic = sqrtf(rms_mic / fs);
    rms_ref = sqrtf(rms_ref / fs);
    rms_out = sqrtf(rms_out / fs);

    g_last_mic_rms = rms_mic;
    g_last_ref_rms = rms_ref;
    g_last_out_rms = rms_out;

    if (rms_mic > 30.0f && rms_ref > 100.0f) {
        float ratio = (rms_out + 1.0f) / (rms_mic + 1.0f);
        if (ratio < 1.0f) g_last_reduction_db = 20.0f * log10f(ratio);
    }

    // Soundboard: pads are mixed into the same stream the voice apps
    // hear, before the shared limiter so mic + sounds can never clip
    // each other into distortion. (The Out meter above stays on the
    // cleaned mic only — it measures AEC reduction, not programme.)
    float sbBuf[MAX_FRAME_SIZE];
    // fs == 160 in high-band mode (Reinit pins the chain to 16 kHz
    // while ticked); anything else falls through to the legacy path.
    if (g_highbandEnabled.load() && fs == 160) {
        // High-band splice: 16 kHz chain output (processed lows)
        // upsampled to 48 kHz, plus the real high band (8-24 kHz) cut
        // from the original 48 kHz mic. Real highs, not synthesized.
        float nvOut[MAX_FRAME_SIZE];
        // Linear-phase 3x FIR upsample of the processed low band
        // (0-8 kHz): flat to 7.5 kHz, images below -55 dB.
        g_up3.process(cleanedFrame, fs, nvOut);
        // --- high band, delayed to land on the processed low band -----
        int16_t mic48[MAX_FRAME_SIZE];
        if (g_micRing48.available() >= (size_t)fsOut)
            g_micRing48.read(mic48, fsOut);
        else
            memset(mic48, 0, fsOut * sizeof(int16_t));
        g_hbDelay.push(mic48, fsOut);
        int16_t aligned48[MAX_FRAME_SIZE];
        g_hbDelay.readDelayed(aligned48, fsOut, g_hbDelaySamples.load());

        // --- the high band, noise-suppressed ---------------------------
        float high[MAX_FRAME_SIZE];
        HbNsProcess(g_hbNs, aligned48, high, fsOut);

        // --- VAD: close the high band while there is no voice ---------
        float peak = 0.0f;
        for (int i = 0; i < fs; i++) {
            const float a = fabsf((float)cleanedFrame[i]);
            if (a > peak) peak = a;
        }
        const float lvlDb = 20.0f * log10f(peak / 32768.0f + 1e-9f);
        float vad = (lvlDb - (float)HB_VAD_DB) / (float)HB_VAD_RAMP_DB;
        if (vad < 0.0f) vad = 0.0f;
        if (vad > 1.0f) vad = 1.0f;
        const float vPrev = g_hbVadGain;
        for (int i = 0; i < fsOut; i++) {
            const float w = (float)(i + 1) / 48.0f;
            g_hbVadGain = vPrev + (vad - vPrev) * (w < 1.0f ? w : 1.0f);
            high[i] *= g_hbVadGain;
        }
        g_hbVadGain = vad;
        g_hbRatioDb.store(lvlDb);
        MixSoundboard(sbBuf, fsOut, outRate);
        FeedMonitor(sbBuf, fsOut);
        const float sbGain = g_soundboardGain.load() * g_outputGain.load();
        float gain = g_micGain.load() * g_outputGain.load();
        const float hbMix = g_hbMix.load();
        for (int i = 0; i < fsOut; i++)
            out[i] = SoftLimit((nvOut[i] + high[i] * hbMix) * gain + sbBuf[i] * sbGain);
    } else {
        MixSoundboard(sbBuf, fs, g_sampleRate.load());
        FeedMonitor(sbBuf, fs);
        const float sbGain = g_soundboardGain.load() * g_outputGain.load();

        float gain = g_micGain.load() * g_outputGain.load();
        for (int i = 0; i < fs; i++)
            out[i] = SoftLimit((float)cleanedFrame[i] * gain / 32768.0f
                               + sbBuf[i] * sbGain);
        for (int i = fs; i < fsOut; i++) out[i] = 0;
    }
    // Remember what we emitted for fail-open fades (mic starve etc).
    memcpy(g_lastOutFrame, out, fsOut * sizeof(int16_t));
    g_lastOutLen = fsOut;
    g_lastOutValid = true;
}

// ============================================================
//  Soundboard monitor (speakers)
// ============================================================
// A second playback device carries a copy of the soundboard mix so
// it is audible locally while voice apps still hear it via VB-CABLE.
// Its own callback drains a ring fed from output_callback, which
// keeps the mic/AEC path completely untouched.
void monitor_callback(ma_device*, void* pOutput, const void*, ma_uint32 frameCount) {
    int16_t* out = (int16_t*)pOutput;
    const ma_uint32 n =
        frameCount < (ma_uint32)MAX_FRAME_SIZE ? frameCount : (ma_uint32)MAX_FRAME_SIZE;

    // The monitor runs on its own clock, so the ring can creep up or
    // drain: trim the excess (same rule as the mic/ref drift guards).
    const size_t driftTarget =
        (size_t)frameSizeForRate(g_sampleRate.load()) * 2;
    if (g_monitorRing.available() > driftTarget)
        g_monitorRing.skip(g_monitorRing.available() - driftTarget);

    ma_uint32 got = 0;
    if (g_monitorRing.available() > 0) {
        got = (ma_uint32)g_monitorRing.available();
        if (got > n) got = n;
        g_monitorRing.read(out, got);
    }
    if (got < frameCount) {
        // Starved tail: fade from the last emitted monitor frame
        // (EmitFading contract on the main path) instead of a hard
        // cut to zero — then remember this frame for the next gap.
        for (ma_uint32 i = got; i < frameCount; i++) {
            ma_uint32 k = i - got;
            int16_t s = (g_lastMonValid && k < (ma_uint32)g_lastMonLen)
                ? g_lastMonFrame[k] : 0;
            float fade = 1.0f - (float)i / (float)(frameCount ? frameCount : 1);
            out[i] = clamp_s16((int)((float)s * fade));
        }
    }
    {
        ma_uint32 keep = frameCount < (ma_uint32)MAX_FRAME_SIZE
            ? frameCount : (ma_uint32)MAX_FRAME_SIZE;
        memcpy(g_lastMonFrame, out, keep * sizeof(int16_t));
        g_lastMonLen = (int)keep;
        g_lastMonValid = true;
    }
}

// Hand one frame of soundboard audio to the monitor. Only while the
// monitor device is actually consuming — otherwise the ring would
// fill with audio nobody reads.
static void FeedMonitor(const float* sb, int fs) {
    if (!g_monitorStarted) return;
    // This ring is single-producer/single-consumer: only the monitor
    // callback may advance the tail, so this side must never skip().
    // If the monitor is behind, drop the frame instead of wrapping
    // over audio it has not read yet. (The two devices request the
    // same sample rate, so drift — and therefore drops — are rare.)
    if (g_monitorRing.available() + (size_t)fs > SpscRing<4096>::capacity)
        return;
    int16_t mon[MAX_FRAME_SIZE];
    const float g = g_monitorGain.load();
    for (int i = 0; i < fs; i++)
        mon[i] = SoftLimit(sb[i] * g);
    g_monitorRing.write(mon, fs);
}

// Which playback device the monitor feeds. -1 follows the output
// device selection ("Your speakers"), so changing speakers on the
// Audio tab carries the monitor along automatically. When the monitor
// is used with "Listen to myself", this ensures the monitor plays
// through the same speakers the user hears.
static int MonitorDeviceIndex() {
    if (g_monitorIndex >= 0 && g_monitorIndex < (int)g_playbackDevices.size())
        return g_monitorIndex;
    return g_outIndex;
}

void StopMonitor() {
    if (!g_monitorStarted) return;
    ma_device_uninit(&g_monitorDevice);
    g_monitorStarted = false;
    g_monitorRing.reset();
}

// Fail-open: a monitor that cannot start must never block the AEC —
// the pads still play to voice apps, just not locally.
void StartMonitor() {
    if (g_monitorStarted || !g_isRunning) return;
    if (!g_monitorEnabled.load()) return;
    // With Listen to myself the main output already lands on the
    // speakers and already carries the soundboard, so a second
    // stream would only double the level.
    if (g_listenToSelf) return;
    if (g_playbackDevices.empty()) return;

    const int monIdx = MonitorDeviceIndex();
    if (monIdx < 0 || monIdx >= (int)g_playbackDevices.size()) return;

    const int sr = g_sampleRate.load();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_s16;
    cfg.playback.channels  = 1;
    cfg.sampleRate         = sr;
    cfg.periodSizeInFrames = frameSizeForRate(sr);
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback       = monitor_callback;
    cfg.playback.pDeviceID = &g_playbackDevices[monIdx].id;

    g_monitorRing.reset();
    if (ma_device_init(&g_context, &cfg, &g_monitorDevice) != MA_SUCCESS)
        return;
    if (ma_device_start(&g_monitorDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_monitorDevice);
        return;
    }
    g_monitorStarted = true;
}

// ============================================================
//  Device enumeration + display filter
// ============================================================
void BuildDisplayIndices() {
    g_micDisplayIndices.clear();
    g_refDisplayIndices.clear();
    g_outDisplayIndices.clear();

    for (int i = 0; i < (int)g_captureDevices.size(); i++) {
        if (!IsVirtualCableDevice(g_captureDevices[i].name))
            g_micDisplayIndices.push_back(i);
    }
    for (int i = 0; i < (int)g_playbackDevices.size(); i++) {
        if (!IsVirtualCableDevice(g_playbackDevices[i].name))
            g_refDisplayIndices.push_back(i);
    }
    for (int i = 0; i < (int)g_playbackDevices.size(); i++) {
        g_outDisplayIndices.push_back(i);
    }

    auto snap = [](int& index, const std::vector<int>& list) {
        if (list.empty()) { index = 0; return; }
        for (int v : list) if (v == index) return;
        index = list[0];
    };
    snap(g_micIndex, g_micDisplayIndices);
    snap(g_refIndex, g_refDisplayIndices);
    // Monitor: -1 means "follow Your speakers" and is always valid;
    // an explicit pick must still point at a real speaker device.
    if (g_monitorIndex != -1) snap(g_monitorIndex, g_refDisplayIndices);
    // Output prefers CABLE Input (fresh installs land on it, not index 0).
    int cable = FindCableInputIndex();
    bool outKept = false;
    for (int v : g_outDisplayIndices) if (v == g_outIndex) { outKept = true; break; }
    if (!outKept) g_outIndex = (cable >= 0) ? cable
        : (g_outDisplayIndices.empty() ? 0 : g_outDisplayIndices[0]);
}

void EnumerateDevices() {
    const bool firstScan = g_captureDevices.empty() && g_playbackDevices.empty();
    std::string micName, refName, outName;
    if (!g_captureDevices.empty()  && g_micIndex < (int)g_captureDevices.size())
        micName = g_captureDevices[g_micIndex].name;
    if (!g_playbackDevices.empty() && g_refIndex < (int)g_playbackDevices.size())
        refName = g_playbackDevices[g_refIndex].name;
    if (!g_playbackDevices.empty() && g_outIndex < (int)g_playbackDevices.size())
        outName = g_playbackDevices[g_outIndex].name;

    if (g_contextInitialized) {
        ma_context_uninit(&g_context);
        g_contextInitialized = false;
    }

    if (ma_context_init(NULL, 0, NULL, &g_context) != MA_SUCCESS) {
        return;
    }
    g_contextInitialized = true;

    ma_device_info* pPlayback; ma_uint32 playbackCount;
    ma_device_info* pCapture;  ma_uint32 captureCount;
    ma_context_get_devices(&g_context, &pPlayback, &playbackCount,
                                       &pCapture,  &captureCount);

    g_captureDevices.assign(pCapture,  pCapture  + captureCount);
    g_playbackDevices.assign(pPlayback, pPlayback + playbackCount);

    const int defMic = FindSystemDefaultIndex(g_captureDevices);
    const int defRef = FindSystemDefaultIndex(g_playbackDevices);

    auto findBy = [](const std::vector<ma_device_info>& list, const std::string& name) -> int {
        if (name.empty()) return -1;
        for (int i = 0; i < (int)list.size(); i++)
            if (name == list[i].name) return i;
        return -1;
    };

    if (firstScan && !g_isFirstRun) {
        // Launch with a saved config: the lists were empty when names
        // were captured, so honor the indices LoadSettings restored —
        // clamp only when they no longer exist (previously this path
        // reset them to device 0 on every launch). Out-of-range falls
        // back to the system default (mic/speakers) or CABLE/default.
        if (g_micIndex < 0 || g_micIndex >= (int)g_captureDevices.size())
            g_micIndex = (defMic >= 0) ? defMic : 0;
        if (g_refIndex < 0 || g_refIndex >= (int)g_playbackDevices.size())
            g_refIndex = (defRef >= 0) ? defRef : 0;
        if (g_outIndex < 0 || g_outIndex >= (int)g_playbackDevices.size()) {
            int cable = FindCableInputIndex();
            g_outIndex = (cable >= 0) ? cable
                       : ((defRef >= 0) ? defRef : 0);
        }
    } else {
        // First run (no config) or a mid-session re-scan: match the
        // previous selection by name; a miss (fresh install, unplugged
        // device) lands on the Windows system default.
        int mi = findBy(g_captureDevices, micName);
        g_micIndex = (mi >= 0) ? mi : ((defMic >= 0) ? defMic : 0);
        int ri = findBy(g_playbackDevices, refName);
        g_refIndex = (ri >= 0) ? ri : ((defRef >= 0) ? defRef : 0);
        int oi = findBy(g_playbackDevices, outName);
        if (oi >= 0) {
            g_outIndex = oi;
        } else {
            // Saved output gone (or first run): CABLE Input when
            // installed (routing target), else the system default.
            int cable = FindCableInputIndex();
            g_outIndex = (cable >= 0) ? cable
                       : ((defRef >= 0) ? defRef : 0);
        }
    }

BuildDisplayIndices();
}

// ============================================================
//  Start / Stop
// ============================================================
void StartAEC() {
    if (g_isRunning) return;
    if (g_captureDevices.empty() || g_playbackDevices.empty()) {
        snprintf(g_statusText, 128, "No devices found");
        return;
    }
    if (g_micIndex >= (int)g_captureDevices.size())  g_micIndex = 0;
    if (g_refIndex >= (int)g_playbackDevices.size()) g_refIndex = 0;
    if (g_outIndex >= (int)g_playbackDevices.size()) g_outIndex = 0;

    // Self-monitor routes the cleaned mic to your speakers (Output)
    // instead of routing it to voice apps (CABLE Input); otherwise
    // Output must be usable. Without VB-CABLE installed/enabled there
    // is nowhere to send the cleaned mic, so refuse to start with a
    // pointer at the fix.
    int outIdx = g_outIndex;
    if (g_listenToSelf) {
        outIdx = g_outIndex;
    } else if (!CableInputPresent()) {
        snprintf(g_statusText, 128, "VB-CABLE not found - install/enable CABLE Input");
        return;
    }

    SaveSettings();
    ReinitEngine();

    if (g_engine.type == ENGINE_NKF && !g_engine.nkf) {
        snprintf(g_statusText, 128, "Failed to load NKF model");
        return;
    }
    if (g_engine.type == ENGINE_DTLN && !g_engine.dtln) {
        snprintf(g_statusText, 128, "Failed to load DTLN model");
        return;
    }
    if (g_engine.type == ENGINE_AEC3 && !g_engine.aec3) {
        snprintf(g_statusText, 128, "Failed to load AEC3 engine");
        return;
    }
    // NS backends are fail-closed at Start: a broken/picked stage
    // must never silently passthrough.
    if (g_nsEnabled.load() && g_nsBackend.load() == 3 &&
        (!g_engine.gtcrn || !GtcrnReady(g_engine.gtcrn))) {
        const char* why = (g_engine.gtcrn && GtcrnLastError(g_engine.gtcrn)[0])
            ? GtcrnLastError(g_engine.gtcrn) : "models/gtcrn_simple.onnx missing";
        snprintf(g_statusText, 128, "GTCRN unavailable - %.90s", why);
        return;
    }
    if (g_nsEnabled.load() && g_nsBackend.load() == 2 &&
        (!g_engine.hush || !HushReady(g_engine.hush))) {
        const char* why = (g_engine.hush && HushLastError(g_engine.hush)[0])
            ? HushLastError(g_engine.hush) : "weya_nc.dll or models/hush_16k_onnx.tar.gz missing";
        snprintf(g_statusText, 128, "Hush unavailable - %.90s", why);
        return;
    }
    g_micRing.reset();
    g_refRing.reset();
    g_micRing48.reset();

    int sr = g_sampleRate.load();
    int fs = frameSizeForRate(sr);

    ma_device_config micCfg = ma_device_config_init(ma_device_type_capture);
    micCfg.capture.format = ma_format_s16;
    micCfg.capture.channels = 1;
    micCfg.sampleRate = g_highbandEnabled.load() ? 48000 : sr;
    micCfg.periodSizeInFrames = g_highbandEnabled.load() ? 480 : fs;
    micCfg.performanceProfile = ma_performance_profile_low_latency;
    micCfg.dataCallback = mic_callback;
    micCfg.capture.pDeviceID = &g_captureDevices[g_micIndex].id;

    ma_device_config loopCfg = ma_device_config_init(ma_device_type_loopback);
    loopCfg.capture.format = ma_format_s16;
    loopCfg.capture.channels = 1;
    loopCfg.sampleRate = sr;
    loopCfg.periodSizeInFrames = fs;
    loopCfg.performanceProfile = ma_performance_profile_low_latency;
    loopCfg.dataCallback = loopback_callback;
    loopCfg.capture.pDeviceID = &g_playbackDevices[g_refIndex].id;

    ma_device_config outCfg = ma_device_config_init(ma_device_type_playback);
    outCfg.playback.format = ma_format_s16;
    outCfg.playback.channels = 1;
    outCfg.sampleRate = OutRate();
    outCfg.periodSizeInFrames = frameSizeForRate(OutRate());
    outCfg.performanceProfile = ma_performance_profile_low_latency;
    outCfg.dataCallback = output_callback;
    outCfg.playback.pDeviceID = &g_playbackDevices[outIdx].id;

    if (ma_device_init(&g_context, &micCfg,  &g_micDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to init mic");
        return;
    }
    if (ma_device_init(&g_context, &loopCfg, &g_loopbackDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to init loopback");
        ma_device_uninit(&g_micDevice);
        return;
    }
    if (ma_device_init(&g_context, &outCfg, &g_outputDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to init output");
        ma_device_uninit(&g_micDevice);
        ma_device_uninit(&g_loopbackDevice);
        return;
    }

    if (ma_device_start(&g_micDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to start mic");
        ma_device_uninit(&g_outputDevice);
        ma_device_uninit(&g_loopbackDevice);
        ma_device_uninit(&g_micDevice);
        return;
    }
    if (ma_device_start(&g_loopbackDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to start loopback");
        ma_device_uninit(&g_outputDevice);
        ma_device_uninit(&g_loopbackDevice);
        ma_device_uninit(&g_micDevice);
        return;
    }
    if (ma_device_start(&g_outputDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Failed to start output");
        ma_device_uninit(&g_outputDevice);
        ma_device_uninit(&g_loopbackDevice);
        ma_device_uninit(&g_micDevice);
        return;
    }

    if (g_highbandEnabled.load()) {
        g_hbDelay.reset();
        HbNsInit(g_hbNs);
        g_hbVadGain = 1.0f;
        g_down3.reset();
        g_up3.reset();
        RefDelayReset();
        // High-band delay: chain latency (AEC + NS) minus high-band NS
        // STFT latency, plus FIR group delay, all in 48 kHz samples.
        int hbDelay = (AecLatency16(g_engine.type) +
                       NsLatency16(g_nsBackend.load())) * 3 - 256 +
                      2 * rs::kHalf;
        if (hbDelay < 0) hbDelay = 0;
        g_hbDelaySamples.store(hbDelay);
    }
    g_isRunning = true;
    g_sessionStart = Clock::now();
    StartMonitor();   // local soundboard monitoring (fail-open)
    const char* engineName = PROFILES[g_profileIndex].name;
    const char* nsTag = "";
    if (g_engine.gtcrn && GtcrnReady(g_engine.gtcrn)) nsTag = " + GTCRN";
    else if (g_engine.hush && HushReady(g_engine.hush)) nsTag = " + Hush";
    else if (g_engine.ns) nsTag = " + DTLN-NS";
    const bool hbLive = g_highbandEnabled.load();
    if (hbLive)
        snprintf(g_statusText, 128, "Running (16000->48000 Hz, %s%s + HiBand)%s",
                 engineName, nsTag, g_listenToSelf ? " [monitor]" : "");
    else
        snprintf(g_statusText, 128, "Running (%d Hz, %s%s)%s", OutRate(), engineName,
                 nsTag, g_listenToSelf ? " [monitor]" : "");
}

void StopAEC() {
    if (!g_isRunning) return;
    StopMonitor();
    ma_device_uninit(&g_micDevice);
    ma_device_uninit(&g_loopbackDevice);
    ma_device_uninit(&g_outputDevice);
    g_isRunning = false;
    // Audio callbacks are dead (devices uninited) — reset soundboard state
    // so a playing pad's voice, queued requests, and indicators don't
    // survive Stop and resume mid-file on the next Start.
    g_soundVoice.sound  = nullptr;
    g_soundVoice.pos    = 0.0;
    g_soundVoice.active = false;
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        g_soundRequest[i].store(0);
        g_soundPlaying[i].store(false);
    }
    g_soundStopAll.store(false);
    snprintf(g_statusText, 128, "Stopped");
}

// ============================================================
//  Custom widgets
// ============================================================
void DrawStatusDot(bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 6.0f;
    ImU32 col  = active ? IM_COL32(80, 220, 100, 255) : IM_COL32(220, 80, 80, 255);
    ImU32 glow = active ? IM_COL32(80, 220, 100, 80)  : IM_COL32(220, 80, 80, 80);
    dl->AddCircleFilled(ImVec2(p.x + r, p.y + r + 2), r + 2, glow, 24);
    dl->AddCircleFilled(ImVec2(p.x + r, p.y + r + 2), r, col, 24);
    ImGui::Dummy(ImVec2(r * 2 + 6, r * 2 + 4));
}

void DrawInlineDot(bool ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 4.0f;
    ImU32 col = ok ? IM_COL32(80, 220, 100, 255) : IM_COL32(220, 80, 80, 255);
    dl->AddCircleFilled(ImVec2(p.x + r, p.y + r + 3), r, col, 16);
    ImGui::Dummy(ImVec2(r * 2 + 6, r * 2 + 4));
}

void DrawLevelMeter(const char* label, float rms, float maxValue) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(60);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 18.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      IM_COL32(25, 25, 30, 255), 4.0f);
    float frac = rms / maxValue;
    if (frac > 1.0f) frac = 1.0f;
    float barW = width * frac;
    if (barW > 1.0f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + barW, pos.y + height),
                          IM_COL32(80, 200, 100, 255), 4.0f);
    }
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                IM_COL32(60, 60, 70, 255), 4.0f);
    ImGui::Dummy(ImVec2(width, height));
}

// ============================================================
//  UI sections
// ============================================================
static bool FilteredDeviceCombo(const char* label, const char* tooltip, int& index,
                                const std::vector<ma_device_info>& devices,
                                const std::vector<int>& displayIndices) {
    int displayPos = -1;
    for (int i = 0; i < (int)displayIndices.size(); i++) {
        if (displayIndices[i] == index) { displayPos = i; break; }
    }

    const char* currentName =
        (displayPos >= 0 && displayIndices[displayPos] < (int)devices.size())
            ? devices[displayIndices[displayPos]].name
            : "(none)";

    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(("##" + std::string(label)).c_str(), currentName)) {
        if (displayIndices.empty()) {
            ImGui::TextDisabled("(no devices)");
        } else {
            for (int i = 0; i < (int)displayIndices.size(); i++) {
                int realIdx = displayIndices[i];
                bool selected = (realIdx == index);
                if (ImGui::Selectable(devices[realIdx].name, selected)) {
                    index = realIdx;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

void DrawDevicesSection() {
    ImGui::SeparatorText("Devices");

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
    ImGui::BeginDisabled(g_isRunning);
    if (ImGui::Button("Refresh", ImVec2(90, 0))) {
        EnumerateDevices();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rescan audio devices without restarting");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(g_isRunning);
    const float labelCol = 190.0f;

    DrawInlineDot(!g_micDisplayIndices.empty());
    ImGui::SameLine();
    ImGui::TextUnformatted("Your microphone");
    ImGui::SameLine(labelCol);
    FilteredDeviceCombo("Microphone", "The mic you speak into",
                        g_micIndex, g_captureDevices, g_micDisplayIndices);

    DrawInlineDot(!g_refDisplayIndices.empty());
    ImGui::SameLine();
    ImGui::TextUnformatted("Your speakers");
    ImGui::SameLine(labelCol);
    FilteredDeviceCombo("Speaker Reference", "The speakers whose sound gets removed from your mic",
                        g_refIndex, g_playbackDevices, g_refDisplayIndices);

    // Shown in both modes: with Listen-to-myself the destination is
    // derived from the Speaker Reference loopback, and when that name
    // doesn't map to a playback endpoint the audio went nowhere with no
    // clue why. Keeping the row visible and editable (relabeled) makes
    // the destination explicit instead of hiding it — same as the web
    // build now does.
    DrawInlineDot(!g_outDisplayIndices.empty());
    ImGui::SameLine();
    ImGui::TextUnformatted(g_listenToSelf
                           ? "Listen to myself - send to"
                           : "Send cleaned sound to");
    ImGui::SameLine(labelCol);
    FilteredDeviceCombo("Output", "Where your cleaned voice goes. CABLE Input is picked automatically\n"
                                  "so voice apps can use it as your microphone. With Listen to myself\n"
                                  "ticked this is the speakers you hear yourself through.",
                        g_outIndex, g_playbackDevices, g_outDisplayIndices);

    if (!CableInputPresent()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
            "VB-CABLE not found - install VB-CABLE and enable CABLE Input,");
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
            "then hit Refresh (or tick Listen to myself below).");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Free download: vb-audio.com/Cable/ (reboot after install).\n"
                              "Windows lists only enabled devices - a disabled CABLE Input\n"
                              "looks the same as not installed: check Sound settings too.");
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Listen to myself", &g_listenToSelf)) {
        // When Listen to myself is on, the output device should match
        // the Speaker Reference (your speakers) so you hear yourself
        // through the same speakers.
        if (g_listenToSelf) {
            g_outIndex = g_refIndex;
        }
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hear yourself through your speakers instead of sending\n"
                          "to voice apps - for testing. Untick to send your voice\n"
                          "to CABLE Input (Discord, Zoom, Teams) again.");

    ImGui::EndDisabled();
}

void DrawEngineSection() {
    // The engine only changes through the Profile menu; this section
    // carries the remaining profile-agnostic knob (sample rate) where
    // it has always lived, under More.
    ImGui::SeparatorText("Processing");
    const float labelCol = 190.0f;

    ImGui::BeginDisabled(g_isRunning);

    ImGui::TextUnformatted("Sample Rate");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-1);

    const char* rates[] = { "16000", "48000" };
    if (g_highbandEnabled.load()) {
        ImGui::TextDisabled("16000 (fixed by High-band splice; 48k out)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "High-band splice always runs the 16 kHz chain: the AEC and\n"
                "NS process 0-8 kHz, then the real 8-24 kHz mic band is\n"
                "spliced back on at the 48 kHz output.\n\n"
                "Untick the splice to return to the native chain rate (AEC3\n"
                "can run at 48000 Hz there; NS still needs 16 kHz).");
    } else if (g_engineIndex == ENGINE_NKF || g_engineIndex == ENGINE_DTLN) {
        ImGui::TextDisabled("16 kHz (automatic)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This profile always runs at 16 kHz - nothing to choose.");
    } else {
        if (ImGui::Combo("##rate", &g_sampleRateIndex, rates, IM_ARRAYSIZE(rates))) {
            g_sampleRate.store(atoi(rates[g_sampleRateIndex]));
            SaveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                (g_engineIndex == ENGINE_OFF)
                    ? "No echo canceller to rate.\n"
                      "16000 lets the NS stage run; 48000 passes the mic\n"
                      "through and skips NS (all backends are 16 kHz).\n"
                      "Turning the splice off with NS off selects 48000."
                    : "Higher = better quality, more CPU\nAEC3 best at 48000\n"
                      "Sticks with your setup until the engine forces 16 kHz.");
    }

    ImGui::EndDisabled();

    // NKF run-state telemetry: how the delay-compensation lock and the
    // self-monitor loop policy are doing while running.
    if (g_isRunning && g_engine.type == ENGINE_NKF && g_engine.nkf) {
        NkfState ns;
        NkfGetState(g_engine.nkf, &ns);
        ImGui::Spacing();
        char nkfLine[96];
        if (ns.giveUp)
            snprintf(nkfLine, sizeof nkfLine, "NKF: failed open (guard)");
        else if (!ns.locked)
            snprintf(nkfLine, sizeof nkfLine, "NKF: shadow (warm-up)");
        else if (!ns.exposed)
            snprintf(nkfLine, sizeof nkfLine, "NKF: shadow (settling)");
        else
            snprintf(nkfLine, sizeof nkfLine, "NKF: live - delay %d ms%s%s",
                     ns.lagSamples / 16,
                     ns.loopActive ? " (loop, adapting)" : "",
                     ns.confident ? " (locked)" : " (estimated)");
        ImGui::TextDisabled("%s", nkfLine);
        // Suppressor telemetry: when the Output meter collapses, one
        // of these lines names the stage doing it. (NS has its own
        // diagnostics under its checkbox.)
        if (ns.notchFreq > 0.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Notch %.0f Hz %+.1f dB",
                               (double)ns.notchFreq, (double)ns.notchDb);
        if (ns.loopDb < -0.5)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Loop trim: %.1f dB%s",
                               ns.loopDb,
                               ns.nocancelHot ? " (no-cancel)" : "");
        // Self-monitor loop, spelled out: with Listen-to-myself the
        // speakers replay our own output back into the mic AND the
        // loopback ref, so the engine fights a moving target — voice
        // sounds robotic and tails warble. Not an engine bug: untick
        // for clean audio (Listen-to-myself is a routing/levels check).
        if (ns.loopActive && g_listenToSelf)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Self-monitor loop: speakers feed your voice "
                               "back - untick Listen to myself for clean audio");
        if (ns.resDb < -0.5)
            ImGui::TextDisabled("RES suppression: %.1f dB", ns.resDb);
        if (ns.guardResets > 0)
            ImGui::TextDisabled("Guard resets: %d%s", ns.guardResets,
                                ns.giveUp ? " (failed open = mic passthrough)" : "");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Delay lock: how far the speaker reference lags the mic\n"
                "(playback chain). Locked = aligned reference lets NKF cancel.\n"
                "During a mic-test loop the engine keeps adapting - it must\n"
                "cancel the speaker pickup continuously or the loop howls.");
    }
}

void DrawGainsSection() {
    ImGui::SeparatorText("Levels");
    const float labelCol = 190.0f;

    float micGain = g_micGain.load() * 100.0f;
    ImGui::TextUnformatted("Microphone level");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-80);
    if (ImGui::SliderFloat("##micgain", &micGain, 0.0f, 200.0f, "%.0f%%")) {
        g_micGain.store(micGain / 100.0f);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", micGain);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How loud your voice goes out. Editable while running.");
}

void DrawAudioTab() {
    ImGui::SeparatorText("Profile");

    ImGui::TextUnformatted("Engine");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Echo engine selection (applies immediately):\n"
            "Off - no echo cancellation (mic passes straight to the\n"
            "noise-suppression stage); lowest CPU.\n"
            "DTLN-AEC 128 - default; best echo + reverb handling.\n"
            "WebRTC AEC3 - Chrome-style echo cancellation, reliable baseline.\n"
            "NKF-AEC (experimental) - weak CPUs, still full cleanup.\n"
            "The NS backend below runs after the engine.\n");
    // Engine name only — the full chain (engine -> NS -> output) is
    // shown once, in the chain line just above the devices section.
    bool wasRunning = g_isRunning;
    for (int r = 0; r < PROFILE_COUNT; r++) {
        int i = ENGINE_RADIO_ORDER[r];
        std::string label = PROFILES[i].name;
        bool sel = (g_profileIndex == i);
        // Other AEC engines are hidden unless the user ticked the
        // Appearance checkbox.
        if (!g_experimentalEngines.load() &&
            ((EngineType)PROFILES[i].engine == ENGINE_NKF ||
             (EngineType)PROFILES[i].engine == ENGINE_AEC3)) continue;
        if (r > 0) ImGui::SameLine();
        if (ImGui::RadioButton(label.c_str(), sel) && !sel) {
            ApplyProfile(i);
            SaveSettings();
            if (wasRunning) { StopAEC(); StartAEC(); }
        }
    }

    // Use-case guidance from the AEC harness: the radios stay names-only.
    ImGui::TextDisabled(
        "Loudspeaker: DTLN-AEC  |  sealed earphone/headset: Off");

    // Post stage: NS backend radio for every engine (Off / DTLN-NS /
    // Hush). Live restart on switch (same path as switching profiles
    // mid-call). Global pref: a profile switch does not reset it.
    // Experimental engines/backends live in exp/main.cpp (aec_exp).
    {
        int nsb = g_nsBackend.load();
        if (nsb < 0 || nsb > 3) nsb = 3;
        int prevNsb = nsb;
        ImGui::TextUnformatted("Noise suppression");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Noise suppression backend (runs after the engine):\n"
                "Off - raw echo-cancelled voice, lowest CPU.\n"
                "DTLN-NS - cheapest stage; steady hiss only, weak on\n"
                "keyboard clicks and background talkers.\n"
                "Hush - best on background talkers; strongest on clicks,\n"
                "heavier stage (needs weya_nc.dll +\n"
                "models/hush_16k_onnx.tar.gz).\n"
                "GTCRN - default; hiss + keyboard.\n"
                "(needs models/gtcrn_simple.onnx).");
        ImGui::RadioButton("Off", &nsb, 0);
        ImGui::SameLine();
        ImGui::RadioButton("DTLN-NS", &nsb, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Hush", &nsb, 2);
        ImGui::SameLine();
        ImGui::RadioButton("GTCRN (default)", &nsb, 3);
        if (nsb != prevNsb) {
            g_nsBackend.store(nsb);
            g_nsEnabled.store(nsb != 0);
            // 48 kHz output is the exp quality default. Every NS backend
            // is 16 kHz, so with the splice off a picked backend would
            // drop the output to 16 kHz. Turn the splice on so the output
            // stays 48 kHz.
            if (nsb != 0 && !g_highbandEnabled.load())
                g_highbandEnabled.store(true);
            SyncChainRate();
            SaveSettings();
            if (g_isRunning) { StopAEC(); StartAEC(); }
        }
        const char* nsHint =
            (nsb == 0) ? "Off - no noise suppression; lowest CPU." :
            (nsb == 1) ? "DTLN-NS - cheapest stage; steady hiss only." :
            (nsb == 2) ? "Hush - background talkers; heaviest of the three." :
                         "GTCRN - default; hiss + keyboard.";
        ImGui::TextDisabled("%s", nsHint);
        // A 48 kHz native chain (Off/AEC3, splice off) skips every NS
        // backend — say so before Start instead of letting the status
        // line claim a stage that will not run.
        if (nsb != 0 && !g_highbandEnabled.load() &&
            g_sampleRate.load() != 16000) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "NS skipped at 48 kHz: all NS backends are 16 kHz.");
            ImGui::TextDisabled(
                "Enable 48 kHz output (high-band splice) to keep NS.");
        }
        // Error text only (telemetry row stays deleted): a failing
        // stage is fail-open, so its message is all that's shown.
        const char* nsErr = "";
        if (nsb == 3 && g_engine.gtcrn) nsErr = GtcrnLastError(g_engine.gtcrn);
        else if (nsb == 2 && g_engine.hush) nsErr = HushLastError(g_engine.hush);
        else if (g_engine.ns) nsErr = DtnsLastError(g_engine.ns);
        if (nsErr && nsErr[0]) ImGui::TextDisabled("%s", nsErr);
    }

    // High-band splice: capture mic at 48 kHz, process the low band
    // (0-8 kHz) through the 16 kHz engine, then splice the real high
    // band (8-24 kHz) back on at the 48 kHz output. Always on by
    // default; unticking returns to the native chain rate (16 kHz for
    // DTLN/NKF/GTCRN, 48 kHz native for Off/AEC3).
    {
        bool hb = g_highbandEnabled.load();
        ImGui::Checkbox("48 kHz output (high-band splice)", &hb);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Outputs 48 kHz. The echo engine and NS still run at\n"
                "16 kHz (all NS backends are 16 kHz), so only the low band\n"
                "(0-8 kHz) is cleaned; the real 8-24 kHz mic band is\n"
                "spliced back on top. This is output fidelity, not 48 kHz\n"
                "echo cancellation. No synthesis, no metallic edge.\n\n"
                "Turning it off returns to the native chain rate: 48 kHz\n"
                "for Off/AEC3 with NS off, 16 kHz when NS must run.\n\n"
                "The 16 kHz NS models cannot reach above 8 kHz, so a broadband\n"
                "transient (keyboard click) would keep its sharp part. A\n"
                "dedicated high-band NS fixes that: STFT spectral subtraction\n"
                "with a decision-directed a priori SNR, over the 8-24 kHz bins\n"
                "only. Measured on real speech with a realistic click:\n"
                "click -11 dB, hiss -9 dB, voice -1 dB.\n\n"
                "The high band is also gated off entirely while the 16 kHz\n"
                "chain says 'no voice' — that removes hiss and clicks typed\n"
                "between words.\n\n"
                "48<->16 kHz uses a 193-tap linear-phase FIR pair: flat to\n"
                "7.5 kHz, aliases below -76 dB, images below -55 dB. The old\n"
                "3-tap/linear pair drooped -3.5 dB at 8 kHz and mirrored the\n"
                "low band's top into the air band.");
        if (hb != g_highbandEnabled.load()) {
            g_highbandEnabled.store(hb);
            SaveSettings();
            SyncChainRate();
            if (g_isRunning) { StopAEC(); StartAEC(); }
        }
        if (g_highbandEnabled.load()) {
            float mixPct = g_hbMix.load() * 100.0f;
            ImGui::SameLine();
            ImGui::PushItemWidth(120);
            if (ImGui::SliderFloat("##hbMix", &mixPct, 0.0f, 100.0f, "%.0f%%"))
                g_hbMix.store(mixPct / 100.0f);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::TextDisabled("High-band mix");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The spliced high band is raw mic, so its level is\n"
                    "taste, not a fixed law. Trade air against hiss/click\n"
                    "residue. 50% preserves the original splice balance;\n"
                    "70-100% gives more air after the FIR resampler fix.");
        }
        const int nsb = g_nsBackend.load();
        hb = g_highbandEnabled.load();
        const int chainRate =
            (hb || g_selectedEngine.load() == ENGINE_DTLN ||
             g_selectedEngine.load() == ENGINE_NKF)
                ? 16000 : g_sampleRate.load();
        const int outRate = hb ? 48000 : chainRate;
        // Every NS backend is 16 kHz. A native 48 kHz AEC3 chain runs
        // engine-only, so never claim a stage that is skipped.
        const bool nsSkipped = (nsb != 0 && chainRate != 16000);

        char nsSuffix[32] = "";
        if (!nsSkipped) {
            if (nsb == 1) snprintf(nsSuffix, sizeof nsSuffix, " -> DTLN-NS");
            else if (nsb == 2) snprintf(nsSuffix, sizeof nsSuffix, " -> Hush");
            else if (nsb == 3) snprintf(nsSuffix, sizeof nsSuffix, " -> GTCRN");
        }
        const char* engineName = PROFILES[g_profileIndex].name;

        char chain[192];
        if (hb)
            snprintf(chain, sizeof chain,
                     "Chain: Mic -> %s%s -> 48 kHz (HiBand splice)",
                     engineName, nsSuffix);
        else if (nsSkipped)
            snprintf(chain, sizeof chain,
                     "Chain: Mic -> %s -> %d kHz (NS skipped: 16 kHz only)",
                     engineName, outRate / 1000);
        else
            snprintf(chain, sizeof chain,
                     "Chain: Mic -> %s%s -> %d kHz",
                     engineName, nsSuffix, outRate / 1000);
        ImGui::TextDisabled("%s", chain);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The full voice path, in order:\n"
                "mic -> echo engine -> noise suppression (if enabled)\n"
                "-> hygiene (80 Hz HPF -> expander -> presence shelf)\n"
                "-> output.\n\n"
                "NS backends are 16 kHz only: a native 48 kHz AEC3 chain\n"
                "(splice off) skips that stage.\n\n"
                "With High-band splice the output is 48 kHz: the 16 kHz\n"
                "chain carries 0-8 kHz and the real mic highs (8-24 kHz)\n"
                "are spliced back on top, gated by the VAD and cleaned by\n"
                "the high-band NS.");
    }

    ImGui::Spacing();
    DrawDevicesSection();
    ImGui::Spacing();
    // More: sample rate + levels. No close-X — the header always
    // stays visible and toggles. First frame applies the persisted
    // default (closed for newcomers, as left for regulars).
    if (!g_advInitDone) {
        ImGui::SetNextItemOpen(g_advancedOpen);
        g_advInitDone = true;
    }
    bool advOpen = ImGui::CollapsingHeader("More");
    if (advOpen != g_advancedOpen) {
        g_advancedOpen = advOpen;
        SaveSettings();
    }
    if (advOpen) {
        DrawEngineSection();
        ImGui::Spacing();
        DrawGainsSection();
    }
}

void DrawAppearanceTab() {
    ImGui::SeparatorText("Wallpaper");
    const float labelCol = 190.0f;

    ImGui::TextUnformatted("Wallpaper");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-120);
    std::vector<const char*> names;
    for (auto& n : g_wallpaperNames) names.push_back(n.c_str());
    if (ImGui::Combo("##wallpaper", &g_wallpaperIndex, names.data(), (int)names.size())) {
        LoadWallpaperByIndex(g_wallpaperIndex);
        SaveSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        ScanWallpapers();
        if (g_wallpaperIndex >= (int)g_wallpaperNames.size()) g_wallpaperIndex = 0;
        LoadWallpaperByIndex(g_wallpaperIndex);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan wallpapers/ folder");

    ImGui::Spacing();
    ImGui::TextDisabled("Put .png / .jpg files in the wallpapers/ folder");

    ImGui::Spacing();
    ImGui::SeparatorText("Behavior");

    bool trayChecked = g_minimizeToTray;
    if (ImGui::Checkbox("Minimize to system tray (X button hides window)", &trayChecked)) {
        g_minimizeToTray = trayChecked;
        UpdateTrayIcon();
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When ON: X hides the window, tray icon stays active\n"
            "When OFF: X closes the app completely");

    if (g_minimizeToTray) {
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click the tray icon to Exit the app.");
    }

    // Experimental engines: NKF-AEC is hidden from the engine radio
    // by default (DTLN is the only thing most users need). Tick this
    // to show it — switching back to DTLN afterwards hides it again
    // and leaves the selection on DTLN. WebRTC AEC3 is a regular
    // choice now, always listed.
    bool exp = g_experimentalEngines.load();
    if ( ImGui::Checkbox("Show other AEC engines", &exp)) {
        g_experimentalEngines.store(exp);
        if (!exp) {
            // Hiding them: fall back to DTLN so the selection can't be
            // left on an engine that is no longer in the list.
            g_engineIndex = ENGINE_DTLN;
            g_selectedEngine.store(ENGINE_DTLN);
            SyncStagePrefsToEngine();
            g_profileIndex = 0;
        }
        SaveSettings();
        if (g_isRunning) { StopAEC(); StartAEC(); }
    }
    if ( ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Off (default): the engine radio shows Off and DTLN-AEC 128.\n"
            "On: also shows WebRTC AEC3 and NKF-AEC.");
}

void DrawSoundboardTab() {
    const float labelCol = 190.0f;

    ImGui::SeparatorText("Level");
    ImGui::TextUnformatted("Soundboard gain");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-120);
    float sbPct = g_soundboardGain.load() * 100.0f;
    if (ImGui::SliderFloat("##soundboardgain", &sbPct, 0.0f, 200.0f, "%.0f%%")) {
        g_soundboardGain.store(sbPct / 100.0f);
        SaveSettings();
    }
    ImGui::TextDisabled("Sounds play into the same stream as your mic, so voice\n"
                        "apps hear both. The Out meter measures mic only.");

    ImGui::Spacing();
    ImGui::SeparatorText("Monitor");

    bool mon = g_monitorEnabled.load();
    if (ImGui::Checkbox("Play soundboard to my speakers", &mon)) {
        g_monitorEnabled.store(mon);
        if (g_isRunning) { if (mon) StartMonitor(); else StopMonitor(); }
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Sends a copy of the soundboard to a speaker device so you hear\n"
            "it locally. Voice apps still get it through VB-CABLE.\n"
            "Needs the AEC to be running.");

    if (mon) {
        ImGui::TextUnformatted("Monitor to");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(-120);
        const char* monName = "(none)";
        if (g_monitorIndex == -1)
            monName = "Your speakers (follow)";
        else if (g_monitorIndex >= 0 && g_monitorIndex < (int)g_playbackDevices.size())
            monName = g_playbackDevices[g_monitorIndex].name;
        bool monChanged = false;
        if (ImGui::BeginCombo("##mondev", monName)) {
            if (ImGui::Selectable("Your speakers (follow)", g_monitorIndex == -1)) {
                g_monitorIndex = -1;
                monChanged = true;
            }
            for (int di : g_refDisplayIndices) {
                if (di < 0 || di >= (int)g_playbackDevices.size()) continue;
                const bool sel = (g_monitorIndex == di);
                if (ImGui::Selectable(g_playbackDevices[di].name, sel)) {
                    g_monitorIndex = di;
                    monChanged = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Where the local copy is played.\n"
                "When Listen to myself is on, the monitor uses the output\n"
                "device (your speakers). Otherwise the monitor follows the\n"
                "reference device chosen on the Audio tab.");
        if (monChanged) {
            SaveSettings();
            if (g_isRunning) { StopMonitor(); StartMonitor(); }
        }
        // With Listen to myself the monitor is the same as the output
        // device, so the combo is hidden and the tooltip explains why.
        if (g_listenToSelf)
            ImGui::TextDisabled("Monitor follows your speakers.");
        else
            ImGui::TextDisabled("Monitor device (local soundboard copy).");

        ImGui::TextUnformatted("Monitor volume");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(-120);
        float monPct = g_monitorGain.load() * 100.0f;
        if (ImGui::SliderFloat("##mongain", &monPct, 0.0f, 200.0f, "%.0f%%")) {
            g_monitorGain.store(monPct / 100.0f);
            SaveSettings();
        }

        if (g_listenToSelf)
            ImGui::TextDisabled("Listen to myself is on: the soundboard already reaches\n"
                                "your speakers through the main output, so the monitor is\n"
                                "paused to avoid playing it twice.");
        else if (!g_isRunning)
            ImGui::TextDisabled("Press Start to hear the monitor.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Sounds");

    // Panic stop for long files: one button instead of hunting for
    // whichever pad is still green.
    bool anyPlaying = false;
    for (int i = 0; i < (int)g_sounds.size() && i < MAX_SOUNDS; i++) {
        if (g_soundPlaying[i].load()) { anyPlaying = true; break; }
    }

    // Rescan is stop-only: the playing voices hold pointers into the
    // sound list, which must not move while the audio thread walks it.
    if (!g_isRunning) {
        if (ImGui::Button("Refresh")) ScanSounds();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan the sounds/ folder");
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Refresh");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop the AEC to rescan the sounds/ folder");
    }

    if (anyPlaying) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.20f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.26f, 0.26f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.42f, 0.14f, 0.14f, 1.00f));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!anyPlaying);
    if (ImGui::Button("Stop all")) g_soundStopAll.store(true);
    ImGui::EndDisabled();
    if (anyPlaying) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Silence every sound currently playing");

    ImGui::SameLine();
    ImGui::TextDisabled("%d sound(s) - put .wav / .mp3 / .flac / .ogg in the sounds/ folder",
                        (int)g_sounds.size());

    if (g_sounds.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Create a sounds/ folder next to the exe and drop audio\n"
                            "files into it, then press Refresh.");
        return;
    }

    if (!g_isRunning) {
        ImGui::Spacing();
        ImGui::TextDisabled("Press Start to use the pads - sounds are played through\n"
                            "the active output device (VB-CABLE).");
    }

    ImGui::Spacing();
    const float padW = 140.0f;
    int cols = (int)(ImGui::GetContentRegionAvail().x / (padW + 10.0f));
    if (cols < 1) cols = 1;

    for (int i = 0; i < (int)g_sounds.size(); i++) {
        if (i % cols != 0) ImGui::SameLine();

        // Green while sounding so it is obvious which pad is live and
        // which click will stop it. PushID (not a ### suffix) so a
        // long filename cannot truncate the ID into a collision.
        const bool playing = g_soundPlaying[i].load();
        if (playing) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.45f, 0.30f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.36f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f, 0.38f, 0.25f, 1.00f));
        }
        ImGui::PushID(i);
        ImGui::BeginDisabled(!g_isRunning);
        if (ImGui::Button(g_sounds[i].name.c_str(), ImVec2(padW, 0))) {
            g_soundRequest[i] = playing ? (uint8_t)2 : (uint8_t)1;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        if (playing) ImGui::PopStyleColor(3);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", g_sounds[i].path.c_str(),
                              playing ? "Playing - click to stop" : "Click to play");
    }
}

void DrawAboutTab() {
    ImGui::SeparatorText("About");

    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), APP_NAME);
    ImGui::Text("Version %s", APP_VERSION);
    ImGui::Spacing();

    ImGui::TextWrapped(
        "A lightweight, open-source acoustic echo cancellation (AEC) client "
        "for Windows. Route your microphone through it and pick up a cleaned, "
        "echo-free signal in any app (Discord, Zoom, Teams, etc.).");

    ImGui::Spacing();
    ImGui::SeparatorText("Features");
    ImGui::BulletText("Four engine choices (Off / DTLN / WebRTC AEC3 / NKF-AEC-exp)");
    ImGui::BulletText("80 Hz high-pass on every path (hum/rumble/DC)");
    ImGui::BulletText("Gentle expander (digital-black floor, no onset chop)");
    ImGui::BulletText("Noise suppression on every engine (Off / DTLN-NS / Hush / GTCRN)");
    ImGui::BulletText("High-band splice: 48 kHz output, real 8-24 kHz mic highs");
    ImGui::BulletText("Real-time processing with low CPU usage");
    ImGui::BulletText("Works with speakers, earphones, and headsets");
    ImGui::BulletText("Selectable sample rate (16 / 48 kHz)");
    ImGui::BulletText("Live level meters");
    ImGui::BulletText("Soundboard - play sounds over your voice");
    ImGui::BulletText("Optional minimize to system tray");

    ImGui::Spacing();
    ImGui::SeparatorText("Credits");
    ImGui::BulletText("WebRTC AP - Google (BSD-3)");
    ImGui::BulletText("NKF-AEC - Jiang et al. (ICASSP 2023, MIT)");
    ImGui::BulletText("DTLN-AEC - Westhausen & Meyer (ICASSP 2021, MIT)");
    ImGui::BulletText("DTLN-NS - networkedaudio port of breizhn/DTLN denoise (MIT)");
    ImGui::BulletText("Hush - pulp-vision (Apache-2.0)");
    ImGui::BulletText("GTCRN - Xiaobin Rong et al. (ICASSP 2024, MIT)");
    ImGui::BulletText("ONNX Runtime - Microsoft (MIT)");
    ImGui::BulletText("Dear ImGui - Omar Cornut (MIT)");
    ImGui::BulletText("miniaudio - David Reid (MIT-0)");
    ImGui::BulletText("GLFW - Marcus Geelnard / Camilla Berglund (zlib)");
    ImGui::BulletText("stb_image - Sean Barrett (public domain)");
    ImGui::BulletText("pocketfft - Max-Planck-Society (BSD-3)");

    ImGui::Spacing();
    ImGui::TextDisabled("Made with C++ and MinGW-w64");
}

// ============================================================
//  Main UI
// ============================================================
void DrawUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin(APP_NAME, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));

    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), APP_NAME);
    ImGui::SameLine();
    ImGui::TextDisabled("v" APP_VERSION);
    ImGui::SameLine(vp->WorkSize.x - 200);
    DrawStatusDot(g_isRunning);
    ImGui::SameLine();
    if (g_isRunning)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running %s", FormatUptime().c_str());
    else
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Idle");

    ImGui::Spacing();

    static int activeTab = 0;  // frame-local: which tab is open (footer buttons follow it)
    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            activeTab = 0;
            ImGui::Spacing();
            DrawAudioTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Soundboard")) {
            activeTab = 1;
            ImGui::Spacing();
            DrawSoundboardTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            activeTab = 2;
            ImGui::Spacing();
            DrawAppearanceTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            activeTab = 3;
            ImGui::Spacing();
            DrawAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Footer buttons (Start / Reset) live on the Audio and Soundboard
    // tabs. Live Levels below stay visible on every tab while running.
    if (activeTab == 0 || activeTab == 1) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        std::string startLabel = g_isRunning
            ? ("Stop " + FormatUptime())
            : std::string("Start");

        if (g_isRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.30f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button(startLabel.c_str(), ImVec2(160, 36))) StopAEC();
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.70f, 0.33f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.45f, 0.20f, 1.0f));
            if (ImGui::Button(startLabel.c_str(), ImVec2(160, 36))) StartAEC();
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults", ImVec2(160, 36))) ResetToDefaults();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore all settings to default");

        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_statusText);
    }

    if (g_isRunning) {
        ImGui::Spacing();
        ImGui::SeparatorText("Live Levels");

        float micRms = g_last_mic_rms.load();
        float refRms = g_last_ref_rms.load();
        float outRms = g_last_out_rms.load();
        float db     = g_last_reduction_db.load();

        DrawLevelMeter("Mic", micRms, 3000.0f);
        DrawLevelMeter("Ref", refRms, 3000.0f);
        DrawLevelMeter("Out", outRms, 3000.0f);

        // Self-diagnosis aid: a permanently silent reference means the
        // wrong speakers are selected — but silence is also normal when
        // nothing plays, so this informs, never warns.
        ImGui::Spacing();
        if (refRms > 5.0f)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "Reference: receiving speaker audio");
        else
            ImGui::TextDisabled("Reference: silent (normal if nothing is playing - \n"
                                "if speakers ARE playing, re-check Your speakers above)");

        ImGui::Spacing();
        if (db < 0.0f)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "Echo reduction: %.1f dB", db);
        else
            ImGui::TextDisabled("Echo reduction: -- dB (silent)");

    }
    } // end footer block

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ============================================================
//  MAIN
// ============================================================
// Crash handlers — plain functions (set_terminate takes a function
// pointer, so no capturing lambdas). Any uncaught exception in the
// audio thread writes the failing expression to aec_crash.txt next
// to the exe, so the user has something to report instead of a silent
// exit.
static void CrashLog(const char* what) {
    FILE* f = fopen("aec_crash.txt", "a");
    if (f) {
        SYSTEMTIME st;
        GetSystemTime(&st);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, what);
        fclose(f);
    }
}
static void CrashOnTerminate() {
    CrashLog("std::terminate: uncaught exception in the audio thread");
    std::abort();
}
static LONG CrashOnUnhandledException(LPEXCEPTION_POINTERS ep) {
    char buf[160];
    snprintf(buf, sizeof buf,
             "Unhandled exception 0x%08X at 0x%p",
             ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    CrashLog(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int, char**) {
#ifdef _WIN32
    // Anchor all relative asset paths (models/, aec_config.txt, imgui.ini,
    // wallpapers/) to the exe directory, so the app works no matter which
    // folder it is launched from (shell CWD, shortcut "Start in", ...).
    // Fail-open: if the exe path can't be determined, keep the launch CWD
    // (previous behavior).
    {
        wchar_t exePath[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::error_code ec;
            std::filesystem::current_path(
                std::filesystem::path(exePath).parent_path(), ec);
        }
    }
    if (!AcquireSingleInstance()) {
        return 0;   // Another instance is already running
    }
#endif

    // Crash handler: an uncaught exception in the audio callback can
    // otherwise silently take the app down with no trace. Write the
    // failing expression to aec_crash.txt next to the exe so the user
    // has something to report. (set_terminate takes a non-capturing
    // function pointer, so the handlers are plain functions — defined
    // at file scope above main().)
    std::set_terminate(CrashOnTerminate);
    SetUnhandledExceptionFilter(CrashOnUnhandledException);
    // High-band resampler prototype: pure computation, no allocation.
    // Must run before any audio device starts (ReinitEngine uses it).
    rs::Init();

    if (!glfwInit()) return 1;
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(700, 720, APP_NAME " v" APP_VERSION, nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Install custom WndProc for tray behavior
    SetupTray(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.FrameRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.TabRounding       = 6.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 10);
    s.WindowPadding     = ImVec2(20, 18);
    s.Colors[ImGuiCol_WindowBg]  = ImVec4(0.06f, 0.06f, 0.08f, 0.82f);
    s.Colors[ImGuiCol_FrameBg]   = ImVec4(0.14f, 0.14f, 0.18f, 0.90f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.26f, 0.95f);
    s.Colors[ImGuiCol_FrameBgActive]  = ImVec4(0.24f, 0.24f, 0.30f, 1.00f);
    s.Colors[ImGuiCol_Header]    = ImVec4(0.20f, 0.30f, 0.50f, 0.85f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.40f, 0.65f, 0.90f);
    s.Colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.28f, 0.35f, 0.60f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    ScanWallpapers();
    ScanSounds();
    LoadSettings();     // loads g_minimizeToTray too
    if (g_wallpaperIndex >= (int)g_wallpaperNames.size()) g_wallpaperIndex = 0;
    LoadWallpaperByIndex(g_wallpaperIndex);

    // Now that g_minimizeToTray is loaded, show or hide tray icon accordingly
    UpdateTrayIcon();

    ReinitEngine();
    EnumerateDevices();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawUI();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        RenderWallpaper(w, h);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // When hidden to tray, throttle the render loop to save CPU
#ifdef _WIN32
        if (g_hwnd && !IsWindowVisible(g_hwnd)) {
            Sleep(50);
        }
#endif
    }

    SaveSettings();
    StopAEC();
    if (g_engine.aec3)  Aec3Destroy(g_engine.aec3);
    if (g_engine.nkf)   NkfDestroy(g_engine.nkf);
    if (g_engine.dtln)  DtlnDestroy(g_engine.dtln);
    if (g_engine.ns)   DtlnNsDestroy(g_engine.ns);
    if (g_engine.hush) HushDestroy(g_engine.hush);
    if (g_contextInitialized) {
        ma_context_uninit(&g_context);
        g_contextInitialized = false;
    }
    if (g_wallpaperTex) glDeleteTextures(1, &g_wallpaperTex);

#ifdef _WIN32
    HideTrayIcon();
#endif

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    #ifdef _WIN32
    ReleaseSingleInstance();
#endif
    return 0;
}
