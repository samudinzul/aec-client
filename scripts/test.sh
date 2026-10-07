#!/usr/bin/env bash
# test.sh — offline test suite. No audio hardware, no mic needed.
#
# Usage (MSYS2 UCRT64 bash, from the repo root):
#   scripts/test.sh                # every available suite
#   scripts/test.sh --native-only  # desktop NKF engine only
#   scripts/test.sh --web-only     # web DTLN chain only
#
# Suites:
#   native — build/nkf_smoke.exe --synth 10: 10 s of loud-tonal ref
#     + voice bursts through NkfProcess -> DTLN-NS. Expects the
#     healthy fingerprint: exit 0, "no trim" verdict, ZERO backstop
#     ATTACK lines in a fresh nkf-phase.log. (A real howl WOULD
#     attack — these expectations are specific to the synth.)
#   web    — python -m web.test_offline --smoke: synthetic pair
#     through the web chain. Expects SMOKE OK. Skipped (not failed)
#     when models or ai-edge-litert are missing.
#
# Exit code: 0 = all ran suites passed, 1 = a failure.
set -euo pipefail

NATIVE=1
WEB=1
for a in "$@"; do
    case "$a" in
        --native-only) WEB=0 ;;
        --web-only) NATIVE=0 ;;
        -h|--help)
            sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown flag: $a (see --help)" >&2; exit 1 ;;
    esac
done

test -f CMakeLists.txt -a -d src || {
    echo "run from the repo root" >&2; exit 1; }

PASS=0
FAIL=0
SKIP=0
report() {  # $1 = name, $2 = PASS|FAIL|SKIP, $3 = detail
    printf '%-8s %s %s\n' "[$2]" "$1" "${3:-}"
    case "$2" in
        PASS) PASS=$((PASS + 1)) ;;
        FAIL) FAIL=$((FAIL + 1)) ;;
        SKIP) SKIP=$((SKIP + 1)) ;;
    esac
}

if [ "$NATIVE" -ne 0 ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*) native_ok=1 ;;
        *) native_ok=0
           report "native" SKIP "(Windows-only binary — run on the dev machine)" ;;
    esac
    if [ "${native_ok:-0}" -ne 0 ] && [ ! -x build/nkf_smoke.exe ]; then
        native_ok=0
        report "native" SKIP "(build/nkf_smoke.exe missing — run scripts/make-desktop-build.sh --with-smoke)"
    fi
    if [ "${native_ok:-0}" -ne 0 ] && [ ! -f models/nkf.onnx ]; then
        native_ok=0
        report "native" SKIP "(models/nkf.onnx missing)"
    fi
    if [ "${native_ok:-0}" -ne 0 ]; then
        # Repo-relative output: nkf_smoke.exe is a native Windows
        # binary — it cannot write MSYS2 /tmp paths (no POSIX
        # translation for argv), so mktemp-in-/tmp always failed
        # with "cannot write". build/ is gitignored; cleaned below.
        OUT_WAV="build/.nkf_smoke_test.wav"
        rm -f "$OUT_WAV"
        rm -f nkf-phase.log
        if build/nkf_smoke.exe --synth 10 "$OUT_WAV" > /tmp/nkf_smoke_out.txt 2>&1; then
            VERDICT=$(grep -a "verdict:" /tmp/nkf_smoke_out.txt | tail -1 || true)
            ATTACKS=$(grep -ac "ATTACK" nkf-phase.log 2>/dev/null || true)
            ATTACKS=${ATTACKS:-0}
            case "$VERDICT" in
                *"no trim"*)
                    if [ "$ATTACKS" -eq 0 ]; then
                        report "native" PASS "(no trim, 0 ATTACK lines)"
                    else
                        report "native" FAIL "($ATTACKS ATTACK lines — see nkf-phase.log)"
                    fi
                    ;;
                *) report "native" FAIL "(verdict: ${VERDICT:-none})" ;;
            esac
        else
            report "native" FAIL "(exit $?, see /tmp/nkf_smoke_out.txt)"
        fi
        rm -f "$OUT_WAV"
    fi
fi

if [ "$WEB" -ne 0 ]; then
    # First interpreter that actually runs wins: a synced Windows
    # .venv looks executable but fails on Linux, so probe it.
    PY=""
    for cand in .venv/Scripts/python.exe .venv/bin/python; do
        if [ -x "$cand" ] && "$cand" --version >/dev/null 2>&1; then
            PY="$cand"; break
        fi
    done
    [ -z "$PY" ] && PY=python3
    if "$PY" -m web.test_offline --smoke > /tmp/web_smoke_out.txt 2>&1; then
        report "web" PASS "($(grep -a -o 'SMOKE OK' /tmp/web_smoke_out.txt | head -1))"
    else
        if grep -aq "models missing" /tmp/web_smoke_out.txt 2>/dev/null; then
            report "web" SKIP "(models/litert missing — fail-open path)"
        else
            report "web" FAIL "(see /tmp/web_smoke_out.txt)"
        fi
    fi
fi

echo ""
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" -eq 0 ]
