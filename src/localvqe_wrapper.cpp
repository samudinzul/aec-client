#include "localvqe_wrapper.h"
#include "localvqe_api.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <direct.h>

struct LocalVqeHandle {
    localvqe_ctx_t ctx = 0;
    std::vector<int16_t> micAccum;
    std::vector<int16_t> refAccum;
    std::vector<int16_t> outAccum;
};

static const int LOCALVQE_HOP = 256;

extern "C" {

LocalVqeHandle* LocalVqeNew(const char* modelPath) {
    // Redirect stdout + stderr to files so we can see what localvqe prints
    freopen("localvqe_stdout.txt", "w", stdout);
    freopen("localvqe_stderr.txt", "w", stderr);

    FILE* log = fopen("localvqe_diag.txt", "w");
    if (log) {
        fprintf(log, "=== LocalVQE Diagnostic ===\n");
        fprintf(log, "Model path: %s\n", modelPath);
        fflush(log);
    }

    // Step 1: List available backends BEFORE loading model
    if (log) { fprintf(log, "Calling localvqe_list_devices()...\n"); fflush(log); }
    localvqe_list_devices();
    if (log) { fprintf(log, "localvqe_list_devices() returned\n"); fflush(log); }

    // Step 2: Create options and set model path explicitly
    if (log) { fprintf(log, "Creating options...\n"); fflush(log); }
    localvqe_options_t opts = localvqe_options_new();
    if (opts == 0) {
        if (log) { fprintf(log, "localvqe_options_new() returned NULL\n"); fclose(log); }
        return nullptr;
    }

    int ret = localvqe_options_set_model_path(opts, modelPath);
    if (log) { fprintf(log, "set_model_path returned: %d\n", ret); fflush(log); }

    // Step 3: Try to create with explicit options
    if (log) { fprintf(log, "Calling localvqe_new_with_options()...\n"); fflush(log); }
    localvqe_ctx_t ctx = localvqe_new_with_options(opts);
    if (log) { fprintf(log, "localvqe_new_with_options returned: %p\n", (void*)ctx); fflush(log); }

    localvqe_options_free(opts);

    // Fallback: try direct new
    if (ctx == 0) {
        if (log) { fprintf(log, "Fallback: calling localvqe_new() directly...\n"); fflush(log); }
        ctx = localvqe_new(modelPath);
        if (log) { fprintf(log, "localvqe_new returned: %p\n", (void*)ctx); fflush(log); }
    }

    if (log) {
        if (ctx == 0) {
            fprintf(log, "ALL METHODS FAILED\n");
            fprintf(log, "Check localvqe_stderr.txt for the real error.\n");
        } else {
            fprintf(log, "SUCCESS ctx=%p\n", (void*)ctx);
        }
        fclose(log);
    }

    if (ctx == 0) {
        return nullptr;
    }

    auto* h = new LocalVqeHandle();
    h->ctx = ctx;

    localvqe_set_noise_gate(h->ctx, 1, -45.0f);

    h->micAccum.reserve(LOCALVQE_HOP * 4);
    h->refAccum.reserve(LOCALVQE_HOP * 4);
    h->outAccum.reserve(LOCALVQE_HOP * 4);
    return h;
}

// ... rest of functions unchanged ...

void LocalVqeProcess(LocalVqeHandle* h, const int16_t* mic, const int16_t* ref,
                     int16_t* out, int hopSamples) {
    if (!h || h->ctx == 0) return;

    h->micAccum.insert(h->micAccum.end(), mic, mic + hopSamples);
    h->refAccum.insert(h->refAccum.end(), ref, ref + hopSamples);

    while (h->micAccum.size() >= (size_t)LOCALVQE_HOP &&
           h->refAccum.size() >= (size_t)LOCALVQE_HOP) {
        int16_t outFrame[LOCALVQE_HOP];
        int ret = localvqe_process_frame_s16(h->ctx,
                                              h->micAccum.data(),
                                              h->refAccum.data(),
                                              LOCALVQE_HOP, outFrame);
        if (ret == 0) {
            h->outAccum.insert(h->outAccum.end(), outFrame, outFrame + LOCALVQE_HOP);
        } else {
            h->outAccum.insert(h->outAccum.end(),
                               h->micAccum.begin(),
                               h->micAccum.begin() + LOCALVQE_HOP);
        }
        h->micAccum.erase(h->micAccum.begin(), h->micAccum.begin() + LOCALVQE_HOP);
        h->refAccum.erase(h->refAccum.begin(), h->refAccum.begin() + LOCALVQE_HOP);
    }

    for (int i = 0; i < hopSamples; i++) {
        if (!h->outAccum.empty()) {
            out[i] = h->outAccum.front();
            h->outAccum.erase(h->outAccum.begin());
        } else {
            out[i] = 0;
        }
    }
}

void LocalVqeReset(LocalVqeHandle* h) {
    if (!h || h->ctx == 0) return;
    h->micAccum.clear();
    h->refAccum.clear();
    h->outAccum.clear();
    localvqe_reset(h->ctx);
}

void LocalVqeDestroy(LocalVqeHandle* h) {
    if (!h) return;
    if (h->ctx) localvqe_free(h->ctx);
    delete h;
}

const char* LocalVqeLastError(LocalVqeHandle* h) {
    if (!h || h->ctx == 0) return "No context";
    return localvqe_last_error(h->ctx);
}

void LocalVqeSetNoiseGate(LocalVqeHandle* h, int enabled, float threshold_dbfs) {
    if (!h || h->ctx == 0) return;
    localvqe_set_noise_gate(h->ctx, enabled, threshold_dbfs);
}

} // extern "C"