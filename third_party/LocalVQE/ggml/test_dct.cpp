/**
 * Test DCT-II matmul layout: encode → decode roundtrip should reconstruct
 * the input, since decoder.linear.weight is encoder.conv.weight.squeeze(1).T
 * and the DCT-II basis is orthonormal.
 *
 * Also validates encoder output against a direct DCT-II formula for an
 * impulse input [1, 0, ..., 0].
 *
 * Usage:
 *   test_dct <model.gguf>
 */

#include "localvqe_graph.h"

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static float max_abs_diff(const float* a, const float* b, int n) {
    float m = 0;
    for (int i = 0; i < n; i++) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_dct <model.gguf>\n");
        return 1;
    }
    const char* gguf_path = argv[1];

    dvqe_graph_model m;
    if (!load_graph_model(gguf_path, m, false)) return 1;

    struct ggml_tensor* W_enc = m.w("encoder.conv.weight");
    struct ggml_tensor* W_dec = m.w("decoder.linear.weight");
    if (!W_enc || !W_dec) {
        fprintf(stderr, "GGUF missing encoder.conv.weight or decoder.linear.weight\n");
        free_graph_model(m);
        return 1;
    }
    printf("encoder.conv.weight   ne=(%lld, %lld, %lld)\n",
           (long long)W_enc->ne[0], (long long)W_enc->ne[1], (long long)W_enc->ne[2]);
    printf("decoder.linear.weight ne=(%lld, %lld)\n",
           (long long)W_dec->ne[0], (long long)W_dec->ne[1]);

    const int K = 512;

    struct ggml_init_params gp = {64 * 1024 * 1024, nullptr, true};
    struct ggml_context* ctx = ggml_init(gp);

    struct ggml_tensor* x = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, K);
    ggml_set_input(x);

    struct ggml_tensor* W_enc_2d = ggml_reshape_2d(ctx, W_enc, K, K);
    struct ggml_tensor* encoded  = ggml_mul_mat(ctx, W_enc_2d, x);
    struct ggml_tensor* decoded  = ggml_mul_mat(ctx, W_dec, encoded);
    ggml_set_output(encoded);
    ggml_set_output(decoded);

    struct ggml_cgraph* graph = ggml_new_graph(ctx);
    ggml_build_forward_expand(graph, encoded);
    ggml_build_forward_expand(graph, decoded);

    ggml_gallocr_t ga = ggml_gallocr_new(ggml_backend_get_default_buffer_type(m.backend));
    if (!ggml_gallocr_alloc_graph(ga, graph)) {
        fprintf(stderr, "alloc_graph failed\n");
        return 1;
    }

    // ── Test 1: impulse → DCT-II basis row 0 (DC) + row-k predictions ─────
    std::vector<float> x_data(K, 0.0f);
    x_data[0] = 1.0f;
    ggml_backend_tensor_set(x, x_data.data(), 0, K * sizeof(float));
    ggml_backend_graph_compute(m.backend, graph);

    std::vector<float> y_enc(K);
    ggml_backend_tensor_get(encoded, y_enc.data(), 0, K * sizeof(float));

    // Reference DCT-II: y[k] = cos(pi * (2n+1) * k / (2N)) with norm[0]=sqrt(1/N),
    // norm[k>0]=sqrt(2/N). For impulse at n=0: y[k] = cos(pi * k / (2N)) * norm[k].
    std::vector<float> y_ref(K);
    for (int k = 0; k < K; k++) {
        float norm = (k == 0) ? std::sqrt(1.0f / K) : std::sqrt(2.0f / K);
        y_ref[k] = std::cos((float)M_PI * k / (2.0f * K)) * norm;
    }
    float err_enc = max_abs_diff(y_enc.data(), y_ref.data(), K);
    printf("\nTest 1 — encoder(impulse) vs analytic DCT-II: max_abs_diff = %.3e\n", err_enc);
    if (err_enc > 1e-5f) {
        fprintf(stderr, "FAIL: encoder layout mismatch\n");
        return 1;
    }

    // ── Test 2: random → encode → decode → roundtrip equals input ───────
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < K; i++) x_data[i] = u(rng);
    ggml_backend_tensor_set(x, x_data.data(), 0, K * sizeof(float));
    ggml_backend_graph_compute(m.backend, graph);

    std::vector<float> y_dec(K);
    ggml_backend_tensor_get(decoded, y_dec.data(), 0, K * sizeof(float));
    float err_rt = max_abs_diff(x_data.data(), y_dec.data(), K);
    printf("Test 2 — decoder(encoder(rand))   roundtrip:      max_abs_diff = %.3e\n", err_rt);
    if (err_rt > 1e-4f) {
        fprintf(stderr, "FAIL: decoder(encoder(x)) != x (basis not orthonormal?)\n");
        return 1;
    }

    printf("\nAll DCT tests PASS.\n");

    ggml_gallocr_free(ga);
    ggml_free(ctx);
    free_graph_model(m);
    return 0;
}
