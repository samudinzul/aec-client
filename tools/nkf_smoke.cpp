// nkf_smoke — offline harness for the desktop NKF-AEC output path.
//
// Runs WAV (or synthetic) mic/ref pairs through the exact pump the
// GUI runs (NkfProcess -> DTLN-NS) and prints a per-second table with
// the engine telemetry that pinpoints output suppression: TDC lock
// state, guard resets, loop flag, and — most importantly — the
// tracking-notch state (freq + broadband effect).
//
// Usage (build first, from the repo root on the Windows dev machine):
//   cmake -B build -G Ninja && cmake --build build --target nkf_smoke
//   build/nkf_smoke.exe mic.wav ref.wav out.wav [--no-ns] [--hb48 out48.wav]
//   build/nkf_smoke.exe --synth 15 out.wav [--no-ns] [--hb48 out48.wav]
//
// --hb48 writes a 48 kHz version of the 16 kHz output with synthesized
// 8-16 kHz air (offline harmonic highband — mirrors highband_extend_
// 16k_to_48k in web/dsp.py). A/B proof before any live dual-rate
// plumbing: the 16 kHz default output is byte-identical with or without
// the flag.
//
// WAVs must be 16 kHz mono int16. --synth generates a loud sustained
// speaker-like tone as ref (echoed into mic with delay) plus periodic
// voice bursts, i.e. the "loud speakers then suppression?" scenario.
//
// Reading the table: the `notchHz`/`notchDb` columns show the tracking
// notch (0 Hz = bypassed). If outRMS collapses while a notch is engaged,
// the notch is your suppressor. If outRMS collapses with no notch and
// `lock=0`, the TDC never locked (check `lag`). `giveUp=1`
// means fail-open mic passthrough (loud, not suppressed).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dtln_ns_wrapper.h"
#include "nkf_wrapper.h"

namespace {

constexpr int kSr = 16000;
constexpr int kFrame = 160;  // pump frame, mirrors frameSizeForRate(16000)

struct Wav {
    std::vector<int16_t> samples;
};

bool ReadWav16Mono(const char* path, Wav& w) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return false; }
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { printf("%s: not a WAV\n", path); fclose(f); return false; }
    const int sr = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    const int ch = h[22] | (h[23] << 8);
    const int bits = h[34] | (h[35] << 8);
    if (sr != kSr || ch != 1 || bits != 16) {
        printf("%s: need 16 kHz mono int16 (got %d Hz, %d ch, %d bit)\n",
               path, sr, ch, bits);
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 44, SEEK_SET);
    const size_t n = (size_t)(bytes - 44) / 2;
    w.samples.resize(n);
    const bool ok = fread(w.samples.data(), 2, n, f) == n;
    fclose(f);
    if (!ok) printf("%s: short read\n", path);
    return ok;
}

bool WriteWavRate(const char* path, const std::vector<int16_t>& s, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return false; }
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4);
    const uint32_t dataBytes = (uint32_t)s.size() * 2;
    const uint32_t riffSize = 36 + dataBytes;
    memcpy(h + 4, &riffSize, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmtSize = 16;
    memcpy(h + 16, &fmtSize, 4);
    const uint16_t audioFmt = 1, channels = 1;
    memcpy(h + 20, &audioFmt, 2);
    memcpy(h + 22, &channels, 2);
    const uint32_t sr = (uint32_t)rate;
    memcpy(h + 24, &sr, 4);
    const uint32_t byteRate = (uint32_t)rate * 2;
    memcpy(h + 28, &byteRate, 4);
    const uint16_t blockAlign = 2, bits = 16;
    memcpy(h + 32, &blockAlign, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dataBytes, 4);
    fwrite(h, 1, 44, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
    return true;
}

bool WriteWav16Mono(const char* path, const std::vector<int16_t>& s) {
    return WriteWavRate(path, s, kSr);
}

static inline int16_t ClampS16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Offline harmonic highband: 16 kHz int16 -> 48 kHz int16 with
// synthesized 8-16 kHz air. Sample-for-sample mirror of
// highband_extend_16k_to_48k in web/dsp.py (float math, same order:
// envelope gate, pre-emphasis HP -> cosine mirror modulator -> post
// highpass -> mix; silence in -> silence out). Harness/A-B use only — the live
// cable path stays pure interpolation until this proves itself.
// Pinned by web/test_parity.py --constants (HB_*).
static const float HB_MIX_DB = -9.0f;
static const float HB_HP_FC = 8000.0f;
static const float HB_GATE_RMS = 100.0f;  // int16 domain, like EXPAND_CLOSE

static void LinearUpsample16to48(const float* x, int n, std::vector<float>& base) {
    base.assign((size_t)n * 3, 0.0f);
    if (n <= 0) return;
    if (n == 1) { base[0] = base[1] = base[2] = x[0]; return; }
    for (int j = 0; j < n * 3; j++) {
        const float pos = (float)j * (float)(n - 1) / (float)(n * 3 - 1);
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        if (i0 > n - 2) i0 = n - 2;
        const float frac = pos - (float)i0;
        base[(size_t)j] = x[i0] * (1.0f - frac) + x[i0 + 1] * frac;
    }
}

// Whole-vector offline extender (the smoke harness is file in/out, so
// no chunk-state claims — the live streaming variant comes later).
static void HighbandExtend(const std::vector<int16_t>& in16,
                           std::vector<int16_t>& out48) {
    const int n = (int)in16.size();
    out48.assign((size_t)n * 3, 0);
    if (n <= 0) return;
    std::vector<float> x((size_t)n);
    bool any = false;
    for (int i = 0; i < n; i++) {
        x[(size_t)i] = (float)in16[(size_t)i] / 32768.0f;
        if (in16[(size_t)i] != 0) any = true;
    }
    std::vector<float> base;
    LinearUpsample16to48(x.data(), n, base);
    if (!any) {  // digital silence: bit-identical pure path
        for (size_t j = 0; j < base.size(); j++) {
            int v = (int)lrintf(base[j] * 32768.0f);
            out48[j] = ClampS16(v);
        }
        return;
    }
    const float gate = HB_GATE_RMS / 32768.0f;
    const float mix = powf(10.0f, HB_MIX_DB / 20.0f);
    const float ap = 1.0f - expf(-2.0f * 3.14159265f * (HB_HP_FC / 4.0f) / 48000.0f);
    const float a = 1.0f - expf(-2.0f * 3.14159265f * HB_HP_FC / 48000.0f);
    const float upK = 1.0f - expf(-1.0f / (0.005f * 48000.0f));
    const float dnK = 1.0f - expf(-1.0f / (0.040f * 48000.0f));
    const float step = 2.0f * 3.14159265f * HB_HP_FC / 48000.0f;
    const float span = 0.1f - gate;
    float env = 0.0f, lpp = 0.0f, lp = 0.0f, phase = 0.0f;
    for (size_t j = 0; j < base.size(); j++) {
        const float b = base[j];
        const float ab = fabsf(b);
        const float k = (ab > env) ? upK : dnK;
        env += (ab - env) * k;
        float g = (span > 0.0f) ? (env - gate) / span : 0.0f;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        lpp += ap * (b - lpp);
        const float pre = b - lpp;
        const float m = pre * cosf(phase);
        phase += step;
        if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        lp += a * (m - lp);
        const float o = b + (m - lp) * g * mix;
        out48[j] = ClampS16((int)lrintf(o * 32768.0f));
    }
}

// Highband RMS of a 48 kHz int16 vector (same 1-pole highpass family,
// table use only — shows whether the top octave is real or empty).
static double HighRms48(const std::vector<int16_t>& s) {
    const float a = 1.0f - expf(-2.0f * 3.14159265f * 8000.0f / 48000.0f);
    float lp = 0.0f;
    double acc = 0.0;
    for (size_t j = 0; j < s.size(); j++) {
        const float v = (float)s[j];
        lp += a * (v - lp);
        const float h = v - lp;
        acc += (double)h * h;
    }
    return sqrt(acc / (s.size() ? (double)s.size() : 1.0));
}

// Synthetic "loud speakers" scenario: sustained loud tone-like ref
// (echoed into mic with delay) + periodic voice bursts in the mic.
void Synth(double seconds, Wav& mic, Wav& ref) {
    const size_t n = (size_t)(seconds * kSr);
    mic.samples.assign(n, 0);
    ref.samples.assign(n, 0);
    const int lag = 2048;  // simulated speaker->mic delay
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / kSr;
        // Loud sustained "speaker" content: strong tonal components.
        const double r = 9000.0 * sin(2.0 * 3.14159265 * 990.0 * t)
                       + 4000.0 * sin(2.0 * 3.14159265 * 1980.0 * t + 1.0)
                       + 1500.0 * sin(2.0 * 3.14159265 * 440.0 * t + 2.0);
        ref.samples[i] = ClampS16((int)lrint(r));
        // Echo of it arriving at the mic, plus voice bursts 1 s on / 1 s off.
        double m = 0.0;
        if (i >= (size_t)lag) m += 0.35 * ref.samples[i - lag];
        const double sec = t - floor(t / 2.0) * 2.0;
        if (sec < 1.0) {
            const double env = sin(3.14159265 * sec);  // syllabic-ish swell
            m += env * (2500.0 * sin(2.0 * 3.14159265 * 220.0 * t)
                      + 1200.0 * sin(2.0 * 3.14159265 * 330.0 * t + 0.7));
        }
        mic.samples[i] = ClampS16((int)lrint(m));
    }
}

double Rms(const int16_t* p, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)p[i] * p[i];
    return sqrt(s / (n ? n : 1));
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    double synthSecs = 15.0;
    const char *micPath = nullptr, *refPath = nullptr, *outPath = nullptr;
    bool useNs = true;
    const char* hb48Path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 2 < argc) {
            synth = true;
            synthSecs = atof(argv[++i]);
            outPath = argv[++i];
        } else if (!strcmp(argv[i], "--no-ns")) {
            useNs = false;
        } else if (!strcmp(argv[i], "--hb48") && i + 1 < argc) {
            hb48Path = argv[++i];
        } else if (!micPath) {
            micPath = argv[i];
        } else if (!refPath) {
            refPath = argv[i];
        } else if (!outPath) {
            outPath = argv[i];
        }
    }

    Wav mic, ref;
    if (synth) {
        Synth(synthSecs, mic, ref);
        printf("synth: %.1f s loud-tonal ref + voice bursts\n", synthSecs);
    } else {
        if (!micPath || !refPath || !outPath) {
            printf("usage: nkf_smoke mic.wav ref.wav out.wav [--no-ns] [--hb48 out48.wav]\n"
                   "   or: nkf_smoke --synth SECS out.wav [--no-ns] [--hb48 out48.wav]\n");
            return 2;
        }
        if (!ReadWav16Mono(micPath, mic) || !ReadWav16Mono(refPath, ref)) return 2;
    }
    const size_t n = mic.samples.size() < ref.samples.size()
                         ? mic.samples.size()
                         : ref.samples.size();
    if (n < (size_t)kSr) { printf("need >= 1 s of audio\n"); return 2; }

    NkfHandle* nkf = NkfNew("models/nkf.onnx");
    if (!nkf) { printf("NkfNew failed (models/nkf.onnx?)\n"); return 1; }
    DtlnNsHandle* ns = useNs ? DtlnNsNew("models/dtln_ns_128") : nullptr;
    if (useNs && !ns) printf("note: DtlnNsNew failed -> NS off\n");
    std::vector<int16_t> out;
    out.reserve(n);

    int16_t micF[kFrame], refF[kFrame], clF[kFrame];
    float wf[kFrame];
    printf("sec  micRMS  engRMS  outRMS  lag  lock conf exp loop rst give notchHz notchDb loopDb resDb\n");
    double accMic = 0, accEng = 0, accOut = 0;
    size_t accN = 0;
    int sec = 0;
    for (size_t pos = 0; pos + kFrame <= n; pos += kFrame) {
        memcpy(micF, mic.samples.data() + pos, sizeof(micF));
        memcpy(refF, ref.samples.data() + pos, sizeof(refF));
        double em = 0;
        for (int i = 0; i < kFrame; i++) em += (double)micF[i] * micF[i];
        accMic += em;
        NkfProcess(nkf, micF, refF, clF, kFrame);
        double e = 0;
        for (int i = 0; i < kFrame; i++) e += (double)clF[i] * clF[i];
        accEng += e;
        if (ns) {
            for (int i = 0; i < kFrame; i++) wf[i] = (float)clF[i] / 32768.0f;
            if (DtlnNsProcess(ns, wf, wf, kFrame) == 1) {
                for (int i = 0; i < kFrame; i++) {
                    int v = (int)lrintf(wf[i] * 32768.0f);
                    clF[i] = ClampS16(v);
                }
            }
        }
        out.insert(out.end(), clF, clF + kFrame);
        double eo = 0;
        for (int i = 0; i < kFrame; i++) eo += (double)clF[i] * clF[i];
        accOut += eo;
        accN += kFrame;
        if (accN >= (size_t)kSr) {
            NkfState st = {};
            NkfGetState(nkf, &st);
            printf("%3d  %6.0f  %6.0f  %6.0f  %5d  %d    %d    %d   %d    %d   %d    %7.0f %+.1f %+.1f %+.1f\n",
                   ++sec, sqrt(accMic / accN), sqrt(accEng / accN),
                   sqrt(accOut / accN), st.lagSamples,
                   st.locked, st.confident, st.exposed, st.loopActive,
                   st.guardResets, st.giveUp, st.notchFreq,
                   st.notchDb, st.loopDb, st.resDb);
            accMic = accEng = accOut = 0;
            accN = 0;
        }
    }
    if (!WriteWav16Mono(outPath, out)) return 1;
    printf("wrote %s (%zu samples)\n", outPath, out.size());
    if (hb48Path) {
        // A/B proof: pure linear 48 kHz (what the cable gets today —
        // honest but empty up top) vs the highband extender on the
        // same 16 kHz output. Compare the hiRMS columns + listen.
        std::vector<float> xf(out.size());
        for (size_t i = 0; i < out.size(); i++)
            xf[i] = (float)out[i] / 32768.0f;
        std::vector<float> pure;
        LinearUpsample16to48(xf.data(), (int)out.size(), pure);
        std::vector<int16_t> pure48(pure.size());
        for (size_t j = 0; j < pure.size(); j++)
            pure48[j] = ClampS16((int)lrintf(pure[j] * 32768.0f));
        std::vector<int16_t> hb48;
        HighbandExtend(out, hb48);
        double peak = 0.0;
        for (size_t j = 0; j < hb48.size(); j++) {
            const double v = fabs((double)hb48[j]);
            if (v > peak) peak = v;
        }
        printf("hb48: pure hiRMS %6.0f | synth hiRMS %6.0f | fullRMS %6.0f | peak %6.0f\n",
               HighRms48(pure48), HighRms48(hb48), Rms(hb48.data(), (int)hb48.size()), peak);
        if (!WriteWavRate(hb48Path, hb48, 48000)) return 1;
        printf("wrote %s (%zu samples @48k)\n", hb48Path, hb48.size());
    }
    NkfState st = {};
    NkfGetState(nkf, &st);
    if (st.notchFreq > 0.0f)
        printf("verdict: notch engaged at %.0f Hz (%+.1f dB) — unexpected on synth\n",
               (double)st.notchFreq, (double)st.notchDb);
    else if (st.giveUp)
        printf("verdict: engine gave up -> fail-open mic passthrough\n");
    else if (!st.locked)
        printf("verdict: TDC never locked (lag %d) -> check ref signal\n",
               st.lagSamples);
    else
        printf("verdict: engine engaged, no trim (output = cancelled signal)\n");
    NkfDestroy(nkf);
    if (ns) DtlnNsDestroy(ns);
    return 0;
}
