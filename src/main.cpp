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
#include "aec3_wrapper.h"
#include "nkf_wrapper.h"
#include "dtln_wrapper.h"
#include "dtln_ns_wrapper.h"
#include "wpe.h"
#include "notch.h"
#include "speech_gate.h"

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
#define APP_VERSION "1.10.0"

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
// Numeric values keep their historical meaning so saved aec_config.txt
// indices keep loading: 0 (SpeexDSP) and 3 (LocalVQE) were retired and
// remap to DTLN (the default) on load.
enum EngineType { ENGINE_AEC3 = 1, ENGINE_NKF = 2, ENGINE_DTLN = 4 };
struct EngineState {
    EngineType  type  = ENGINE_DTLN;
    Aec3Handle* aec3  = nullptr;
    NkfHandle*  nkf   = nullptr;
    DtlnHandle* dtln  = nullptr;
    DtlnNsHandle* ns = nullptr;   // DTLN noise reduction (DTLN/AEC3 only)
    WpeHandle*  wpe   = nullptr;   // WPE dereverb post stage (per g_wpeEnabled)
    NotchHandle* notch = nullptr;  // adaptive notch post stage (per g_notchEnabled)
};

// ============================================================
//  Profiles
// ============================================================
// Profile = engine selection. The combo label shows the engine name
// directly; the running chain is engine + the post stages while they
// are ticked. Filter length comes from here; sample rate, gains,
// devices and the two post-stage ticks are free knobs.
struct Profile {
    const char* name;
    int   engine;
    int   filterIdx;
};

static const Profile PROFILES[] = {
    { "DTLN-AEC 128", ENGINE_DTLN, 0 },
    { "WebRTC AEC3",  ENGINE_AEC3, 2 },
    { "NKF-AEC",      ENGINE_NKF,  0 },
};
const int PROFILE_COUNT = (int)(sizeof(PROFILES) / sizeof(PROFILES[0]));

// ============================================================
//  Globals
// ============================================================
EngineState g_engine;

ma_device g_micDevice, g_loopbackDevice, g_outputDevice;
ma_context g_context;
static bool g_contextInitialized = false;
const int MAX_FRAME_SIZE = 480;
// Last emitted output frame (audio thread only) — fail-open gaps fade
// from it instead of hard-muting (10 ms holes are audible clicks).
int16_t            g_lastOutFrame[MAX_FRAME_SIZE];
int                g_lastOutLen = 0;
bool               g_lastOutValid = false;
SpscRing<4096> g_micRing, g_refRing;
bool g_isRunning = false;
Clock::time_point g_sessionStart;

std::atomic<int>   g_sampleRate(16000);
std::atomic<int>   g_selectedEngine(ENGINE_DTLN);
std::atomic<int>   g_filterLengthMs(50);
std::atomic<bool>  g_enablePreprocess(false);
// Post-stage ticks are global prefs (like the gains): a profile
// switch never resets them. Both default ON for every engine.
std::atomic<bool>  g_wpeEnabled{ true };      // WPE dereverb post stage (NKF only)
std::atomic<bool>  g_notchEnabled{ true };    // adaptive notch feedback suppression (NKF only)
std::atomic<bool>  g_nsEnabled{ true };        // DTLN noise reduction (DTLN/AEC3 only)
std::atomic<bool>  g_experimentalEngines{ false }; // show WebRTC AEC3 / NKF-AEC in the list
// Near-end speech gate (audio thread): RMS hysteresis on the engine
// output + sustained-loudness watchdog feeding WpeSetSpeech /
// NotchSetSpeech each frame (see speech_gate.h).
SpeechGate          g_speechGate;             // .on / .stuck read by UI tooltip
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
// Post-stage prefs are engine-scoped (WPE + notch = NKF, DTLN-NR =
    // DTLN/AEC3). Syncs each stage to the engine's defaults so a toggle
    // left on for another engine cannot leak into a chain that doesn't
    // run it — the panel hides the irrelevant toggles anyway, but stale
    // prefs were what made Start look broken after a switch.
static void SyncStagePrefsToEngine();

void SaveSettings() {
    std::ofstream f("aec_config.txt");
    if (!f.is_open()) return;
    f << g_micIndex << "\n" << g_refIndex << "\n" << g_outIndex << "\n"
      << g_engineIndex << "\n" << g_sampleRateIndex << "\n" << g_filterIndex << "\n"
      << (g_wpeEnabled.load() ? 1 : 0) << "\n"  // WPE flag (was preprocess/DFN)
      << (int)(g_micGain.load() * 100) << "\n"
      << (int)(g_outputGain.load() * 100) << "\n"
      << g_wallpaperIndex << "\n"
      << g_profileIndex << "\n"
       << (g_minimizeToTray ? 1 : 0) << "\n"
       << 0 << "\n"  // retired: voice gate enable (kept for file alignment)
       << (g_listenToSelf ? 1 : 0) << "\n"
       << (g_advancedOpen ? 1 : 0) << "\n"
       << 0 << "\n"  // retired: voice detector (kept for file alignment)
<< (g_notchEnabled.load() ? 1 : 0) << "\n"  // notch flag (was custom-profile)
        << (g_nsEnabled.load() ? 1 : 0) << "\n"    // DTLN noise reduction flag
<< 0 << "\n"  // retired: calibrated gate open threshold
        << 0 << "\n"  // retired: calibrated gate close threshold
        << 0 << "\n"  // retired: show-legacy-engines toggle
        << (g_experimentalEngines.load() ? 1 : 0) << "\n"  // experimental engines flag
       << 0 << "\n"  // retired: nuked DEC-toggle slot
       << 6 << "\n"  // config gen (6 = v2.0: WPE + notch post stages)
       << 0 << "\n"; // retired: NKF WPE dereverb (kept for file alignment)
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
        // Retired engines (SpeexDSP=0, LocalVQE=3): remap to the default
        // (DTLN) so old configs never point at a deleted engine.
        if (g_engineIndex == 0 || g_engineIndex == 3) {
            g_engineIndex = ENGINE_DTLN;
            g_sampleRateIndex = 0;
            g_sampleRate.store(16000);
        }
        if (g_engineIndex != ENGINE_AEC3 && g_engineIndex != ENGINE_NKF &&
            g_engineIndex != ENGINE_DTLN)
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
        if (f >> le_skip) { (void)le_skip; }
        // (Retired field: nuked DEC-toggle slot, always 0. Read to keep
        // old and new config files positionally aligned.)
        int dc_skip = 0;
        if (f >> dc_skip) { (void)dc_skip; }
        // New field — preset layout generation (missing = pre-cut layout
        // that still had "High Quality" at index 2).
        if (f >> pgen) { (void)pgen; }
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
        // Post stages: gen6 persists both live toggles. Older files
        // carry the retired preprocess/DFN flag and the retired
        // custom-profile flag in those slots — ignored; both stages
        // default ON (global prefs, like the gains).
        if (pgen >= 6) {
            g_wpeEnabled.store(wpeRaw != 0);
            g_notchEnabled.store(notchRaw != 0);
        } else {
            g_wpeEnabled.store(true);
            g_notchEnabled.store(true);
        }
        if (pgen >= 7) g_nsEnabled.store(nsRaw != 0);
        else           g_nsEnabled.store(true);
        // Experimental engines (WebRTC AEC3 / NKF-AEC) are hidden by
        // default — only shown when the user ticks the Appearance
        // checkbox. Older files default OFF (DTLN only).
        if (pgen >= 8) g_experimentalEngines.store(expRaw != 0);
        else           g_experimentalEngines.store(false);
        // If the persisted engine is hidden, fall back to DTLN so the
        // selection can't be left on an engine that isn't in the list.
        if (!g_experimentalEngines.load() &&
            (g_engineIndex == ENGINE_AEC3 || g_engineIndex == ENGINE_NKF)) {
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
    // Post-stage prefs are engine-scoped: set the engine first, then
    // sync — otherwise "both on" leaks into a chain that doesn't run
    // them (DTLN/AEC3 have no WPE or notch).
    SyncStagePrefsToEngine();
    g_minimizeToTray = true;
    g_listenToSelf = false;
    g_micGain.store(1.0f);
    g_outputGain.store(1.0f);
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
// gains, devices and the post-stage ticks are untouched.
// Post-stage prefs are engine-scoped (WPE + notch = NKF, DTLN-NR =
    // DTLN/AEC3). Call this whenever the engine changes so a toggle
    // left on for another engine cannot leak into a chain that doesn't
    // run it — the panel hides the irrelevant toggles anyway, but
    // stale prefs were what made Start look broken after a switch.
    static void SyncStagePrefsToEngine() {
        if (g_engineIndex == ENGINE_NKF) {
            g_wpeEnabled.store(true);
            g_notchEnabled.store(true);
            g_nsEnabled.store(true);   // harmless: NKF ignores it
        } else {
            g_nsEnabled.store(true);
            g_wpeEnabled.store(false); // DTLN/AEC3 don't run WPE
            g_notchEnabled.store(false);
        }
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
    // Post stages are engine-scoped: WPE + notch belong to NKF, the
    // DTLN-NR stage to DTLN/AEC3. Switching engines resets each to its
    // default so a toggle left on for another engine can never leak
    // into a chain that doesn't run it (and confuse Start).
    SyncStagePrefsToEngine();
}

// ============================================================
//  Engine init
// ============================================================
void ReinitEngine() {
    if (g_engine.aec3)  { Aec3Destroy(g_engine.aec3); g_engine.aec3  = nullptr; }
    if (g_engine.nkf)   { NkfDestroy(g_engine.nkf);   g_engine.nkf   = nullptr; }
    if (g_engine.dtln)  { DtlnDestroy(g_engine.dtln);  g_engine.dtln  = nullptr; }
    if (g_engine.ns)   { DtlnNsDestroy(g_engine.ns);   g_engine.ns   = nullptr; }
    if (g_engine.wpe)   { WpeDestroy(g_engine.wpe);    g_engine.wpe   = nullptr; }
    if (g_engine.notch) { NotchDestroy(g_engine.notch); g_engine.notch = nullptr; }

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
    }

    // DTLN noise reduction: the DTLN-AEC pair cancels echo but leaves
    // background noise; the DTLN-NR pair (same 512/128/257 DSP, single
    // mic feed) runs after it. AEC3 also gets it (WebRTC NS is retired).
    // NKF keeps its own WPE + notch post chain — no DTLN NS there.
    if (g_nsEnabled.load() && (eng == ENGINE_AEC3 || eng == ENGINE_DTLN))
        g_engine.ns = DtlnNsNew("models/dtln_ns_128");

    // WPE dereverb + adaptive notch are NKF-only: the DTLN and AEC3 paths
    // use the DTLN-NR noise reduction stage instead (WebRTC NS retired).
    if (eng == ENGINE_NKF && g_wpeEnabled.load())
        g_engine.wpe   = WpeNew(sr);
    if (eng == ENGINE_NKF && g_notchEnabled.load())
        g_engine.notch = NotchNew(sr);
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
    g_micRing.write((const int16_t*)pInput, frameCount);
}
void loopback_callback(ma_device*, void*, const void* pInput, ma_uint32 frameCount) {
    if (!pInput) return;
    g_refRing.write((const int16_t*)pInput, frameCount);
}
void output_callback(ma_device*, void* pOutput, const void*, ma_uint32 frameCount) {
    int16_t* out = (int16_t*)pOutput;
    int fs = frameSizeForRate(g_sampleRate.load());
    if ((int)frameCount != fs) { EmitFading(out, frameCount); return; }

    const size_t DRIFT_TARGET = (size_t)fs * 2;
    const size_t DRIFT_THRESHOLD = (size_t)fs / 16;
    if (g_micRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_micRing.skip(g_micRing.available() - DRIFT_TARGET);
    if (g_refRing.available() > DRIFT_TARGET + DRIFT_THRESHOLD)
        g_refRing.skip(g_refRing.available() - DRIFT_TARGET);

    // Loopback starves while the speakers are idle (no frames flow),
    // so never stall the mic path on it: missing ref reads as digital
    // silence, which is exactly what "nothing playing" means to the
    // AEC. (CABLE-output sessions looked dead until sound played on
    // the speakers; monitor sessions never starved because they always
    // drive the speakers — same bug, masked.)
    if (g_micRing.available() < (size_t)fs) {
        EmitFading(out, frameCount);
        return;
    }

    int16_t micFrame[MAX_FRAME_SIZE];
    int16_t refFrame[MAX_FRAME_SIZE];
    int16_t cleanedFrame[MAX_FRAME_SIZE];
    float   wpeBuf[MAX_FRAME_SIZE];
    memset(cleanedFrame, 0, sizeof(cleanedFrame));

    g_micRing.read(micFrame, fs);
    if (g_refRing.available() < (size_t)fs)
        memset(refFrame, 0, fs * sizeof(int16_t));  // speakers idle: silent ref
    else
        g_refRing.read(refFrame, fs);

    if (g_engine.type == ENGINE_AEC3 && g_engine.aec3)
        Aec3CancelEcho(g_engine.aec3, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_NKF && g_engine.nkf)
        NkfProcess(g_engine.nkf, micFrame, refFrame, cleanedFrame, fs);
    else if (g_engine.type == ENGINE_DTLN && g_engine.dtln)
        DtlnProcess(g_engine.dtln, micFrame, refFrame, cleanedFrame, fs);
    else
        memcpy(cleanedFrame, micFrame, fs * sizeof(int16_t));

    // Speech gate for the post stages: RMS hysteresis + stuck-tone
    // watchdog on the ENGINE output (never on the stages' own output,
    // so a notch cut can't flip it back). A sustained howl pins the
    // voice flag for ever; the watchdog releases the *notch* gate
    // after ~5 s so feedback suppression can actually latch the tone.
    {
        float engSq = 0;
        for (int i = 0; i < fs; i++) engSq += (float)cleanedFrame[i] * cleanedFrame[i];
        const float frameDurMs = 1000.0f * (float)fs / (float)g_sampleRate.load();
        SpeechGateUpdate(&g_speechGate, sqrtf(engSq / fs), frameDurMs);
    }

    // DTLN noise reduction: runs on the engine output, 16 kHz only
    // (the DTLN-NR model is 16 kHz; DTLN/AEC3 at other rates skip it).
    // Fail-open: a null or failed stage passes the frame straight
    // through — it never mutes the near-end voice.
    if (g_engine.ns && g_sampleRate.load() == 16000) {
        for (int i = 0; i < fs; i++)
            wpeBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
        DtlnNsProcess(g_engine.ns, wpeBuf, wpeBuf, fs);
        for (int i = 0; i < fs; i++)
            cleanedFrame[i] = clamp_s16((int)lrintf(wpeBuf[i] * 32768.0f));
    }

    // Post chain: WPE dereverb + adaptive notch are NKF-only — the DTLN
    // and AEC3 paths use the DTLN-NR noise reduction stage instead
    // (WebRTC NS is retired). Meters below show post-stage output,
    // i.e. what Discord hears.
    if (g_engine.type == ENGINE_NKF && g_engine.wpe) {
        WpeSetSpeech(g_engine.wpe, SpeechGateForWpe(&g_speechGate));
        for (int i = 0; i < fs; i++)
            wpeBuf[i] = (float)cleanedFrame[i] * (1.0f / 32768.0f);
        WpeProcess(g_engine.wpe, wpeBuf, wpeBuf, fs);
        for (int i = 0; i < fs; i++)
            cleanedFrame[i] = clamp_s16((int)lrintf(wpeBuf[i] * 32768.0f));
    }
    if (g_engine.type == ENGINE_NKF && g_engine.notch) {
        NotchSetSpeech(g_engine.notch, SpeechGateForNotch(&g_speechGate));
        NotchProcess(g_engine.notch, cleanedFrame, fs);
    }

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
        out[i] = SoftLimit((float)cleanedFrame[i] * gain / 32768.0f);
    // Remember what we emitted for fail-open fades (mic starve etc).
    memcpy(g_lastOutFrame, out, fs * sizeof(int16_t));
    g_lastOutLen = fs;
    g_lastOutValid = true;
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

    // Self-monitor routes the cleaned mic to the speakers (Speaker
    // Reference) instead of Output; otherwise Output must be usable.
    // Without VB-CABLE installed/enabled there is nowhere to send the
    // cleaned mic, so refuse to start with a pointer at the fix.
    int outIdx = g_outIndex;
    if (g_listenToSelf) {
        outIdx = g_refIndex;
    } else if (!CableInputPresent()) {
        snprintf(g_statusText, 128, "VB-CABLE not found - install/enable CABLE Input");
        return;
    }

    SaveSettings();
    ReinitEngine();
    g_sessionStartedOnce = true;  // retire the first-run setup card

    if (g_engine.type == ENGINE_NKF && !g_engine.nkf) {
        snprintf(g_statusText, 128, "Failed to load NKF model");
        return;
    }
    if (g_engine.type == ENGINE_DTLN && !g_engine.dtln) {
        snprintf(g_statusText, 128, "Failed to load DTLN model");
        return;
    }
    if (g_wpeEnabled.load() && !g_engine.wpe) {
        snprintf(g_statusText, 128, "Failed to init WPE (unsupported rate)");
        return;
    }
    if (g_notchEnabled.load() && !g_engine.notch) {
        snprintf(g_statusText, 128, "Failed to init notch (unsupported rate)");
        return;
    }

    g_micRing.reset();
    g_refRing.reset();
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
    const char* engineName =
        (g_engine.type == ENGINE_AEC3) ? "AEC3" :
        (g_engine.type == ENGINE_NKF)  ? "NKF-AEC" : "DTLN-AEC";
    snprintf(g_statusText, 128, "Running (%d Hz, %s%s%s)%s", sr, engineName,
             g_engine.wpe ? " + WPE" : "",
             g_engine.notch ? " + Notch" : "",
             g_listenToSelf ? " [monitor]" : "");
}

void StopAEC() {
    if (!g_isRunning) return;
    ma_device_uninit(&g_micDevice);
    ma_device_uninit(&g_loopbackDevice);
    ma_device_uninit(&g_outputDevice);
    g_isRunning = false;
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
    if (g_engineIndex == ENGINE_NKF || g_engineIndex == ENGINE_DTLN) {
        ImGui::TextDisabled("16 kHz (automatic)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This profile always runs at 16 kHz - nothing to choose.");
    } else {
        if (ImGui::Combo("##rate", &g_sampleRateIndex, rates, IM_ARRAYSIZE(rates))) {
            g_sampleRate.store(atoi(rates[g_sampleRateIndex]));
            SaveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Higher = better quality, more CPU\nAEC3 best at 48000\n"
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Delay lock: how far the speaker reference lags the mic\n"
                "(playback chain). Locked = aligned reference lets NKF cancel.\n"
                "During a mic-test loop the engine keeps adapting — it must\n"
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

    ImGui::SeparatorText("Profile");

    ImGui::TextUnformatted("Profile");
    ImGui::SameLine(190.0f);
    ImGui::SetNextItemWidth(-1);
    // The label shows the engine itself, plus the chain segments while
    // the post stages are ticked — no hidden state.
std::string profilePreview = PROFILES[g_profileIndex].name;
    if (g_wpeEnabled.load() && g_engineIndex == ENGINE_NKF)
        profilePreview += " -> WPE";
    if (g_notchEnabled.load() && g_engineIndex == ENGINE_NKF)
        profilePreview += " -> Notch";
    if (g_nsEnabled.load() &&
        (g_engineIndex == ENGINE_DTLN || g_engineIndex == ENGINE_AEC3))
        profilePreview += " -> NS";
    bool wasRunning = g_isRunning;
    if ( ImGui::BeginCombo("##profile", profilePreview.c_str())) {
        for (int i = 0; i < PROFILE_COUNT; i++) {
            std::string label = PROFILES[i].name;
            EngineType le = (EngineType)PROFILES[i].engine;
            if (g_wpeEnabled.load() && le == ENGINE_NKF) label += " -> WPE";
            if (g_notchEnabled.load() && le == ENGINE_NKF) label += " -> Notch";
            if (g_nsEnabled.load() &&
                (le == ENGINE_DTLN || le == ENGINE_AEC3)) label += " -> NS";
            bool sel = (g_profileIndex == i);
            // Experimental engines (WebRTC AEC3 / NKF-AEC) are hidden
            // unless the user ticked the Appearance checkbox — the
            // default is DTLN only.
            if (!g_experimentalEngines.load() &&
                (le == ENGINE_AEC3 || le == ENGINE_NKF)) continue;
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (!sel) {
                    ApplyProfile(i);
                    if (wasRunning) { StopAEC(); StartAEC(); }
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Engine selection (the engine only changes through this menu):\n"
            "DTLN-AEC 128 - default; best echo + reverb handling, no post filter.\n"
            "WebRTC AEC3 - Chrome-style echo cancellation, reliable baseline.\n"
            "NKF-AEC - weak CPUs, still full cleanup.\n"
            "The ticks below append '-> WPE' / '-> Notch' to the chain shown here.");

    // Post stages: engine-aware — each toggle only appears for the engine
    // that actually runs it. Live restart on toggle (same path as
    // switching profiles mid-call). Global prefs: a profile switch
    // does not reset them.
    {
        const bool isNkf = (g_engineIndex == ENGINE_NKF);
        const bool isDtlnOrAec3 = (g_engineIndex == ENGINE_DTLN ||
                                   g_engineIndex == ENGINE_AEC3);
        if (isNkf) {
            bool wpe = g_wpeEnabled.load();
            if ( ImGui::Checkbox("Dereverb (WPE)", &wpe)) {
                g_wpeEnabled.store(wpe);
                SaveSettings();
                if (g_isRunning) { StopAEC(); StartAEC(); }
            }
            if ( ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Weighted prediction error dereverberation: eats late room\n"
                    "echo and tail (~32 ms extra delay). Runs after the engine;\n"
                    "adaptation tightens while you talk so voice level survives.");
        }
        if (isNkf) {
            bool nch = g_notchEnabled.load();
            if ( ImGui::Checkbox("Feedback suppression (notch)", &nch)) {
                g_notchEnabled.store(nch);
                SaveSettings();
                if (g_isRunning) { StopAEC(); StartAEC(); }
            }
            if ( ImGui::IsItemHovered()) {
                std::string tip =
                    "Two adaptive notch filters that track narrowband howling /\n"
                    "ringing tones (speaker-mic loops). Exact bypass until a tone\n"
                    "is actually captured; voice harmonics never latch it.";
                if (g_engine.notch) {
                    const double f0 = NotchFreq(g_engine.notch, 0);
                    const double f1 = NotchFreq(g_engine.notch, 1);
                    const int e0 = NotchEngaged(g_engine.notch, 0);
                    const int e1 = NotchEngaged(g_engine.notch, 1);
                    char live[192];
                    if (e0 && e1)
                        snprintf(live, sizeof live, "\nLive: engaged %.0f Hz + %.0f Hz", f0, f1);
                    else if (e0)
                        snprintf(live, sizeof live, "\nLive: engaged %.0f Hz (2nd idle)", f0);
                    else if (e1)
                        snprintf(live, sizeof live, "\nLive: engaged %.0f Hz (1st idle)", f1);
                    else
                        snprintf(live, sizeof live, "\nLive: bypassed (no tone latched)");
                    tip += live;
                    const bool frozen = SpeechGateForNotch(&g_speechGate) != 0;
                    snprintf(live, sizeof live, "\nAdaptation: %s%s",
                             frozen ? "frozen (voice)" : "running",
                             g_speechGate.stuck ? " [sustained tone]" : "");
                    tip += live;
                } else {
                    tip += "\nLive: not running";
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        if (isDtlnOrAec3) {
            bool ns = g_nsEnabled.load();
            if ( ImGui::Checkbox("Noise suppression (DTLN-NS)", &ns)) {
                g_nsEnabled.store(ns);
                SaveSettings();
                if (g_isRunning) { StopAEC(); StartAEC(); }
            }
            if ( ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "DTLN noise reduction: a second DTLN pair that removes\n"
                    "background noise from the mic (same DSP as the echo\n"
                    "canceller, minus the loud-playback feed). Runs after the\n"
                    "engine on the DTLN and WebRTC AEC3 paths. NKF uses its own\n"
                    "WPE dereverb + notch instead.");
        }
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
            "When ON:  X hides the window, tray icon stays active\n"
            "When OFF: X closes the app completely");

    if (g_minimizeToTray) {
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click the tray icon to Exit the app.");
    }

    // Experimental engines: WebRTC AEC3 and NKF-AEC are hidden from the
    // profile list by default (DTLN is the only thing most users need).
    // Tick this to show them — switching back to DTLN afterwards hides
    // them again and leaves the selection on DTLN.
    bool exp = g_experimentalEngines.load();
    if ( ImGui::Checkbox("Show experimental engines (WebRTC AEC3, NKF-AEC)", &exp)) {
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
            "Off (default): the profile list shows DTLN-AEC 128 only.\n"
            "On: also shows WebRTC AEC3 and NKF-AEC. These are real\n"
            "engines but less tested than the default — switch back to\n"
            "DTLN-AEC 128 to hide them again.");
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
    ImGui::BulletText("Three processing profiles (DTLN / WebRTC AEC3 / NKF-AEC)");
    ImGui::BulletText("WPE dereverb + adaptive notch feedback suppression on top of echo removal");
    ImGui::BulletText("Real-time processing with low CPU usage");
    ImGui::BulletText("Works with speakers, earphones, and headsets");
    ImGui::BulletText("Selectable sample rate (16 / 48 kHz)");
    ImGui::BulletText("Live level meters with peak hold");
    ImGui::BulletText("Optional minimize to system tray");

    ImGui::Spacing();
    ImGui::SeparatorText("Credits");
    ImGui::BulletText("WebRTC AP     - Google (BSD-3)");
    ImGui::BulletText("NKF-AEC       - Jiang et al. (ICASSP 2023, MIT)");
    ImGui::BulletText("DTLN-AEC      - Westhausen & Meyer (ICASSP 2021, MIT)");
    ImGui::BulletText("WPE dereverb - in-tree (weighted prediction error)");
    ImGui::BulletText("Adaptive notch - Widrow & Hoff LMS (1960)");
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

    ImGui::Spacing();

    static int activeTab = 0;  // frame-local: which tab is open (footer buttons follow it)
    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            activeTab = 0;
            ImGui::Spacing();
            DrawAudioTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            activeTab = 1;
            ImGui::Spacing();
            DrawAppearanceTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            activeTab = 2;
            ImGui::Spacing();
            DrawAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Footer buttons (Start / Reset) live on the Audio tab only.
    // Live Levels below stay visible on every tab while running.
    if (activeTab == 0) {
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
            ImGui::TextDisabled("Reference: silent (normal if nothing is playing  - \n"
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
    if (g_engine.aec3)  Aec3Destroy(g_engine.aec3);
    if (g_engine.nkf)   NkfDestroy(g_engine.nkf);
    if (g_engine.dtln)  DtlnDestroy(g_engine.dtln);
    if (g_engine.ns)   DtlnNsDestroy(g_engine.ns);
    if (g_engine.wpe)   WpeDestroy(g_engine.wpe);
    if (g_engine.notch) NotchDestroy(g_engine.notch);
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
