#!/usr/bin/env bash
# make-web-build.sh — prepare + verify the web runtime (the web
# counterpart to make-desktop-build.sh: pure Python needs no
# compiler, so "build" means environment + checks).
#
# Usage (MSYS2 UCRT64 bash, from the repo root):
#   scripts/make-web-build.sh          # venv + full packages + checks
#   scripts/make-web-build.sh --fresh  # wipe .venv first, rebuild it
#   scripts/make-web-build.sh --lean   # base packages only
#                                      # (DTLN profiles, ~140 MB)
#   scripts/make-web-build.sh --run    # build, then launch the GUI
#                                      # (native window via web.gui,
#                                      # attaches if a server runs)
#
# Steps:
#   1. Find a Python (py launcher preferred, PATH fallback;
#      version-checked 3.10-3.14 like web/start.bat).
#   2. Create .venv unless present (--fresh wipes it first).
#   3. pip install the requirement set (quiet no-op when satisfied).
#   4. Byte-compile every web/*.py (syntax over the whole backend).
#   5. Run the offline engine smoke (all available engines).
#
# The venv layout differs per OS (.venv/Scripts on Windows,
# .venv/bin elsewhere); everything here resolves through a
# probed interpreter, so the script runs on MSYS2 and Linux.
set -euo pipefail

FRESH=0
LEAN=0
RUN=0
for a in "$@"; do
    case "$a" in
        --fresh) FRESH=1 ;;
        --lean) LEAN=1 ;;
        --run) RUN=1 ;;
        -h|--help)
            sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown flag: $a (see --help)" >&2; exit 1 ;;
    esac
done

test -f CMakeLists.txt -a -d web || {
    echo "run from the repo root" >&2; exit 1; }

# ---- 1. interpreter (same preference as web/start.bat) ----
PY_BIN=""
if py -3 --version >/dev/null 2>&1; then PY_BIN="py -3";
elif command -v python3 >/dev/null; then PY_BIN="python3";
elif command -v python >/dev/null; then PY_BIN="python"; fi
[ -n "$PY_BIN" ] || { echo "no Python found" >&2; exit 1; }
# shellcheck disable=SC2086
if ! $PY_BIN -c "import sys; raise SystemExit(0 if (3,10) <= sys.version_info < (3,15) else 1)"; then
    echo "Python 3.10-3.14 required" >&2; $PY_BIN --version; exit 1
fi

# ---- 2. venv (usability-probed, not just present: a synced
# foreign-platform .venv looks executable but cannot run — never
# auto-wipe that; only --fresh destroys) ----
find_vpy() {
    VPY=""
    for cand in .venv/Scripts/python.exe .venv/bin/python; do
        if "$cand" --version >/dev/null 2>&1; then VPY="$cand"; break; fi
    done
}
find_vpy
if [ -z "$VPY" ]; then
    if [ ! -d .venv ] || [ "$FRESH" -ne 0 ]; then
        if [ "$FRESH" -ne 0 ]; then
            echo "-- removing .venv (fresh environment)"
            rm -rf .venv
        else
            echo "-- creating .venv (first run only)"
        fi
        # shellcheck disable=SC2086
        $PY_BIN -m venv .venv
        find_vpy
    fi
fi
[ -n "$VPY" ] || {
    echo "existing .venv is unusable here (foreign platform?) — rerun with --fresh to rebuild it" >&2
    exit 1; }

# ---- 3. packages (quiet; no-op when satisfied) ----
if [ "$LEAN" -ne 0 ]; then
    REQ="web/requirements-base.txt"
else
    REQ="web/requirements.txt"
fi
echo "-- packages (${REQ})"
if ! "$VPY" -m pip install -q --disable-pip-version-check -r "$REQ"; then
    echo "pip install failed — connect to the internet and retry" >&2
    exit 1
fi

# ---- 4. byte-compile the backend ----
echo "-- compile check"
if ! "$VPY" -m compileall -q web; then
    echo "compile check FAILED" >&2; exit 1
fi
echo "   compile OK"

# ---- 5. offline engine smoke ----
echo "-- engine smoke"
if "$VPY" -m web.test_offline --smoke; then
    echo "WEB BUILD OK (.venv + packages + smoke)"
else
    echo "engine smoke FAILED" >&2; exit 1
fi

# ---- 6. launch (opt-in --run, like make-desktop-build.sh) ----
if [ "$RUN" -ne 0 ]; then
    echo ""
    # Detached, terminal stays free (pywebview window owns its
    # message loop; closing the shell won't kill it). gui.py
    # attaches to a running server instead of starting a second
    # one, so re-running is harmless. Needs a GUI session —
    # headless/SSH shells fail here with a clear message from
    # pywebview, not silently.
    "$VPY" -m web.gui & disown
    echo "launched web GUI (pid $!) — http://localhost:8000/ in the window"
    echo "(if no window appears, run \"$VPY -m web.gui\" in the foreground for the error)"
fi
