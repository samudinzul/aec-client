// Hush NS backend — thin loader + frame bridge over the prebuilt
// weya_nc library. Mirrors the dtln_ns_wrapper conventions:
// dynamic LoadLibrary (same pattern as the TFLite loader), always
// returns a handle, Ready()==0 stays off, processing never mutes.

#include "hush_wrapper.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <string>

// weya_nc C ABI (deployment/include/weya_nc.h) — resolved at
// runtime so the app starts and runs fine without the DLL.
typedef void* WeyaModelPtr;
typedef void* WeyaSessionPtr;
typedef WeyaModelPtr (*FnModelLoadPath)(const char*);
typedef void (*FnModelFree)(WeyaModelPtr);
typedef WeyaSessionPtr (*FnSessionCreate)(const WeyaModelPtr, size_t, float);
typedef void (*FnSessionFree)(WeyaSessionPtr);
typedef size_t (*FnFrameLen)(const WeyaSessionPtr);
typedef float (*FnProcess)(WeyaSessionPtr, const float*, float*);
typedef void (*FnReset)(WeyaSessionPtr);

struct HushApi {
#ifdef _WIN32
    HMODULE dll = nullptr;
#else
    void* dll = nullptr;
#endif
    FnModelLoadPath modelLoadPath = nullptr;
    FnModelFree modelFree = nullptr;
    FnSessionCreate sessionCreate = nullptr;
    FnSessionFree sessionFree = nullptr;
    FnFrameLen frameLen = nullptr;
    FnProcess process = nullptr;
    FnReset reset = nullptr;
    bool ok = false;
};

struct HushHandle {
    HushApi api;
    WeyaModelPtr model = nullptr;
    WeyaSessionPtr session = nullptr;
    int frameLen = 0;
    char lastError[256] = { 0 };
    // Frame bridge: the 16 kHz desktop path always feeds exactly one
    // Hush frame (160 samples), so these FIFOs sit empty in practice;
    // they only exist so odd sizes degrade to a hold instead of a cut.
    static const int kFifoCap = 1024;
    float inFifo[kFifoCap] = { 0 };
    int inFill = 0;
    float outFifo[kFifoCap] = { 0 };
    int outFill = 0;
    float lastOut = 0.0f;
    float lastInRms = 0.0f, lastOutRms = 0.0f;
    int dropped = 0;
};

static void HushSetError(HushHandle* h, const char* msg) {
    if (!h || !msg) return;
    strncpy(h->lastError, msg, sizeof(h->lastError) - 1);
    h->lastError[sizeof(h->lastError) - 1] = '\0';
}

static bool FileExists(const char* p) {
    if (!p || !*p) return false;
    FILE* f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static bool LoadHushApi(HushApi& api, char* errBuf, size_t errSize) {
#ifdef _WIN32
    api.dll = LoadLibraryA("weya_nc.dll");  // exe dir first, like TFLite
#else
    (void)api; (void)errBuf; (void)errSize;
    return false;
#endif
#ifdef _WIN32
    if (!api.dll) {
        snprintf(errBuf, errSize, "weya_nc.dll not found next to the exe");
        return false;
    }
#define HUSH_RESOLVE(field, sym)                                   \
    api.field = reinterpret_cast<decltype(api.field)>(             \
        GetProcAddress(api.dll, sym));                             \
    if (!api.field) {                                              \
        snprintf(errBuf, errSize, "weya_nc.dll missing symbol: %s", sym); \
        FreeLibrary(api.dll); api.dll = nullptr;                   \
        return false;                                              \
    }
    HUSH_RESOLVE(modelLoadPath, "weya_nc_model_load_from_path");
    HUSH_RESOLVE(modelFree, "weya_nc_model_free");
    HUSH_RESOLVE(sessionCreate, "weya_nc_session_create");
    HUSH_RESOLVE(sessionFree, "weya_nc_session_free");
    HUSH_RESOLVE(frameLen, "weya_nc_get_frame_length");
    HUSH_RESOLVE(process, "weya_nc_process_frame");
    HUSH_RESOLVE(reset, "weya_nc_reset");
#undef HUSH_RESOLVE
    api.ok = true;
    return true;
#endif
}

extern "C" {

HushHandle* HushNew(const char* modelPath) {
    auto* h = new HushHandle();
    const char* mp = (modelPath && *modelPath) ? modelPath
                                               : "models/hush_16k_onnx.tar.gz";
    char err[256] = { 0 };
    if (!LoadHushApi(h->api, err, sizeof err)) {
        HushSetError(h, err[0] ? err : "weya_nc library unavailable");
        return h;
    }
    if (!FileExists(mp)) {
        char buf[256];
        snprintf(buf, sizeof buf, "hush bundle missing: %s", mp);
        HushSetError(h, buf);
        return h;
    }
    h->model = h->api.modelLoadPath(mp);
    if (!h->model) {
        HushSetError(h, "weya_nc_model_load_from_path returned NULL");
        return h;
    }
    // 16 kHz stream, 18 dB attenuation cap (natural noise floor;
    // 100 = unlimited, heard as aggressive gating on voice).
    h->session = h->api.sessionCreate(h->model, 16000, 18.0f);
    if (!h->session) {
        HushSetError(h, "weya_nc_session_create returned NULL");
        return h;
    }
    size_t fl = h->api.frameLen(h->session);
    // frameOut below is 512 floats: refuse anything bigger (the
    // shipped model reports 160; this is future-proofing, not a path).
    if (fl == 0 || fl > 512) {
        char buf[256];
        snprintf(buf, sizeof buf, "bogus hush frame length %llu",
                 (unsigned long long)fl);
        HushSetError(h, buf);
        return h;
    }
    h->frameLen = (int)fl;
    return h;
}

int HushReady(const HushHandle* h) {
    return (h && h->session && h->frameLen > 0) ? 1 : 0;
}

int HushFrameLen(const HushHandle* h) {
    return (h && h->session) ? h->frameLen : 0;
}

int HushProcess(HushHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    if (!HushReady(h)) return 0;  // caller treats 0 as passthrough
    // RMS followers run on every call (valid or not), like DTLN-NS.
    double si = 0.0;
    for (int i = 0; i < n; i++) si += (double)in[i] * in[i];
    h->lastInRms = (float)sqrt(si / n);
    // Feed whole native frames; leftovers wait in the FIFO.
    for (int i = 0; i < n; i++) {
        if (h->inFill < HushHandle::kFifoCap) {
            h->inFifo[h->inFill++] = in[i];
        } else {
            h->dropped++;
        }
        while (h->inFill >= h->frameLen) {
            // frameLen <= 512 guaranteed at New; the DLL writes
            // exactly frameLen samples into frameOut.
            float frameOut[512];
            int fl = h->frameLen;
            h->api.process(h->session, h->inFifo, frameOut);
            memmove(h->inFifo, h->inFifo + fl,
                    (size_t)(h->inFill - fl) * sizeof(float));
            h->inFill -= fl;
            for (int k = 0; k < fl; k++) {
                if (h->outFill < HushHandle::kFifoCap)
                    h->outFifo[h->outFill++] = frameOut[k];
                else
                    h->dropped++;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (h->outFill > 0) {
            out[i] = h->outFifo[0];
            memmove(h->outFifo, h->outFifo + 1,
                    (size_t)(h->outFill - 1) * sizeof(float));
            h->outFill--;
            h->lastOut = out[i];
        } else {
            // Priming shortfall: hold the last value (a step to zero
            // clicks; flat DC holds inaudibly until frames flow).
            out[i] = h->lastOut;
        }
    }
    double so = 0.0;
    for (int i = 0; i < n; i++) so += (double)out[i] * out[i];
    h->lastOutRms = (float)sqrt(so / n);
    return 1;
}

void HushReset(HushHandle* h) {
    if (!h) return;
    h->inFill = 0;
    h->outFill = 0;
    h->lastOut = 0.0f;
    h->lastInRms = h->lastOutRms = 0.0f;
    h->dropped = 0;
    if (h->api.reset && h->session) h->api.reset(h->session);
}

void HushDestroy(HushHandle* h) {
    if (!h) return;
    if (h->api.sessionFree && h->session) h->api.sessionFree(h->session);
    if (h->api.modelFree && h->model) h->api.modelFree(h->model);
#ifdef _WIN32
    if (h->api.dll) FreeLibrary(h->api.dll);
#endif
    delete h;
}

const char* HushLastError(const HushHandle* h) {
    return (h && h->lastError[0]) ? h->lastError : "";
}

void HushStatsGet(const HushHandle* h, HushStats* s) {
    if (!s) return;
    if (!h) {
        s->backend = 0; s->frameLen = 0;
        s->inRms = s->outRms = 0.0f; s->dropped = 0;
        return;
    }
    s->backend = HushReady(h) ? 3 : 0;
    s->frameLen = HushFrameLen(h);
    s->inRms = h->lastInRms;
    s->outRms = h->lastOutRms;
    s->dropped = h->dropped;
}

}  // extern "C"
