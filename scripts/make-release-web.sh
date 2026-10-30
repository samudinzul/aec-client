#!/usr/bin/env bash
# make-release-web.sh — assemble the pure-Python web UI
# release bundle + zip. A SEPARATE asset from the
# desktop AEC-Client-vX-win64.zip (both sit on the
# same GitHub release page).
#
# Usage:  scripts/make-release-web.sh [version] [-v]
#         (default: APP_VERSION from src/main.cpp)
#
# Contract:
#   release/vX/
#     AEC-Web-vX-win64/
#       web/        (the app: start.bat, server, DSP, GUI)
#       web/sounds/ (empty; users drop soundboard pads here)
#       models/     (the two DTLN pairs + nkf.onnx + hush bundle —
#                    all three profiles work offline; AEC3 needs no
#                    model file, its pip package installs via
#                    requirements.txt; Hush needs only its DLL
#                    user-supplied — data ships, PE never does)
#       LICENSE
#       README.txt  (end-user doc, version stamped)
#     AEC-Web-vX-win64.zip  (single top folder)
#
# One version folder holds both assets' stages + zips (web here,
# desktop next to it via make-release-desktop.sh); the zips
# themselves keep the flat GitHub-asset layout (zip root = asset
# folder).
#
# FROZEN at 16 kHz: this is the zero-flag fallback, not desktop
# parity. It ships the DTLN/AEC3/NKF + DTLN-NS/Hush chain only;
# GTCRN, the 48 kHz high-band splice, automatic 48 kHz output, and
# Web changes are limited to security,
# compatibility, and release mechanics (see SECURITY.md).
#
# No .exe, no DLL, no .pyd, no .venv, no __pycache__. A third-party
# user-supplied library (e.g. Hush's weya_nc for the optional NS
# backend) must NEVER be committed or staged: any PE binary ships
# the exact AV-heuristic risk this build exists to dodge (selection
# stays fail-closed without it). Nothing in here can trip the
# AI heuristics that flag the unsigned
# desktop build (see SECURITY.md).
#
# release/ is gitignored; run from the repo root.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

VERBOSE=0
END=0
POSITIONAL=()
for a in "$@"; do
    if [ "$END" -ne 0 ]; then POSITIONAL+=("$a"); continue; fi
    case "$a" in
        --) END=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            usage_from_header "$0"; exit 0 ;;
        -*) usage_error "unknown flag: $a" ;;
        *) POSITIONAL+=("$a") ;;
    esac
done
if [ "${#POSITIONAL[@]}" -gt 1 ]; then
    usage_error "too many arguments: ${POSITIONAL[*]}"
fi
require_dir web
require_dir models
require_file web/start.bat
require_file src/main.cpp
if [ "${#POSITIONAL[@]}" -eq 1 ]; then
    VER="${POSITIONAL[0]}"
else
    VER="$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)"
fi
echo "-- version: $VER"
if [ "$VERBOSE" -ne 0 ]; then set -x; fi
need_cmd zip "pacman -S zip"
need_cmd python3 "pacman -S python"

RELDIR="release/v${VER}"
mkdir -p "$RELDIR"
# Drop legacy flat-layout leftovers of this version (pre-version-folder
# runs staged straight under release/): a stale flat zip next to the
# new folder could get uploaded to the wrong release by mistake.
rm -rf "release/AEC-Web-v${VER}-win64" "release/AEC-Web-v${VER}-win64.zip"
STAGE="${RELDIR}/AEC-Web-v${VER}-win64"
rm -rf "$STAGE"
mkdir -p "$STAGE/models"

# The app, minus caches and local junk. cp runs BEFORE any mkdir
# under $STAGE/web: pre-creating $STAGE/web/sounds first makes the
# target exist, and `cp -r web` then nests web/ inside web/
# ($STAGE/web/web/...) instead of copying it as $STAGE/web.
cp -r web "$STAGE/web"
# The sounds/ folder is guaranteed to exist even if the checkout
# has none (cp copies nothing then) — it ships EMPTY, see below.
mkdir -p "$STAGE/web/sounds"
rm -rf "$STAGE/web/__pycache__"
rm -f  "$STAGE/web/start.log"
find "$STAGE/web" -name '*.pyc' -delete
# Drop dev-only test files from the release bundle (keep in repo).
rm -f "$STAGE"/web/test_*.py

# The sounds/ folder ships EMPTY — users drop their own pads in.
# The dev machine's demo mp3s are local test fixtures, not release
# content (and shipping them would also leak the dev audio into the
# bundle), so strip everything but the folder itself.
find "$STAGE/web/sounds" -mindepth 1 -delete

# Models: the two DTLN pairs + the NKF model (the NKF-AEC
# profile fails closed at Start without it) + the Hush NS bundle
# (data like the rest — only its weya_nc.dll stays user-supplied
# and must never be staged; the asserts below fail the build on
# any PE).
for m in models/dtln_aec_128_1.tflite \
         models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite \
         models/dtln_ns_128_2.tflite \
         models/nkf.onnx \
         models/hush_16k_onnx.tar.gz; do
    test -f "$m" || { echo "missing model: $m" >&2; exit 1; }
    cp "$m" "$STAGE/models/"
done

cp LICENSE "$STAGE/"
sed "s/%%VERSION%%/${VER}/g" scripts/templates/README-web.txt > "$STAGE/README.txt"

# ---- contract asserts ----
fail=0
for f in web/start.bat web/server.py web/chain.py \
         web/audio.py web/aec3.py web/nkf.py web/gui.py \
         web/hush.py \
         web/requirements.txt \
         web/ui/index.html web/ui/imgui_web.html \
         web/ui/imgui_web.js web/ui/imgui_web.css \
         web/ui/favicon.ico \
         models/dtln_aec_128_1.tflite models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite models/dtln_ns_128_2.tflite \
         models/nkf.onnx models/hush_16k_onnx.tar.gz \
         README.txt LICENSE; do
    test -f "$STAGE/$f" || { echo "missing: $f" >&2; fail=1; }
done
# sounds/ ships as an EXISTING, EMPTY dir (pads are user content).
test -d "$STAGE/web/sounds" || { echo "missing: web/sounds/" >&2; fail=1; }
test -z "$(ls -A "$STAGE/web/sounds")" || { echo "web/sounds/ not empty" >&2; fail=1; }
# Zero PE binaries, no exceptions (see contract header): .pyd
# included — a shipped accelerator would flag the whole zip.
bad=$(find "$STAGE" \( -name '*.exe' -o -name '*.dll' \
    -o -name '*.pyd' -o -name '*.pyc' -o -name '__pycache__' \
    -o -name '.venv' -o -name 'start.log' -o -name '*.pdb' \) \
    -print || true)
if [ -n "$bad" ]; then echo "FORBIDDEN FILES STAGED:"; echo "$bad"; fail=1; fi
grep -q "v${VER}" "$STAGE/README.txt" || { echo "README.txt version stamp wrong" >&2; fail=1; }
[ $fail -ne 0 ] && exit 1

rm -f "${RELDIR}/AEC-Web-v${VER}-win64.zip"
(cd "$RELDIR" && zip -qr "AEC-Web-v${VER}-win64.zip" "AEC-Web-v${VER}-win64")

# ---- zip asserts ----
python3 -c "
import zipfile, re
names = zipfile.ZipFile('release/v${VER}/AEC-Web-v${VER}-win64.zip').namelist()
roots = sorted({n.split('/')[0] for n in names})
assert roots == ['AEC-Web-v${VER}-win64'], f'zip root layout wrong: {roots}'
pat = re.compile(r'__pycache__|\.pyc$|\.venv|start\.log|\.exe$|\.dll$|\.pyd$|\.pdb$', re.I)
bad = [n for n in names if pat.search(n)]
assert not bad, f'forbidden files in zip: {bad}'
print(f'zip entries: {len(names)}, single root, no packed binaries, no caches')
"

echo "----"
du -h "release/v${VER}/AEC-Web-v${VER}-win64.zip"
sha256sum "release/v${VER}/AEC-Web-v${VER}-win64.zip"
echo "RELEASE OK: $STAGE + zip (separate asset from AEC-Client-v${VER}-win64.zip)"
