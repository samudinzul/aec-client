#pragma once
#include <cstdio>
#include <cstdarg>

// Shared transition/diagnostic event log: appends to nkf-phase.log in
// the working directory (same file the NKF wrapper has always used).
// One fopen/fprintf/fclose per event — audio-thread safe at event rate
// only; never call per block or per sample.
inline void AecPhase(const char* fmt, ...) {
    FILE* f = fopen("nkf-phase.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
