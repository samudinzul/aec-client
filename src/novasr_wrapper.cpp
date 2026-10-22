#include "novasr_wrapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"
#include <windows.h>

// NovaSR streaming tail: 16 kHz in -> 48 kHz out through the baked graph
// (mix [1,1,T] -> enh [1,1,3T-4]), one STEP per call with HALO overlap
// discarded on both sides (see the header contract). The -4 quirk of the
// export (12-tap resampler alignment) is absorbed here: a WINDOW-sample
// run yields exactly 3*STEP kept samples.

namespace {

constexpr int kHalo = 128;    // @16 kHz = 8 ms algorithmic delay
constexpr int kStep = 160;    // chain frame @16 kHz (frameSizeForRate)
constexpr int kWindow = kHalo * 2 + kStep;  // 416
constexpr int kKeepLo = kHalo * 3;          // 384
constexpr int kKeepHi = (kHalo + kStep) * 3;  // 864 -> 480 kept

double NowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

}  // namespace

struct NovasrHandle {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeCPU);
    std::string inName, outName;
    std::vector<float> fifo;   // 16 kHz input delay line
    bool primed = false;
    char lastError[256] = {0};
    bool neural = false;
    float lastInRms = 0.0f, lastOutRms = 0.0f;
    float msEma = 0.0f;
};

static void SetErr(NovasrHandle* h, const char* msg) {
    if (!h || !msg) return;
    strncpy(h->lastError, msg, sizeof(h->lastError) - 1);
    h->lastError[sizeof(h->lastError) - 1] = '\0';
}

NovasrHandle* NovasrNew(const char* modelPath) {
    NovasrHandle* h = new (std::nothrow) NovasrHandle();
    if (!h) return nullptr;
    h->fifo.reserve((size_t)kWindow * 2);
    if (!modelPath || !modelPath[0]) {
        SetErr(h, "novasr: no model path (rebuild with exp/novasr/novasr.onnx staged)");
        return h;
    }
    try {
        h->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                             "novasr-exp");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Windows ONNX Runtime requires wide-string paths (same
        // conversion as third_party/REAL_TIME_NKF_AEC/c/NKFImpl.h).
        std::wstring widePath(modelPath, modelPath + strlen(modelPath));
        h->session =
            std::make_unique<Ort::Session>(*h->env, widePath.c_str(), opts);
        Ort::AllocatorWithDefaultOptions alloc;
        if (h->session->GetInputCount() != 1 ||
            h->session->GetOutputCount() != 1) {
            SetErr(h, "novasr: need 1-in/1-out graph (mix->enh)");
            h->session.reset();
            return h;
        }
        Ort::AllocatedStringPtr inNm =
            h->session->GetInputNameAllocated(0, alloc);
        Ort::AllocatedStringPtr outNm =
            h->session->GetOutputNameAllocated(0, alloc);
        h->inName = inNm.get();
        h->outName = outNm.get();
    } catch (const Ort::Exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "novasr: %s", e.what());
        SetErr(h, buf);
        h->session.reset();
    } catch (const std::exception& e) {
        char buf[256];
        snprintf(buf, sizeof(buf), "novasr: %s", e.what());
        SetErr(h, buf);
        h->session.reset();
    }
    return h;
}

int NovasrReady(const NovasrHandle* h) {
    return (h && h->session) ? 1 : 0;
}

// Pure linear 16->48, same endpoint-exact math as the harness baseline:
// the fail-open fallback (and the no-model path shape).
static void LinearUp3(const float* x, int n, float* base) {
    if (n <= 0) return;
    if (n == 1) { base[0] = base[1] = base[2] = x[0]; return; }
    for (int j = 0; j < n * 3; j++) {
        const float pos = (float)j * (float)(n - 1) / (float)(n * 3 - 1);
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        if (i0 > n - 2) i0 = n - 2;
        const float frac = pos - (float)i0;
        base[j] = x[i0] * (1.0f - frac) + x[i0 + 1] * frac;
    }
}

int NovasrProcess(NovasrHandle* h, const float* in, float* out, int n) {
    if (!h || !in || !out || n <= 0) return 0;
    // Telemetry first (fail-open paths below still report levels).
    {
        double s = 0.0;
        for (int i = 0; i < n; i++) s += (double)in[i] * in[i];
        h->lastInRms = (float)sqrt(s / (double)n);
    }
    if (!h->session) {
        LinearUp3(in, n, out);
        double s = 0.0;
        for (int i = 0; i < n * 3; i++) s += (double)out[i] * out[i];
        h->lastOutRms = (float)sqrt(s / (double)(n * 3));
        h->neural = false;
        return 0;
    }
    // Delay line: prime once with edge replication (8 ms smear, never
    // silence), then every call appends n and emits 3 *** consumed.
    if (!h->primed) {
        h->fifo.assign((size_t)kHalo * 2, in[0]);
        h->primed = true;
    }
    h->fifo.insert(h->fifo.end(), in, in + n);
    int emitted = 0;   // out advances in kStep blocks; remainder stays
    int consumed = 0;
    bool okAll = true;
    std::vector<float> win;
    win.reserve(kWindow);
    while ((int)h->fifo.size() >= consumed + kWindow &&
           emitted + kStep * 3 <= n * 3) {
        win.assign(h->fifo.begin() + consumed,
                   h->fifo.begin() + consumed + kWindow);
        const double t0 = NowMs();
        bool ok = false;
        try {
            std::vector<int64_t> shape = {1, 1, (int64_t)kWindow};
            Ort::Value inT = Ort::Value::CreateTensor<float>(
                h->memInfo, win.data(), win.size(), shape.data(), shape.size());
            const char* inPtr = h->inName.c_str();
            const char* outPtr = h->outName.c_str();
            std::vector<Ort::Value> outs = h->session->Run(
                Ort::RunOptions{nullptr}, &inPtr, &inT, 1, &outPtr, 1);
            size_t m = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (m == (size_t)(kWindow * 3 - 4)) {
                const float* y = outs[0].GetTensorData<float>();
                for (int j = 0; j < kStep * 3; j++)
                    out[emitted + j] = y[kKeepLo + j];
                ok = true;
            }
        } catch (const Ort::Exception& e) {
            SetErr(h, e.what());
        } catch (const std::exception& e) {
            SetErr(h, e.what());
        }
        const double dt = NowMs() - t0;
        h->msEma += (float)((dt - h->msEma) * 0.05);
        if (!ok) {
            // Single-block fallback keeps the stream whole (fail-open);
            // the latch above explains the row in the UI.
            LinearUp3(win.data() + kHalo, kStep, out + emitted);
            okAll = false;
        }
        emitted += kStep * 3;
        consumed += kStep;
    }
    h->fifo.erase(h->fifo.begin(), h->fifo.begin() + consumed);
    // Remainder (< kWindow) stays buffered: its 3x arrives with the
    // next call (constant 8 ms delay — no drift, no seams). Zero-fill
    // any tail the caller over-allocated (defensive; live n == 160).
    for (int j = emitted; j < n * 3; j++) out[j] = 0.0f;
    {
        double s = 0.0;
        for (int j = 0; j < emitted; j++) s += (double)out[j] * out[j];
        h->lastOutRms = emitted ? (float)sqrt(s / (double)emitted) : 0.0f;
    }
    h->neural = okAll && emitted > 0;
    return (okAll && emitted > 0) ? 1 : 0;
}

void NovasrReset(NovasrHandle* h) {
    if (!h) return;
    h->fifo.clear();
    h->primed = false;
    h->lastError[0] = 0;
    h->neural = false;
    h->lastInRms = h->lastOutRms = h->msEma = 0.0f;
}

void NovasrDestroy(NovasrHandle* h) {
    delete h;
}

const char* NovasrLastError(const NovasrHandle* h) {
    if (!h) return "null handle";
    return h->lastError;
}

void NovasrStatsGet(const NovasrHandle* h, NovasrStats* s) {
    if (!s) return;
    if (!h) { memset(s, 0, sizeof(*s)); return; }
    s->ready = NovasrReady(h);
    s->neural = h->neural ? 1 : 0;
    s->inRms = h->lastInRms;
    s->outRms = h->lastOutRms;
    s->ms = h->msEma;
}

void NovasrDump(const NovasrHandle* h) {
    if (!h) { printf("novasr: null\n"); return; }
    printf("novasr: in=%s ready=%d neural=%d ms=%.2f err=%s\n",
           h->inName.c_str(), NovasrReady(h), h->neural ? 1 : 0,
           (double)h->msEma, h->lastError[0] ? h->lastError : "ok");
}
