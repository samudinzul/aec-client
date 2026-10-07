#!/usr/bin/env bash
# build.sh — configure + build the desktop app, lean and bloat-free.
#
# Quiet by default: cmake/ninja chatter goes to build/build.log, the
# console shows only progress, failures, and the size audit.
# Pass --verbose (-v) to stream the full tool output live.
#
# Two different kinds of "clean", kept apart on purpose:
#   - CLEAN OUTPUT (always, no flag): builds ONLY the app
#     (aec_gui.exe) — no nkf_smoke, no stray logs left in build/.
#   - FRESH CONFIGURE (--fresh): wipe build/ first, then configure
#     from scratch. Use after CMakeLists changes or when the cache
#     looks stale.
#
# Usage (MSYS2 UCRT64 bash, from the repo root):
#   scripts/build.sh              # build app (MinSizeRel)
#   scripts/build.sh --fresh      # wipe build/, then build app
#   scripts/build.sh --debug      # debuggable build instead of size build
#   scripts/build.sh --with-smoke # also build nkf_smoke.exe (needed by
#                                 # scripts/test.sh --native-only)
#   scripts/build.sh --run        # build, then launch aec_gui.exe
#   scripts/build.sh -v           # verbose: full cmake/ninja output
#
# --run launches build/aec_gui.exe detached (terminal stays free).
# DLLs + models are staged next to the exe by CMake, and the app
# anchors itself to the exe directory, so it runs from build/ as-is.
# Harmless if already running: the app focuses the existing window.
#
# What "lightweight" means here:
#   - MinSizeRel (-Os -DNDEBUG) by default, not Release (-O3)
#   - -ffunction-sections/-fdata-sections + --gc-sections: dead code
#     (e.g. unused imgui widgets, unused miniaudio backends) is
#     dropped instead of linked in
#   - -s: symbols stripped from the binaries
#   - ccache used automatically when installed
#   - NO UPX packing, ever: packed PEs trip the exact AV heuristics
#     (Wacatac.B!ml / susgen) this project exists to dodge
#   - nkf_smoke stays a console app (no -mwindows on it)
#
# After the build it prints a size audit (exe + DLLs + models) so
# bloat is visible immediately. Pair with scripts/make-release.sh
# to ship.
set -euo pipefail

FRESH=0
WITH_SMOKE=0
RUN=0
VERBOSE=0
BUILD_TYPE="MinSizeRel"
for a in "$@"; do
    case "$a" in
        --fresh) FRESH=1 ;;
        --debug) BUILD_TYPE="Debug" ;;
        --with-smoke) WITH_SMOKE=1 ;;
        --run) RUN=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            sed -n '2,42p' "$0"; exit 0 ;;
        *) echo "unknown flag: $a (see --help)" >&2; exit 1 ;;
    esac
done

test -f CMakeLists.txt -a -d src || {
    echo "run from the repo root" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja not found" >&2; exit 1; }

if [ "$FRESH" -ne 0 ]; then
    echo "-- removing build/ (fresh configure)"
    rm -rf build
fi
mkdir -p build

LOG="build/build.log"
# run_step <label> <cmd...>: stream output live under -v, else capture
# to $LOG and print only OK/FAILED (+ tail on failure).
run_step() {
    local label="$1"; shift
    if [ "$VERBOSE" -ne 0 ]; then
        "$@"
        return
    fi
    echo "-- ${label} (log: ${LOG})"
    if "$@" >>"$LOG" 2>&1; then
        echo "   ${label} OK"
    else
        echo "${label} FAILED — last 30 lines of ${LOG}:" >&2
        tail -n 30 "$LOG" >&2 || true
        exit 1
    fi
}

# Size-first flags. Passed on the command line (not baked into
# CMakeLists) so a plain `cmake -B build` still gives a normal build.
CXX_FLAGS="-ffunction-sections -fdata-sections"
LINK_FLAGS="-s -Wl,--gc-sections"
if [ "$BUILD_TYPE" = "Debug" ]; then
    CXX_FLAGS=""
    LINK_FLAGS=""
fi

CC_LAUNCHER=()
if command -v ccache >/dev/null; then
    CC_LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    echo "-- ccache: on"
else
    echo "-- ccache: not installed (pacman -S ccache for faster rebuilds)"
fi

# Fresh log per run (build/ itself is gitignored).
: > "$LOG"

# shellcheck disable=SC2086
run_step "configure (${BUILD_TYPE})" \
    cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LINK_FLAGS}" \
    "${CC_LAUNCHER[@]:-}"

NPROC=$(nproc 2>/dev/null || echo 4)
if [ "$WITH_SMOKE" -ne 0 ]; then
    run_step "build all (-j${NPROC})" \
        cmake --build build -j "$NPROC"
else
    run_step "build aec_gui (-j${NPROC})" \
        cmake --build build -j "$NPROC" --target aec_gui
    # A clean build leaves no diagnostic tools or logs behind.
    rm -f build/nkf_smoke.exe build/nkf_smoke.pdb nkf-phase.log build/nkf-phase.log
fi

echo ""
echo "-- size audit (bloat check)"
if [ "$WITH_SMOKE" -ne 0 ]; then
    ls -la build/aec_gui.exe build/nkf_smoke.exe 2>/dev/null || true
else
    ls -la build/aec_gui.exe 2>/dev/null || true
fi
echo ""
echo "DLLs next to the exe:"
du -h build/*.dll 2>/dev/null || echo "(no DLLs staged)"
echo ""
echo "Models (bundled, unchanged by build flags):"
du -h -c models/nkf.onnx models/dtln_aec_128_*.tflite models/dtln_ns_128_*.tflite 2>/dev/null || true
echo ""
if [ "$WITH_SMOKE" -ne 0 ]; then
    echo "BUILD OK: build/aec_gui.exe + build/nkf_smoke.exe"
    echo "Next: scripts/test.sh --native-only, or ship with scripts/make-release.sh"
else
    echo "BUILD OK: build/aec_gui.exe"
    echo "Next: ship with scripts/make-release.sh (needs --with-smoke for scripts/test.sh --native-only)"
fi

if [ "$RUN" -ne 0 ]; then
    echo ""
    if [ ! -x build/aec_gui.exe ]; then
        echo "cannot --run: build/aec_gui.exe missing" >&2; exit 1
    fi
    ./build/aec_gui.exe & disown
    echo "launched aec_gui.exe (pid $!)"
fi
