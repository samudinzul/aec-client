#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// --- Windows tray support ---
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "libaec.h"
#include "aec3_wrapper.h"
#include "nkf_wrapper.h"
#include "localvqe_wrapper.h"
#include "dtln_wrapper.h"
#include "pvad_wrapper.h"
#include "silero_wrapper.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <chrono>
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
#define APP_VERSION "1.3.2"

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
enum EngineType { ENGINE_SPEEX = 0, ENGINE_AEC3 = 1, ENGINE_NKF = 2, ENGINE_LOCALVQE = 3, ENGINE_DTLN = 4 };
struct EngineState {
    EngineType  type  = ENGINE_SPEEX;
    Aec*        speex = nullptr;
    Aec3Handle* aec3  = nullptr;
    NkfHandle*  nkf   = nullptr;
    LocalVqeHandle* localvqe = nullptr;
    DtlnHandle* dtln  = nullptr;
};

// ============================================================
//  Presets
// ============================================================
struct Preset {
    const char* name;
    int   engine;
    int   sampleRateIdx;
    int   filterIdx;
    bool  preprocess;  // retired, always false (kept for initializer shape)
    float micGain;
    float outGain;
};

static const Preset PRESETS[] = {
    { "Manual settings",      1, 1, 1, false, 1.00f, 1.00f },
    { "Discord (recommended)",1, 1, 2, false, 1.00f, 1.00f },
    { "Low CPU",              0, 0, 0, false, 1.00f, 1.00f },
    { "High Quality",         1, 1, 4, false, 1.00f, 1.00f },
    { "Noisy Room",           0, 0, 3, false, 1.20f, 1.00f },
    { "Echo-Heavy Room",      1, 1, 2, false, 1.00f, 1.00f },
};
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ============================================================
//  Globals
// ============================================================
EngineState g_engine;

// ============================================================
//  Silero voice gate (neural VAD -> fixed gate -> output)
//  Post-AEC: speech passes, silence is muted on every engine.
//  SpeexDSP runs at 16 kHz only so the detector eats natively.
//  Live at 16 kHz direct and 48 kHz via internal downsample
//  (feed only). g_vadGain/g_vadHang live on the audio thread;
//  UI only reads the atomics.
// ============================================================
SileroHandle*      g_vad = nullptr;  // (re)created in ReinitEngine (rate-agnostic; feed is 16 kHz)
// Personalized gate (identity layer): lazy handle, worker-thread verify.
// Audio thread touches ONLY the atomics below — never the wrapper mutex
// (the worker can hold it ~90 ms per inference).
PvadHandle*        g_pvad = nullptr;
std::atomic<bool>  g_pvadEnabled{ false };  // persisted, default off
std::atomic<bool>  g_pvadReady{ false };    // voiceprint present (cached)
std::atomic<float> g_pvadMatch{ -2.0f };    // last worker cosine; < -1.5 = none yet
std::atomic<float> g_pvadThreshold{ PVAD_DEFAULT_THRESHOLD };
std::atomic<bool>  g_pvadEnrolling{ false };
std::atomic<int>   g_pvadEnrollSamples{ 0 };
std::thread        g_pvadThread;
std::atomic<bool>  g_pvadRun{ false };
std::mutex         g_pvadHistMtx;
std::vector<float> g_pvadHist;  // mic history for worker snapshots (cap 3 s)
#define PVAD_NEED_SAMPLES 128000   // 8 s enrollment
#define PVAD_VERIFY_SAMPLES 32000  // 2 s verify window
#define PVAD_HIST_CAP 48000        // 3 s history
ma_device g_enrollMicDevice, g_enrollOutDevice;
bool g_enrollMicOn = false, g_enrollMonOn = false;
bool g_enrollOwnedAudio = false;  // enrollment opened devices itself
Clock::time_point g_enrollStart;
std::atomic<bool>  g_vadEnabled{ true };  // default ON, zero-click
std::atomic<float> g_vadProb{ 0.0f };     // last chunk probability, for display
std::atomic<float> g_vadMs{ 0.0f };       // last inference cost in ms, for display
std::atomic<float> g_vadOpen{ 0.50f };    // calibrated open threshold (persisted)
std::atomic<float> g_vadClose{ 0.30f };   // calibrated close threshold (persisted)
std::atomic<bool>  g_vadCalibrating{ false };  // one-tap calibration in progress
Clock::time_point  g_vadCalStart;          // UI thread only
std::vector<float> g_vadCalSamples;        // UI thread only (prob samples)
std::string        g_vadCalMsg;            // result / error line, UI thread only
bool               g_vadCalMsgIsErr = false;
bool               g_vadCalWasRunning = false;  // restore idle after auto-started calibration
bool               g_calMonitor = false;  // one-shot: next StartAEC routes to speakers (calibration monitor)
#define VAD_CAL_SECONDS 5
float              g_vadGain = 1.0f;      // audio thread only
int                g_vadHang = 0;         // audio thread only (500 ms hangover)
ma_data_converter  g_vadResampler;        // 48 kHz -> 16 kHz for the VAD feed
bool               g_vadResamplerReady = false;  // (un)init with the audio idle
ma_data_converter  g_pvadResampler;       // engine rate -> 16 kHz for PVAD
bool               g_pvadResamplerReady = false;
ma_device g_micDevice, g_loopbackDevice, g_outputDevice;
ma_context g_context;
static bool g_contextInitialized = false;
const int MAX_FRAME_SIZE = 480;
SpscRing<4096> g_micRing, g_refRing;
bool g_isRunning = false;
Clock::time_point g_sessionStart;

std::atomic<int>   g_sampleRate(48000);
std::atomic<int>   g_selectedEngine(ENGINE_AEC3);
std::atomic<int>   g_filterLengthMs(50);
std::atomic<bool>  g_enablePreprocess(false);
std::atomic<float> g_micGain(1.0f);
std::atomic<float> g_outputGain(1.0f);
std::atomic<float> g_last_reduction_db(0.0f);
std::atomic<float> g_last_mic_rms(0.0f), g_last_ref_rms(0.0f), g_last_out_rms(0.0f);

std::atomic<float> g_peakMic(0.0f), g_peakRef(0.0f), g_peakOut(0.0f);
Clock::time_point g_peakMicTime, g_peakRefTime, g_peakOutTime;

std::vector<ma_device_info> g_captureDevices, g_playbackDevices;

// Display filter lists (indices into the full arrays above)
std::vector<int> g_micDisplayIndices;
std::vector<int> g_refDisplayIndices;
std::vector<int> g_outDisplayIndices;

int  g_micIndex = 0, g_refIndex = 0, g_outIndex = 0;
bool g_listenToSelf = false;  // self-monitor: route cleaned mic to Speaker Reference instead of Output
bool g_isFirstRun = false;      // no config file at launch: show the setup card
bool g_sessionStartedOnce = false;  // setup card retires after first Start
bool g_advancedOpen = false;    // Advanced collapse state (persisted)
bool g_advInitDone = false;     // first-frame default apply (see DrawAudioTab)
bool g_forgetArmed = false;     // Forget-my-voice two-step confirm
Clock::time_point g_forgetArmTime;
int  g_engineIndex = 1, g_sampleRateIndex = 1, g_filterIndex = 2;
int  g_presetIndex = 1;
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

// ============================================================
//  System tray (Windows)
// ============================================================
#ifdef _WIN32

void ShowTrayIcon() {
    if (g_trayIconActive || !g_hwnd) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"AEC Client — running");
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
//  Settings
// ============================================================
void SaveSettings() {
    std::ofstream f("aec_config.txt");
    if (!f.is_open()) return;
    f << g_micIndex << "\n" << g_refIndex << "\n" << g_outIndex << "\n"
      << g_engineIndex << "\n" << g_sampleRateIndex << "\n" << g_filterIndex << "\n"
      << g_preprocessEnabled << "\n"
      << (int)(g_micGain.load() * 100) << "\n"
      << (int)(g_outputGain.load() * 100) << "\n"
      << g_wallpaperIndex << "\n"
      << g_presetIndex << "\n"
       << (g_minimizeToTray ? 1 : 0) << "\n"
       << (g_vadEnabled.load() ? 1 : 0) << "\n"
       << (g_listenToSelf ? 1 : 0) << "\n"
       << (g_advancedOpen ? 1 : 0) << "\n"
       << 0 << "\n"  // retired: voice detector (kept for file alignment)
       << (g_pvadEnabled.load() ? 1 : 0) << "\n"
       << g_vadOpen.load() << "\n"
       << g_vadClose.load() << "\n";
}

void LoadSettings() {
    std::ifstream f("aec_config.txt");
    g_isFirstRun = !f.is_open();
    g_advancedOpen = !g_isFirstRun;  // newcomers start simple; regulars keep full view
    if (!f.is_open()) return;
    int mg, og;
    if (f >> g_micIndex >> g_refIndex >> g_outIndex
          >> g_engineIndex >> g_sampleRateIndex >> g_filterIndex
          >> g_preprocessEnabled >> mg >> og) {
        g_micGain.store(mg / 100.0f);
        g_outputGain.store(1.0f);  // single visible level: output stage fixed
        // Rates are {16000, 48000}: old index 1 (32 kHz, removed) and any
        // out-of-range index land on 48 kHz — no stranded configs.
        if (g_sampleRateIndex == 0) { g_sampleRate.store(16000); }
        else { g_sampleRateIndex = 1; g_sampleRate.store(48000); }
        // SpeexDSP is 16 kHz only now: old Speex-at-48 kHz configs land
        // on 16 kHz automatically (gate eats natively there).
        if (g_engineIndex == ENGINE_SPEEX) { g_sampleRateIndex = 0; g_sampleRate.store(16000); }
        if (g_engineIndex < ENGINE_SPEEX || g_engineIndex > ENGINE_DTLN)
            g_engineIndex = ENGINE_AEC3;
        g_selectedEngine.store(g_engineIndex);
        g_enablePreprocess.store(g_preprocessEnabled);
        // Speex cleanup retired: old configs with it on land on gate-only.
        g_preprocessEnabled = false;
        g_enablePreprocess.store(false);
        int wp = -1;
        if (f >> wp) g_wallpaperIndex = wp;
        int pi = -1;
        if (f >> pi) g_presetIndex = pi;

        // New field — default true if missing (backward compat with old config)
        int mt = 1;
        if (f >> mt) g_minimizeToTray = (mt != 0);

        // New field — voice gate defaults ON if missing
        int ve = 1;
        if (f >> ve) g_vadEnabled.store(ve != 0);

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
        // New field — owner-only voice defaults OFF if missing
        int po = 0;
        if (f >> po) g_pvadEnabled.store(po != 0);
        // New fields — calibrated gate thresholds (defaults 0.50/0.30)
        float vo = 0.50f, vc = 0.30f;
        if (f >> vo) {
            if (vo >= 0.10f && vo <= 0.90f) g_vadOpen.store(vo);
            if (f >> vc) {
                if (vc >= 0.05f && vc < g_vadOpen.load()) g_vadClose.store(vc);
            }
        }
    }
}

void ResetToDefaults() {
    g_micIndex = 0; g_refIndex = 0; g_outIndex = 0;
    g_engineIndex = 1; g_sampleRateIndex = 1; g_filterIndex = 2;
    g_presetIndex = 1;
    g_preprocessEnabled = false;
    g_minimizeToTray = true;
    g_vadEnabled.store(true);
    g_vadOpen.store(0.50f);
    g_vadClose.store(0.30f);
    g_vadCalibrating.store(false);
    g_vadCalSamples.clear();
    g_vadCalMsg.clear();
    g_vadCalWasRunning = false;
    g_listenToSelf = false;
    g_micGain.store(1.0f);
    g_outputGain.store(1.0f);
    g_advancedOpen = true;  // resetting users want the full controls
    g_advInitDone = false;  // re-apply the default next frame
    g_sampleRate.store(48000);
    g_selectedEngine.store(ENGINE_AEC3);
    g_filterLengthMs.store(50);
    g_enablePreprocess.store(false);
    g_wallpaperIndex = 0;
    LoadWallpaperByIndex(0);
    UpdateTrayIcon();
    SaveSettings();
}

void ApplyPreset(int idx) {
    if (idx <= 0 || idx >= PRESET_COUNT) return;
    const Preset& p = PRESETS[idx];

    g_engineIndex      = p.engine;
    g_sampleRateIndex  = p.sampleRateIdx;
    g_filterIndex      = p.filterIdx;
    g_preprocessEnabled= false;  // retired: gate does the silencing

    static const int rates[] = { 16000, 48000 };
    static const int filters[] = { 30, 50, 80, 120, 200 };

    g_selectedEngine.store(p.engine);
    g_sampleRate.store(rates[p.sampleRateIdx]);
    g_filterLengthMs.store(filters[p.filterIdx]);
    g_enablePreprocess.store(false);  // retired: gate does the silencing
    g_micGain.store(p.micGain);
    g_outputGain.store(1.0f);  // single visible level: output stage fixed
    g_presetIndex = idx;
}

// ============================================================
//  Engine init
// ============================================================
void ReinitEngine() {
    if (g_engine.speex) { AecDestroy(g_engine.speex); g_engine.speex = nullptr; }
    if (g_engine.aec3)  { Aec3Destroy(g_engine.aec3); g_engine.aec3  = nullptr; }
    if (g_engine.nkf)   { NkfDestroy(g_engine.nkf);   g_engine.nkf   = nullptr; }
    if (g_engine.localvqe) { LocalVqeDestroy(g_engine.localvqe); g_engine.localvqe = nullptr; }
    if (g_engine.dtln)  { DtlnDestroy(g_engine.dtln);  g_engine.dtln  = nullptr; }

    EngineType eng = (EngineType)g_selectedEngine.load();

    // SpeexDSP joins the 16 kHz-only club: the voice gate eats natively
    // instead of off a downsampled feed, which is where it under-scored.
    if (eng == ENGINE_SPEEX || eng == ENGINE_NKF || eng == ENGINE_LOCALVQE || eng == ENGINE_DTLN) {
        g_sampleRate.store(16000);
        g_sampleRateIndex = 0;
    }

    int sr = g_sampleRate.load();
    int fs = frameSizeForRate(sr);

    if (eng == ENGINE_SPEEX) {
        int filterLen = sr * g_filterLengthMs.load() / 1000;
        // Preprocess (cleanup) permanently off: the Silero gate does the
        // silencing now, and the two stacked sounded aggressive.
        g_engine.speex = AecNew(fs, filterLen, sr, false);
        g_engine.type  = ENGINE_SPEEX;
    } else if (eng == ENGINE_AEC3) {
        g_engine.aec3 = Aec3New(sr, fs);
        g_engine.type = ENGINE_AEC3;
    } else if (eng == ENGINE_NKF) {
        g_engine.nkf = NkfNew("models/nkf.onnx");
        g_engine.type = ENGINE_NKF;
    } else if (eng == ENGINE_LOCALVQE) {
        g_engine.localvqe = LocalVqeNew("models/localvqe-v1.4-aec-200K-f32.gguf");
        g_engine.type = ENGINE_LOCALVQE;
    } else if (eng == ENGINE_DTLN) {
        g_engine.dtln = DtlnNew("models/dtln_aec_512");
        g_engine.type = ENGINE_DTLN;
    }

    // Voice gate models are rate-agnostic (16 kHz direct, 48 kHz via
    // internal downsample in VadGateApply).
    if (g_vad) { SileroDestroy(g_vad); g_vad = nullptr; }
    g_vad = SileroNew("models/silero_vad.onnx");
    // Fail-open: a broken handle (missing model) must read as gate-off,
    // never as eternal silence. Null it when unusable.
}

// Reset VAD state + gate. Call on Start/Stop/engine change (audio idle).
static void VadReset() {
    if (g_vad) SileroReset(g_vad);
    if (g_vadResamplerReady) {
        ma_data_converter_uninit(&g_vadResampler, NULL);
        g_vadResamplerReady = false;
    }
    if (g_pvadResamplerReady) {
        ma_data_converter_uninit(&g_pvadResampler, NULL);
        g_pvadResamplerReady = false;
    }
    g_vadGain = 1.0f;
    g_vadHang = 0;
    g_vadProb.store(0.0f);
}

// Audio-thread gate. Called from output_callback after AEC, before
// metering so meters show what Discord hears. Lock-free, no allocation.
// Detector (Silero, 32 ms cadence) feeds a pure
// neural decision with fixed hysteresis — open at 0.50, close at 0.30
// (uncertain band holds the last decision). Same pair on every engine.
// ~30 ms fade open, ~50 ms fade to hard mute, 300 ms hangover against
// clipping word tails.
static void VadGateApply(int16_t* cleaned, int fs) {
    int sr = g_sampleRate.load();
    if (!g_vad || (sr != 16000 && sr != 48000) || !g_vadEnabled.load()) {
        g_vadGain = 1.0f;
        g_vadHang = 0;
        return;
    }
    static float fbuf[MAX_FRAME_SIZE];
    for (int i = 0; i < fs; i++) fbuf[i] = cleaned[i] / 32768.0f;
    // Silero eats 16 kHz: direct at 16 kHz, miniaudio downsample at 48 kHz
    // (proven: resampled feed scores 0.945/0.008 vs native 0.956/0.010).
    static float fbuf16[192];
    const float* feed = fbuf;
    int feedN = fs;
    if (sr == 48000) {
        if (!g_vadResamplerReady) {
            ma_data_converter_config cfg = ma_data_converter_config_init(
                ma_format_f32, ma_format_f32, 1, 1, 48000, 16000);
            if (ma_data_converter_init(&cfg, NULL, &g_vadResampler) != MA_SUCCESS) {
                g_vadGain = 1.0f;
                g_vadHang = 0;
                return;  // fail-open
            }
            g_vadResamplerReady = true;
        }
        ma_uint64 inCount = (ma_uint64)fs, outCount = 192;
        if (ma_data_converter_process_pcm_frames(&g_vadResampler, fbuf, &inCount,
                                                 fbuf16, &outCount) != MA_SUCCESS ||
            outCount == 0) {
            return;  // hold last decision this frame (fail-soft)
        }
        feed = fbuf16;
        feedN = (int)outCount;
    }
    static float lastProb = 0.0f;
    float prob = 0.0f;
    auto t0 = Clock::now();
    if (SileroPush(g_vad, feed, feedN, &prob)) {
        lastProb = prob;
        g_vadProb.store(prob);
        float ms = (float)std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - t0).count() / 1000.0f;
        g_vadMs.store(ms);
    }
    // Calibrated hysteresis (defaults 0.50/0.30). One-tap calibration
    // rewrites these from measured speech/silence probs — never drifting
    // mid-call, so the v1.3.1 adaptive-mute failure can't recur.
    // Dynamics: 500 ms hangover (don't clip endings/pauses), fast
    // attack (fully open in ~40 ms), gentle release, plus pre-open
    // creep through the uncertain band so onsets are already partway
    // open by the time the open line trips.
    const float kVadOpen = g_vadOpen.load(), kVadClose = g_vadClose.load();
    if (lastProb >= kVadOpen) {
        g_vadHang = 50;
        g_vadGain += (1.0f - g_vadGain) * 0.5f;
        if (g_vadGain > 0.99f) g_vadGain = 1.0f;
    } else if (lastProb <= kVadClose) {
        if (g_vadHang > 0) {
            g_vadHang--;
        } else {
            g_vadGain += (0.0f - g_vadGain) * 0.15f;
            if (g_vadGain < 0.01f) g_vadGain = 0.0f;
        }
    } else {
        // Uncertain band (prob ramping at an onset, or dipping
        // mid-speech): hold the hangover and lean open — speech passes
        // sooner, pauses don't chop. True silence still reads below
        // the close line and releases as before.
        if (g_vadHang > 0) g_vadHang--;
        g_vadGain += (1.0f - g_vadGain) * 0.08f;
        if (g_vadGain > 0.99f) g_vadGain = 1.0f;
    }
    // Identity layer (owner-only mode): the VAD above decides WHEN,
    // the worker's cosine decides WHO. Fail-open on any uncertainty —
    // mute only on a measured mismatch. Atomics only here; the worker
    // may hold the wrapper mutex for ~90 ms per check.
    if (g_pvadEnabled.load() && g_pvadReady.load()) {
        float m = g_pvadMatch.load();
        if (m > -1.5f && m < g_pvadThreshold.load()) {
            g_vadGain += (0.0f - g_vadGain) * 0.15f;
            if (g_vadGain < 0.01f) g_vadGain = 0.0f;
        }
    }
    if (g_vadGain < 1.0f) {
        for (int i = 0; i < fs; i++)
            cleaned[i] = clamp_s16((int)((float)cleaned[i] * g_vadGain));
    }
}

// ============================================================
//  Audio callbacks
// ============================================================
void mic_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    g_micRing.write((const int16_t*)pInput, frameCount);
    // Owned enrollment (app idle — output_callback isn't running):
    // feed + count here. The live path counts in output_callback.
    if (g_pvadEnrolling.load() && !g_isRunning && g_pvad && g_enrollOwnedAudio) {
        const int16_t* s = (const int16_t*)pInput;
        uint32_t off = 0;
        float tmp[1024];
        while (off < frameCount) {
            uint32_t n = frameCount - off > 1024 ? 1024 : frameCount - off;
            for (uint32_t i = 0; i < n; i++) tmp[i] = s[off + i] / 32768.0f;
            PvadFeedEnroll(g_pvad, tmp, (int)n);
            g_pvadEnrollSamples.fetch_add((int)n);
            off += n;
        }
    }
}
void loopback_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    g_refRing.write((const int16_t*)pInput, frameCount);
}
void output_callback(ma_device*, void* pOutput, const void*, ma_uint32 frameCount) {
    int16_t* out = (int16_t*)pOutput;
    int fs = frameSizeForRate(g_sampleRate.load());
    if ((int)frameCount != fs) { memset(out, 0, frameCount * sizeof(int16_t)); return; }

    const size_t DRIFT_TARGET = (size_t)fs * 2;
    const size_t DRIFT_THRESHOLD = (size_t)fs / 16;
    if (g_micRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_micRing.skip(g_micRing.available() - DRIFT_TARGET);
    if (g_refRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_refRing.skip(g_refRing.available() - DRIFT_TARGET);

    if (g_micRing.available() < (size_t)fs || g_refRing.available() < (size_t)fs) {
        memset(out, 0, frameCount * sizeof(int16_t));
        return;
    }

    int16_t micFrame[MAX_FRAME_SIZE];
    int16_t refFrame[MAX_FRAME_SIZE];
    int16_t cleanedFrame[MAX_FRAME_SIZE];

    g_micRing.read(micFrame, fs);
    g_refRing.read(refFrame, fs);

    // PVAD taps raw mic (owner voice, pre-echo-removal): 16 kHz floats
    // for history (worker verify) and enrollment. Bounded memcpy under
    // a short lock; inference never happens here.
    if (g_pvadEnabled.load() || g_pvadEnrolling.load()) {
        int srNow = g_sampleRate.load();
        if (srNow == 16000) {
            float tmp[MAX_FRAME_SIZE];
            for (int i = 0; i < fs; i++) tmp[i] = micFrame[i] / 32768.0f;
            if (g_pvadEnabled.load()) {
                std::lock_guard<std::mutex> lk(g_pvadHistMtx);
                g_pvadHist.insert(g_pvadHist.end(), tmp, tmp + fs);
                if ((int)g_pvadHist.size() > PVAD_HIST_CAP)
                    g_pvadHist.erase(g_pvadHist.begin(),
                        g_pvadHist.begin() + (g_pvadHist.size() - PVAD_HIST_CAP));
            }
            if (g_pvadEnrolling.load() && g_pvad) {
                PvadFeedEnroll(g_pvad, tmp, fs);
                g_pvadEnrollSamples.fetch_add(fs);
            }
        } else if (srNow == 48000) {
            if (!g_pvadResamplerReady) {
                ma_data_converter_config cfg = ma_data_converter_config_init(
                    ma_format_f32, ma_format_f32, 1, 1, 48000, 16000);
                if (ma_data_converter_init(&cfg, NULL, &g_pvadResampler) == MA_SUCCESS)
                    g_pvadResamplerReady = true;
            }
            if (g_pvadResamplerReady) {
                float f32[MAX_FRAME_SIZE];
                for (int i = 0; i < fs; i++) f32[i] = micFrame[i] / 32768.0f;
                float dn[MAX_FRAME_SIZE];
                ma_uint64 inC = (ma_uint64)fs, outC = MAX_FRAME_SIZE;
                if (ma_data_converter_process_pcm_frames(&g_pvadResampler, f32, &inC,
                                                         dn, &outC) == MA_SUCCESS && outC > 0) {
                    int got = (int)outC;
                    if (g_pvadEnabled.load()) {
                        std::lock_guard<std::mutex> lk(g_pvadHistMtx);
                        g_pvadHist.insert(g_pvadHist.end(), dn, dn + got);
                        if ((int)g_pvadHist.size() > PVAD_HIST_CAP)
                            g_pvadHist.erase(g_pvadHist.begin(),
                                g_pvadHist.begin() + (g_pvadHist.size() - PVAD_HIST_CAP));
                    }
                    if (g_pvadEnrolling.load() && g_pvad) {
                        PvadFeedEnroll(g_pvad, dn, got);
                        g_pvadEnrollSamples.fetch_add(got);
                    }
                }
            }
        }
    }

    if (g_engine.type == ENGINE_SPEEX && g_engine.speex)
        AecCancelEcho(g_engine.speex, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_AEC3 && g_engine.aec3)
        Aec3CancelEcho(g_engine.aec3, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_NKF && g_engine.nkf)
        NkfProcess(g_engine.nkf, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_LOCALVQE && g_engine.localvqe)
        LocalVqeProcess(g_engine.localvqe, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_DTLN && g_engine.dtln)
        DtlnProcess(g_engine.dtln, micFrame, refFrame, cleanedFrame, fs);
    else
        memcpy(cleanedFrame, micFrame, fs * sizeof(int16_t));

    // Silero voice gate: speech passes, silence muted (16 + 48 kHz).
    VadGateApply(cleanedFrame, fs);

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

    auto now = Clock::now();
    if (rms_mic > g_peakMic.load()) { g_peakMic = rms_mic; g_peakMicTime = now; }
    if (rms_ref > g_peakRef.load()) { g_peakRef = rms_ref; g_peakRefTime = now; }
    if (rms_out > g_peakOut.load()) { g_peakOut = rms_out; g_peakOutTime = now; }

    if (rms_mic > 30.0f && rms_ref > 100.0f) {
        float ratio = (rms_out + 1.0f) / (rms_mic + 1.0f);
        if (ratio < 1.0f) g_last_reduction_db = 20.0f * log10f(ratio);
    }

    float gain = g_micGain.load() * g_outputGain.load();
    for (int i = 0; i < fs; i++)
        out[i] = clamp_s16((int)((float)cleanedFrame[i] * gain));
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
    // Output prefers CABLE Input (fresh installs land on it, not index 0).
    int cable = FindCableInputIndex();
    bool outKept = false;
    for (int v : g_outDisplayIndices) if (v == g_outIndex) { outKept = true; break; }
    if (!outKept) g_outIndex = (cable >= 0) ? cable
        : (g_outDisplayIndices.empty() ? 0 : g_outDisplayIndices[0]);
}

void EnumerateDevices() {
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

    auto findBy = [](const std::vector<ma_device_info>& list, const std::string& name) -> int {
        if (name.empty()) return -1;
        for (int i = 0; i < (int)list.size(); i++)
            if (name == list[i].name) return i;
        return -1;
    };
    int mi = findBy(g_captureDevices, micName);   if (mi >= 0) g_micIndex = mi; else g_micIndex = 0;
    int ri = findBy(g_playbackDevices, refName);  if (ri >= 0) g_refIndex = ri; else g_refIndex = 0;
    int oi = findBy(g_playbackDevices, outName);
    if (oi >= 0) {
        g_outIndex = oi;
    } else {
        // Saved output gone (or first run): prefer CABLE Input over index 0.
        int cable = FindCableInputIndex();
        g_outIndex = (cable >= 0) ? cable : 0;
    }

    BuildDisplayIndices();
}

// ============================================================
//  Personalized gate (identity layer): worker-thread ECAPA verify.
//  UI/worker threads only for Ensure/Verify; audio thread only
//  memcpys 16 kHz mic floats into the history (never blocks on the
//  wrapper mutex — inference stays on the worker).
// ============================================================
static void EnsurePvad() {
    if (g_pvad) return;
    g_pvad = PvadNew("models/ecapa-speaker-v1.onnx");
    if (!g_pvad) return;
    if (PvadLoad(g_pvad, "models/voiceprint.bin")) {
        g_pvadReady.store(true);
        g_pvadThreshold.store(PvadThreshold(g_pvad));
    }
}

static void PvadWorker() {
    std::vector<float> snap;
    snap.reserve(PVAD_VERIFY_SAMPLES);
    while (g_pvadRun.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!g_pvadRun.load() || !g_isRunning ||
            !g_pvadEnabled.load() || !g_pvadReady.load() || !g_pvad)
            continue;
        snap.clear();
        {
            std::lock_guard<std::mutex> lk(g_pvadHistMtx);
            if ((int)g_pvadHist.size() < PVAD_VERIFY_SAMPLES) continue;
            snap.assign(g_pvadHist.end() - PVAD_VERIFY_SAMPLES, g_pvadHist.end());
        }
        float m = PvadVerify(g_pvad, snap.data(), (int)snap.size());
        if (m > -1.5f) g_pvadMatch.store(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(750));  // ~1 Hz
    }
}

static void PvadStartWorker() {
    if (g_pvadThread.joinable()) return;
    g_pvadRun.store(true);
    g_pvadThread = std::thread(PvadWorker);
}

static void PvadStopWorker() {
    g_pvadRun.store(false);
    if (g_pvadThread.joinable()) g_pvadThread.join();
}

// ---- One-tap enrollment session (mic-only + monitor) ----
// Runs when the app itself is idle: opens just the mic at 16 kHz
// (miniaudio resamples) plus a raw passthrough to Your speakers.
// No loopback, no AEC, no CABLE involved. Owned sessions tear down
// on timer end/cancel; a user-started run is never touched.
static void enroll_monitor_callback(ma_device*,
                                    void* pOutput, const void*,
                                    ma_uint32 frameCount) {
    int16_t* out = (int16_t*)pOutput;
    size_t avail = g_micRing.available();
    size_t n = avail < frameCount ? avail : frameCount;
    if (n > 0) g_micRing.read(out, n);
    if (n < frameCount) memset(out + n, 0, (frameCount - n) * sizeof(int16_t));
}

static void StopEnrollCapture() {
    if (g_enrollMonOn) { ma_device_uninit(&g_enrollOutDevice); g_enrollMonOn = false; }
    if (g_enrollMicOn) { ma_device_uninit(&g_enrollMicDevice); g_enrollMicOn = false; }
    g_enrollOwnedAudio = false;
}

static bool StartEnrollCapture() {
    if (g_captureDevices.empty() || g_micIndex >= (int)g_captureDevices.size()) {
        snprintf(g_statusText, 128, "No microphone found");
        return false;
    }
    g_micRing.reset();
    ma_device_config micCfg = ma_device_config_init(ma_device_type_capture);
    micCfg.capture.format = ma_format_s16;
    micCfg.capture.channels = 1;
    micCfg.sampleRate = 16000;  // PVAD eats 16 kHz; miniaudio resamples
    micCfg.dataCallback = mic_callback;
    micCfg.capture.pDeviceID = &g_captureDevices[g_micIndex].id;
    if (ma_device_init(&g_context, &micCfg, &g_enrollMicDevice) != MA_SUCCESS) {
        snprintf(g_statusText, 128, "Could not open microphone");
        return false;
    }
    if (ma_device_start(&g_enrollMicDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_enrollMicDevice);
        snprintf(g_statusText, 128, "Could not start microphone");
        return false;
    }
    g_enrollMicOn = true;
    // Monitor is a courtesy: hear yourself while enrolling. Silent
    // fallback if the speakers can't open — enrollment continues.
    if (!g_playbackDevices.empty() && g_refIndex < (int)g_playbackDevices.size()) {
        ma_device_config outCfg = ma_device_config_init(ma_device_type_playback);
        outCfg.playback.format = ma_format_s16;
        outCfg.playback.channels = 1;
        outCfg.sampleRate = 16000;
        outCfg.dataCallback = enroll_monitor_callback;
        outCfg.playback.pDeviceID = &g_playbackDevices[g_refIndex].id;
        if (ma_device_init(&g_context, &outCfg, &g_enrollOutDevice) == MA_SUCCESS) {
            if (ma_device_start(&g_enrollOutDevice) == MA_SUCCESS) {
                g_enrollMonOn = true;
            } else {
                ma_device_uninit(&g_enrollOutDevice);
            }
        }
    }
    g_enrollStart = Clock::now();
    g_enrollOwnedAudio = true;
    return true;
}

// Forward declarations (defined below: main Start/Stop)
void StartAEC();
void StopAEC();

// Begin enrollment: stop a running session first (owned mic-only +
// monitor session needs the devices), then capture. Stays idle after
// finalize — no auto-start.
static bool PvadStartEnrollFlow() {
    EnsurePvad();
    if (!g_pvad) return false;
    if (g_isRunning) StopAEC();
    if (!StartEnrollCapture()) return false;
    PvadBeginEnroll(g_pvad);
    g_pvadEnrollSamples.store(0);
    g_pvadEnrolling.store(true);
    g_forgetArmed = false;
    return true;
}

// ============================================================
//  Start / Stop
// ============================================================
void StartAEC() {
    if (g_isRunning) return;
    // An owned enrollment capture owns the mic — yield it first.
    if (g_enrollOwnedAudio) {
        g_pvadEnrolling.store(false);
        StopEnrollCapture();
    }
    if (g_captureDevices.empty() || g_playbackDevices.empty()) {
        snprintf(g_statusText, 128, "No devices found");
        return;
    }
    if (g_micIndex >= (int)g_captureDevices.size())  g_micIndex = 0;
    if (g_refIndex >= (int)g_playbackDevices.size()) g_refIndex = 0;
    if (g_outIndex >= (int)g_playbackDevices.size()) g_outIndex = 0;

    // Self-monitor routes the cleaned mic to the speakers (Speaker
    // Reference) instead of Output; otherwise Output must be usable.
    // Without VB-CABLE installed/enabled there is nowhere to send the
    // cleaned mic, so refuse to start with a pointer at the fix.
    int outIdx = g_outIndex;
    // Calibration monitor: hear yourself through the speakers while the
    // 5 s listen runs (same path as Listen-to-myself; needs no CABLE).
    if (g_listenToSelf || g_calMonitor) {
        outIdx = g_refIndex;
    } else if (!CableInputPresent()) {
        snprintf(g_statusText, 128, "VB-CABLE not found — install/enable CABLE Input");
        return;
    }

    SaveSettings();
    ReinitEngine();
    g_sessionStartedOnce = true;  // retire the first-run setup card

    if (g_engine.type == ENGINE_NKF && !g_engine.nkf) {
        snprintf(g_statusText, 128, "Failed to load NKF model");
        return;
    }
    if (g_engine.type == ENGINE_LOCALVQE && !g_engine.localvqe) {
        snprintf(g_statusText, 128, "Failed to load LocalVQE model");
        return;
    }
    if (g_engine.type == ENGINE_DTLN && !g_engine.dtln) {
        snprintf(g_statusText, 128, "Failed to load DTLN model");
        return;
    }

    g_micRing.reset();
    g_refRing.reset();
    VadReset();
    if (g_pvadResamplerReady) {
        ma_data_converter_uninit(&g_pvadResampler, NULL);
        g_pvadResamplerReady = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_pvadHistMtx);
        g_pvadHist.clear();
    }
    g_pvadMatch.store(-2.0f);  // fail-open until first verification
    g_pvadEnrolling.store(false);
    g_pvadEnrollSamples.store(0);
    g_peakMic.store(0); g_peakRef.store(0); g_peakOut.store(0);

    int sr = g_sampleRate.load();
    int fs = frameSizeForRate(sr);

    ma_device_config micCfg = ma_device_config_init(ma_device_type_capture);
    micCfg.capture.format = ma_format_s16;
    micCfg.capture.channels = 1;
    micCfg.sampleRate = sr;
    micCfg.periodSizeInFrames = fs;
    micCfg.dataCallback = mic_callback;
    micCfg.capture.pDeviceID = &g_captureDevices[g_micIndex].id;

    ma_device_config loopCfg = ma_device_config_init(ma_device_type_loopback);
    loopCfg.capture.format = ma_format_s16;
    loopCfg.capture.channels = 1;
    loopCfg.sampleRate = sr;
    loopCfg.periodSizeInFrames = fs;
    loopCfg.dataCallback = loopback_callback;
    loopCfg.capture.pDeviceID = &g_playbackDevices[g_refIndex].id;

    ma_device_config outCfg = ma_device_config_init(ma_device_type_playback);
    outCfg.playback.format = ma_format_s16;
    outCfg.playback.channels = 1;
    outCfg.sampleRate = sr;
    outCfg.periodSizeInFrames = fs;
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

    ma_device_start(&g_micDevice);
    ma_device_start(&g_loopbackDevice);
    ma_device_start(&g_outputDevice);

    g_isRunning = true;
    g_sessionStart = Clock::now();
    if (g_pvadEnabled.load()) {
        EnsurePvad();  // no-op if the model file is absent
        PvadStartWorker();
    }
    const char* engineName =
        (g_engine.type == ENGINE_SPEEX) ? "SpeexDSP" :
        (g_engine.type == ENGINE_AEC3)  ? "AEC3" :
        (g_engine.type == ENGINE_NKF)   ? "NKF-AEC" :
        (g_engine.type == ENGINE_DTLN)  ? "DTLN-AEC" : "LocalVQE";
    snprintf(g_statusText, 128, "Running (%d Hz, %s)%s", sr, engineName,
             (g_listenToSelf || g_calMonitor) ? " [monitor]" : "");
}

void StopAEC() {
    if (!g_isRunning) return;
    PvadStopWorker();
    g_pvadEnrolling.store(false);
    StopEnrollCapture();  // no-op unless an owned session is open
    ma_device_uninit(&g_micDevice);
    ma_device_uninit(&g_loopbackDevice);
    ma_device_uninit(&g_outputDevice);
    g_isRunning = false;
    VadReset();
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

void DrawLevelMeter(const char* label, float rms, float peak, float maxValue) {
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
        ImU32 col;
        if (frac < 0.6f)       col = IM_COL32(80, 200, 100, 255);
        else if (frac < 0.85f) col = IM_COL32(230, 200, 60, 255);
        else                   col = IM_COL32(230, 80, 80, 255);
        dl->AddRectFilled(pos, ImVec2(pos.x + barW, pos.y + height), col, 4.0f);
    }
    float peakFrac = peak / maxValue;
    if (peakFrac > 1.0f) peakFrac = 1.0f;
    float peakX = pos.x + width * peakFrac;
    if (peakX > pos.x)
        dl->AddLine(ImVec2(peakX - 1, pos.y), ImVec2(peakX - 1, pos.y + height),
                    IM_COL32(255, 255, 255, 220), 2.0f);
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                IM_COL32(60, 60, 70, 255), 4.0f);
    ImGui::Dummy(ImVec2(width, height));
}

// ============================================================
//  UI sections
// ============================================================
static void MarkPresetCustom() {
    g_presetIndex = 0;
}

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
    if (ImGui::Button("Refresh", ImVec2(90, 0))) {
        EnumerateDevices();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rescan audio devices without restarting");

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

    // Hidden while Listen-to-myself reroutes to Your speakers:
    // showing it would suggest it still does something.
    if (!g_listenToSelf) {
        DrawInlineDot(!g_outDisplayIndices.empty());
        ImGui::SameLine();
        ImGui::TextUnformatted("Send cleaned sound to");
        ImGui::SameLine(labelCol);
        FilteredDeviceCombo("Output", "Where your cleaned voice goes. CABLE Input is picked automatically\n"
                                      "so voice apps can use it as your microphone",
                            g_outIndex, g_playbackDevices, g_outDisplayIndices);
    }

    if (!CableInputPresent()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
            "VB-CABLE not found — install VB-CABLE and enable CABLE Input,");
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
            "then hit Refresh (or tick Listen to myself below).");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Free download: vb-audio.com/Cable/ (reboot after install).\n"
                              "Windows lists only enabled devices — a disabled CABLE Input\n"
                              "looks the same as not installed: check Sound settings too.");
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Listen to myself", &g_listenToSelf)) {
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hear yourself through your speakers instead of sending\n"
                          "to voice apps — for testing. Untick to send your voice\n"
                          "to CABLE Input (Discord, Zoom, Teams) again.");

    ImGui::EndDisabled();
}

void DrawEngineSection() {
    ImGui::SeparatorText("Engine");
    const float labelCol = 190.0f;

    ImGui::BeginDisabled(g_isRunning);

    ImGui::TextUnformatted("Engine");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-1);
    const char* engines[] = {
        "SpeexDSP (lightest on CPU)",
        "WebRTC AEC3 (best quality, recommended)",
        "NKF-AEC (experimental)",
        "LocalVQE v1.4-AEC (natural voice)",
        "DTLN-AEC 512 (neural)"
    };
    if (ImGui::Combo("##engine", &g_engineIndex, engines, IM_ARRAYSIZE(engines))) {
        g_selectedEngine.store(g_engineIndex);
        if (g_engineIndex == ENGINE_SPEEX || g_engineIndex == ENGINE_NKF || g_engineIndex == ENGINE_LOCALVQE || g_engineIndex == ENGINE_DTLN) {
            g_sampleRateIndex = 0;
            g_sampleRate.store(16000);
        }
        MarkPresetCustom();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "SpeexDSP = lightest on CPU, phone quality (16 kHz automatic)\n"
            "AEC3 = best voice, more CPU (recommended)\n"
            "NKF-AEC = experimental neural engine (16 kHz automatic)\n"
            "LocalVQE = natural voice, keeps room sound (16 kHz automatic)\n"
            "DTLN-AEC 512 = neural echo + noise removal (16 kHz automatic)");

    ImGui::TextUnformatted("Sample Rate");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(-1);

    const char* rates[] = { "16000", "48000" };
    if (g_engineIndex == ENGINE_SPEEX || g_engineIndex == ENGINE_NKF || g_engineIndex == ENGINE_LOCALVQE || g_engineIndex == ENGINE_DTLN) {
        ImGui::TextDisabled("16 kHz (automatic)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This engine always runs at 16 kHz — nothing to choose.");
    } else {
        if (ImGui::Combo("##rate", &g_sampleRateIndex, rates, IM_ARRAYSIZE(rates))) {
            g_sampleRate.store(atoi(rates[g_sampleRateIndex]));
            MarkPresetCustom();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher = better quality, more CPU\nAEC3 best at 48000");
    }

    if (g_engineIndex == ENGINE_SPEEX) {
        ImGui::TextUnformatted("Echo tail");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(-1);
        const char* filterLengths[] = { "30", "50", "80", "120", "200" };
        if (ImGui::Combo("##tail", &g_filterIndex, filterLengths, IM_ARRAYSIZE(filterLengths))) {
            g_filterLengthMs.store(atoi(filterLengths[g_filterIndex]));
            MarkPresetCustom();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("SpeexDSP only - how much echo to cancel (ms)");
    } else if (g_engineIndex == ENGINE_AEC3) {
        ImGui::TextDisabled("AEC3 tunes itself — no extra settings.");
    } else if (g_engineIndex == ENGINE_NKF) {
        ImGui::TextDisabled("Experimental neural engine. Runs at 16 kHz automatically.");
    } else if (g_engineIndex == ENGINE_LOCALVQE) {
        ImGui::TextDisabled("Removes only echo — your voice and room sound stay natural.");
        ImGui::TextDisabled("Runs at 16 kHz automatically. Very light on CPU.");
    } else if (g_engineIndex == ENGINE_DTLN) {
        ImGui::TextDisabled("Neural echo + noise removal. Runs at 16 kHz automatically.");
        ImGui::TextDisabled("Needs an extra download — see About for details.");
    }

    ImGui::EndDisabled();
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
        MarkPresetCustom();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", micGain);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How loud your voice goes out. Editable while running.");
}

void DrawVadSection() {
    ImGui::SeparatorText("Voice gate");

    bool enabled = g_vadEnabled.load();
    if (ImGui::Checkbox("Mute when no speech (neural voice detector)", &enabled)) {
        g_vadEnabled.store(enabled);
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "After echo removal, the app listens for speech many times a second.\n"
            "Speech passes through; silence is muted. No setup, no recording.");

    if (!g_vad) {
        ImGui::TextDisabled("Voice detector is missing its data file — gate is off.");
        ImGui::TextDisabled("Reinstall the app or see About for details.");
    } else if (enabled) {
        float prob = g_vadProb.load();
        float open = g_vadOpen.load();
        DrawInlineDot(prob >= open);
        ImGui::SameLine();
        if (prob >= open)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Speaking (%.2f)", prob);
        else
            ImGui::TextDisabled("Silent (%.2f)", prob);
        // One-tap calibration: 5 s of normal speech sets open/close from
        // measured probs. Bounded + persisted — no mid-call drift.
        if (g_vadCalibrating.load() && !g_isRunning) {
            // User hit Stop mid-calibration — abort, don't judge stale audio.
            g_vadCalibrating.store(false);
            g_vadCalSamples.clear();
            g_vadCalMsg = "Stopped — calibration cancelled.";
            g_vadCalMsgIsErr = true;
        }
        if (g_vadCalibrating.load()) {
            g_vadCalSamples.push_back(prob);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - g_vadCalStart).count();
            int left = VAD_CAL_SECONDS - (int)(elapsed / 1000);
            if (left < 0) left = 0;
            int pct = (int)(elapsed * 100 / (VAD_CAL_SECONDS * 1000));
            if (pct > 100) pct = 100;
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                "Calibrating… keep speaking normally (%d%%, %ds left)", pct, left);
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                g_vadCalibrating.store(false);
                g_vadCalSamples.clear();
                g_vadCalMsg.clear();
                if (!g_vadCalWasRunning && g_isRunning) StopAEC();  // undo auto-start
            }
            if (elapsed >= VAD_CAL_SECONDS * 1000) {
                g_vadCalibrating.store(false);
                // Percentiles of what the detector actually saw.
                std::vector<float> s = g_vadCalSamples;
                g_vadCalSamples.clear();
                if (s.size() < 30) {
                    g_vadCalMsg = "Too few samples — try again while running.";
                    g_vadCalMsgIsErr = true;
                } else {
                    std::sort(s.begin(), s.end());
                    auto pct = [&](float p) { return s[(size_t)(p * (s.size() - 1))]; };
                    float p10 = pct(0.10f), p50 = pct(0.50f), p90 = pct(0.90f);
                    if (p90 < 0.25f || (p90 - p10) < 0.10f) {
                        char buf[160];
                        snprintf(buf, sizeof(buf),
                            "Didn't hear clear speech (peak %.2f) — kept %.2f/%.2f. "
                            "Move closer / turn up, then retry.",
                            p90, (double)g_vadOpen.load(), (double)g_vadClose.load());
                        g_vadCalMsg = buf;
                        g_vadCalMsgIsErr = true;
                    } else {
                        float openT = (p10 + p50) * 0.5f;
                        if (openT < 0.18f) openT = 0.18f;
                        if (openT > 0.65f) openT = 0.65f;
                        float closeT = openT * 0.6f;
                        if (closeT < 0.10f) closeT = 0.10f;
                        if (closeT > 0.45f) closeT = 0.45f;
                        if (closeT > openT - 0.05f) closeT = openT - 0.05f;
                        g_vadOpen.store(openT);
                        g_vadClose.store(closeT);
                        SaveSettings();
                        char buf[160];
                        snprintf(buf, sizeof(buf),
                            "Calibrated for this mic: open %.2f close %.2f "
                            "(speech %.2f, quiet %.2f).",
                            (double)openT, (double)closeT, (double)p50, (double)p10);
                        g_vadCalMsg = buf;
                        g_vadCalMsgIsErr = false;
                    }
                }
                // Learn-my-voice contract: back to idle after, Start clickable.
                // Only undo the auto-start — a session that was already
                // running stays running.
                if (!g_vadCalWasRunning && g_isRunning) StopAEC();
            }
        } else {
            float openT = g_vadOpen.load(), closeT = g_vadClose.load();
            bool custom = (fabsf(openT - 0.50f) > 0.005f || fabsf(closeT - 0.30f) > 0.005f);
            ImGui::TextDisabled("Sensitivity: %s (%.2f/%.2f)",
                custom ? "calibrated" : "normal", (double)openT, (double)closeT);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open/close lines. Calibrate rewrites them;\n"
                                  "Reset restores 0.50/0.30.");
            // Step-process like Learn my voice: works idle (auto-starts
            // the session) or running; 5 s listen, then a kept result
            // with Re-calibrate. Back to idle after when it auto-started
            // (Start clickable again); an already-running session stays up.
            const char* calLabel = custom ? "Re-calibrate" : "Calibrate for my mic";
            if (ImGui::Button(calLabel, ImVec2(180, 0))) {
                g_vadCalWasRunning = g_isRunning;
                // Idle path monitors through the speakers (hear yourself +
                // live meters, no CABLE needed) — consumed by Start below.
                g_calMonitor = !g_isRunning;
                if (!g_isRunning) StartAEC();
                g_calMonitor = false;
                if (!g_isRunning || g_vad == nullptr) {
                    g_vadCalMsg = "Couldn't start audio — check devices / status above.";
                    g_vadCalMsgIsErr = true;
                } else {
                    g_vadCalSamples.clear();
                    g_vadCalSamples.reserve(400);
                    g_vadCalMsg.clear();
                    g_vadCalStart = Clock::now();
                    g_vadCalibrating.store(true);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Starts audio if needed (you'll hear yourself),\n"
                                  "then listens 5 s while you speak. Back to idle\n"
                                  "after — press Start to use it.");
            ImGui::SameLine();
            if (custom && ImGui::Button("Reset", ImVec2(80, 0))) {
                g_vadOpen.store(0.50f);
                g_vadClose.store(0.30f);
                g_vadCalMsg.clear();
                SaveSettings();
            }
            if (!g_vadCalMsg.empty()) {
                if (g_vadCalMsgIsErr)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", g_vadCalMsg.c_str());
                else
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", g_vadCalMsg.c_str());
            }
            ImGui::TextDisabled("Tip: re-calibrate after switching mic or engine.");
        }
    } else {
        ImGui::TextDisabled("Gate is off — everything passes through unchanged.");
    }

    // ---- Owner-only voice (experimental personal gate) ----
    ImGui::Spacing();
    bool pvadOn = g_pvadEnabled.load();
    if (ImGui::Checkbox("Only my voice (experimental)", &pvadOn)) {
        EnsurePvad();
        g_pvadEnabled.store(pvadOn && g_pvad != nullptr);
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("After learning your voice, the gate also mutes OTHER\n"
                          "voices (TV, family, roommates) — only you pass through.\n"
                          "Your voiceprint never leaves this PC.\n"
                          "Checks ~1/sec: speech starts always pass — muting\n"
                          "only on a measured mismatch, never on timing.\n"
                          "Off by default; needs a voiceprint below to do anything.");
    if (g_pvadEnabled.load() && g_pvad) {
        if (g_pvadEnrolling.load()) {
            int pct = g_pvadEnrollSamples.load() * 100 / PVAD_NEED_SAMPLES;
            if (pct > 100) pct = 100;
            int secsLeft = 8 - (int)(std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - g_enrollStart).count());
            if (secsLeft < 0) secsLeft = 0;
            if (!g_enrollOwnedAudio) secsLeft = -1;  // live path: no timer
            if (secsLeft >= 0)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                   "Listening… keep speaking (%d%%, %ds left)", pct, secsLeft);
            else
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                   "Listening… keep speaking normally (%d%%)", pct);
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                g_pvadEnrolling.store(false);
                StopEnrollCapture();
            }
            // Finalize on the UI thread (embedding takes ~0.1-1 s).
            if (g_pvadEnrollSamples.load() >= PVAD_NEED_SAMPLES) {
                g_pvadEnrolling.store(false);
                StopEnrollCapture();
                if (PvadFinishEnroll(g_pvad) &&
                    PvadSave(g_pvad, "models/voiceprint.bin")) {
                    g_pvadReady.store(true);
                    g_pvadThreshold.store(PvadThreshold(g_pvad));
                    g_pvadMatch.store(-2.0f);
                }
            }
        } else if (!g_pvadReady.load()) {
            ImGui::TextDisabled("No voice learned yet.");
            if (ImGui::Button("Learn my voice", ImVec2(160, 0))) {
                PvadStartEnrollFlow();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Listens for 8 seconds (you'll hear yourself),\n"
                                  "then saves your voiceprint on this PC.\n"
                                  "Enroll in a quiet moment (no TV/music).");
            ImGui::TextDisabled("Takes 8 seconds. Your voiceprint never leaves this PC.");
        } else {
            float m = g_pvadMatch.load();
            if (m < -1.5f) {
                ImGui::TextDisabled("Voice learned. Waiting for speech to check…");
            } else if (m >= g_pvadThreshold.load()) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                   "Voice learned. Last check: you (%.2f)", m);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                   "Voice learned. Last check: not you (%.2f) — muted", m);
            }
            if (ImGui::Button("Re-learn my voice", ImVec2(160, 0))) {
                PvadStartEnrollFlow();
            }
            ImGui::SameLine();
            if (!g_forgetArmed) {
                if (ImGui::Button("Forget my voice", ImVec2(160, 0))) {
                    g_forgetArmed = true;
                    g_forgetArmTime = Clock::now();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Deletes your voiceprint from this PC.");
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - g_forgetArmTime).count();
                if (elapsed > 5) {
                    g_forgetArmed = false;
                } else if (ImGui::Button("Click again to confirm", ImVec2(160, 0))) {
                    if (g_pvad) PvadClearVoiceprint(g_pvad);
                    std::remove("models/voiceprint.bin");
                    g_pvadReady.store(false);
                    g_pvadMatch.store(-2.0f);
                    g_forgetArmed = false;
                }
            }
        }
    } else if (pvadOn && !g_pvad) {
        ImGui::TextDisabled("Voice model missing — owner check unavailable.");
    }
}

void DrawAudioTab() {
    // First-run setup card: 3 live checklist steps for newcomers.
    // Retires after the first Start; returning users never see it.
    if (g_isFirstRun && !g_sessionStartedOnce && !g_isRunning) {
        ImGui::SeparatorText("Get started");
        ImGui::TextWrapped("Three steps, then press Start below. Your choices save automatically.");
        ImGui::Spacing();
        auto step = [](bool ok, const char* done, const char* todo) {
            DrawInlineDot(ok);
            ImGui::SameLine();
            if (ok) ImGui::TextUnformatted(done);
            else ImGui::TextDisabled("%s", todo);
        };
        std::string micName = "Pick Your microphone below.";
        step(!g_micDisplayIndices.empty(), "Microphone selected.", micName.c_str());
        std::string refName = "Pick Your speakers below.";
        step(!g_refDisplayIndices.empty(), "Speakers selected.", refName.c_str());
        step(CableInputPresent(), "Discord output ready (CABLE Input).",
             "Install VB-CABLE so voice apps can hear you (see warning below).");
        ImGui::Spacing();
    }

    ImGui::SeparatorText("Preset");

    ImGui::TextUnformatted("Quick preset");
    ImGui::SameLine(190.0f);
    ImGui::SetNextItemWidth(-1);
    std::vector<const char*> presetNames;
    for (int i = 0; i < PRESET_COUNT; i++) presetNames.push_back(PRESETS[i].name);

    bool wasRunning = g_isRunning;
    if (ImGui::Combo("##preset", &g_presetIndex, presetNames.data(), (int)presetNames.size())) {
        if (g_presetIndex > 0) {
            ApplyPreset(g_presetIndex);
            if (wasRunning) { StopAEC(); StartAEC(); }
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One-click setups for common uses.\n"
                          "Changing anything by hand switches this to Manual settings.");

    ImGui::Spacing();
    DrawDevicesSection();
    ImGui::Spacing();
    // Advanced: engine, levels, gate. No close-X — the header always
    // stays visible and toggles. First frame applies the persisted
    // default (closed for newcomers, as left for regulars).
    if (!g_advInitDone) {
        ImGui::SetNextItemOpen(g_advancedOpen);
        g_advInitDone = true;
    }
    bool advOpen = ImGui::CollapsingHeader("Advanced");
    if (advOpen != g_advancedOpen) {
        g_advancedOpen = advOpen;
        SaveSettings();
    }
    if (advOpen) {
        DrawEngineSection();
        ImGui::Spacing();
        DrawGainsSection();
        ImGui::Spacing();
        DrawVadSection();
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
            "When ON:  X hides the window, tray icon stays active\n"
            "When OFF: X closes the app completely");

    if (g_minimizeToTray) {
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click the tray icon to Exit the app.");
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
    ImGui::BulletText("Five AEC engines: SpeexDSP, WebRTC AEC3, NKF-AEC, LocalVQE v1.4-AEC, DTLN-AEC 512");
    ImGui::BulletText("Voice gate — neural speech detector mutes silence");
    ImGui::BulletText("Real-time processing with low CPU usage");
    ImGui::BulletText("Works with speakers, earphones, and headsets");
    ImGui::BulletText("Selectable sample rate (16 / 48 kHz)");
    ImGui::BulletText("Live level meters with peak hold");
    ImGui::BulletText("Presets for common scenarios");
    ImGui::BulletText("Optional minimize to system tray");

    ImGui::Spacing();
    ImGui::SeparatorText("Credits");
    ImGui::BulletText("SpeexDSP      - Xiph.Org Foundation (BSD-3)");
    ImGui::BulletText("WebRTC AP     - Google (BSD-3)");
    ImGui::BulletText("NKF-AEC       - Jiang et al. (ICASSP 2023, MIT)");
    ImGui::BulletText("DTLN-AEC      - Westhausen & Meyer (ICASSP 2021, MIT)");
    ImGui::BulletText("LocalVQE      - LocalAI (Apache-2.0)");
    ImGui::BulletText("Silero VAD    - Silero Team (MIT)");
    ImGui::BulletText("ECAPA voice   - SpeechBrain (Apache-2.0)");
    ImGui::BulletText("ONNX Runtime  - Microsoft (MIT)");
    ImGui::BulletText("Dear ImGui    - Omar Cornut (MIT)");
    ImGui::BulletText("miniaudio     - David Reid (MIT-0)");
    ImGui::BulletText("GLFW          - Marcus Geelnard / Camilla Berglund (zlib)");
    ImGui::BulletText("stb_image     - Sean Barrett (public domain)");

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
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running  %s", FormatUptime().c_str());
    else
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Idle");

    // Voice-gate pill: SPEAKING lights up on any speech, SILENT when muted.
    // Visible from every tab while the gate can act (16 or 48 kHz).
    int vadSr = g_sampleRate.load();
    if (g_vad && g_vadEnabled.load() && (vadSr == 16000 || vadSr == 48000)) {
        bool speaking = g_vadProb.load() >= g_vadOpen.load();
        ImGui::SameLine();
        DrawStatusDot(speaking);
        ImGui::SameLine();
        if (speaking)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SPEAKING");
        else
            ImGui::TextDisabled("SILENT");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Green SPEAKING = speech going to Discord\n"
                "Grey SILENT = gate muted (no speech)\n"
                "Detail lives under Audio → Voice gate");
    }

    ImGui::Spacing();

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::Spacing();
            DrawAudioTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            ImGui::Spacing();
            DrawAppearanceTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            ImGui::Spacing();
            DrawAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Footer on every tab: Start / Stop + status + Live Levels,
    // so you can stop or watch levels without switching back to Audio.
    {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        std::string startLabel = g_isRunning
            ? ("Stop   " + FormatUptime())
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

        DrawLevelMeter("Mic", micRms, g_peakMic.load(), 3000.0f);
        DrawLevelMeter("Ref", refRms, g_peakRef.load(), 3000.0f);
        DrawLevelMeter("Out", outRms, g_peakOut.load(), 3000.0f);

        // Self-diagnosis aid: a permanently silent reference means the
        // wrong speakers are selected — but silence is also normal when
        // nothing plays, so this informs, never warns.
        ImGui::Spacing();
        if (refRms > 5.0f)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "Reference: receiving speaker audio");
        else
            ImGui::TextDisabled("Reference: silent (normal if nothing is playing —\n"
                                "if speakers ARE playing, re-check Your speakers above)");

        ImGui::Spacing();
        if (db < 0.0f)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                               "Echo reduction: %.1f dB", db);
        else
            ImGui::TextDisabled("Echo reduction: -- dB (silent)");

        auto now = Clock::now();
        auto decayMs = [&](std::atomic<float>& peak, Clock::time_point& tp) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - tp).count();
            if (ms > 1500) peak.store(peak.load() * 0.92f);
        };
        decayMs(g_peakMic, g_peakMicTime);
        decayMs(g_peakRef, g_peakRefTime);
        decayMs(g_peakOut, g_peakOutTime);
    }
    } // end footer block

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ============================================================
//  MAIN
// ============================================================
int main(int, char**) {
#ifdef _WIN32
    if (!AcquireSingleInstance()) {
        return 0;   // Another instance is already running
    }
#endif

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
    if (g_engine.speex) AecDestroy(g_engine.speex);
    if (g_engine.aec3)  Aec3Destroy(g_engine.aec3);
    if (g_engine.nkf)   NkfDestroy(g_engine.nkf);
    if (g_engine.localvqe) LocalVqeDestroy(g_engine.localvqe);
    if (g_engine.dtln)  DtlnDestroy(g_engine.dtln);
    if (g_vad) { SileroDestroy(g_vad); g_vad = nullptr; }
    if (g_pvad) { PvadDestroy(g_pvad); g_pvad = nullptr; }
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
