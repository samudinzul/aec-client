#!/usr/bin/env bash
# make-release.sh — assemble a clean AEC Client release bundle + zip.
#
# Usage:  scripts/make-release.sh 1.3.0
#
# Contract (matches v1.2.x precedent, enforced by asserts below):
#   release/AEC-Client-vX-win64/
#     aec_gui.exe + *.dll (runtime only) + libs/tensorflowlite_c.dll
#   models/          (explicit allowlist: nkf, dtln pair, silero)
#     LICENSES/        (third-party credits)
#     README.txt       (end-user doc, version stamped)
#     wallpapers/      (empty; users bring their own)
#   release/AEC-Client-vX-win64.zip  (top folder only — no releases/
#     subfolder, no imgui.ini, no aec_config.txt, no .pdb, no junk)
#
# release/ is gitignored; run from the repo root in MSYS2 bash.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <version>   (e.g. $0 1.3.0)" >&2
    exit 1
fi
VER="$1"
test -d src -a -d build -a -f CMakeLists.txt || {
    echo "run from the repo root" >&2; exit 1; }

# Version must match the app identity.
APP_VER=$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)
if [ "$APP_VER" != "$VER" ]; then
    echo "APP_VERSION ($APP_VER) != requested ($VER); bump src/main.cpp first" >&2
    exit 1
fi

test -x build/aec_gui.exe || { echo "build/aec_gui.exe missing; build first" >&2; exit 1; }
command -v zip >/dev/null || { echo "zip not found (pacman -S zip)" >&2; exit 1; }

STAGE="release/AEC-Client-v${VER}-win64"
rm -rf "$STAGE"
mkdir -p "$STAGE/models" "$STAGE/LICENSES" "$STAGE/wallpapers"

cp build/aec_gui.exe "$STAGE/"
cp build/*.dll "$STAGE/"
cp libs/tensorflowlite_c.dll "$STAGE/" 2>/dev/null || true

# Models: explicit allowlist from repo source of truth (never build/
# leftovers — stale files like the nuked ECAPA pair must not ship).
for m in models/nkf.onnx \
         models/dtln_aec_128_1.tflite \
         models/dtln_aec_128_2.tflite \
         models/silero_vad.onnx; do
    test -f "$m" || { echo "missing model: $m" >&2; exit 1; }
    cp "$m" "$STAGE/models/"
done

cp LICENSES/THIRD-PARTY.txt "$STAGE/LICENSES/"
sed "s/%%VERSION%%/${VER}/g" scripts/README.txt > "$STAGE/README.txt"

# ---- contract asserts ----
fail=0
forbidden='imgui\.ini|aec_config\.txt|\.pdb$|\.dll\.a$|\.o$|CMakeFiles|releases/|enrollment|embedding|\.wav$|\.mp3$|\.flac$|gtcrn'
bad=$(find "$STAGE" | grep -Ei "$forbidden" || true)
if [ -n "$bad" ]; then echo "FORBIDDEN FILES STAGED:"; echo "$bad"; fail=1; fi
test -f "$STAGE/aec_gui.exe" || { echo "missing exe" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_1.tflite" || { echo "missing dtln 128 stage1" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_2.tflite" || { echo "missing dtln 128 stage2" >&2; fail=1; }
test -f "$STAGE/models/silero_vad.onnx" || { echo "missing silero" >&2; fail=1; }
test -f "$STAGE/models/nkf.onnx" || { echo "missing nkf" >&2; fail=1; }
# Old default must never ship again
if ls "$STAGE"/models/dtln_aec_512_* >/dev/null 2>&1; then
    echo "forbidden: dtln_aec_512_* staged (use 128 pair only)"; fail=1
fi
test -f "$STAGE/README.txt" || { echo "missing README.txt" >&2; fail=1; }
grep -q "v${VER}" "$STAGE/README.txt" || { echo "README.txt version stamp wrong" >&2; fail=1; }
[ $fail -ne 0 ] && exit 1

rm -f "release/AEC-Client-v${VER}-win64.zip"
(cd release && zip -qr "AEC-Client-v${VER}-win64.zip" "AEC-Client-v${VER}-win64")

# ---- zip asserts ----
python3 -c "
import zipfile, re, sys
names = zipfile.ZipFile('release/AEC-Client-v${VER}-win64.zip').namelist()
assert len(names) > 100, f'zip suspiciously small ({len(names)})'
roots = sorted({n.split('/')[0] for n in names})
assert roots == ['AEC-Client-v${VER}-win64'], f'zip root layout wrong: {roots}'
pat = re.compile(r'releases/|imgui\.ini|aec_config|config\.txt|\.pdb$|\.dll\.a$|enrollment|embedding|\.wav$|\.mp3$|\.flac$', re.I)
bad = [n for n in names if pat.search(n)]
assert not bad, f'forbidden files in zip: {bad}'
print(f'zip entries: {len(names)}, single root OK, no strays')
"

echo "----"
du -h "release/AEC-Client-v${VER}-win64.zip"
sha256sum "release/AEC-Client-v${VER}-win64.zip"
echo "RELEASE OK: $STAGE + zip, no releases folder, no strays"
