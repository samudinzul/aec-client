/**
 * Test AlignBlock: cross-attention soft delay estimation.
 *
 * Forward:
 *   Q = pconv_mic(x_mic)        -- 1x1 Conv2d(C, H)
 *   K = pconv_ref(x_ref)        -- 1x1 Conv2d(C, H)
 *   Ku = unfold_k(K)            -- pad(0,0,dmax-1,0) + unfold(dmax,1) -> (B,H,T,dmax,F)
 *   V = sum(Q * Ku, dim=F) / sqrt(F)  -> (B,H,T,dmax)
 *   V = conv(V)                 -- pad(1,1,4,0) + Conv2d(H,1,(5,3)) -> (B,1,T,dmax)
 *   A = softmax(V / temp, dim=dmax)
 *   unfold x_ref along time     -> (B,C,T,dmax,F)
 *   aligned = sum(A * x_ref_unf, dim=dmax) -> (B,C,T,F)
 *
 * Usage:
 *   test_align --gguf model.gguf \
 *     --input-mic align_input_0.npy --input-ref align_input_1.npy \
 *     --expected align_output.npy
 */

#include "common.h"

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ── Conv2d helper (1x1 or small kernel) ────────────────────────────────────

static void conv2d_1x1(
    const float* input, int C_in, int T, int F,
    const float* weight, const float* bias, int C_out,
    float* output
) {
    for (int co = 0; co < C_out; co++) {
        for (int t = 0; t < T; t++) {
            for (int f = 0; f < F; f++) {
                float sum = bias ? bias[co] : 0.0f;
                for (int ci = 0; ci < C_in; ci++) {
                    sum += weight[co * C_in + ci] *
                           input[ci * T * F + t * F + f];
                }
                output[co * T * F + t * F + f] = sum;
            }
        }
    }
}

static void conv2d(
    const float* input, int C_in, int T_in, int F_in,
    const float* weight, const float* bias,
    int C_out, int kT, int kF,
    int stride_t, int stride_f,
    float* output, int T_out, int F_out
) {
    for (int co = 0; co < C_out; co++) {
        for (int t = 0; t < T_out; t++) {
            for (int f = 0; f < F_out; f++) {
                float sum = bias ? bias[co] : 0.0f;
                for (int ci = 0; ci < C_in; ci++) {
                    for (int kt = 0; kt < kT; kt++) {
                        for (int kf = 0; kf < kF; kf++) {
                            sum += weight[co * C_in * kT * kF + ci * kT * kF + kt * kF + kf] *
                                   input[ci * T_in * F_in + (t * stride_t + kt) * F_in + (f * stride_f + kf)];
                        }
                    }
                }
                output[co * T_out * F_out + t * F_out + f] = sum;
            }
        }
    }
}

static void softmax_last_dim(float* data, int outer, int dim) {
    for (int i = 0; i < outer; i++) {
        float* row = data + i * dim;
        float max_val = row[0];
        for (int j = 1; j < dim; j++)
            if (row[j] > max_val) max_val = row[j];
        float sum = 0.0f;
        for (int j = 0; j < dim; j++) {
            row[j] = std::exp(row[j] - max_val);
            sum += row[j];
        }
        for (int j = 0; j < dim; j++)
            row[j] /= sum;
    }
}

int main(int argc, char** argv) {
    const char* gguf_path = nullptr;
    const char* mic_path = nullptr;
    const char* ref_path = nullptr;
    const char* expected_path = nullptr;
    const char* output_path = nullptr;
    float temperature = 1.0f;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gguf" && i + 1 < argc) gguf_path = argv[++i];
        else if (arg == "--input-mic" && i + 1 < argc) mic_path = argv[++i];
        else if (arg == "--input-ref" && i + 1 < argc) ref_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--temperature" && i + 1 < argc) temperature = std::stof(argv[++i]);
        else {
            fprintf(stderr, "Usage: test_align --gguf model.gguf "
                    "--input-mic mic.npy --input-ref ref.npy --expected expected.npy\n");
            return 1;
        }
    }

    if (!mic_path || !ref_path || !gguf_path) {
        fprintf(stderr, "Error: --input-mic, --input-ref, and --gguf required\n");
        return 1;
    }

    NpyArray x_mic = npy_load(mic_path);   // (1, C, T, F)
    NpyArray x_ref = npy_load(ref_path);   // (1, C, T, F)

    printf("x_mic shape:");
    for (auto s : x_mic.shape) printf(" %lld", (long long)s);
    printf("\nx_ref shape:");
    for (auto s : x_ref.shape) printf(" %lld", (long long)s);
    printf("\n");

    int C = (int)x_mic.dim(1);
    int T = (int)x_mic.dim(2);
    int F = (int)x_mic.dim(3);

    // Load weights from GGUF
    struct ggml_context* ggml_ctx = nullptr;
    struct gguf_init_params gparams;
    gparams.no_alloc = false;
    gparams.ctx = &ggml_ctx;
    struct gguf_context* gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "Failed to load GGUF\n"); return 1; }

    // Read dmax from GGUF metadata
    int dmax_idx = gguf_find_key(gctx, "localvqe.dmax");
    int dmax = dmax_idx >= 0 ? (int)gguf_get_val_u32(gctx, dmax_idx) : 32;

    int ah_idx = gguf_find_key(gctx, "localvqe.align_hidden");
    int H = ah_idx >= 0 ? (int)gguf_get_val_u32(gctx, ah_idx) : 32;

    NpyArray pconv_mic_w = load_tensor_from_ggml(ggml_ctx, "align.pconv_mic.weight", gctx);
    NpyArray pconv_mic_b = load_tensor_from_ggml(ggml_ctx, "align.pconv_mic.bias", gctx);
    NpyArray pconv_ref_w = load_tensor_from_ggml(ggml_ctx, "align.pconv_ref.weight", gctx);
    NpyArray pconv_ref_b = load_tensor_from_ggml(ggml_ctx, "align.pconv_ref.bias", gctx);
    NpyArray smooth_w = load_tensor_from_ggml(ggml_ctx, "align.conv.1.weight", gctx);
    NpyArray smooth_b = load_tensor_from_ggml(ggml_ctx, "align.conv.1.bias", gctx);

    gguf_free(gctx);
    if (ggml_ctx) ggml_free(ggml_ctx);

    printf("AlignBlock: C=%d H=%d T=%d F=%d dmax=%d temp=%.2f\n",
           C, H, T, F, dmax, temperature);

    // Step 1: Q = pconv_mic(x_mic)  (1x1 conv, C -> H)
    std::vector<float> Q(H * T * F);
    conv2d_1x1(x_mic.data.data(), C, T, F,
               pconv_mic_w.data.data(), pconv_mic_b.data.data(), H,
               Q.data());

    // Step 2: K = pconv_ref(x_ref)
    std::vector<float> K(H * T * F);
    conv2d_1x1(x_ref.data.data(), C, T, F,
               pconv_ref_w.data.data(), pconv_ref_b.data.data(), H,
               K.data());

    // Step 3: Unfold K along time with causal padding
    // Pad K: (H, T, F) -> (H, T+dmax-1, F) with dmax-1 zeros on top
    int T_padded = T + dmax - 1;
    std::vector<float> K_padded(H * T_padded * F, 0.0f);
    for (int h = 0; h < H; h++) {
        for (int t = 0; t < T; t++) {
            for (int f = 0; f < F; f++) {
                K_padded[h * T_padded * F + (t + dmax - 1) * F + f] =
                    K[h * T * F + t * F + f];
            }
        }
    }

    // Unfold: extract dmax-length windows along time
    // Ku[h, t, d, f] = K_padded[h, t + d, f]  for d in [0, dmax)
    // Note: d=0 is the most delayed (oldest), d=dmax-1 is current frame
    // PyTorch unfold_k result: (B, H*dmax, T*F) then view as (B, H, dmax, T, F)
    // then permute to (B, H, T, dmax, F)
    std::vector<float> Ku(H * T * dmax * F);
    for (int h = 0; h < H; h++) {
        for (int t = 0; t < T; t++) {
            for (int d = 0; d < dmax; d++) {
                for (int f = 0; f < F; f++) {
                    Ku[((h * T + t) * dmax + d) * F + f] =
                        K_padded[h * T_padded * F + (t + d) * F + f];
                }
            }
        }
    }

    // Step 4: Similarity V = sum(Q * Ku, dim=F) / sqrt(F)
    // Q: (H, T, F), Ku: (H, T, dmax, F)
    // V: (H, T, dmax)
    float scale = 1.0f / std::sqrt((float)F);
    std::vector<float> V(H * T * dmax, 0.0f);
    for (int h = 0; h < H; h++) {
        for (int t = 0; t < T; t++) {
            for (int d = 0; d < dmax; d++) {
                float sum = 0.0f;
                for (int f = 0; f < F; f++) {
                    sum += Q[h * T * F + t * F + f] *
                           Ku[((h * T + t) * dmax + d) * F + f];
                }
                V[(h * T + t) * dmax + d] = sum * scale;
            }
        }
    }

    // Step 5: Smoothing conv: pad(1,1,4,0) + Conv2d(H, 1, (5,3))
    // V: (H, T, dmax) -> pad to (H, T+4, dmax+2) -> conv -> (1, T, dmax)
    int T_v_pad = T + 4;
    int D_v_pad = dmax + 2;
    std::vector<float> V_padded(H * T_v_pad * D_v_pad, 0.0f);
    for (int h = 0; h < H; h++) {
        for (int t = 0; t < T; t++) {
            for (int d = 0; d < dmax; d++) {
                V_padded[h * T_v_pad * D_v_pad + (t + 4) * D_v_pad + (d + 1)] =
                    V[(h * T + t) * dmax + d];
            }
        }
    }

    int T_conv_out = T_v_pad - 5 + 1;
    int D_conv_out = D_v_pad - 3 + 1;
    std::vector<float> V_conv(1 * T_conv_out * D_conv_out);
    conv2d(V_padded.data(), H, T_v_pad, D_v_pad,
           smooth_w.data.data(), smooth_b.data.data(),
           1, 5, 3, 1, 1,
           V_conv.data(), T_conv_out, D_conv_out);

    // Step 6: Softmax over delay dimension (with temperature)
    for (int i = 0; i < T * dmax; i++) {
        V_conv[i] /= temperature;
    }
    softmax_last_dim(V_conv.data(), T, dmax);

    // Step 7: Unfold x_ref and weighted sum
    // Pad x_ref: (C, T, F) -> (C, T+dmax-1, F)
    std::vector<float> ref_padded(C * T_padded * F, 0.0f);
    for (int c = 0; c < C; c++) {
        for (int t = 0; t < T; t++) {
            for (int f = 0; f < F; f++) {
                ref_padded[c * T_padded * F + (t + dmax - 1) * F + f] =
                    x_ref.data[c * T * F + t * F + f];
            }
        }
    }

    // aligned[c, t, f] = sum_d A[t, d] * ref_padded[c, t+d, f]
    NpyArray result;
    result.shape = {1, (int64_t)C, (int64_t)T, (int64_t)F};
    result.data.resize(C * T * F, 0.0f);

    for (int c = 0; c < C; c++) {
        for (int t = 0; t < T; t++) {
            for (int d = 0; d < dmax; d++) {
                float a = V_conv[t * dmax + d];
                for (int f = 0; f < F; f++) {
                    result.data[c * T * F + t * F + f] +=
                        a * ref_padded[c * T_padded * F + (t + d) * F + f];
                }
            }
        }
    }

    printf("Output shape:");
    for (auto s : result.shape) printf(" %lld", (long long)s);
    printf("\n");

    if (output_path) {
        npy_save(output_path, result);
        printf("Saved output to %s\n", output_path);
    }

    if (expected_path) {
        NpyArray expected = npy_load(expected_path);
        printf("Expected shape:");
        for (auto s : expected.shape) printf(" %lld", (long long)s);
        printf("\n");

        if (result.numel() != expected.numel()) {
            fprintf(stderr, "Element count mismatch: got %lld expected %lld\n",
                    (long long)result.numel(), (long long)expected.numel());
            return 1;
        }
        float max_err = max_abs_diff(result.data.data(), expected.data.data(), result.numel());
        float mean_err = mean_abs_diff(result.data.data(), expected.data.data(), result.numel());
        print_result("align", max_err, mean_err);
        return max_err < 1e-2f ? 0 : 1;
    }

    return 0;
}
