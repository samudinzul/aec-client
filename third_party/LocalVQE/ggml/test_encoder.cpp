/**
 * Test EncoderBlock: ZeroPad + Conv2d + ELU + ResidualBlock.
 *
 * After BN folding:
 *   EncoderBlock: pad(1,1,3,0) -> Conv2d(in,out,(4,3),stride=(1,2)) -> ELU
 *                 -> ResidualBlock: pad(1,1,3,0) -> Conv2d(out,out,(4,3)) -> ELU + skip
 *
 * Usage:
 *   test_encoder --gguf model.gguf --block mic_enc1 \
 *     --input mic_enc1_input.npy --expected mic_enc1_output.npy
 */

#include "common.h"

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ── Helpers ────────────────────────────────────────────────────────────────

static inline float elu(float x, float alpha = 1.0f) {
    return x > 0.0f ? x : alpha * (std::exp(x) - 1.0f);
}

/**
 * 2D convolution: manual implementation matching PyTorch Conv2d.
 *
 * Input layout:  (C_in, T, F)  — channels-first, row-major
 * Weight layout: (C_out, C_in, kT, kF) — row-major
 * Output layout: (C_out, T_out, F_out)
 *
 * Padding is applied externally before calling this function.
 */
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
                            int t_idx = t * stride_t + kt;
                            int f_idx = f * stride_f + kf;
                            sum += weight[co * C_in * kT * kF + ci * kT * kF + kt * kF + kf] *
                                   input[ci * T_in * F_in + t_idx * F_in + f_idx];
                        }
                    }
                }
                output[co * T_out * F_out + t * F_out + f] = sum;
            }
        }
    }
}

/**
 * Zero-pad a (C, T, F) tensor with padding [left, right, top, bottom].
 * Returns new tensor of shape (C, T + top + bottom, F + left + right).
 */
static std::vector<float> zero_pad(
    const float* input, int C, int T, int F,
    int pad_left, int pad_right, int pad_top, int pad_bottom
) {
    int T_pad = T + pad_top + pad_bottom;
    int F_pad = F + pad_left + pad_right;
    std::vector<float> out(C * T_pad * F_pad, 0.0f);
    for (int c = 0; c < C; c++) {
        for (int t = 0; t < T; t++) {
            for (int f = 0; f < F; f++) {
                out[c * T_pad * F_pad + (t + pad_top) * F_pad + (f + pad_left)] =
                    input[c * T * F + t * F + f];
            }
        }
    }
    return out;
}

/**
 * Run EncoderBlock forward pass (after BN folding).
 *
 * Main path: pad -> conv -> elu
 * Residual:  pad -> conv -> elu + skip
 */
static NpyArray encoder_block(
    const float* input, int C_in, int T, int F,
    const float* conv_w, const float* conv_b, int C_out,
    const float* res_conv_w, const float* res_conv_b,
    int stride_t, int stride_f
) {
    // Main conv: pad(1,1,3,0) -> Conv2d(C_in, C_out, (4,3), stride)
    auto padded = zero_pad(input, C_in, T, F, 1, 1, 3, 0);
    int T_pad = T + 3;
    int F_pad = F + 2;
    int T_out = (T_pad - 4) / stride_t + 1;
    int F_out = (F_pad - 3) / stride_f + 1;

    std::vector<float> main_out(C_out * T_out * F_out);
    conv2d(padded.data(), C_in, T_pad, F_pad,
           conv_w, conv_b, C_out, 4, 3, stride_t, stride_f,
           main_out.data(), T_out, F_out);

    // ELU
    for (auto& v : main_out) v = elu(v);

    // ResidualBlock: pad -> conv -> elu + skip
    auto res_padded = zero_pad(main_out.data(), C_out, T_out, F_out, 1, 1, 3, 0);
    int T_res_pad = T_out + 3;
    int F_res_pad = F_out + 2;
    // ResidualBlock conv has stride (1,1)
    int T_res_out = T_res_pad - 4 + 1;
    int F_res_out = F_res_pad - 3 + 1;

    std::vector<float> res_out(C_out * T_res_out * F_res_out);
    conv2d(res_padded.data(), C_out, T_res_pad, F_res_pad,
           res_conv_w, res_conv_b, C_out, 4, 3, 1, 1,
           res_out.data(), T_res_out, F_res_out);

    // ELU + skip connection
    int64_t n = C_out * T_res_out * F_res_out;
    for (int64_t i = 0; i < n; i++) {
        res_out[i] = elu(res_out[i]) + main_out[i];
    }

    NpyArray result;
    result.shape = {1, (int64_t)C_out, (int64_t)T_res_out, (int64_t)F_res_out};
    result.data = std::move(res_out);
    return result;
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* gguf_path = nullptr;
    const char* input_path = nullptr;
    const char* expected_path = nullptr;
    const char* output_path = nullptr;
    std::string block_name = "mic_enc1";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gguf" && i + 1 < argc) gguf_path = argv[++i];
        else if (arg == "--input" && i + 1 < argc) input_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--block" && i + 1 < argc) block_name = argv[++i];
        else {
            fprintf(stderr, "Usage: test_encoder --gguf model.gguf --block mic_enc1 "
                    "--input input.npy --expected expected.npy [--output output.npy]\n");
            return 1;
        }
    }

    if (!input_path) { fprintf(stderr, "Error: --input required\n"); return 1; }

    // Load input
    NpyArray input = npy_load(input_path);
    printf("Input shape:");
    for (auto s : input.shape) printf(" %lld", (long long)s);
    printf("\n");

    // Input is (1, C_in, T, F) — strip batch dim
    int C_in = (int)input.dim(1);
    int T = (int)input.dim(2);
    int F = (int)input.dim(3);
    printf("EncoderBlock '%s': C_in=%d T=%d F=%d\n", block_name.c_str(), C_in, T, F);

    // Load weights from GGUF or from .npy files
    NpyArray conv_w, conv_b, res_conv_w, res_conv_b;

    if (gguf_path) {
        // Load from GGUF
        // For now, load using ggml API
        struct ggml_context* ggml_ctx = nullptr;
        struct gguf_init_params gparams;
        gparams.no_alloc = false;
        gparams.ctx = &ggml_ctx;

        struct gguf_context* gctx = gguf_init_from_file(gguf_path, gparams);
        if (!gctx) { fprintf(stderr, "Failed to load GGUF: %s\n", gguf_path); return 1; }

        conv_w = load_tensor_from_ggml(ggml_ctx, block_name + ".conv.weight", gctx);
        conv_b = load_tensor_from_ggml(ggml_ctx, block_name + ".conv.bias", gctx);
        res_conv_w = load_tensor_from_ggml(ggml_ctx, block_name + ".resblock.conv.weight", gctx);
        res_conv_b = load_tensor_from_ggml(ggml_ctx, block_name + ".resblock.conv.bias", gctx);

        printf("Conv weight shape:");
        for (auto s : conv_w.shape) printf(" %lld", (long long)s);
        printf("\n");

        gguf_free(gctx);
        if (ggml_ctx) ggml_free(ggml_ctx);
    } else {
        fprintf(stderr, "Error: --gguf required for weight loading\n");
        return 1;
    }

    int C_out = (int)conv_w.dim(0);
    printf("C_out=%d\n", C_out);

    // Determine stride from block name
    int stride_t = 1, stride_f = 2;  // default for encoder blocks

    // Run encoder block
    NpyArray result = encoder_block(
        input.data.data(), C_in, T, F,
        conv_w.data.data(), conv_b.data.data(), C_out,
        res_conv_w.data.data(), res_conv_b.data.data(),
        stride_t, stride_f
    );

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
        print_result(block_name, max_err, mean_err);

        return max_err < 1e-2f ? 0 : 1;
    }

    printf("Done (no comparison — pass --expected to verify)\n");
    return 0;
}
