/**
 * Test DecoderBlock: skip_conv + ResidualBlock + SubpixelConv2d + ChannelAffine + ELU.
 *
 * DecoderBlock forward (after BN folding):
 *   y = x + skip_conv(x_en)                -- 1x1 Conv2d skip connection
 *   y = resblock(y)                         -- pad -> conv -> elu + skip
 *   y = deconv(y)                           -- SubpixelConv2d: pad -> conv -> pixel shuffle
 *   if not is_last:
 *     y = elu(channel_affine(y))            -- ChannelAffine replaces folded BN
 *
 * DecoderBlock takes TWO inputs: x (from previous decoder) and x_en (encoder skip).
 *
 * Usage:
 *   test_decoder --gguf model.gguf --block dec5 \
 *     --input-0 dec5_input_0.npy --input-1 dec5_input_1.npy \
 *     --expected dec5_output.npy
 */

#include "common.h"

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static inline float elu(float x, float alpha = 1.0f) {
    return x > 0.0f ? x : alpha * (std::exp(x) - 1.0f);
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
 * SubpixelConv2d: pad -> Conv2d(C_in, C_out*2, (4,3)) -> pixel shuffle
 *
 * einops: rearrange("b (r c) t f -> b c t (r f)", r=2)
 * This CONCATENATES: output freq = [r=0 all f, r=1 all f]
 * For channel c, freq index out_f:
 *   if out_f < F_conv: source = conv channel c, freq out_f
 *   else:              source = conv channel c + C_out, freq out_f - F_conv
 */
static void subpixel_conv(
    const float* input, int C_in, int T, int F,
    const float* weight, const float* bias, int C_out,
    float* output  // (C_out, T, 2*F_conv)
) {
    // Pad and conv
    auto padded = zero_pad(input, C_in, T, F, 1, 1, 3, 0);
    int T_pad = T + 3;
    int F_pad = F + 2;
    int T_conv = T_pad - 4 + 1;  // stride 1
    int F_conv = F_pad - 3 + 1;
    int C_conv = C_out * 2;

    std::vector<float> conv_out(C_conv * T_conv * F_conv);
    conv2d(padded.data(), C_in, T_pad, F_pad,
           weight, bias, C_conv, 4, 3, 1, 1,
           conv_out.data(), T_conv, F_conv);

    // Pixel shuffle: (C_out*2, T, F) -> (C_out, T, 2*F)
    // rearrange("b (r c) t f -> b c t (r f)", r=2)
    // channel decomposition: chan = r * C_out + c  (r varies slowest)
    // output[c, t, r*F_conv + f] = conv_out[r*C_out + c, t, f]
    int F_out = 2 * F_conv;
    for (int c = 0; c < C_out; c++) {
        for (int t = 0; t < T_conv; t++) {
            for (int r = 0; r < 2; r++) {
                for (int f = 0; f < F_conv; f++) {
                    int src_ch = r * C_out + c;
                    output[c * T_conv * F_out + t * F_out + r * F_conv + f] =
                        conv_out[src_ch * T_conv * F_conv + t * F_conv + f];
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    const char* gguf_path = nullptr;
    const char* input0_path = nullptr;  // x (from previous decoder/bottleneck)
    const char* input1_path = nullptr;  // x_en (encoder skip)
    const char* expected_path = nullptr;
    const char* output_path = nullptr;
    std::string block_name = "dec5";
    bool is_last = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gguf" && i + 1 < argc) gguf_path = argv[++i];
        else if (arg == "--input-0" && i + 1 < argc) input0_path = argv[++i];
        else if (arg == "--input-1" && i + 1 < argc) input1_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--block" && i + 1 < argc) block_name = argv[++i];
        else if (arg == "--is-last") is_last = true;
        else {
            fprintf(stderr, "Usage: test_decoder --gguf model.gguf --block dec5 "
                    "--input-0 x.npy --input-1 x_en.npy --expected expected.npy\n");
            return 1;
        }
    }

    if (!input0_path || !input1_path || !gguf_path) {
        fprintf(stderr, "Error: --input-0, --input-1, and --gguf required\n");
        return 1;
    }

    NpyArray x_arr = npy_load(input0_path);
    NpyArray x_en_arr = npy_load(input1_path);
    printf("x shape:");
    for (auto s : x_arr.shape) printf(" %lld", (long long)s);
    printf("\nx_en shape:");
    for (auto s : x_en_arr.shape) printf(" %lld", (long long)s);
    printf("\n");

    int C = (int)x_arr.dim(1);
    int T = (int)x_arr.dim(2);
    int F = (int)x_arr.dim(3);

    // Load weights from GGUF
    struct ggml_context* ggml_ctx = nullptr;
    struct gguf_init_params gparams;
    gparams.no_alloc = false;
    gparams.ctx = &ggml_ctx;
    struct gguf_context* gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "Failed to load GGUF\n"); return 1; }

    NpyArray skip_w = load_tensor_from_ggml(ggml_ctx, block_name + ".skip_conv.weight", gctx);
    NpyArray skip_b = load_tensor_from_ggml(ggml_ctx, block_name + ".skip_conv.bias", gctx);
    NpyArray res_conv_w = load_tensor_from_ggml(ggml_ctx, block_name + ".resblock.conv.weight", gctx);
    NpyArray res_conv_b = load_tensor_from_ggml(ggml_ctx, block_name + ".resblock.conv.bias", gctx);
    NpyArray deconv_w = load_tensor_from_ggml(ggml_ctx, block_name + ".deconv.conv.weight", gctx);
    NpyArray deconv_b = load_tensor_from_ggml(ggml_ctx, block_name + ".deconv.conv.bias", gctx);

    NpyArray bn_scale, bn_bias;
    if (!is_last) {
        bn_scale = load_tensor_from_ggml(ggml_ctx, block_name + ".bn.scale", gctx);
        bn_bias = load_tensor_from_ggml(ggml_ctx, block_name + ".bn.bias", gctx);
    }

    int C_out_deconv = (int)deconv_w.dim(0) / 2;  // Conv2d output = C_out * 2

    gguf_free(gctx);
    if (ggml_ctx) ggml_free(ggml_ctx);

    printf("DecoderBlock '%s': C=%d T=%d F=%d C_out=%d is_last=%d\n",
           block_name.c_str(), C, T, F, C_out_deconv, is_last);

    // Step 1: y = x + skip_conv(x_en)
    // skip_conv is 1x1 Conv2d(C, C), no padding
    std::vector<float> skip_out(C * T * F);
    conv2d(x_en_arr.data.data(), C, T, F,
           skip_w.data.data(), skip_b.data.data(), C, 1, 1, 1, 1,
           skip_out.data(), T, F);

    // Add: y = x + skip_out
    std::vector<float> y(C * T * F);
    for (int64_t i = 0; i < (int64_t)(C * T * F); i++) {
        y[i] = x_arr.data[i] + skip_out[i];
    }

    // Step 2: ResidualBlock
    auto res_padded = zero_pad(y.data(), C, T, F, 1, 1, 3, 0);
    int T_pad = T + 3;
    int F_pad = F + 2;
    int T_res = T_pad - 4 + 1;
    int F_res = F_pad - 3 + 1;

    std::vector<float> res_out(C * T_res * F_res);
    conv2d(res_padded.data(), C, T_pad, F_pad,
           res_conv_w.data.data(), res_conv_b.data.data(), C, 4, 3, 1, 1,
           res_out.data(), T_res, F_res);

    // ELU + skip
    for (int64_t i = 0; i < (int64_t)(C * T_res * F_res); i++) {
        res_out[i] = elu(res_out[i]) + y[i];
    }

    // Step 3: SubpixelConv2d
    int F_deconv = 2 * F;  // frequency doubles
    std::vector<float> deconv_out(C_out_deconv * T * F_deconv);
    subpixel_conv(res_out.data(), C, T, F,
                  deconv_w.data.data(), deconv_b.data.data(), C_out_deconv,
                  deconv_out.data());

    // Step 4: ChannelAffine + ELU (if not is_last)
    if (!is_last) {
        for (int c = 0; c < C_out_deconv; c++) {
            float s = bn_scale.data[c];
            float b = bn_bias.data[c];
            for (int t = 0; t < T; t++) {
                for (int f = 0; f < F_deconv; f++) {
                    int idx = c * T * F_deconv + t * F_deconv + f;
                    deconv_out[idx] = elu(deconv_out[idx] * s + b);
                }
            }
        }
    }

    NpyArray result;
    result.shape = {1, (int64_t)C_out_deconv, (int64_t)T, (int64_t)F_deconv};
    result.data = std::move(deconv_out);

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

    return 0;
}
