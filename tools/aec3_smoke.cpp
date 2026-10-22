// aec3_smoke — offline harness for the desktop WebRTC AEC3 output path.
//
// Runs WAV mic/ref pairs through the exact pump the GUI runs
// (Aec3CancelEcho, 160-sample frames @ 16 kHz) and prints a per-second
// mic/engine RMS table. Engine-only (no NS stage): compare against
// the web side with `python -m web.test_parity --run mic.wav ref.wav
// <this out.wav> --engine aec3`. 16 kHz only (the web pump is 16 kHz;
// the GUI's 48 kHz AEC3 mode is not covered here).
//
// Usage (build first, from the repo root on the Windows dev machine):
//   cmake --build build --target aec3_smoke
//   build/aec3_smoke.exe mic.wav ref.wav out.wav
//
// WAVs must be 16 kHz mono int16.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aec3_wrapper.h"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("usage: aec3_smoke mic.wav ref.wav out.wav\n");
        return 2;
    }
    Wav mic, ref;
    if (!ReadWav16Mono(argv[1], mic) || !ReadWav16Mono(argv[2], ref)) return 2;
    const size_t n = mic.samples.size() < ref.samples.size()
                         ? mic.samples.size()
                         : ref.samples.size();
    if (n < (size_t)kSr) { printf("need >= 1 s of audio\n"); return 2; }

    Aec3Handle* aec3 = Aec3New(kSr, kFrame);
    if (!aec3) { printf("Aec3New failed\n"); return 1; }
    std::vector<int16_t> out;
    out.reserve(n);

    int16_t micF[kFrame], refF[kFrame], clF[kFrame];
    printf("sec  micRMS  engRMS\n");
    double accMic = 0, accEng = 0;
    size_t accN = 0;
    int sec = 0;
    for (size_t pos = 0; pos + kFrame <= n; pos += kFrame) {
        memcpy(micF, mic.samples.data() + pos, sizeof(micF));
        memcpy(refF, ref.samples.data() + pos, sizeof(refF));
        double em = 0;
        for (int i = 0; i < kFrame; i++) em += (double)micF[i] * micF[i];
        accMic += em;
        Aec3CancelEcho(aec3, micF, refF, clF, kFrame);
        double e = 0;
        for (int i = 0; i < kFrame; i++) e += (double)clF[i] * clF[i];
        accEng += e;
        out.insert(out.end(), clF, clF + kFrame);
        accN += kFrame;
        if (accN >= (size_t)kSr) {
            printf("%3d  %6.0f  %6.0f\n",
                   ++sec, sqrt(accMic / accN), sqrt(accEng / accN));
            accMic = accEng = 0;
            accN = 0;
        }
    }
    if (!WriteWav16Mono(argv[3], out)) return 1;
    printf("wrote %s (%zu samples)\n", argv[3], out.size());
    Aec3Destroy(aec3);
    return 0;
}
