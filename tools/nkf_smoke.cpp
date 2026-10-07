// nkf_smoke — offline harness for the desktop NKF-AEC output path.
//
// Runs WAV (or synthetic) mic/ref pairs through the exact pump the
// GUI runs (NkfProcess -> speech gate -> WPE -> notch) and prints a
// per-second table with the engine telemetry that pinpoints output
// suppression: TDC lock state, guard resets, loop flag, and — most
// importantly — the howl-backstop trim in dB.
//
// Usage (build first, from the repo root on the Windows dev machine):
//   cmake -B build -G Ninja && cmake --build build --target nkf_smoke
//   build/nkf_smoke.exe mic.wav ref.wav out.wav [--no-wpe] [--no-notch]
//   build/nkf_smoke.exe --synth 15 out.wav [--no-wpe] [--no-notch]
//
// WAVs must be 16 kHz mono int16. --synth generates a loud sustained
// speaker-like tone as ref (echoed into mic with delay) plus periodic
// voice bursts, i.e. the "loud speakers then suppression?" scenario.
//
// Reading the table: the `bsDb` column is the backstop output trim
// (0 = full level). If outRMS collapses while bsDb dives negative,
// the backstop is your suppressor. If outRMS collapses with bsDb at
// 0 and `lock=0`, the TDC never locked (check `lag`). `giveUp=1`
// means fail-open mic passthrough (loud, not suppressed).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nkf_wrapper.h"
#include "notch.h"
#include "speech_gate.h"
#include "wpe.h"

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

bool WriteWav16Mono(const char* path, const std::vector<int16_t>& s) {
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
    const uint32_t sr = kSr;
    memcpy(h + 24, &sr, 4);
    const uint32_t byteRate = kSr * 2;
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

static inline int16_t ClampS16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
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
    bool useWpe = true, useNotch = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--synth") && i + 2 < argc) {
            synth = true;
            synthSecs = atof(argv[++i]);
            outPath = argv[++i];
        } else if (!strcmp(argv[i], "--no-wpe")) {
            useWpe = false;
        } else if (!strcmp(argv[i], "--no-notch")) {
            useNotch = false;
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
            printf("usage: nkf_smoke mic.wav ref.wav out.wav [--no-wpe] [--no-notch]\n"
                   "   or: nkf_smoke --synth SECS out.wav [--no-wpe] [--no-notch]\n");
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
    WpeHandle* wpe = useWpe ? WpeNew(kSr) : nullptr;
    NotchHandle* notch = useNotch ? NotchNew(kSr) : nullptr;
    if (useWpe && !wpe) printf("note: WpeNew failed -> WPE off\n");
    if (useNotch && !notch) printf("note: NotchNew failed -> notch off\n");
    SpeechGate gate = {};
    std::vector<int16_t> out;
    out.reserve(n);

    int16_t micF[kFrame], refF[kFrame], clF[kFrame];
    float wf[kFrame];
    printf("sec  micRMS  outRMS  lag  lock conf exp loop rst give bsDb\n");
    double accMic = 0, accOut = 0;
    size_t accN = 0;
    int sec = 0;
    for (size_t pos = 0; pos + kFrame <= n; pos += kFrame) {
        memcpy(micF, mic.samples.data() + pos, sizeof(micF));
        memcpy(refF, ref.samples.data() + pos, sizeof(refF));
        NkfProcess(nkf, micF, refF, clF, kFrame);
        // Gate on the engine output, exactly like the GUI pump.
        double e = 0;
        for (int i = 0; i < kFrame; i++) e += (double)clF[i] * clF[i];
        SpeechGateUpdate(&gate, (float)sqrt(e / kFrame), 10.0f);
        if (wpe) {
            WpeSetSpeech(wpe, SpeechGateForWpe(&gate));
            for (int i = 0; i < kFrame; i++) wf[i] = (float)clF[i] / 32768.0f;
            WpeProcess(wpe, wf, wf, kFrame);
            for (int i = 0; i < kFrame; i++) {
                int v = (int)lrintf(wf[i] * 32768.0f);
                clF[i] = ClampS16(v);
            }
        }
        if (notch) {
            NotchSetSpeech(notch, SpeechGateForNotch(&gate));
            NotchProcess(notch, clF, kFrame);
        }
        out.insert(out.end(), clF, clF + kFrame);
        accMic += e;
        double eo = 0;
        for (int i = 0; i < kFrame; i++) eo += (double)clF[i] * clF[i];
        accOut += eo;
        accN += kFrame;
        if (accN >= (size_t)kSr) {
            NkfState st = {};
            NkfGetState(nkf, &st);
            printf("%3d  %6.0f  %6.0f  %4d  %d    %d    %d   %d    %d   %d    %+.1f\n",
                   ++sec, sqrt(accMic / accN), sqrt(accOut / accN), st.lagSamples,
                   st.locked, st.confident, st.exposed, st.loopActive,
                   st.guardResets, st.giveUp, st.backstopDb);
            accMic = accOut = 0;
            accN = 0;
        }
    }
    if (!WriteWav16Mono(outPath, out)) return 1;
    printf("wrote %s (%zu samples)\n", outPath, out.size());
    NkfState st = {};
    NkfGetState(nkf, &st);
    if (st.backstopDb < -1.0)
        printf("verdict: backstop trimmed output to %+.1f dB (tonal wire?)\n",
               st.backstopDb);
    else if (st.giveUp)
        printf("verdict: engine gave up -> fail-open mic passthrough\n");
    else if (!st.locked)
        printf("verdict: TDC never locked (lag %d) -> check ref signal\n",
               st.lagSamples);
    else
        printf("verdict: engine engaged, no trim (output = cancelled signal)\n");
    NkfDestroy(nkf);
    return 0;
}
