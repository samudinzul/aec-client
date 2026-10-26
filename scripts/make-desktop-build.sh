#!/usr/bin/env bash
# make-desktop-build.sh — configure + build the desktop app, lean and bloat-free.
#
# Quiet by default: cmake/ninja chatter goes to build/build.log, the
# console shows only progress, failures, and the size audit.
# Pass --verbose (-v) to stream the full tool output live.
#
# Two different kinds of "clean", kept apart on purpose:
#   - CLEAN OUTPUT (always, no flag): builds ONLY the app
#     (aec_gui.exe) — no smoke tools, no stray logs left in build/.
#     --exp swaps the app target to the experimental aec_exp.exe.
#   - FRESH CONFIGURE (--fresh): wipe build/ first, then configure
#     from scratch. Use after CMakeLists changes or when the cache
#     looks stale.
#
# Usage (MSYS2 UCRT64 bash, from the repo root):
#   scripts/make-desktop-build.sh              # build app (MinSizeRel)
#   scripts/make-desktop-build.sh --fresh      # wipe build/, then build app
#   scripts/make-desktop-build.sh --debug      # debuggable build instead of size build
#   scripts/make-desktop-build.sh --exp        # build ONLY aec_exp.exe (experimental fork)
#   scripts/make-desktop-build.sh --with-smoke # ALSO build the offline smoke tools
#                                              # (nkf_smoke + dtln_smoke + aec3_smoke;
#                                              # nkf_smoke feeds scripts/test.sh --native-only)
#   scripts/make-desktop-build.sh --run        # build, then launch aec_gui.exe
#                                              # (aec_exp.exe with --exp)
#   scripts/make-desktop-build.sh -v           # verbose: full cmake/ninja output
#
# The flags are independent and composable:
#   --exp         selects the APP: aec_exp.exe instead of aec_gui.exe.
#                 It never builds aec_gui, so the official shipped exe
#                 is left untouched.
#   --with-smoke  ADDS the smoke tools to whichever app is selected.
#                 It is NOT needed for a normal app build, and it is
#                 not an alternative to --exp.
#
#   scripts/make-desktop-build.sh --exp                 # exp app only
#   scripts/make-desktop-build.sh --exp --with-smoke    # exp app + smoke tools
#   scripts/make-desktop-build.sh --with-smoke          # official app + smoke tools
#
# --run launches the selected app detached (terminal stays free).
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
#   - NO UPX packing, ever: packed PEs trip the exact aggressive
#     AI heuristics (see SECURITY.md) this project exists to dodge
#   - nkf_smoke stays a console app (no -mwindows on it)
#
# After the build it prints a size audit (exe + DLLs + models) so
# bloat is visible immediately. Pair with scripts/make-release-desktop.sh
# to ship.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

FRESH=0
WITH_SMOKE=0
WITH_EXP=0
RUN=0
VERBOSE=0
END=0
BUILD_TYPE="MinSizeRel"
for a in "$@"; do
    if [ "$END" -ne 0 ]; then usage_error "unexpected argument: $a"; fi
    case "$a" in
        --) END=1 ;;
        --fresh) FRESH=1 ;;
        --debug) BUILD_TYPE="Debug" ;;
        --exp) WITH_EXP=1 ;;
        --with-smoke) WITH_SMOKE=1 ;;
        --run) RUN=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            usage_from_header "$0"; exit 0 ;;
        *) usage_error "unknown flag: $a" ;;
    esac
done

require_file CMakeLists.txt
require_dir src
need_cmd cmake
need_cmd ninja

if [ "$FRESH" -ne 0 ]; then
    # Windows locks a running program's file AND its folder: wiping
    # build/ while the app is open dies with "Device or resource busy"
    # (possibly after half-deleting the contents). Fail upfront.
    if tasklist.exe 2>/dev/null | grep -iq "aec_gui.exe\|aec_exp.exe\|nkf_smoke.exe\|dtln_smoke.exe\|aec3_smoke.exe\|gtcrn_smoke.exe\|localvqe_smoke.exe"; then
        echo "close the app and any smoke exe first — running programs lock build/" >&2
        exit 1
    fi
    echo "-- removing build/ (fresh configure)"
    # A stale lock (shell sitting inside build/, indexer/antivirus, a
    # zombie handle tasklist missed) can still EBUSY the rmdir even
    # after the contents are gone. Fall back to emptying in place —
    # an empty build/ reconfigures identically — instead of dying.
    if ! rm -rf build 2>/dev/null; then
        echo "-- build/ locked, emptying in place instead" >&2
        find build -mindepth 1 -delete 2>/dev/null || true
        rmdir build 2>/dev/null || true
    fi
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

# Flag semantics (composable, not alternatives):
#   --exp         -> which APP to build (aec_exp instead of aec_gui)
#   --with-smoke  -> ADDS the smoke tools to that app build
APP_TARGET="aec_gui"
APP_EXE="aec_gui.exe"
if [ "$WITH_EXP" -ne 0 ]; then
    APP_TARGET="aec_exp"
    APP_EXE="aec_exp.exe"
fi
TARGETS=("$APP_TARGET")
if [ "$WITH_SMOKE" -ne 0 ]; then
    TARGETS+=(nkf_smoke dtln_smoke aec3_smoke)
fi
run_step "build ${TARGETS[*]} (-j${NPROC})" \
    cmake --build build -j "$NPROC" --target "${TARGETS[@]}"

# An app-only build leaves no diagnostic tools or logs behind.
if [ "$WITH_SMOKE" -eq 0 ]; then
    rm -f build/nkf_smoke.exe build/nkf_smoke.pdb nkf-phase.log build/nkf-phase.log
fi

echo ""
echo "-- size audit (bloat check)"
ls -la "build/${APP_EXE}" 2>/dev/null || true
if [ "$WITH_SMOKE" -ne 0 ]; then
    ls -la build/nkf_smoke.exe build/dtln_smoke.exe build/aec3_smoke.exe 2>/dev/null || true
fi
echo ""
echo "DLLs next to the exe:"
du -h build/*.dll 2>/dev/null || echo "(no DLLs staged)"
echo ""
echo "Models (bundled, unchanged by build flags):"
if [ "$WITH_EXP" -ne 0 ]; then
    du -h -c exp/models/gtcrn_simple.onnx exp/models/LocalVQEAECResidualMask.onnx 2>/dev/null || true
else
    du -h -c models/nkf.onnx models/dtln_aec_128_*.tflite models/dtln_ns_128_*.tflite 2>/dev/null || true
fi
echo ""
if [ "$WITH_EXP" -ne 0 ] && [ "$WITH_SMOKE" -ne 0 ]; then
    echo "BUILD OK: build/aec_exp.exe + smoke exes"
    echo "Next: scripts/test.sh --native-only, or run build/aec_exp.exe"
elif [ "$WITH_EXP" -ne 0 ]; then
    echo "BUILD OK: build/aec_exp.exe"
    echo "Next: run build/aec_exp.exe (or --exp --run)"
elif [ "$WITH_SMOKE" -ne 0 ]; then
    echo "BUILD OK: build/aec_gui.exe + smoke exes"
    echo "Next: scripts/test.sh --native-only, or ship with scripts/make-release-desktop.sh"
else
    echo "BUILD OK: build/aec_gui.exe"
    echo "Next: ship with scripts/make-release-desktop.sh (add --with-smoke for scripts/test.sh --native-only)"
fi

if [ "$RUN" -ne 0 ]; then
    echo ""
    RUN_EXE="build/aec_gui.exe"
    if [ "$WITH_EXP" -ne 0 ]; then
        RUN_EXE="build/aec_exp.exe"
    fi
    if [ ! -x "$RUN_EXE" ]; then
        echo "cannot --run: ${RUN_EXE} missing" >&2; exit 1
    fi
    "./${RUN_EXE}" & disown
    echo "launched ${RUN_EXE#build/} (pid $!)"
fi
