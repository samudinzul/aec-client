#include "gtcrn_wrapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"

// v0 plumbing: loads the session, captures the IO contract for the V1
// inference path, runs passthrough. V1 fills the marked section once
// exp/probe.py output confirms node names/shapes on the dev machine.

struct GtcrnHandle {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    std::vector<std::vector<int64_t>> inShapes;
    std::vector<std::vector<int64_t>> outShapes;
    int mixIndex = -1;  // input carrying the 257-bin spectrum
    std::string ioDump;
    std::string error;
    bool ready = false;
    float inRms = 0.0f, outRms = 0.0f;
    int dropped = 0;
};

static std::string ShapeStr(const std::vector<int64_t>& s) {
    std::string r = "[";
    for (size_t i = 0; i < s.size(); i++) {
        if (i) r += ",";
        r += std::to_string(s[i]);
    }
    return r + "]";
}

GtcrnHandle* GtcrnNew(const char* modelPath) {
    GtcrnHandle* h = new GtcrnHandle();
    if (!modelPath || !*modelPath) {
        h->error = "gtcrn: no model path (run exp/get_models.sh)";
        return h;
    }
    try {
        h->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                            "gtcrn-exp");
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
        const size_t ni = h->session->GetInputCount();
        const size_t no = h->session->GetOutputCount();
        for (size_t i = 0; i < ni; i++) {
            Ort::AllocatedStringPtr nm =
                h->session->GetInputNameAllocated(i, alloc);
            h->inNames.emplace_back(nm.get());
            Ort::TypeInfo ti = h->session->GetInputTypeInfo(i);
            h->inShapes.push_back(
                ti.GetTensorTypeAndShapeInfo().GetShape());
        }
        for (size_t i = 0; i < no; i++) {
            Ort::AllocatedStringPtr nm =
                h->session->GetOutputNameAllocated(i, alloc);
            h->outNames.emplace_back(nm.get());
            Ort::TypeInfo ti = h->session->GetOutputTypeInfo(i);
            h->outShapes.push_back(
                ti.GetTensorTypeAndShapeInfo().GetShape());
        }
        // The spectrum input is the one with a 257-bin dimension
        // (upstream: mix[1,257,1,2]); the rest are state caches.
        for (size_t i = 0; i < h->inShapes.size(); i++)
            for (int64_t d : h->inShapes[i])
                if (d == 257) h->mixIndex = (int)i;
        std::string dump;
        for (size_t i = 0; i < h->inNames.size(); i++)
            dump += "IN " + h->inNames[i] + " " +
                    ShapeStr(h->inShapes[i]) + "\n";
        for (size_t i = 0; i < h->outNames.size(); i++)
            dump += "OUT " + h->outNames[i] + " " +
                    ShapeStr(h->outShapes[i]) + "\n";
        h->ioDump = dump;
        if (h->mixIndex < 0) {
            h->error =
                "gtcrn: no 257-bin input found — unexpected graph; "
                "see GtcrnDump. Staying off.";
            return h;
        }
        h->ready = true;
    } catch (const Ort::Exception& e) {
        h->error = std::string("gtcrn: ORT load failed: ") + e.what();
    } catch (const std::exception& e) {
        h->error = std::string("gtcrn: load failed: ") + e.what();
    }
    return h;
}

int GtcrnReady(const GtcrnHandle* h) { return (h && h->ready) ? 1 : 0; }

int GtcrnProcess(GtcrnHandle* h, const float* in, float* out, int n) {
    if (!h || n <= 0) return 0;
    if (!in || !out) return 0;
    // ---- V1 (not yet): FIFO 256-hop accumulation -> sqrt-Hann STFT(512)
    // -> Run(mix + conv/tra/inter caches) -> ISTFT overlap-add ->
    // cache carry. See exp/README contract + probe.py node names. ----
    // v0: fail-open passthrough with RMS followers (never mute).
    double si = 0.0, so = 0.0;
    for (int i = 0; i < n; i++) {
        out[i] = in[i];
        si += (double)in[i] * in[i];
        so += (double)out[i] * out[i];
    }
    h->inRms = (float)sqrt(si / n);
    h->outRms = (float)sqrt(so / n);
    return h->ready ? 1 : 0;
}

void GtcrnReset(GtcrnHandle* h) {
    if (!h) return;
    h->inRms = h->outRms = 0.0f;
    h->dropped = 0;
}

void GtcrnDestroy(GtcrnHandle* h) { delete h; }

const char* GtcrnLastError(const GtcrnHandle* h) {
    return (h && !h->error.empty()) ? h->error.c_str() : "";
}

void GtcrnStatsGet(const GtcrnHandle* h, GtcrnStats* s) {
    if (!s) return;
    if (!h) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->ready = h->ready ? 1 : 0;
    s->neural = 0;  // v0 bypass; V1 sets 1 when Run() engages
    s->inRms = h->inRms;
    s->outRms = h->outRms;
    s->dropped = h->dropped;
}

void GtcrnDump(const GtcrnHandle* h) {
    if (!h) {
        printf("gtcrn: null handle\n");
        return;
    }
    printf("gtcrn: ready=%d mixInput=%d\n%s",
           h->ready ? 1 : 0, h->mixIndex,
           h->ioDump.empty() ? "(no session)\n" : h->ioDump.c_str());
    if (!h->error.empty()) printf("gtcrn: note: %s\n", h->error.c_str());
    printf("gtcrn: inRms=%.1f outRms=%.1f dropped=%d (v0 bypass)\n",
           (double)h->inRms * 32768.0, (double)h->outRms * 32768.0,
           h->dropped);
}
