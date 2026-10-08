#include <iostream>
#include <vector>
#include <sstream>
#include <cstring>
#include <limits>
#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>
#include <iostream>
#include <complex>
#include "onnxruntime_cxx_api.h"
#include "pocketfft_hdronly.h"
#include "AudioFile.h"

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// ============================================================
//  NKF model expects 1024-sample blocks (513-bin FFT)
//  Updated to match nkf.onnx dimensions
// ============================================================
#define SAMEPLERATE  (16000)
#define BLOCK_LEN    (1024)
#define BLOCK_SHIFT  (512)
#define FFT_OUT_SIZE (513)
#define NKF_LEN (4)
// Intrinsic residual echo suppressor (Wiener post-filter — mirrors
// web/nkf.py RES_*, proven there first): per-bin gain from the core's
// own echohat. Echo-minority bins pass at unity (double-talk safety
// structural); echo-majority bins trim toward the floor.
#define RES_EMA     (0.15)
#define RES_THRESH  (0.5)
#define RES_FLOOR   (0.1)
#define RES_RELEASE (0.25)
typedef std::complex<double> cpx_type;

struct nkf_engine {
    float mic_buffer[BLOCK_LEN] = { 0 };
    float out_buffer[BLOCK_LEN] = { 0 };
    float lpb_buffer[BLOCK_LEN] = { 0 };

    float lpb_real[FFT_OUT_SIZE * NKF_LEN] = { 0 };
    float lpb_imag[FFT_OUT_SIZE * NKF_LEN] = { 0 };
    double h_prior_real[FFT_OUT_SIZE * NKF_LEN] = { 0 };
    double h_prior_imag[FFT_OUT_SIZE * NKF_LEN] = { 0 };
    double h_posterior_real[FFT_OUT_SIZE * NKF_LEN] = { 0 };
    double h_posterior_imag[FFT_OUT_SIZE * NKF_LEN] = { 0 };

    std::vector<std::vector<float>> instates;
};

class NKFImpl {
public:
    int Enhance(std::string in_audio, std::string lpb_audio, std::string out_audio);

    // ========================================================
    //  Real-time API (added for AEC Client integration)
    // ========================================================
    void Reset() {
        ResetInout();
    }

    void ProcessBlock(const float* mic_new, const float* lpb_new, float* out);

    // Current RES attenuation estimate, dB (telemetry for NkfGetState).
    // NOTE: must stay in this public block — everything below the
    // first `private:` label is inaccessible to the wrapper.
    double ResDb() const { return m_resDb; }

    // Freeze adaptation: keep applying the current filter (echohat
    // synthesis still runs) but skip the ONNX inference and the
    // Kalman/state updates. Used when the engine sits inside a
    // self-monitor feedback loop — a converged filter is stable
    // there, continued adaptation is not.
    void SetFrozen(bool f) { m_frozen = f; }

private:
    // APPEND-ONLY RULE (this file got burned by violating it): new data
    // members go at the END of the class, never inserted — inserting
    // shifts every member offset, and any binary still compiled from an
    // older copy of this header then reads garbage (hard SEGV on first
    // ProcessBlock). The engine is now compiled straight into aec_gui;
    // keep the rule anyway.
    void init_engine_threads(int inter_threads, int intra_threads) {
        session_options.SetIntraOpNumThreads(intra_threads);
        session_options.SetInterOpNumThreads(inter_threads);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    };

    void init_onnx_model(const std::string ModelPath) {
        init_engine_threads(1, 1);
        // Windows ONNX Runtime requires wide-string paths
        std::wstring wideModelPath(ModelPath.begin(), ModelPath.end());
        session = std::make_shared<Ort::Session>(env, wideModelPath.c_str(), session_options);
    };

    void ResetInout() {
        m_pEngine.instates.clear();
        m_pEngine.instates.resize(4);
        for (int i = 0; i < 4; i++) {
            m_pEngine.instates[i].clear();
            m_pEngine.instates[i].resize(FFT_OUT_SIZE * 18);
            std::fill(m_pEngine.instates[i].begin(), m_pEngine.instates[i].end(), 0);
        }
        memset(m_pEngine.mic_buffer, 0, BLOCK_LEN * sizeof(float));
        memset(m_pEngine.lpb_buffer, 0, BLOCK_LEN * sizeof(float));
        memset(m_pEngine.out_buffer, 0, BLOCK_LEN * sizeof(float));
        memset(m_pEngine.lpb_real, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(float));
        memset(m_pEngine.lpb_imag, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(float));
        memset(m_pEngine.h_posterior_real, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(double));
        memset(m_pEngine.h_posterior_imag, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(double));
        memset(m_pEngine.h_prior_real, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(double));
        memset(m_pEngine.h_prior_imag, 0, FFT_OUT_SIZE * NKF_LEN * sizeof(double));
        memset(m_resPe, 0, FFT_OUT_SIZE * sizeof(double));
        memset(m_resPm, 0, FFT_OUT_SIZE * sizeof(double));
        m_resDb = 0.0;
    };

    void ExportWAV(const std::string& Filename,
                   const std::vector<float>& Data, unsigned SampleRate);
    void OnnxInfer();

public:
    NKFImpl(const std::string ModelPath) {
        init_onnx_model(ModelPath);
        // Periodic Hann (torch.hann_window, what the model trained on):
        // w[i] = sin^2(pi*i/N). NOT the symmetric sin(pi*i/(N-1)) —
        // at 50% overlap the periodic window sums to exactly 1.0.
        for (int i = 0; i < BLOCK_LEN; i++) {
            const float s = sinf(PI * i / BLOCK_LEN);
            m_windows[i] = s * s;
        }
        mic_res.resize(BLOCK_LEN);
        lpb_res.resize(BLOCK_LEN);
        fft_shape.assign(1, (size_t)BLOCK_LEN);
        fft_axes.assign(1, (size_t)0);
        fft_stride_in.assign(1, (ptrdiff_t)sizeof(double));
        fft_stride_out.assign(1, (ptrdiff_t)sizeof(cpx_type));
        ort_inputs.reserve(6);
        ResetInout();
    }

private:
    // OnnxRuntime resources
    Ort::Env env;
    Ort::SessionOptions session_options;
    std::shared_ptr<Ort::Session> session = nullptr;
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU);

    nkf_engine m_pEngine;
    std::vector<Ort::Value> ort_inputs;
    std::vector<Ort::Value> ort_outputs;

    std::vector<const char*> input_node_names = {
        "in_real", "in_imag", "in_hrr", "in_hir", "in_hri", "in_hii"
    };

    std::vector<const char*> output_node_names = {
        "enh_real", "enh_imag", "out_hrr", "out_hir", "out_hri", "out_hii"
    };

    const int64_t infea_node_dims[3] = { FFT_OUT_SIZE, 1, 2 * NKF_LEN + 1 };
    const int64_t in_states_dims[3] = { 1, FFT_OUT_SIZE, 18 };

    float m_windows[BLOCK_LEN] = { 0 };

    // Appended (not inserted): keep every pre-existing member offset stable.
    std::vector<cpx_type> mic_res;
    std::vector<cpx_type> lpb_res;
    std::vector<size_t>    fft_shape;
    std::vector<size_t>    fft_axes;
    std::vector<ptrdiff_t> fft_stride_in;
    std::vector<ptrdiff_t> fft_stride_out;

    // Newest member — must stay LAST (see append-only rule above).
    bool m_frozen = false;

    // Intrinsic RES state — appended, never inserted (same rule).
    double m_resPe[FFT_OUT_SIZE] = { 0 };
    double m_resPm[FFT_OUT_SIZE] = { 0 };
    double m_resDb = 0.0;
};