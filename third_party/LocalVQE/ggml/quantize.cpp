/**
 * Re-quantize a LocalVQE F32 GGUF with mixed Q4_K/Q8_0 + WHT.
 *
 * Strategy:
 *   - Conv/GRU/FC where row_size is (or pads to ≤50% overhead) multiple of 256: WHT+Q4_K
 *   - Other quantizable layers: WHT+Q8_0 (block 32)
 *   - Biases, AlignBlock, dec1: kept F32
 *
 * Weights are stored with PADDED dimensions (IC_padded for conv, ne0_padded for matmul).
 * The graph builder pads input activations to match at runtime.
 *
 * Usage:
 *   quantize input.gguf output.gguf [--no-wht]
 */

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ── Layer classification ─────────────────────────────────────────────────

static bool should_quantize(const char* name) {
    std::string s(name);
    if (s.find(".bias") != std::string::npos) return false;
    if (s.find(".bn.") != std::string::npos) return false;
    if (s.rfind("align.", 0) == 0) return false;
    if (s.rfind("dec1.", 0) == 0) return false;
    if (s.find(".weight") != std::string::npos) return true;
    if (s.find("weight_ih") != std::string::npos) return true;
    if (s.find("weight_hh") != std::string::npos) return true;
    return false;
}

struct quant_plan {
    enum ggml_type type;    // GGML_TYPE_Q4_K or GGML_TYPE_Q8_0
    int64_t ne_padded[4];   // padded shape (same as original if no padding needed)
    int64_t ne_orig[4];     // original shape
    int ndim;
    float pad_pct;
};

// Compute quantization plan for a tensor.
// For conv weights (4D): row_size = KW*KH*IC, needs to be multiple of block_size.
// For matmul weights (2D): ne[0] needs to be multiple of block_size.
static quant_plan plan_tensor(struct ggml_tensor* t) {
    quant_plan p = {};
    p.ndim = ggml_n_dims(t);
    for (int d = 0; d < 4; d++) {
        p.ne_orig[d] = t->ne[d];
        p.ne_padded[d] = t->ne[d];
    }

    // Determine the row size that needs alignment
    int64_t row_size;
    if (p.ndim == 4) {
        // Conv: (KW, KH, IC, OC) → mul_mat row = KW*KH*IC
        row_size = t->ne[0] * t->ne[1] * t->ne[2];
    } else if (p.ndim == 2) {
        // Matmul: (ne0, ne1) → row = ne0
        row_size = t->ne[0];
    } else {
        row_size = t->ne[0];
    }

    // Helper: find smallest IC_padded >= IC such that kw_kh * IC_padded is a multiple of blk.
    // For 2D tensors (matmul), kw_kh=1.
    auto find_padded = [&](enum ggml_type type) -> int64_t {
        int64_t blk = ggml_blck_size(type);
        if (p.ndim == 4) {
            int64_t kw_kh = t->ne[0] * t->ne[1];
            int64_t ic = t->ne[2];
            // Find smallest IC_padded >= IC where kw_kh * IC_padded % blk == 0
            // IC_padded must be an integer.
            // LCM(kw_kh, blk) / kw_kh gives the step size for IC_padded.
            auto gcd = [](int64_t a, int64_t b) -> int64_t { while(b){int64_t t=b;b=a%b;a=t;} return a; };
            int64_t step = blk / gcd(kw_kh, blk);
            int64_t ic_padded = ((ic + step - 1) / step) * step;
            return kw_kh * ic_padded;
        } else {
            return ((row_size + blk - 1) / blk) * blk;
        }
    };

    auto apply_padded = [&](enum ggml_type type, int64_t padded_row) {
        p.type = type;
        p.pad_pct = (float)(padded_row - row_size) / (float)row_size * 100.0f;
        if (p.ndim == 4) {
            int64_t kw_kh = t->ne[0] * t->ne[1];
            p.ne_padded[2] = padded_row / kw_kh;
        } else {
            p.ne_padded[0] = padded_row;
        }
    };

    // Try Q4_K (block 256) first
    if (row_size % ggml_blck_size(GGML_TYPE_Q4_K) == 0) {
        p.type = GGML_TYPE_Q4_K;
        p.pad_pct = 0;
        return p;
    }

    int64_t padded_q4k = find_padded(GGML_TYPE_Q4_K);
    float pct_q4k = (float)(padded_q4k - row_size) / (float)row_size * 100.0f;
    if (pct_q4k <= 50.0f) {
        apply_padded(GGML_TYPE_Q4_K, padded_q4k);
        return p;
    }

    // Fall back to Q8_0 (block 32)
    if (row_size % ggml_blck_size(GGML_TYPE_Q8_0) == 0) {
        p.type = GGML_TYPE_Q8_0;
        p.pad_pct = 0;
        return p;
    }

    int64_t padded_q8 = find_padded(GGML_TYPE_Q8_0);
    apply_padded(GGML_TYPE_Q8_0, padded_q8);
    return p;
}

// ── Fast Walsh-Hadamard Transform ────────────────────────────────────────

static void fast_wht_inplace(float* x, int n) {
    for (int h = 1; h < n; h *= 2) {
        for (int i = 0; i < n; i += h * 2) {
            for (int j = i; j < i + h; j++) {
                float a = x[j];
                float b = x[j + h];
                x[j]     = a + b;
                x[j + h] = a - b;
            }
        }
    }
}

static int next_pow2(int n) {
    int p = 1;
    while (p < n) p *= 2;
    return p;
}

static void make_sign_vector(float* signs, int n, uint32_t seed) {
    uint32_t state = seed;
    for (int i = 0; i < n; i++) {
        state = state * 1664525u + 1013904223u;
        signs[i] = (state >> 31) ? 1.0f : -1.0f;
    }
}

static uint32_t hash_string(const char* s) {
    uint32_t h = 5381;
    while (*s) { h = h * 33 + (uint8_t)*s++; }
    return h;
}

// Apply WHT to rows of a 2D matrix. Pads cols to power of 2, applies random
// sign + WHT + normalization. Returns the transformed (rows × padded_cols) buffer.
static std::vector<float> wht_transform_rows(
    const float* data, int64_t rows, int64_t cols, const char* name
) {
    int64_t pcols = next_pow2((int)cols);
    std::vector<float> buf(rows * pcols, 0.0f);
    std::vector<float> signs(pcols);
    make_sign_vector(signs.data(), pcols, hash_string(name));
    float norm = 1.0f / sqrtf((float)pcols);

    for (int64_t r = 0; r < rows; r++) {
        float* row = &buf[r * pcols];
        for (int64_t c = 0; c < cols; c++) row[c] = data[r * cols + c];
        for (int64_t c = 0; c < pcols; c++) row[c] *= signs[c];
        fast_wht_inplace(row, (int)pcols);
        for (int64_t c = 0; c < pcols; c++) row[c] *= norm;
    }
    return buf;
}

// ── Main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* inp_path = nullptr;
    const char* out_path = nullptr;
    bool use_wht = true;

    for (int i = 1; i < argc; i++) {
        std::string a(argv[i]);
        if (a == "--no-wht") { use_wht = false; }
        else if (!inp_path) inp_path = argv[i];
        else if (!out_path) out_path = argv[i];
    }
    if (!inp_path || !out_path) {
        fprintf(stderr, "Usage: quantize input.gguf output.gguf [--no-wht]\n");
        return 1;
    }

    printf("Requantizing %s -> %s%s\n", inp_path, out_path, use_wht ? " (WHT)" : "");
    fflush(stdout);

    // Load input GGUF with tensor data
    struct ggml_context* inp_ctx = nullptr;
    struct gguf_init_params uparams = { /*.no_alloc=*/ false, /*.ctx=*/ &inp_ctx };
    struct gguf_context* inp = gguf_init_from_file(inp_path, uparams);
    if (!inp) { fprintf(stderr, "Failed to load: %s\n", inp_path); return 1; }

    int n_tensors = gguf_get_n_tensors(inp);
    printf("Loaded %d tensors\n", n_tensors);

    // Context for quantized tensor objects
    struct ggml_init_params qp = { (size_t)32 * 1024 * 1024, nullptr, false };
    struct ggml_context* q_ctx = ggml_init(qp);

    // Build output GGUF
    struct gguf_context* out = gguf_init_empty();
    gguf_set_kv(out, inp);

    int n_q4k = 0, n_q8 = 0, n_f32 = 0;
    size_t total_inp = 0, total_out = 0;
    std::vector<std::vector<uint8_t>> qdata_storage;

    for (int i = 0; i < n_tensors; i++) {
        const char* name = gguf_get_tensor_name(inp, i);
        struct ggml_tensor* t = ggml_get_tensor(inp_ctx, name);
        if (!t) continue;

        size_t inp_bytes = ggml_nbytes(t);
        total_inp += inp_bytes;

        if (t->type != GGML_TYPE_F32 || !should_quantize(name)) {
            gguf_add_tensor(out, t);
            total_out += inp_bytes;
            n_f32++;
            continue;
        }

        quant_plan plan = plan_tensor(t);
        const auto* traits = ggml_get_type_traits(plan.type);
        const char* type_name = (plan.type == GGML_TYPE_Q4_K) ? "Q4_K" : "Q8_0";
        int64_t blk = traits->blck_size;

        // Build padded F32 weight with correct shape
        int64_t n_padded = 1;
        for (int d = 0; d < 4; d++) n_padded *= plan.ne_padded[d];

        std::vector<float> f32_padded(n_padded, 0.0f);
        float* src = (float*)t->data;

        // Copy original data into padded buffer (respecting layout)
        if (plan.ndim == 4) {
            // Conv: (KW, KH, IC, OC) — copy with IC padding
            int64_t kw = plan.ne_orig[0], kh = plan.ne_orig[1];
            int64_t ic = plan.ne_orig[2], oc = plan.ne_orig[3];
            int64_t ic_pad = plan.ne_padded[2];
            for (int64_t o = 0; o < oc; o++)
                for (int64_t c = 0; c < ic; c++)
                    for (int64_t h = 0; h < kh; h++)
                        for (int64_t w = 0; w < kw; w++)
                            f32_padded[o * ic_pad * kh * kw + c * kh * kw + h * kw + w] =
                                src[o * ic * kh * kw + c * kh * kw + h * kw + w];
        } else if (plan.ndim == 2) {
            // Matmul: (ne0, ne1) — copy with ne0 padding
            int64_t n0 = plan.ne_orig[0], n1 = plan.ne_orig[1];
            int64_t n0_pad = plan.ne_padded[0];
            for (int64_t j = 0; j < n1; j++)
                for (int64_t k = 0; k < n0; k++)
                    f32_padded[j * n0_pad + k] = src[j * n0 + k];
        } else {
            memcpy(f32_padded.data(), src, ggml_nelements(t) * sizeof(float));
        }

        // Apply WHT if enabled
        float* quant_data = f32_padded.data();
        std::vector<float> wht_buf;
        if (use_wht) {
            // Treat as 2D: row = what mul_mat sees as ne[0]
            int64_t row_size, n_rows;
            if (plan.ndim == 4) {
                row_size = plan.ne_padded[0] * plan.ne_padded[1] * plan.ne_padded[2];
                n_rows = plan.ne_padded[3];
            } else {
                row_size = plan.ne_padded[0];
                n_rows = n_padded / row_size;
            }
            wht_buf = wht_transform_rows(f32_padded.data(), n_rows, row_size, name);
            quant_data = wht_buf.data();
            // WHT output may have more columns due to power-of-2 padding
            int64_t wht_cols = next_pow2((int)row_size);
            if (wht_cols != row_size) {
                // Need to use only the first row_size elements per row from WHT output
                // Actually, WHT pads to power of 2 internally. For quantization we need
                // to quantize the full WHT output (including pow2 padding), then at load
                // time the iWHT will produce the correct padded result.
                // Update padded shape to include WHT's pow2 padding.
                if (plan.ndim == 4) {
                    // Row = KW*KH*IC_padded, WHT pads to pow2
                    int64_t kw_kh = plan.ne_padded[0] * plan.ne_padded[1];
                    plan.ne_padded[2] = wht_cols / kw_kh;
                } else {
                    plan.ne_padded[0] = wht_cols;
                }
                n_padded = n_rows * wht_cols;
            }
        }

        // Verify alignment
        int64_t row_after;
        if (plan.ndim == 4) {
            row_after = plan.ne_padded[0] * plan.ne_padded[1] * plan.ne_padded[2];
        } else {
            row_after = plan.ne_padded[0];
        }
        if (row_after % blk != 0) {
            fprintf(stderr, "ERROR: %s row_size %lld not aligned to block %lld\n",
                    name, (long long)row_after, (long long)blk);
            return 1;
        }

        // Quantize
        size_t out_bytes = ggml_row_size(plan.type, n_padded);
        qdata_storage.emplace_back(out_bytes);
        auto& qdata = qdata_storage.back();
        traits->from_float_ref(quant_data, qdata.data(), n_padded);

        // Create tensor with padded shape
        struct ggml_tensor* qt;
        if (plan.ndim == 4) {
            qt = ggml_new_tensor_4d(q_ctx, plan.type,
                plan.ne_padded[0], plan.ne_padded[1], plan.ne_padded[2], plan.ne_padded[3]);
        } else if (plan.ndim == 2) {
            qt = ggml_new_tensor_2d(q_ctx, plan.type, plan.ne_padded[0], plan.ne_padded[1]);
        } else {
            qt = ggml_new_tensor_1d(q_ctx, plan.type, n_padded);
        }
        ggml_set_name(qt, name);
        memcpy(qt->data, qdata.data(), out_bytes);

        gguf_add_tensor(out, qt);
        total_out += out_bytes;

        // Store metadata
        char key[256];
        for (int d = 0; d < plan.ndim; d++) {
            snprintf(key, sizeof(key), "localvqe.shape.%s.%d", name, d);
            gguf_set_val_u32(out, key, (uint32_t)plan.ne_orig[d]);
        }
        snprintf(key, sizeof(key), "localvqe.shape.%s.ndim", name);
        gguf_set_val_u32(out, key, (uint32_t)plan.ndim);
        if (use_wht) {
            snprintf(key, sizeof(key), "localvqe.wht.%s", name);
            gguf_set_val_bool(out, key, true);
        }

        const char* label = (plan.type == GGML_TYPE_Q4_K) ? "Q4_K" : "Q8_0";
        printf("  %-42s %s  [%lld,%lld,%lld,%lld] -> [%lld,%lld,%lld,%lld]  pad=%.0f%%\n",
               name, label,
               (long long)plan.ne_orig[0], (long long)plan.ne_orig[1],
               (long long)plan.ne_orig[2], (long long)plan.ne_orig[3],
               (long long)plan.ne_padded[0], (long long)plan.ne_padded[1],
               (long long)plan.ne_padded[2], (long long)plan.ne_padded[3],
               plan.pad_pct);

        if (plan.type == GGML_TYPE_Q4_K) n_q4k++; else n_q8++;
    }

    gguf_write_to_file(out, out_path, false);

    printf("\nDone: %d Q4_K + %d Q8_0 + %d F32\n", n_q4k, n_q8, n_f32);
    printf("Size: %.1f MB -> %.1f MB (%.1f%%)\n",
           total_inp / 1e6, total_out / 1e6, total_out * 100.0 / total_inp);

    gguf_free(out);
    gguf_free(inp);
    ggml_free(q_ctx);
    ggml_free(inp_ctx);
    return 0;
}
