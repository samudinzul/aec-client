#include "gtcrn_wrapper.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"

// GTCRN streaming inference (V1): sqrt-Hann STFT(512)/256-hop outside the
// graph, exactly like the reference (k2-fsa/sherpa-onnx
// offline-speech-denoiser-gtcrn-impl.h: hann_sqrt window, n_fft 512,
// hop 256, mix[1,257,1,2] + 3 zero-init caches in, enh + updated caches
// out, ISTFT overlap-add). FFT is a local radix-2 (torch-normalized:
// raw forward, 1/N inverse) so there is no new dependency.

namespace {

constexpr int kN = 512;
constexpr int kHop = 256;
constexpr int kBins = kN / 2 + 1;  // 257
constexpr double kPi = 3.14159265358979323846;

void Fft512(double* re, double* im, bool inverse) {
    for (int i = 1, j = 0; i < kN; i++) {
        int bit = kN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= kN; len <<= 1) {
        const double ang = 2.0 * kPi / len * (inverse ? 1.0 : -1.0);
        const double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < kN; i += len) {
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
        for (int i = 0; i < kN; i++) { re[i] /= kN; im[i] /= kN; }
}

long NumEl(const std::vector<int64_t>& s) {
    long n = 1;
    for (int64_t d : s) {
        if (d <= 0) return -1;
        n *= (long)d;
    }
    return n;
}

}  // namespace

enum SlotKind { SLOT_MIX, SLOT_CONV, SLOT_TRA, SLOT_INTER, SLOT_UNKNOWN };

struct GtcrnHandle {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeCPU);
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    std::vector<const char*> inPtr;
    std::vector<const char*> outPtr;
    std::vector<SlotKind> inSlot;   // session input order
    std::vector<SlotKind> outSlot;  // session output order (MIX=enh)
    std::vector<int64_t> mixShape = {1, kBins, 1, 2};
    std::vector<int64_t> convShape, traShape, interShape;
    std::vector<float> mixBuf = std::vector<float>(kBins * 2);
    std::vector<float> enhBuf = std::vector<float>(kBins * 2);
    std::vector<float> convBuf, traBuf, interBuf;
    std::vector<float> win = std::vector<float>(kN);  // sqrt-Hann
    std::vector<float> winBuf = std::vector<float>(kN, 0.0f);
    std::vector<float> olaBuf = std::vector<float>(kN, 0.0f);
    std::vector<float> inQ, outQ;
    std::string ioDump;
    std::string error;
    bool ready = false;
    bool neural = false;   // a Run() has produced audio
    bool latched = false;  // Run failed -> fail-open passthrough
    float inRms = 0.0f, outRms = 0.0f;
    int dropped = 0;
};

static std::string ShapeStr(const std::vector<int64_t>& s) {
    std::string r = "[";
    for (size_t i = 0; i < s.size(); i++) {
        if (i) r += ",";
        r += std::to_string(s[i]);
    }
    return r + "]";
}

static SlotKind KindOf(const std::string& name,
                       const std::vector<int64_t>& shape, bool isInput) {
    std::string lo = name;
    for (char& c : lo) c = (char)tolower(c);
    if (lo.find("conv") != std::string::npos) return SLOT_CONV;
    if (lo.find("tra") != std::string::npos) return SLOT_TRA;
    if (lo.find("inter") != std::string::npos) return SLOT_INTER;
    if (lo.find("mix") != std::string::npos ||
        lo.find("enh") != std::string::npos)
        return SLOT_MIX;
    if (isInput)
        for (int64_t d : shape)
            if (d == kBins) return SLOT_MIX;
    return SLOT_UNKNOWN;
}

GtcrnHandle* GtcrnNew(const char* modelPath) {
    GtcrnHandle* h = new GtcrnHandle();
    h->inQ.reserve(16384);
    h->outQ.reserve(16384);
    for (int i = 0; i < kN; i++)
        h->win[i] = (float)sin(kPi * i / kN);  // sqrt-Hann (periodic)
    if (!modelPath || !*modelPath) {
        h->error = "gtcrn: no model path (run get_models.sh)";
        return h;
    }
    try {
        h->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                            "gtcrn-exp");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Windows ONNX Runtime requires wide-string paths (same
        // conversion as third_party/REAL_TIME_NKF_AEC/c/NKFImpl.h).
        std::wstring widePath(modelPath, modelPath + strlen(modelPath));
        h->session =
            std::make_unique<Ort::Session>(*h->env, widePath.c_str(), opts);
        Ort::AllocatorWithDefaultOptions alloc;
        const size_t ni = h->session->GetInputCount();
        const size_t no = h->session->GetOutputCount();
        std::vector<std::vector<int64_t>> inShapes, outShapes;
        for (size_t i = 0; i < ni; i++) {
            Ort::AllocatedStringPtr nm =
                h->session->GetInputNameAllocated(i, alloc);
            h->inNames.emplace_back(nm.get());
            Ort::TypeInfo ti = h->session->GetInputTypeInfo(i);
            inShapes.push_back(ti.GetTensorTypeAndShapeInfo().GetShape());
            h->inSlot.push_back(
                KindOf(h->inNames.back(), inShapes.back(), true));
        }
        for (size_t i = 0; i < no; i++) {
            Ort::AllocatedStringPtr nm =
                h->session->GetOutputNameAllocated(i, alloc);
            h->outNames.emplace_back(nm.get());
            Ort::TypeInfo ti = h->session->GetOutputTypeInfo(i);
            outShapes.push_back(ti.GetTensorTypeAndShapeInfo().GetShape());
            h->outSlot.push_back(
                KindOf(h->outNames.back(), outShapes.back(), false));
        }
        for (size_t i = 0; i < ni; i++) h->inPtr.push_back(h->inNames[i].c_str());
        for (size_t i = 0; i < no; i++) h->outPtr.push_back(h->outNames[i].c_str());
        std::string dump;
        for (size_t i = 0; i < ni; i++)
            dump += "IN " + h->inNames[i] + " " + ShapeStr(inShapes[i]) + "\n";
        for (size_t i = 0; i < no; i++)
            dump += "OUT " + h->outNames[i] + " " + ShapeStr(outShapes[i]) + "\n";
        h->ioDump = dump;
        // Expect exactly: 1 mix + conv + tra + inter.
        int nMix = 0, nCache = 0;
        for (SlotKind k : h->inSlot)
            if (k == SLOT_MIX) nMix++;
            else if (k != SLOT_UNKNOWN) nCache++;
        if (ni != 4 || no != 4 || nMix != 1 || nCache != 3) {
            h->error = "gtcrn: unexpected graph (need mix+3 caches, "
                       "4 outs) — see GtcrnDump. Staying off.";
            return h;
        }
        for (size_t i = 0; i < ni; i++) {
            long n = NumEl(inShapes[i]);
            if (n < 0) {
                h->error = "gtcrn: symbolic dims unsupported. Staying off.";
                return h;
            }
            if (h->inSlot[i] == SLOT_CONV) {
                h->convBuf.assign((size_t)n, 0.0f);
                h->convShape = inShapes[i];
            }
            if (h->inSlot[i] == SLOT_TRA) {
                h->traBuf.assign((size_t)n, 0.0f);
                h->traShape = inShapes[i];
            }
            if (h->inSlot[i] == SLOT_INTER) {
                h->interBuf.assign((size_t)n, 0.0f);
                h->interShape = inShapes[i];
            }
        }
        h->ready = true;
    } catch (const Ort::Exception& e) {
        h->error = std::string("gtcrn: ORT load failed: ") + e.what();
    } catch (const std::exception& e) {
        h->error = std::string("gtcrn: load failed: ") + e.what();
    }
    return h;
}

int GtcrnReady(const GtcrnHandle* h) { return (h && h->ready) ? 1 : 0; }

// One 256-sample hop: window -> STFT -> Run -> ISTFT -> overlap-add.
// Returns false on ORT failure (caller latches to passthrough).
static bool RunHop(GtcrnHandle* h, const float* hop256, float* emit256) {
    memmove(h->winBuf.data(), h->winBuf.data() + kHop,
            (kN - kHop) * sizeof(float));
    memcpy(h->winBuf.data() + kN - kHop, hop256, kHop * sizeof(float));
    double re[kN], im[kN];
    for (int i = 0; i < kN; i++) {
        re[i] = (double)h->winBuf[i] * h->win[i];
        im[i] = 0.0;
    }
    Fft512(re, im, false);
    for (int k = 0; k < kBins; k++) {
        h->mixBuf[2 * k] = (float)re[k];
        h->mixBuf[2 * k + 1] = (float)im[k];
    }
    const float* enh = nullptr;
    try {
        std::vector<Ort::Value> inputs;
        inputs.reserve(h->inPtr.size());
        for (size_t i = 0; i < h->inPtr.size(); i++) {
            float* data = h->mixBuf.data();
            size_t count = h->mixBuf.size();
            std::vector<int64_t>* shape = &h->mixShape;
            if (h->inSlot[i] == SLOT_CONV) {
                data = h->convBuf.data();
                count = h->convBuf.size();
                shape = &h->convShape;
            } else if (h->inSlot[i] == SLOT_TRA) {
                data = h->traBuf.data();
                count = h->traBuf.size();
                shape = &h->traShape;
            } else if (h->inSlot[i] == SLOT_INTER) {
                data = h->interBuf.data();
                count = h->interBuf.size();
                shape = &h->interShape;
            }
            inputs.push_back(Ort::Value::CreateTensor<float>(
                h->memInfo, data, count, shape->data(), shape->size()));
        }
        std::vector<Ort::Value> outs = h->session->Run(
            Ort::RunOptions{nullptr}, h->inPtr.data(), inputs.data(),
            inputs.size(), h->outPtr.data(), h->outPtr.size());
        int enhIdx = 0;
        for (size_t k = 0; k < h->outSlot.size(); k++)
            if (h->outSlot[k] == SLOT_MIX) { enhIdx = (int)k; break; }
        // Copy out NOW: outs dies at the end of this block.
        memcpy(h->enhBuf.data(), outs[(size_t)enhIdx].GetTensorData<float>(),
               h->enhBuf.size() * sizeof(float));
        enh = h->enhBuf.data();
        // Cache carry: outputs back into the next hop's inputs.
        // Copy is bounded by BOTH sides (never overread a short out).
        for (size_t k = 0; k < outs.size(); k++) {
            if ((int)k == enhIdx) continue;
            const size_t got =
                outs[k].GetTensorTypeAndShapeInfo().GetElementCount();
            const float* d = outs[k].GetTensorData<float>();
            std::vector<float>* dst = nullptr;
            if (h->outSlot[k] == SLOT_CONV) dst = &h->convBuf;
            if (h->outSlot[k] == SLOT_TRA) dst = &h->traBuf;
            if (h->outSlot[k] == SLOT_INTER) dst = &h->interBuf;
            if (dst && !dst->empty() && got > 0) {
                size_t cpy = dst->size() < got ? dst->size() : got;
                memcpy(dst->data(), d, cpy * sizeof(float));
                if (cpy != dst->size() || cpy != got) {
                    h->error = "gtcrn: cache shape drift — latched off";
                    return false;
                }
            }
        }
        h->neural = true;
    } catch (const std::exception& e) {
        h->error = std::string("gtcrn: Run failed (latched to "
                               "passthrough): ") +
                   e.what();
        return false;
    }
    double re2[kN], im2[kN];
    for (int k = 0; k < kBins; k++) {
        re2[k] = enh[2 * k];
        im2[k] = enh[2 * k + 1];
    }
    for (int k = kBins; k < kN; k++) {
        re2[k] = re2[kN - k];
        im2[k] = -im2[kN - k];
    }
    Fft512(re2, im2, true);
    for (int i = 0; i < kN; i++)
        h->olaBuf[(size_t)i] += (float)(re2[i] * h->win[i]);
    memcpy(emit256, h->olaBuf.data(), kHop * sizeof(float));
    memmove(h->olaBuf.data(), h->olaBuf.data() + kHop,
            (kN - kHop) * sizeof(float));
    memset(h->olaBuf.data() + kN - kHop, 0, kHop * sizeof(float));
    return true;
}

int GtcrnProcess(GtcrnHandle* h, const float* in, float* out, int n) {
    if (!h || n <= 0) return 0;
    if (!in || !out) return 0;
    if (!h->ready || h->latched) {
        // Fail-open passthrough (never mute).
        double si = 0.0;
        for (int i = 0; i < n; i++) {
            out[i] = in[i];
            si += (double)in[i] * in[i];
        }
        h->inRms = h->outRms = (float)sqrt(si / (n ? n : 1));
        return 0;
    }
    double si = 0.0;
    for (int i = 0; i < n; i++) si += (double)in[i] * in[i];
    h->inRms = (float)sqrt(si / n);
    if (h->inQ.size() > 32768) {  // stall guard: drop oldest, count it
        size_t drop = h->inQ.size() - 32768;
        h->inQ.erase(h->inQ.begin(), h->inQ.begin() + (long)drop);
        h->dropped += (int)drop;
    }
    h->inQ.insert(h->inQ.end(), in, in + n);
    float hop[kHop], emit[kHop];
    while (h->inQ.size() >= (size_t)kHop) {
        memcpy(hop, h->inQ.data(), kHop * sizeof(float));
        h->inQ.erase(h->inQ.begin(), h->inQ.begin() + kHop);
        if (!RunHop(h, hop, emit)) {
            // Latch: fail open. Everything buffered goes straight out.
            h->latched = true;
            h->outQ.clear();
            h->outQ.insert(h->outQ.end(), hop, hop + kHop);
            h->outQ.insert(h->outQ.end(), h->inQ.begin(), h->inQ.end());
            h->inQ.clear();
            break;
        }
        h->outQ.insert(h->outQ.end(), emit, emit + kHop);
    }
    double so = 0.0;
    for (int i = 0; i < n; i++) {
        float v;
        if (!h->outQ.empty()) {
            v = h->outQ.front();
            h->outQ.erase(h->outQ.begin());
        } else {
            v = in[i];  // defensive: counts always match, never mute
        }
        out[i] = v;
        so += (double)v * v;
    }
    h->outRms = (float)sqrt(so / n);
    return h->latched ? 0 : 1;
}

void GtcrnReset(GtcrnHandle* h) {
    if (!h) return;
    std::fill(h->winBuf.begin(), h->winBuf.end(), 0.0f);
    std::fill(h->olaBuf.begin(), h->olaBuf.end(), 0.0f);
    std::fill(h->convBuf.begin(), h->convBuf.end(), 0.0f);
    std::fill(h->traBuf.begin(), h->traBuf.end(), 0.0f);
    std::fill(h->interBuf.begin(), h->interBuf.end(), 0.0f);
    h->inQ.clear();
    h->outQ.clear();
    h->latched = false;
    h->neural = false;
    h->inRms = h->outRms = 0.0f;
    h->dropped = 0;
}

void GtcrnDestroy(GtcrnHandle* h) { delete h; }

const char* GtcrnLastError(const GtcrnHandle* h) {
    return (h && !h->error.empty()) ? h->error.c_str() : "";
}

void GtcrnStatsGet(const GtcrnHandle* h, GtcrnStats* s) {
    if (!s) return;
    if (!h) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->ready = h->ready ? 1 : 0;
    s->neural = h->neural ? 1 : 0;
    s->inRms = h->inRms;
    s->outRms = h->outRms;
    s->dropped = h->dropped;
}

void GtcrnDump(const GtcrnHandle* h) {
    if (!h) {
        printf("gtcrn: null handle\n");
        return;
    }
    printf("gtcrn: ready=%d neural=%d latched=%d\n",
           h->ready ? 1 : 0, h->neural ? 1 : 0, h->latched ? 1 : 0);
    printf("%s", h->ioDump.empty() ? "(no session)\n" : h->ioDump.c_str());
    if (!h->error.empty()) printf("gtcrn: note: %s\n", h->error.c_str());
    printf("gtcrn: inRms=%.1f outRms=%.1f dropped=%d caches=%zu/%zu/%zu\n",
           (double)h->inRms * 32768.0, (double)h->outRms * 32768.0,
           h->dropped, h->convBuf.size(), h->traBuf.size(),
           h->interBuf.size());
}
