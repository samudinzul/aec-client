/**
 * LocalVQE model — GGUF loading and tensor helpers.
 *
 * Forward pass and streaming are implemented via GGML graphs in
 * localvqe_graph.cpp. This file provides only the model loading
 * function used by block-level tests.
 */

#include "localvqe_model.h"

#include "ggml.h"
#include "gguf.h"

#include <cstdio>

static uint32_t gguf_u32(struct gguf_context* ctx, const char* key) {
    int idx = gguf_find_key(ctx, key);
    if (idx < 0) { fprintf(stderr, "GGUF missing key: %s\n", key); return 0; }
    return gguf_get_val_u32(ctx, idx);
}

bool load_model(const char* path, localvqe_model& model, bool verbose) {
    struct ggml_context* ggml_ctx = nullptr;
    struct gguf_init_params params;
    params.no_alloc = false;
    params.ctx = &ggml_ctx;

    struct gguf_context* gctx = gguf_init_from_file(path, params);
    if (!gctx) { fprintf(stderr, "Failed to load: %s\n", path); return false; }

    auto& hp = model.hparams;
    hp.n_fft        = (int)gguf_u32(gctx, "localvqe.n_fft");
    hp.hop_length   = (int)gguf_u32(gctx, "localvqe.hop_length");
    hp.n_freq_bins  = (int)gguf_u32(gctx, "localvqe.n_freq_bins");
    hp.sample_rate  = (int)gguf_u32(gctx, "localvqe.sample_rate");
    hp.dmax         = (int)gguf_u32(gctx, "localvqe.dmax");
    hp.align_hidden = (int)gguf_u32(gctx, "localvqe.align_hidden");
    int idx = gguf_find_key(gctx, "localvqe.power_law_c");
    hp.power_law_c = idx >= 0 ? gguf_get_val_f32(gctx, idx) : 0.3f;
    idx = gguf_find_key(gctx, "localvqe.bn_folded");
    hp.bn_folded = idx >= 0 ? gguf_get_val_bool(gctx, idx) : true;
    idx = gguf_find_key(gctx, "localvqe.version");
    hp.version = idx >= 0 ? (int)gguf_get_val_u32(gctx, idx) : 1;

    // Kernel size (default to 4x3 for backward compat)
    idx = gguf_find_key(gctx, "localvqe.kernel_size_h");
    hp.kernel_size_h = idx >= 0 ? (int)gguf_get_val_u32(gctx, idx) : 4;
    idx = gguf_find_key(gctx, "localvqe.kernel_size_w");
    hp.kernel_size_w = idx >= 0 ? (int)gguf_get_val_u32(gctx, idx) : 3;

    // Bottleneck hidden size
    idx = gguf_find_key(gctx, "localvqe.bottleneck_hidden");
    hp.bottleneck_hidden = idx >= 0 ? (int)gguf_get_val_u32(gctx, idx) : 0;

    int mic_n = (int)gguf_u32(gctx, "localvqe.mic_channels.count");
    hp.mic_channels.resize(mic_n);
    for (int i = 0; i < mic_n; i++) {
        char k[64]; snprintf(k, sizeof(k), "localvqe.mic_channels.%d", i);
        hp.mic_channels[i] = (int)gguf_u32(gctx, k);
    }
    int far_n = (int)gguf_u32(gctx, "localvqe.far_channels.count");
    hp.far_channels.resize(far_n);
    for (int i = 0; i < far_n; i++) {
        char k[64]; snprintf(k, sizeof(k), "localvqe.far_channels.%d", i);
        hp.far_channels[i] = (int)gguf_u32(gctx, k);
    }

    if (verbose)
        printf("Config: n_fft=%d hop=%d dmax=%d c=%.2f kernel=(%d,%d)\n",
               hp.n_fft, hp.hop_length, hp.dmax, hp.power_law_c,
               hp.kernel_size_h, hp.kernel_size_w);

    int n_tensors = gguf_get_n_tensors(gctx);
    for (int i = 0; i < n_tensors; i++) {
        const char* name = gguf_get_tensor_name(gctx, i);
        NpyArray arr = load_tensor_from_ggml(ggml_ctx, name, gctx, verbose);
        if (arr.data.empty()) continue;
        model.tensors[name] = std::move(arr);
    }
    if (verbose) printf("Loaded %zu tensors\n", model.tensors.size());

    gguf_free(gctx);
    if (ggml_ctx) ggml_free(ggml_ctx);
    return true;
}
