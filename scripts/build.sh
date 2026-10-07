#!/usr/bin/env bash
# build.sh — configure + build the desktop app, lean and bloat-free.
#
# Usage (MSYS2 UCRT64 bash, from the repo root):
#   scripts/build.sh            # clean-configure + build (MinSizeRel)
#   scripts/build.sh --clean    # wipe build/ first (stale objects gone)
#   scripts/build.sh --debug    # debuggable build instead of size build
#   scripts/build.sh --app-only # build just aec_gui.exe (no nkf_smoke.exe,
#                               # no diagnostic logs left behind)
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

CLEAN=0
APP_ONLY=0
BUILD_TYPE="MinSizeRel"
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;;
        --debug) BUILD_TYPE="Debug" ;;
        --app-only) APP_ONLY=1 ;;
        -h|--help)
            sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown flag: $a (see --help)" >&2; exit 1 ;;
    esac
done

test -f CMakeLists.txt -a -d src || {
    echo "run from the repo root" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja not found" >&2; exit 1; }

if [ "$CLEAN" -ne 0 ]; then
    echo "-- removing build/ (fresh configure)"
    rm -rf build
fi

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

echo "-- configure (${BUILD_TYPE})"
# shellcheck disable=SC2086
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LINK_FLAGS}" \
    "${CC_LAUNCHER[@]:-}"

echo "-- build"
NPROC=$(nproc 2>/dev/null || echo 4)
if [ "$APP_ONLY" -ne 0 ]; then
    cmake --build build -j "$NPROC" --target aec_gui
    # A clean app build leaves no diagnostic tools or logs behind.
    rm -f build/nkf_smoke.exe build/nkf_smoke.pdb nkf-phase.log build/nkf-phase.log
else
    cmake --build build -j "$NPROC"
fi

echo ""
echo "-- size audit (bloat check)"
ls -la build/aec_gui.exe build/nkf_smoke.exe 2>/dev/null || true
echo ""
echo "DLLs next to the exe:"
du -h build/*.dll 2>/dev/null || echo "(no DLLs staged)"
echo ""
echo "Models (bundled, unchanged by build flags):"
du -h -c models/nkf.onnx models/dtln_aec_128_*.tflite models/dtln_ns_128_*.tflite 2>/dev/null || true
echo ""
echo "BUILD OK: build/aec_gui.exe + build/nkf_smoke.exe"
echo "Next: run ./build/nkf_smoke.exe --synth 15 out.wav, or ship with scripts/make-release.sh <ver>"
