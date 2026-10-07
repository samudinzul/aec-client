#!/usr/bin/env bash
# make-release-desktop.sh — assemble a clean AEC Client release bundle + zip.
#
# Usage:  scripts/make-release-desktop.sh [version]  (default: APP_VERSION)
#
# Contract (matches v1.2.x precedent, enforced by asserts below):
#   release/AEC-Client-vX-win64/
#     aec_gui.exe + *.dll (runtime only) + libs/tensorflowlite_c.dll
#   models/          (explicit allowlist: nkf, dtln pair)
#     LICENSES/        (third-party credits)
#     README.txt       (end-user doc, version stamped)
#     wallpapers/      (empty; users bring their own)
#   release/AEC-Client-vX-win64.zip  (top folder only — no releases/
#     subfolder, no imgui.ini, no aec_config.txt, no .pdb, no junk)
#
# release/ is gitignored; run from the repo root in MSYS2 bash.
set -euo pipefail

if [ $# -gt 1 ]; then
    echo "usage: $0 [version]   (default: APP_VERSION from src/main.cpp)" >&2
    exit 1
fi
VER="${1:-$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)}"
echo "-- version: $VER"
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
# leftovers — stale files like the nuked ECAPA/Silero pair must not ship).
for m in models/nkf.onnx \
         models/dtln_aec_128_1.tflite \
         models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite \
         models/dtln_ns_128_2.tflite; do
    test -f "$m" || { echo "missing model: $m" >&2; exit 1; }
    cp "$m" "$STAGE/models/"
done

cp LICENSES/THIRD-PARTY.txt "$STAGE/LICENSES/"
sed "s/%%VERSION%%/${VER}/g" scripts/templates/README-desktop.txt > "$STAGE/README.txt"

# ---- contract asserts ----
fail=0
forbidden='imgui\.ini|aec_config\.txt|dfn|deepfilter|\.pdb$|\.dll\.a$|\.o$|CMakeFiles|releases/|enrollment|embedding|\.wav$|\.mp3$|\.flac$|gtcrn|silero|libabsl_|libprotobuf|libre2|libonnx\.dll|libutf8|libgomp'
bad=$(find "$STAGE" | grep -Ei "$forbidden" || true)
if [ -n "$bad" ]; then echo "FORBIDDEN FILES STAGED:"; echo "$bad"; fail=1; fi
test -f "$STAGE/aec_gui.exe" || { echo "missing exe" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_1.tflite" || { echo "missing dtln 128 stage1" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_2.tflite" || { echo "missing dtln 128 stage2" >&2; fail=1; }
test -f "$STAGE/models/nkf.onnx" || { echo "missing nkf" >&2; fail=1; }
# Old default must never ship again
if ls "$STAGE"/models/dtln_aec_512_* >/dev/null 2>&1; then
    echo "forbidden: dtln_aec_512_* staged (use 128 pair only)"; fail=1
fi
test -f "$STAGE/README.txt" || { echo "missing README.txt" >&2; fail=1; }
grep -q "v${VER}" "$STAGE/README.txt" || { echo "README.txt version stamp wrong" >&2; fail=1; }
# Slim-runtime contract: vendored MS onnxruntime (uncompressed) + its
# VC++ runtime; the old 27.5 MB MSYS2 build / absl cluster must not return.
test -f "$STAGE/onnxruntime.dll" || { echo "missing onnxruntime.dll" >&2; fail=1; }
ort_sz=$(stat -c %s "$STAGE/onnxruntime.dll" 2>/dev/null || echo 0)
if [ "$ort_sz" -ge 25000000 ]; then
    echo "onnxruntime.dll is ${ort_sz}B (>25MB) — MSYS2 build leaked in?"; fail=1
fi
for d in msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll; do
    test -f "$STAGE/$d" || { echo "missing VC++ runtime: $d" >&2; fail=1; }
done
[ $fail -ne 0 ] && exit 1

rm -f "release/AEC-Client-v${VER}-win64.zip"
(cd release && zip -qr "AEC-Client-v${VER}-win64.zip" "AEC-Client-v${VER}-win64")

# ---- zip asserts ----
python3 -c "
import zipfile, re, sys
names = zipfile.ZipFile('release/AEC-Client-v${VER}-win64.zip').namelist()
assert len(names) > 15, f'zip suspiciously small ({len(names)})'
roots = sorted({n.split('/')[0] for n in names})
assert roots == ['AEC-Client-v${VER}-win64'], f'zip root layout wrong: {roots}'
pat = re.compile(r'releases/|imgui\.ini|aec_config|dfn|deepfilter|config\.txt|\.pdb$|\.dll\.a$|enrollment|embedding|\.wav$|\.mp3$|\.flac$|libabsl_|libprotobuf|libre2|libonnx\.dll|libutf8|libgomp', re.I)
bad = [n for n in names if pat.search(n)]
assert not bad, f'forbidden files in zip: {bad}'
print(f'zip entries: {len(names)}, single root OK, no strays')
"

# ============================================================
#  Code signing (optional, off by default).
#
#  The unsigned build trips Microsoft's Wacatac.B!ml heuristic
#  (unsigned PE + bundled ML runtimes). A code-signing certificate
#  is the only fix. Set AEC_SIGN=1 to enable; you must also have
#  the cert installed in your cert store and signtool on PATH.
#
#  Two routes:
#    Paid — DigiCert / Sectigo EV code-signing cert (~$200-400/yr).
#    Free — Microsoft Trusted Signing (Azure account, no cost):
#          signtool sign /f <cert.pfx> /p <password> ...
#          or the az signtool flow. See README.md.
# ============================================================
if [ "${AEC_SIGN:-0}" = "1" ]; then
    if ! command -v signtool >/dev/null; then
        echo "AEC_SIGN=1 but signotool not found; skipping signing" >&2
    else
        signtool sign /v /sha1 "${AEC_CERT_SHA1:-}" \
            /t "${AEC_TIMESTAMP_URL:-http://timestamp.digicert.com}" \
            "$STAGE/aec_gui.exe"
        echo "signed aec_gui.exe"
    fi
fi

echo "----"
du -h "release/AEC-Client-v${VER}-win64.zip"
sha256sum "release/AEC-Client-v${VER}-win64.zip"
echo "RELEASE OK: $STAGE + zip, no releases folder, no strays"
