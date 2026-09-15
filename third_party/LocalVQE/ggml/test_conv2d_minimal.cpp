/**
 * Minimal ggml_conv_2d test: verify against hand-computed values.
 * Input: (W=5, H=3, C=1, N=1) all ones
 * Kernel: (KW=3, KH=2, IC=1, OC=1) all ones
 * Stride: (1, 1), no padding
 * Expected output: (3, 2, 1, 1) all 6.0 (3*2*1 = 6)
 */
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    struct ggml_init_params params = {16 * 1024 * 1024, nullptr, true};  // no_alloc=true
    struct ggml_context* ctx = ggml_init(params);

    // Kernel: (KW=3, KH=2, IC=1, OC=1) = all 1.0
    struct ggml_tensor* kernel = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, 3, 2, 1, 1);
    ggml_set_name(kernel, "kernel");
    ggml_set_input(kernel);
    float k_data[6] = {1,1,1, 1,1,1};

    // Input: (W=5, H=3, C=1, N=1) = all 1.0
    struct ggml_tensor* input = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, 5, 3, 1, 1);
    ggml_set_name(input, "input");
    ggml_set_input(input);
    float i_data[15];
    for (int i = 0; i < 15; i++) i_data[i] = 1.0f;

    // Conv2d: stride=1, no padding
    struct ggml_tensor* conv = ggml_conv_2d(ctx, kernel, input, 1, 1, 0, 0, 1, 1);
    ggml_set_output(conv);
    printf("Expected output shape: (3, 2, 1, 1)\n");
    printf("Actual output shape: (%lld, %lld, %lld, %lld)\n",
           (long long)conv->ne[0], (long long)conv->ne[1],
           (long long)conv->ne[2], (long long)conv->ne[3]);

    // Build and run
    struct ggml_cgraph* graph = ggml_new_graph(ctx);
    ggml_build_forward_expand(graph, conv);

    ggml_backend_load_all();
    ggml_backend_t backend = ggml_backend_init_by_name("CPU", nullptr);
    if (!backend) { fprintf(stderr, "Failed to init CPU backend\n"); return 1; }
    ggml_gallocr_t galloc = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend));
    ggml_gallocr_alloc_graph(galloc, graph);

    ggml_backend_tensor_set(kernel, k_data, 0, sizeof(k_data));
    ggml_backend_tensor_set(input, i_data, 0, sizeof(i_data));

    ggml_backend_graph_compute(backend, graph);

    int n = ggml_nelements(conv);
    std::vector<float> out(n);
    ggml_backend_tensor_get(conv, out.data(), 0, n * sizeof(float));
    printf("Output values (%d elements): ", n);
    for (int i = 0; i < n; i++) printf("%.1f ", out[i]);
    printf("\n");
    printf("Expected: all 6.0\n");

    // Test 2: non-trivial kernel with stride
    // Input: (W=6, H=3, C=2, N=1) = [chan0: all 1.0, chan1: all 2.0]
    // Kernel: (KW=3, KH=2, IC=2, OC=1) = all 1.0
    // Stride: (2, 1), no padding
    // output_W = (6-3)/2 + 1 = 2
    // output_H = (3-2)/1 + 1 = 2
    // Each output = sum of 3*2*2 = 12 inputs, with chan0=1 and chan1=2: 6*1 + 6*2 = 18
    printf("\n--- Test 2: stride + multi-channel ---\n");

    struct ggml_context* ctx2 = ggml_init(params);  // also no_alloc=true
    struct ggml_tensor* k2 = ggml_new_tensor_4d(ctx2, GGML_TYPE_F32, 3, 2, 2, 1);
    ggml_set_input(k2);
    float k2_data[12]; for (int i = 0; i < 12; i++) k2_data[i] = 1.0f;

    struct ggml_tensor* i2 = ggml_new_tensor_4d(ctx2, GGML_TYPE_F32, 6, 3, 2, 1);
    ggml_set_input(i2);
    float i2_data[36];
    for (int c = 0; c < 2; c++)
        for (int h = 0; h < 3; h++)
            for (int w = 0; w < 6; w++)
                i2_data[c * 18 + h * 6 + w] = (c == 0) ? 1.0f : 2.0f;

    struct ggml_tensor* c2 = ggml_conv_2d(ctx2, k2, i2, 2, 1, 0, 0, 1, 1);
    ggml_set_output(c2);
    printf("Shape: (%lld, %lld, %lld, %lld)\n",
           (long long)c2->ne[0], (long long)c2->ne[1],
           (long long)c2->ne[2], (long long)c2->ne[3]);

    struct ggml_cgraph* g2 = ggml_new_graph(ctx2);
    ggml_build_forward_expand(g2, c2);
    ggml_gallocr_t ga2 = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend));
    ggml_gallocr_alloc_graph(ga2, g2);
    ggml_backend_tensor_set(k2, k2_data, 0, sizeof(k2_data));
    ggml_backend_tensor_set(i2, i2_data, 0, sizeof(i2_data));
    ggml_backend_graph_compute(backend, g2);

    int n2 = ggml_nelements(c2);
    std::vector<float> out2(n2);
    ggml_backend_tensor_get(c2, out2.data(), 0, n2 * sizeof(float));
    printf("Output (%d): ", n2);
    for (int i = 0; i < n2; i++) printf("%.1f ", out2[i]);
    printf("\nExpected: all 18.0\n");

    // Test 3: Use external weight tensor (simulating weight_ctx)
    // This tests whether using a weight from a different context works
    printf("\n--- Test 3: External weight context ---\n");

    struct ggml_init_params wparams = {1024 * 1024, nullptr, false};  // no_alloc=false for weight data
    struct ggml_context* wctx = ggml_init(wparams);

    struct ggml_tensor* ext_k = ggml_new_tensor_4d(wctx, GGML_TYPE_F32, 3, 2, 1, 1);
    // Set weight data directly in the tensor
    float ext_k_data[6] = {1,1,1, 1,1,1};
    memcpy(ext_k->data, ext_k_data, sizeof(ext_k_data));

    struct ggml_context* ctx3 = ggml_init(params);  // no_alloc=true
    struct ggml_tensor* i3 = ggml_new_tensor_4d(ctx3, GGML_TYPE_F32, 5, 3, 1, 1);
    ggml_set_input(i3);

    // Use external kernel in conv2d
    struct ggml_tensor* c3 = ggml_conv_2d(ctx3, ext_k, i3, 1, 1, 0, 0, 1, 1);
    ggml_set_output(c3);

    struct ggml_cgraph* g3 = ggml_new_graph(ctx3);
    ggml_build_forward_expand(g3, c3);
    ggml_gallocr_t ga3 = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend));
    ggml_gallocr_alloc_graph(ga3, g3);
    ggml_backend_tensor_set(i3, i_data, 0, sizeof(i_data));
    // ext_k data is set directly in wctx memory - check if backend can read it

    ggml_backend_graph_compute(backend, g3);

    int n3 = ggml_nelements(c3);
    std::vector<float> out3(n3);
    ggml_backend_tensor_get(c3, out3.data(), 0, n3 * sizeof(float));
    printf("Shape: (%lld, %lld, %lld, %lld)\n",
           (long long)c3->ne[0], (long long)c3->ne[1],
           (long long)c3->ne[2], (long long)c3->ne[3]);
    printf("Output (%d): ", n3);
    for (int i = 0; i < n3; i++) printf("%.1f ", out3[i]);
    printf("\nExpected: all 6.0\n");

    ggml_gallocr_free(ga3);
    ggml_gallocr_free(ga2);
    ggml_gallocr_free(galloc);
    ggml_backend_free(backend);
    ggml_free(ctx3);
    ggml_free(wctx);
    ggml_free(ctx2);
    ggml_free(ctx);
    printf("\nDone.\n");
    return 0;
}
