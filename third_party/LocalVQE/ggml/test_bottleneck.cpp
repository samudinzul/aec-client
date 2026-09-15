/**
 * Test Bottleneck: reshape + GRU + Linear + reshape.
 *
 * Forward:
 *   x: (B,C,T,F) -> rearrange("b c t f -> b t (c f)") -> (B,T,C*F)
 *   GRU(C*F -> H, batch_first=True) -> (B,T,H)
 *   Linear(H -> C*F) -> (B,T,C*F)
 *   rearrange("b t (c f) -> b c t f") -> (B,C,T,F)
 *
 * Usage:
 *   test_bottleneck --gguf model.gguf \
 *     --input bottleneck_input.npy --expected bottleneck_output.npy
 */

#include "common.h"

#include "ggml.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

/**
 * GRU forward pass over all T time steps.
 *
 * input:     (T, input_size)  row-major
 * hidden:    (hidden_size,)   initial hidden state (updated in-place)
 * weight_ih: (3*H, input_size)
 * weight_hh: (3*H, H)
 * bias_ih:   (3*H,)
 * bias_hh:   (3*H,)
 * output:    (T, hidden_size) row-major
 */
static void gru_forward(
    const float* input, int T, int input_size,
    float* hidden, int hidden_size,
    const float* weight_ih, const float* weight_hh,
    const float* bias_ih, const float* bias_hh,
    float* output
) {
    std::vector<float> gates_ih(3 * hidden_size);
    std::vector<float> gates_hh(3 * hidden_size);

    for (int t = 0; t < T; t++) {
        const float* x_t = input + t * input_size;

        // gates_ih = weight_ih @ x_t + bias_ih
        for (int i = 0; i < 3 * hidden_size; i++) {
            float sum = bias_ih[i];
            for (int j = 0; j < input_size; j++) {
                sum += weight_ih[i * input_size + j] * x_t[j];
            }
            gates_ih[i] = sum;
        }

        // gates_hh = weight_hh @ hidden + bias_hh
        for (int i = 0; i < 3 * hidden_size; i++) {
            float sum = bias_hh[i];
            for (int j = 0; j < hidden_size; j++) {
                sum += weight_hh[i * hidden_size + j] * hidden[j];
            }
            gates_hh[i] = sum;
        }

        // r, z, n gates
        for (int i = 0; i < hidden_size; i++) {
            float r = sigmoid(gates_ih[i] + gates_hh[i]);
            float z = sigmoid(gates_ih[hidden_size + i] + gates_hh[hidden_size + i]);
            float n = std::tanh(gates_ih[2 * hidden_size + i] +
                               r * gates_hh[2 * hidden_size + i]);
            hidden[i] = (1.0f - z) * n + z * hidden[i];
        }

        // Copy hidden to output
        std::memcpy(output + t * hidden_size, hidden, hidden_size * sizeof(float));
    }
}

/**
 * Linear layer: output = input @ weight^T + bias
 *
 * input:  (T, in_features)
 * weight: (out_features, in_features)
 * bias:   (out_features,)
 * output: (T, out_features)
 */
static void linear(
    const float* input, int T, int in_features,
    const float* weight, const float* bias, int out_features,
    float* output
) {
    for (int t = 0; t < T; t++) {
        for (int o = 0; o < out_features; o++) {
            float sum = bias ? bias[o] : 0.0f;
            for (int j = 0; j < in_features; j++) {
                sum += weight[o * in_features + j] * input[t * in_features + j];
            }
            output[t * out_features + o] = sum;
        }
    }
}

int main(int argc, char** argv) {
    const char* gguf_path = nullptr;
    const char* input_path = nullptr;
    const char* expected_path = nullptr;
    const char* output_path = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gguf" && i + 1 < argc) gguf_path = argv[++i];
        else if (arg == "--input" && i + 1 < argc) input_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else {
            fprintf(stderr, "Usage: test_bottleneck --gguf model.gguf "
                    "--input input.npy --expected expected.npy\n");
            return 1;
        }
    }

    if (!input_path || !gguf_path) {
        fprintf(stderr, "Error: --input and --gguf required\n");
        return 1;
    }

    // Load input: (1, C, T, F)
    NpyArray input = npy_load(input_path);
    printf("Input shape:");
    for (auto s : input.shape) printf(" %lld", (long long)s);
    printf("\n");

    int C = (int)input.dim(1);
    int T = (int)input.dim(2);
    int F = (int)input.dim(3);
    int input_size = C * F;
    printf("Bottleneck: C=%d T=%d F=%d -> input_size=%d\n", C, T, F, input_size);

    // Load weights from GGUF
    struct ggml_context* ggml_ctx = nullptr;
    struct gguf_init_params gparams;
    gparams.no_alloc = false;
    gparams.ctx = &ggml_ctx;

    struct gguf_context* gctx = gguf_init_from_file(gguf_path, gparams);
    if (!gctx) { fprintf(stderr, "Failed to load GGUF\n"); return 1; }

    NpyArray gru_wih = load_tensor_from_ggml(ggml_ctx, "bottleneck.gru.weight_ih_l0", gctx);
    NpyArray gru_whh = load_tensor_from_ggml(ggml_ctx, "bottleneck.gru.weight_hh_l0", gctx);
    NpyArray gru_bih = load_tensor_from_ggml(ggml_ctx, "bottleneck.gru.bias_ih_l0", gctx);
    NpyArray gru_bhh = load_tensor_from_ggml(ggml_ctx, "bottleneck.gru.bias_hh_l0", gctx);
    NpyArray fc_w = load_tensor_from_ggml(ggml_ctx, "bottleneck.fc.weight", gctx);
    NpyArray fc_b = load_tensor_from_ggml(ggml_ctx, "bottleneck.fc.bias", gctx);

    int hidden_size = (int)gru_whh.dim(1);  // (3*H, H)
    int out_features = (int)fc_w.dim(0);     // (out, in)
    printf("GRU hidden_size=%d, FC out=%d\n", hidden_size, out_features);

    gguf_free(gctx);
    if (ggml_ctx) ggml_free(ggml_ctx);

    // Reshape input: (1,C,T,F) -> (T, C*F)
    // PyTorch rearrange "b c t f -> b t (c f)" means contiguous in (c,f) order
    std::vector<float> reshaped(T * input_size);
    for (int t = 0; t < T; t++) {
        for (int c = 0; c < C; c++) {
            for (int f = 0; f < F; f++) {
                reshaped[t * input_size + c * F + f] =
                    input.data[c * T * F + t * F + f];
            }
        }
    }

    // GRU forward
    std::vector<float> hidden(hidden_size, 0.0f);
    std::vector<float> gru_out(T * hidden_size);
    gru_forward(reshaped.data(), T, input_size,
                hidden.data(), hidden_size,
                gru_wih.data.data(), gru_whh.data.data(),
                gru_bih.data.data(), gru_bhh.data.data(),
                gru_out.data());

    // Linear: (T, H) -> (T, C*F)
    std::vector<float> fc_out(T * out_features);
    linear(gru_out.data(), T, hidden_size,
           fc_w.data.data(), fc_b.data.data(), out_features,
           fc_out.data());

    // Reshape back: (T, C*F) -> (1,C,T,F)
    NpyArray result;
    result.shape = {1, (int64_t)C, (int64_t)T, (int64_t)F};
    result.data.resize(C * T * F);
    for (int t = 0; t < T; t++) {
        for (int c = 0; c < C; c++) {
            for (int f = 0; f < F; f++) {
                result.data[c * T * F + t * F + f] =
                    fc_out[t * out_features + c * F + f];
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
        if (result.numel() != expected.numel()) {
            fprintf(stderr, "Element count mismatch: got %lld expected %lld\n",
                    (long long)result.numel(), (long long)expected.numel());
            return 1;
        }
        float max_err = max_abs_diff(result.data.data(), expected.data.data(), result.numel());
        float mean_err = mean_abs_diff(result.data.data(), expected.data.data(), result.numel());
        print_result("bottleneck", max_err, mean_err);
        return max_err < 1e-2f ? 0 : 1;
    }

    return 0;
}
