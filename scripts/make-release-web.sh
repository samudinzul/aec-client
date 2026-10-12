#!/usr/bin/env bash
# make-release-web.sh — assemble the pure-Python web UI
# release bundle + zip. A SEPARATE asset from the
# desktop AEC-Client-vX-win64.zip (both sit on the
# same GitHub release page).
#
# Usage:  scripts/make-release-web.sh [version]  (default: APP_VERSION)
#
# Contract:
#   release/vX/
#     AEC-Web-vX-win64/
#       web/        (the app: start.bat, server, DSP, GUI)
#       web/sounds/ (empty; users drop soundboard pads here)
#       models/     (the two DTLN pairs + nkf.onnx — all three
#                    profiles work offline; AEC3 needs no model file,
#                    its pip package installs via requirements.txt)
#       LICENSE
#       README.txt  (end-user doc, version stamped)
#     AEC-Web-vX-win64.zip  (single top folder)
#
# One version folder holds both assets' stages + zips (web here,
# desktop next to it via make-release-desktop.sh); the zips
# themselves keep the flat GitHub-asset layout (zip root = asset
# folder).
#
# No .exe, no DLL, no .pyd, no .venv, no __pycache__ — not even
# the optional local aec_dsp accelerator: a .pyd IS a PE binary
# and ships the exact AV-heuristic risk this build exists to
# dodge (it stays a local-only build; the UI falls back to
# NumPy without it). Nothing in here can trip the
# AI heuristics that flag the unsigned
# desktop build (see SECURITY.md).
#
# release/ is gitignored; run from the repo root.
set -euo pipefail

if [ $# -gt 1 ]; then
    echo "usage: $0 [version]   (default: APP_VERSION from src/main.cpp)" >&2
    exit 1
fi
VER="${1:-$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)}"
echo "-- version: $VER"
test -d web -a -d models -a -f web/start.bat || {
    echo "run from the repo root" >&2; exit 1; }
command -v zip >/dev/null || { echo "zip not found" >&2; exit 1; }

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

# The sounds/ folder ships EMPTY — users drop their own pads in.
# The dev machine's demo mp3s are local test fixtures, not release
# content (and shipping them would also leak the dev audio into the
# bundle), so strip everything but the folder itself.
find "$STAGE/web/sounds" -mindepth 1 -delete

# Models: the two DTLN pairs + the NKF model (the NKF-AEC
# profile fails closed at Start without it).
for m in models/dtln_aec_128_1.tflite \
         models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite \
         models/dtln_ns_128_2.tflite \
         models/nkf.onnx; do
    test -f "$m" || { echo "missing model: $m" >&2; exit 1; }
    cp "$m" "$STAGE/models/"
done

cp LICENSE "$STAGE/"
sed "s/%%VERSION%%/${VER}/g" scripts/templates/README-web.txt > "$STAGE/README.txt"

# ---- contract asserts ----
fail=0
for f in web/start.bat web/server.py web/chain.py \
         web/audio.py web/aec3.py web/nkf.py web/gui.py \
         web/requirements.txt \
         web/ui/index.html web/ui/imgui_web.html \
         web/ui/imgui_web.js web/ui/imgui_web.css \
         web/ui/favicon.ico \
         models/dtln_aec_128_1.tflite models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite models/dtln_ns_128_2.tflite \
         models/nkf.onnx \
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
