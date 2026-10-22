#include "localvqe_wrapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"
#include "localvqe_aec_frontend.h"

// LocalVQE v1.4-AEC runner (V1): vendored Kalman frontend +
// reference-flow codec + mask Run. Ports soniqo/speech-core
// onnx_localvqe_echo_canceller.cpp (Apache-2.0) DSP exactly:
// bins 1..256 packed (DC dropped), Nyquist doubled on synthesis,
// 7 carried states, overlap-add. No speech-core or JSON dependency:
// the two bundle JSONs are read by a strict minimal extractor below.

namespace {

constexpr int kSr = 16000;
constexpr int kHop = 256;   // 16 ms frame
constexpr int kFft = 512;
constexpr int kBins = 256;  // packed bins (FFT bins 1..256)
constexpr int kWeights = 2742;
constexpr double kPi = 3.14159265358979323846;

const char* kRunId = "v1.4.r005";
const char* kGgufSha =
    "b6e43138588a83bfe903ab5e143b4020b91c1e1629f5a575ac5855ff0003c731";

const char* kInNames[] = {
    "mic_spectrum", "reference_spectrum", "conv_state",
    "align_key_state", "align_reference_state", "align_smooth_state",
    "s4_real_state", "s4_imag_state", "ccm_state",
};
const char* kOutNames[] = {
    "enhanced_spectrum", "next_conv_state", "next_align_key_state",
    "next_align_reference_state", "next_align_smooth_state",
    "next_s4_real_state", "next_s4_imag_state", "next_ccm_state",
};
// State tensor shapes (input slot i+2 == output slot i+1).
const int64_t kStateShapes[][4] = {
    {1, 75936}, {1, 16, 15, 64}, {1, 20, 15, 64}, {1, 16, 4, 16},
    {1, 80}, {1, 80}, {1, 2, 2, 256},
};
const int kStateRank[] = {2, 4, 4, 4, 2, 2, 4};

void Fft(double* re, double* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / len * (inverse ? 1.0 : -1.0);
        const double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;
            for (int j = 0; j < len / 2; j++) {
                const double ur = re[i + j], ui = im[i + j];
                const double vr = re[i + j + len / 2] * cwr -
                                  im[i + j + len / 2] * cwi;
                const double vi = re[i + j + len / 2] * cwi +
                                  im[i + j + len / 2] * cwr;
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr;
                im[i + j + len / 2] = ui - vi;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

long NumEl(const int64_t* shape, int rank) {
    long n = 1;
    for (int i = 0; i < rank; i++) {
        if (shape[i] <= 0) return -1;
        n *= (long)shape[i];
    }
    return n;
}

bool ReadFile(const char* path, std::string& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        err = std::string("missing file: ") + (path ? path : "(null)");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 24)) {
        fclose(f);
        err = "file unreadable/too large";
        return false;
    }
    out.assign((size_t)n, '\0');
    bool ok = n == 0 || fread(&out[0], 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) err = "short read";
    return ok;
}

// Minimal strict JSON extractors (bundle files are machine-generated;
// full parser would be a new dependency for 3 keys).
const char* FindKey(const std::string& js, const char* key) {
    std::string q = std::string("\"") + key + "\"";
    const char* p = strstr(js.c_str(), q.c_str());
    if (!p) return nullptr;
    p = strchr(p + q.size(), ':');
    return p ? p + 1 : nullptr;
}

bool ExtractFloatArray(const std::string& js, const char* key, int count,
                       std::vector<float>& out, std::string& err) {
    const char* p = FindKey(js, key);
    if (!p || *p != '[' && (p = strchr(p, '[')) == nullptr) {
        err = std::string("bad/missing array: ") + key;
        return false;
    }
    p++;
    out.clear();
    out.reserve((size_t)count);
    char* end = nullptr;
    for (int i = 0; i < count; i++) {
        double v = strtod(p, &end);
        if (end == p || !std::isfinite(v)) {
            err = std::string("bad number in ") + key;
            return false;
        }
        out.push_back((float)v);
        p = end;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ',')
            p++;
    }
    if (*p != ']') {
        err = std::string("array length != ") + std::to_string(count) +
              " in " + key;
        return false;
    }
    return true;
}

bool ExtractString(const std::string& js, const char* key, std::string& out) {
    const char* p = FindKey(js, key);
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    out.clear();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        out.push_back(*p++);
    }
    return *p == '"';
}

bool ExtractInt(const std::string& js, const char* key, long& out) {
    const char* p = FindKey(js, key);
    if (!p) return false;
    char* end = nullptr;
    out = strtol(p, &end, 10);
    return end != p;
}

}  // namespace

struct LocalVqeHandle {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeCPU);
    std::vector<const char*> inPtr = {
        kInNames[0], kInNames[1], kInNames[2], kInNames[3], kInNames[4],
        kInNames[5], kInNames[6], kInNames[7], kInNames[8]};
    std::vector<const char*> outPtr = {
        kOutNames[0], kOutNames[1], kOutNames[2], kOutNames[3],
        kOutNames[4], kOutNames[5], kOutNames[6], kOutNames[7]};
    localvqe_aec_daf* daf = nullptr;
    std::vector<float> window = std::vector<float>(kFft, 0.0f);
    std::vector<std::vector<float>> states;  // 7 carried mask states
    std::vector<float> micHist = std::vector<float>(kHop, 0.0f);
    std::vector<float> refHist = std::vector<float>(kHop, 0.0f);
    std::vector<float> overlap = std::vector<float>(kFft, 0.0f);
    std::vector<float> enhBuf = std::vector<float>(kBins * 2);
    std::vector<float> micQ, refQ, outQ;
    std::string ioDump;
    std::string error;
    bool ready = false;
    bool neural = false;
    bool latched = false;
    float inRms = 0.0f, refRms = 0.0f, outRms = 0.0f;
    int dropped = 0;
};

static void AnalyzeOne(LocalVqeHandle* h, const float* frame,
                       std::vector<float>& hist, std::vector<float>& spec) {
    double re[kFft], im[kFft];
    for (int i = 0; i < kHop; i++) re[i] = hist[(size_t)i];
    for (int i = 0; i < kHop; i++) re[kHop + i] = frame[i];
    for (int i = 0; i < kHop; i++) hist[(size_t)i] = frame[i];
    for (int i = 0; i < kFft; i++) {
        re[i] *= h->window[(size_t)i];
        im[i] = 0.0;
    }
    Fft(re, im, kFft, false);
    spec.assign(kBins * 2, 0.0f);
    for (int b = 0; b < kBins; b++) {
        spec[(size_t)b] = (float)re[b + 1];
        spec[(size_t)(kBins + b)] = (float)im[b + 1];
    }
}

LocalVqeHandle* LocalVqeNew(const char* onnxPath, const char* frontendPath,
                            const char* configPath) {
    LocalVqeHandle* h = new LocalVqeHandle();
    h->micQ.reserve(16384);
    h->refQ.reserve(16384);
    h->outQ.reserve(16384);
    std::string err;
    if (!onnxPath || !*onnxPath || !frontendPath || !*frontendPath ||
        !configPath || !*configPath)
        err = "need onnx + frontend + config paths (run exp/get_models.sh)";
    else
    do {
        std::string cfg, front;
        if (!ReadFile(configPath, cfg, err)) break;
        if (!ReadFile(frontendPath, front, err)) break;
        // Provenance gate: refuse anything but v1.4.r005 float32.
        std::string s;
        long v = 0;
        if (!ExtractString(cfg, "model_type", s) ||
            s != "localvqe-v1.4-aec-onnx") {
            err = "config: not a localvqe-v1.4-aec-onnx bundle";
            break;
        }
        if (!ExtractString(cfg, "source_gguf_sha256", s) || s != kGgufSha) {
            err = "config: source GGUF hash mismatch (not v1.4.r005)";
            break;
        }
        if (!ExtractString(cfg, "precision", s) || s != "float32") {
            err = "config: need float32 bundle";
            break;
        }
        if (!ExtractInt(front, "format_version", v) || v != 1 ||
            !ExtractInt(front, "sample_rate", v) || v != kSr ||
            !ExtractInt(front, "fft_size", v) || v != kFft ||
            !ExtractInt(front, "hop_size", v) || v != kHop ||
            !ExtractInt(front, "daf_block_size", v) || v != 128 ||
            !ExtractInt(front, "daf_partitions", v) || v != 128 ||
            !ExtractInt(front, "daf_iterations", v) || v != 2) {
            err = "frontend: geometry mismatch (need 16k/512/256/128x128x2)";
            break;
        }
        if (!ExtractString(front, "source_run_id", s) || s != kRunId) {
            err = "frontend: not v1.4.r005 weights";
            break;
        }
        std::vector<float> weights;
        if (!ExtractFloatArray(front, "controller_weights", kWeights,
                               weights, err))
            break;
        if (!ExtractFloatArray(front, "analysis_window", kFft, h->window,
                               err))
            break;
        h->daf = localvqe_aec_daf_create(weights.data(), weights.size());
        if (!h->daf) {
            err = "frontend: daf_create failed";
            break;
        }
        try {
            h->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                                "localvqe-exp");
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(1);
            opts.SetInterOpNumThreads(1);
            opts.SetGraphOptimizationLevel(
                GraphOptimizationLevel::ORT_ENABLE_ALL);
            std::wstring widePath(onnxPath, onnxPath + strlen(onnxPath));
            h->session = std::make_unique<Ort::Session>(*h->env,
                                                        widePath.c_str(),
                                                        opts);
            // Strict graph-signature check (same contract as upstream).
            Ort::AllocatorWithDefaultOptions alloc;
            if (h->session->GetInputCount() != 9 ||
                h->session->GetOutputCount() != 8) {
                err = "onnx: need 9 inputs / 8 outputs";
                break;
            }
            std::string dump;
            for (size_t i = 0; i < 9; i++) {
                Ort::AllocatedStringPtr nm =
                    h->session->GetInputNameAllocated(i, alloc);
                if (strcmp(nm.get(), kInNames[i]) != 0) {
                    err = std::string("onnx input order mismatch at ") +
                          std::to_string(i);
                    break;
                }
                Ort::TypeInfo ti = h->session->GetInputTypeInfo(i);
                auto sh = ti.GetTensorTypeAndShapeInfo().GetShape();
                dump += std::string("IN ") + nm.get() + "\n";
                if (i < 2) {
                    if (sh.size() != 4 || sh[0] != 1 || sh[1] != 2 ||
                        sh[2] != 1 || sh[3] != kBins) {
                        err = "onnx: spectrum inputs must be [1,2,1,256]";
                        break;
                    }
                } else {
                    size_t st = i - 2;
                    if ((int)sh.size() != kStateRank[st]) {
                        err = "onnx: state rank mismatch";
                        break;
                    }
                    for (int d = 0; d < kStateRank[st]; d++)
                        if (sh[(size_t)d] != kStateShapes[st][d]) {
                            err = "onnx: state shape mismatch";
                            break;
                        }
                    if (!err.empty()) break;
                }
            }
            if (!err.empty()) break;
            for (size_t i = 0; i < 8; i++) {
                Ort::AllocatedStringPtr nm =
                    h->session->GetOutputNameAllocated(i, alloc);
                if (strcmp(nm.get(), kOutNames[i]) != 0) {
                    err = std::string("onnx output order mismatch at ") +
                          std::to_string(i);
                    break;
                }
                dump += std::string("OUT ") + nm.get() + "\n";
            }
            if (!err.empty()) break;
            h->ioDump = dump;
            h->states.clear();
            for (int s = 0; s < 7; s++) {
                long n = NumEl(kStateShapes[s], kStateRank[s]);
                h->states.emplace_back((size_t)n, 0.0f);
            }
            h->ready = true;
        } catch (const Ort::Exception& e) {
            err = std::string("ORT load failed: ") + e.what();
        } catch (const std::exception& e) {
            err = std::string("load failed: ") + e.what();
        }
    } while (false);
    if (!h->ready) {
        h->error = err.empty() ? "unknown init failure" : err;
        if (h->daf) {
            localvqe_aec_daf_destroy(h->daf);
            h->daf = nullptr;
        }
        h->session.reset();
    }
    return h;
}

int LocalVqeReady(const LocalVqeHandle* h) {
    return (h && h->ready) ? 1 : 0;
}

// One 256-sample hop. Returns false on failure (caller latches).
static bool RunHop(LocalVqeHandle* h, const float* mic, const float* ref,
                   float* emit) {
    float residual[kHop], echoEst[kHop];
    if (!localvqe_aec_daf_process(h->daf, mic, ref, kHop, residual, echoEst))
        return false;
    std::vector<float> micSpec, refSpec;
    AnalyzeOne(h, residual, h->micHist, micSpec);
    AnalyzeOne(h, echoEst, h->refHist, refSpec);
    std::vector<float> next[7];
    try {
        std::vector<Ort::Value> inputs;
        inputs.reserve(9);
        const int64_t specShape[] = {1, 2, 1, kBins};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            h->memInfo, micSpec.data(), micSpec.size(), specShape, 4));
        inputs.push_back(Ort::Value::CreateTensor<float>(
            h->memInfo, refSpec.data(), refSpec.size(), specShape, 4));
        for (int s = 0; s < 7; s++)
            inputs.push_back(Ort::Value::CreateTensor<float>(
                h->memInfo, h->states[(size_t)s].data(),
                h->states[(size_t)s].size(), kStateShapes[s],
                (size_t)kStateRank[s]));
        std::vector<Ort::Value> outs = h->session->Run(
            Ort::RunOptions{nullptr}, h->inPtr.data(), inputs.data(),
            inputs.size(), h->outPtr.data(), h->outPtr.size());
        memcpy(h->enhBuf.data(), outs[0].GetTensorData<float>(),
               h->enhBuf.size() * sizeof(float));
        for (int s = 0; s < 7; s++) {
            const float* d = outs[(size_t)(s + 1)].GetTensorData<float>();
            next[s].assign(d, d + h->states[(size_t)s].size());
        }
        for (int s = 0; s < 7; s++) h->states[(size_t)s].swap(next[s]);
        h->neural = true;
    } catch (const std::exception& e) {
        h->error =
            std::string("Run failed (latched to passthrough): ") + e.what();
        return false;
    }
    for (float v : h->enhBuf)
        if (!std::isfinite(v)) {
            h->error = "mask produced NaN/Inf (latched to passthrough)";
            return false;
        }
    // Synthesize (ports LocalVQECodec::synthesize: DC dropped,
    // Nyquist doubled, windowed overlap-add).
    double re[kFft], im[kFft];
    for (int i = 0; i < kFft; i++) re[i] = im[i] = 0.0;
    for (int b = 1; b < kBins; b++) {
        re[b] = h->enhBuf[(size_t)(b - 1)];
        im[b] = h->enhBuf[(size_t)(kBins + b - 1)];
        re[kFft - b] = re[b];
        im[kFft - b] = -im[b];
    }
    re[kBins] = 2.0 * h->enhBuf[kBins - 1];
    Fft(re, im, kFft, true);
    for (int i = 0; i < kFft; i++)
        h->overlap[(size_t)i] += (float)(re[i] * h->window[(size_t)i]);
    memcpy(emit, h->overlap.data(), kHop * sizeof(float));
    memmove(h->overlap.data(), h->overlap.data() + kHop,
            (kFft - kHop) * sizeof(float));
    memset(h->overlap.data() + kFft - kHop, 0, kHop * sizeof(float));
    return true;
}

int LocalVqeProcess(LocalVqeHandle* h, const float* mic, const float* ref,
                    float* out, int n) {
    if (!h || n <= 0) return 0;
    if (!mic || !out) return 0;
    double si = 0.0, sr = 0.0;
    for (int i = 0; i < n; i++) {
        si += (double)mic[i] * mic[i];
        if (ref) sr += (double)ref[i] * ref[i];
    }
    h->inRms = (float)sqrt(si / n);
    h->refRms = ref ? (float)sqrt(sr / n) : 0.0f;
    if (!h->ready || h->latched || !ref) {
        for (int i = 0; i < n; i++) out[i] = mic[i];
        h->outRms = h->inRms;
        return 0;
    }
    if (h->micQ.size() > 32768) {
        size_t drop = h->micQ.size() - 32768;
        h->micQ.erase(h->micQ.begin(), h->micQ.begin() + (long)drop);
        h->refQ.erase(h->refQ.begin(), h->refQ.begin() + (long)drop);
        h->dropped += (int)drop;
    }
    h->micQ.insert(h->micQ.end(), mic, mic + n);
    h->refQ.insert(h->refQ.end(), ref, ref + n);
    float m[kHop], r[kHop], e[kHop];
    while (h->micQ.size() >= (size_t)kHop) {
        memcpy(m, h->micQ.data(), kHop * sizeof(float));
        memcpy(r, h->refQ.data(), kHop * sizeof(float));
        h->micQ.erase(h->micQ.begin(), h->micQ.begin() + kHop);
        h->refQ.erase(h->refQ.begin(), h->refQ.begin() + kHop);
        if (!RunHop(h, m, r, e)) {
            h->latched = true;
            h->outQ.clear();
            h->outQ.insert(h->outQ.end(), m, m + kHop);
            h->outQ.insert(h->outQ.end(), h->micQ.begin(), h->micQ.end());
            h->micQ.clear();
            h->refQ.clear();
            break;
        }
        h->outQ.insert(h->outQ.end(), e, e + kHop);
    }
    double so = 0.0;
    for (int i = 0; i < n; i++) {
        float v;
        if (!h->outQ.empty()) {
            v = h->outQ.front();
            h->outQ.erase(h->outQ.begin());
        } else {
            v = mic[i];  // defensive: counts always match, never mute
        }
        out[i] = v;
        so += (double)v * v;
    }
    h->outRms = (float)sqrt(so / n);
    return h->latched ? 0 : 1;
}

void LocalVqeReset(LocalVqeHandle* h) {
    if (!h) return;
    if (h->daf) localvqe_aec_daf_reset(h->daf);
    for (auto& s : h->states) std::fill(s.begin(), s.end(), 0.0f);
    std::fill(h->micHist.begin(), h->micHist.end(), 0.0f);
    std::fill(h->refHist.begin(), h->refHist.end(), 0.0f);
    std::fill(h->overlap.begin(), h->overlap.end(), 0.0f);
    h->micQ.clear();
    h->refQ.clear();
    h->outQ.clear();
    h->latched = false;
    h->neural = false;
    h->inRms = h->refRms = h->outRms = 0.0f;
    h->dropped = 0;
}

void LocalVqeDestroy(LocalVqeHandle* h) {
    if (!h) return;
    if (h->daf) localvqe_aec_daf_destroy(h->daf);
    delete h;
}

const char* LocalVqeLastError(const LocalVqeHandle* h) {
    return (h && !h->error.empty()) ? h->error.c_str() : "";
}

void LocalVqeStatsGet(const LocalVqeHandle* h, LocalVqeStats* s) {
    if (!s) return;
    if (!h) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->ready = h->ready ? 1 : 0;
    s->neural = h->neural ? 1 : 0;
    s->delaySamples = h->daf ? localvqe_aec_daf_current_delay_samples(h->daf)
                             : 0;
    s->delayConf = h->daf ? localvqe_aec_daf_delay_confidence(h->daf) : 0.0f;
    s->inRms = h->inRms;
    s->refRms = h->refRms;
    s->outRms = h->outRms;
    s->dropped = h->dropped;
}

void LocalVqeDump(const LocalVqeHandle* h) {
    if (!h) {
        printf("localvqe: null handle\n");
        return;
    }
    printf("localvqe: ready=%d neural=%d latched=%d\n",
           h->ready ? 1 : 0, h->neural ? 1 : 0, h->latched ? 1 : 0);
    printf("%s", h->ioDump.empty() ? "(no session)\n" : h->ioDump.c_str());
    if (!h->error.empty()) printf("localvqe: note: %s\n", h->error.c_str());
    LocalVqeStats st = {};
    LocalVqeStatsGet(h, &st);
    printf("localvqe: mic=%.0f ref=%.0f out=%.0f delay=%d conf=%.1f dropped=%d\n",
           (double)st.inRms * 32768.0, (double)st.refRms * 32768.0,
           (double)st.outRms * 32768.0, st.delaySamples,
           (double)st.delayConf, st.dropped);
}
