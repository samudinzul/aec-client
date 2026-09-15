/**
 * Test Complex Convolving Mask (CCM).
 *
 * CCM forward:
 *   m: (B,27,T,F) mask from decoder
 *   x: (B,F,T,2) input STFT (real,imag)
 *
 * 1. Decompose mask: (B, 3 basis, 9 kernel, T, F)
 * 2. H_real = sum(v_real * m, dim=basis)  -> (B,9,T,F)
 *    H_imag = sum(v_imag * m, dim=basis)  -> (B,9,T,F)
 * 3. Reshape to M_real, M_imag: (B,3,3,T,F)
 * 4. Unfold input: pad(1,1,2,0) -> unfold(3,3) -> (B,2,3,3,T,F)
 * 5. Complex multiply and sum over kernel dims
 * 6. Output: (B,F,T,2)
 *
 * Usage:
 *   test_ccm --input-mask ccm_input_0.npy --input-stft ccm_input_1.npy \
 *     --expected ccm_output.npy
 */

#include "common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Cube-root-of-unity basis vectors (hardcoded, matches ccm.py)
static const float V_REAL[3] = {1.0f, -0.5f, -0.5f};
static const float V_IMAG[3] = {0.0f, 0.86602540378f, -0.86602540378f};  // sqrt(3)/2

int main(int argc, char** argv) {
    const char* mask_path = nullptr;   // (1,27,T,F)
    const char* stft_path = nullptr;   // (1,F,T,2)
    const char* expected_path = nullptr;
    const char* output_path = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--input-mask" && i + 1 < argc) mask_path = argv[++i];
        else if (arg == "--input-stft" && i + 1 < argc) stft_path = argv[++i];
        else if (arg == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else {
            fprintf(stderr, "Usage: test_ccm --input-mask mask.npy --input-stft stft.npy "
                    "--expected expected.npy\n");
            return 1;
        }
    }

    if (!mask_path || !stft_path) {
        fprintf(stderr, "Error: --input-mask and --input-stft required\n");
        return 1;
    }

    NpyArray mask = npy_load(mask_path);   // (1, 27, T, F)
    NpyArray stft = npy_load(stft_path);   // (1, F, T, 2)

    printf("Mask shape:");
    for (auto s : mask.shape) printf(" %lld", (long long)s);
    printf("\nSTFT shape:");
    for (auto s : stft.shape) printf(" %lld", (long long)s);
    printf("\n");

    int T = (int)mask.dim(2);
    int F_mask = (int)mask.dim(3);
    int F_stft = (int)stft.dim(1);

    if (F_mask != F_stft) {
        fprintf(stderr, "F mismatch: mask F=%d, stft F=%d\n", F_mask, F_stft);
        return 1;
    }
    int F = F_mask;
    printf("CCM: T=%d F=%d\n", T, F);

    // Step 1-2: Decompose mask into H_real, H_imag
    // mask: (27, T, F) -> reshape to (3, 9, T, F) -> weighted sum over basis
    // mask channel layout: (r=0,c=0), (r=0,c=1), ..., (r=0,c=8), (r=1,c=0), ...
    // rearrange("b (r c) t f -> b r c t f", r=3) means r varies slowest
    std::vector<float> H_real(9 * T * F, 0.0f);
    std::vector<float> H_imag(9 * T * F, 0.0f);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 9; c++) {
            int ch = r * 9 + c;  // mask channel index
            for (int t = 0; t < T; t++) {
                for (int f = 0; f < F; f++) {
                    float val = mask.data[ch * T * F + t * F + f];
                    H_real[c * T * F + t * F + f] += V_REAL[r] * val;
                    H_imag[c * T * F + t * F + f] += V_IMAG[r] * val;
                }
            }
        }
    }

    // Step 3: Reshape H to M: (9, T, F) -> (3, 3, T, F)
    // H_real[c, t, f] where c = m*3 + n -> M_real[m, n, t, f]
    // Already contiguous, just reinterpret indices.

    // Step 4: Unfold input STFT
    // stft: (1, F, T, 2) -> permute to (2, T, F) then pad(1,1,2,0) -> unfold(3,3)
    // x_perm: (2, T, F)
    // After ZeroPad2d([1,1,2,0]): (2, T+2, F+2)
    // Unfold(3,3) extracts 3x3 patches -> (2, 3, 3, T, F)

    int T_pad = T + 2;  // top=2, bottom=0
    int F_pad = F + 2;  // left=1, right=1

    // Build padded x: (2, T_pad, F_pad)
    std::vector<float> x_pad(2 * T_pad * F_pad, 0.0f);
    for (int c = 0; c < 2; c++) {
        for (int t = 0; t < T; t++) {
            for (int f = 0; f < F; f++) {
                // stft layout: (F, T, 2)
                float val = stft.data[f * T * 2 + t * 2 + c];
                // x_perm layout: (2, T, F) -> padded: (2, T+2, F+2)
                x_pad[c * T_pad * F_pad + (t + 2) * F_pad + (f + 1)] = val;
            }
        }
    }

    // Extract 3x3 patches: x_unfold[c, m, n, t, f] = x_pad[c, t+m, f+n]
    // c in {0=real, 1=imag}, m in {0,1,2}, n in {0,1,2}
    std::vector<float> x_unfold(2 * 3 * 3 * T * F);
    for (int c = 0; c < 2; c++) {
        for (int m = 0; m < 3; m++) {
            for (int n = 0; n < 3; n++) {
                for (int t = 0; t < T; t++) {
                    for (int f = 0; f < F; f++) {
                        float val = x_pad[c * T_pad * F_pad + (t + m) * F_pad + (f + n)];
                        x_unfold[((c * 3 + m) * 3 + n) * T * F + t * F + f] = val;
                    }
                }
            }
        }
    }

    // Step 5: Complex multiply and sum over kernel dims (m, n)
    // x_enh_real[t,f] = sum_{m,n} M_real[m,n,t,f] * x_unfold[0,m,n,t,f]
    //                  - sum_{m,n} M_imag[m,n,t,f] * x_unfold[1,m,n,t,f]
    // x_enh_imag[t,f] = sum_{m,n} M_real[m,n,t,f] * x_unfold[1,m,n,t,f]
    //                  + sum_{m,n} M_imag[m,n,t,f] * x_unfold[0,m,n,t,f]
    std::vector<float> enh_real(T * F, 0.0f);
    std::vector<float> enh_imag(T * F, 0.0f);

    for (int m = 0; m < 3; m++) {
        for (int n = 0; n < 3; n++) {
            int kernel_idx = m * 3 + n;  // index into H_real/H_imag (0..8)
            for (int t = 0; t < T; t++) {
                for (int f = 0; f < F; f++) {
                    int tf = t * F + f;
                    float mr = H_real[kernel_idx * T * F + tf];
                    float mi = H_imag[kernel_idx * T * F + tf];
                    float xr = x_unfold[((0 * 3 + m) * 3 + n) * T * F + tf];
                    float xi = x_unfold[((1 * 3 + m) * 3 + n) * T * F + tf];
                    enh_real[tf] += mr * xr - mi * xi;
                    enh_imag[tf] += mr * xi + mi * xr;
                }
            }
        }
    }

    // Step 6: Output layout (B,F,T,2)
    // stack([enh_real, enh_imag], dim=3).transpose(1,2) -> (1,F,T,2)
    // enh_real/imag are (T,F), output is (F,T,2)
    NpyArray result;
    result.shape = {1, (int64_t)F, (int64_t)T, 2};
    result.data.resize(F * T * 2);
    for (int f = 0; f < F; f++) {
        for (int t = 0; t < T; t++) {
            result.data[f * T * 2 + t * 2 + 0] = enh_real[t * F + f];
            result.data[f * T * 2 + t * 2 + 1] = enh_imag[t * F + f];
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
        print_result("ccm", max_err, mean_err);
        return max_err < 1e-2f ? 0 : 1;
    }

    return 0;
}
