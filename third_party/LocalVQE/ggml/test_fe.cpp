/**
 * Test power-law feature extraction (FE) block.
 *
 * FE has no learnable weights — it's pure computation:
 *   Input:  (1, F, T, 2)  STFT with real/imag
 *   Output: (1, 2, T, F)  power-law compressed, planes separated
 *
 * Usage:
 *   test_fe --input fe_mic_input.npy --expected fe_mic_output.npy
 *   test_fe --input fe_mic_input.npy --output fe_mic_ggml.npy
 */

#include "common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

/**
 * Power-law feature extraction.
 * x: (F, T, 2) -> out: (2, T, F)
 * Compresses magnitude: |X|^c, preserving phase direction.
 */
static void power_law_fe(
    const float* x, float* out,
    int F, int T, float c
) {
    const float eps = 1e-12f;
    for (int f = 0; f < F; f++) {
        for (int t = 0; t < T; t++) {
            int idx_in = f * T * 2 + t * 2;
            float re = x[idx_in];
            float im = x[idx_in + 1];
            float mag = std::sqrt(re * re + im * im + eps);
            float scale = std::pow(mag, c - 1.0f) / (1.0f + eps);
            out[0 * T * F + t * F + f] = re * scale;
            out[1 * T * F + t * F + f] = im * scale;
        }
    }
}

int main(int argc, char** argv) {
    const char* input_path = nullptr;
    const char* expected_path = nullptr;
    const char* output_path = nullptr;
    float power_law_c = 0.3f;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) input_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else if (arg == "--c" && i + 1 < argc) power_law_c = std::stof(argv[++i]);
        else {
            fprintf(stderr, "Usage: test_fe --input <input.npy> [--expected <expected.npy>] [--output <output.npy>] [--c 0.3]\n");
            return 1;
        }
    }

    if (!input_path) {
        fprintf(stderr, "Error: --input required\n");
        return 1;
    }

    // Load input: (1, F, T, 2) or (F, T, 2)
    NpyArray input = npy_load(input_path);
    printf("Input shape:");
    for (auto s : input.shape) printf(" %lld", (long long)s);
    printf("\n");

    // Handle batch dimension
    int F, T;
    const float* in_data;
    if (input.ndim() == 4) {
        // (1, F, T, 2)
        F = (int)input.dim(1);
        T = (int)input.dim(2);
        in_data = input.data.data();
    } else if (input.ndim() == 3) {
        // (F, T, 2)
        F = (int)input.dim(0);
        T = (int)input.dim(1);
        in_data = input.data.data();
    } else {
        fprintf(stderr, "Error: expected 3D or 4D input\n");
        return 1;
    }

    printf("FE: F=%d T=%d c=%.2f\n", F, T, power_law_c);

    // Compute FE
    std::vector<float> out_data(2 * T * F);
    power_law_fe(in_data, out_data.data(), F, T, power_law_c);

    // Save output if requested
    if (output_path) {
        npy_save(output_path, out_data.data(), {1, 2, (int64_t)T, (int64_t)F});
        printf("Saved output to %s\n", output_path);
    }

    // Compare with expected if provided
    if (expected_path) {
        NpyArray expected = npy_load(expected_path);
        printf("Expected shape:");
        for (auto s : expected.shape) printf(" %lld", (long long)s);
        printf("\n");

        // Handle batch dim in expected
        const float* exp_data = expected.data.data();
        int64_t n = 2 * T * F;

        if (expected.numel() != n && expected.numel() != n) {
            fprintf(stderr, "Error: expected numel %lld but got %lld\n",
                    (long long)n, (long long)expected.numel());
            return 1;
        }

        float max_err = max_abs_diff(out_data.data(), exp_data, n);
        float mean_err = mean_abs_diff(out_data.data(), exp_data, n);
        print_result("FE (power-law)", max_err, mean_err);

        return max_err < 1e-2f ? 0 : 1;
    }

    printf("Done (no comparison — pass --expected to verify)\n");
    return 0;
}
