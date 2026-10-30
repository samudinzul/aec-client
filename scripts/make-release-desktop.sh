#!/usr/bin/env bash
# make-release-desktop.sh — assemble a clean AEC Client release bundle + zip.
#
# Usage:  scripts/make-release-desktop.sh [version] [-v]
#         (default: APP_VERSION from src/main.cpp)
#
# Contract (matches v1.2.x precedent, enforced by asserts below):
#   release/vX/
#     AEC-Client-vX-win64/
#       aec_gui.exe + *.dll (runtime only) + libs/tensorflowlite_c.dll
#       models/        (explicit allowlist: nkf, dtln pair, hush bundle)
#       LICENSES/      (third-party credits)
#       README.txt     (end-user doc, version stamped)
#       wallpapers/    (empty; users bring their own)
#       sounds/        (empty; users drop soundboard pads here)
#     AEC-Client-vX-win64.zip  (top folder only — no releases/
#       subfolder, no imgui.ini, no aec_config.txt, no .pdb, no junk)
#
# One version folder holds both assets' stages + zips (desktop here,
# web next to it via make-release-web.sh); the zips themselves keep
# the flat GitHub-asset layout (zip root = asset folder).
#
# release/ is gitignored; run from the repo root in MSYS2 bash.
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
require_dir src
require_dir build
require_file CMakeLists.txt
if [ "${#POSITIONAL[@]}" -eq 1 ]; then
    VER="${POSITIONAL[0]}"
else
    VER="$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)"
fi
echo "-- version: $VER"
if [ "$VERBOSE" -ne 0 ]; then set -x; fi

# Version must match the app identity.
APP_VER=$(grep -oP '#define APP_VERSION "\K[^"]+' src/main.cpp | head -1)
if [ "$APP_VER" != "$VER" ]; then
    echo "APP_VERSION ($APP_VER) != requested ($VER); bump src/main.cpp first" >&2
    exit 1
fi

test -x build/aec_gui.exe || { echo "build/aec_gui.exe missing; build first" >&2; exit 1; }
need_cmd zip "pacman -S zip"
need_cmd python3 "pacman -S python"

RELDIR="release/v${VER}"
mkdir -p "$RELDIR"
# Drop legacy flat-layout leftovers of this version (pre-version-folder
# runs staged straight under release/): a stale flat zip next to the
# new folder could get uploaded to the wrong release by mistake.
rm -rf "release/AEC-Client-v${VER}-win64" "release/AEC-Client-v${VER}-win64.zip"
STAGE="${RELDIR}/AEC-Client-v${VER}-win64"
rm -rf "$STAGE"
mkdir -p "$STAGE/models" "$STAGE/LICENSES" "$STAGE/wallpapers" "$STAGE/sounds"

cp build/aec_gui.exe "$STAGE/"
cp build/*.dll "$STAGE/"
cp libs/tensorflowlite_c.dll "$STAGE/" 2>/dev/null || true

# Models: explicit allowlist from repo source of truth (never build/
# leftovers — stale files like the nuked ECAPA/Silero pair must not ship).
for m in models/nkf.onnx \
         models/dtln_aec_128_1.tflite \
         models/dtln_aec_128_2.tflite \
         models/dtln_ns_128_1.tflite \
         models/dtln_ns_128_2.tflite \
         models/gtcrn_simple.onnx \
         models/hush_16k_onnx.tar.gz; do
    test -f "$m" || { echo "missing model: $m" >&2; exit 1; }
    cp "$m" "$STAGE/models/"
done

cp LICENSES/THIRD-PARTY.txt "$STAGE/LICENSES/"
sed "s/%%VERSION%%/${VER}/g" scripts/templates/README-desktop.txt > "$STAGE/README.txt"

# ---- contract asserts ----
fail=0
forbidden='imgui\.ini|aec_config\.txt|dfn|deepfilter|\.pdb$|\.dll\.a$|\.o$|CMakeFiles|releases/|enrollment|embedding|\.wav$|\.mp3$|\.flac$|silero|libabsl_|libprotobuf|libre2|libonnx\.dll|libutf8|libgomp'
bad=$(find "$STAGE" | grep -Ei "$forbidden" || true)
if [ -n "$bad" ]; then echo "FORBIDDEN FILES STAGED:"; echo "$bad"; fail=1; fi
test -f "$STAGE/aec_gui.exe" || { echo "missing exe" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_1.tflite" || { echo "missing dtln 128 stage1" >&2; fail=1; }
test -f "$STAGE/models/dtln_aec_128_2.tflite" || { echo "missing dtln 128 stage2" >&2; fail=1; }
test -f "$STAGE/models/nkf.onnx" || { echo "missing nkf" >&2; fail=1; }
test -f "$STAGE/models/gtcrn_simple.onnx" || { echo "missing gtcrn" >&2; fail=1; }
test -f "$STAGE/models/hush_16k_onnx.tar.gz" || { echo "missing hush bundle" >&2; fail=1; }
test -f "$STAGE/weya_nc.dll" || { echo "missing weya_nc.dll (build copies libs/weya_nc.dll)" >&2; fail=1; }
# Old default must never ship again
if ls "$STAGE"/models/dtln_aec_512_* >/dev/null 2>&1; then
    echo "forbidden: dtln_aec_512_* staged (use 128 pair only)"; fail=1
fi
test -f "$STAGE/README.txt" || { echo "missing README.txt" >&2; fail=1; }
grep -q "v${VER}" "$STAGE/README.txt" || { echo "README.txt version stamp wrong" >&2; fail=1; }
# AV-posture lock: version.rc metadata must match APP_VERSION, so the
# shipped exe is never anonymous or version-lying (both worsen the
# SmartScreen/ML-heuristic reputation profile — see SECURITY.md).
rc_ver=$(grep -oP 'VALUE "FileVersion",\s*"\K[^"]+' version.rc | head -1)
test "$rc_ver" = "$VER" || { echo "version.rc FileVersion ($rc_ver) != $VER" >&2; fail=1; }
rc_fver=$(grep -oP 'FILEVERSION\s+\K[0-9,]+' version.rc | head -1)
test "$rc_fver" = "$(printf '%s' "$VER" | tr . ,),0" || { echo "version.rc FILEVERSION ($rc_fver) != $VER" >&2; fail=1; }
man_ver=$(grep -oP 'assemblyIdentity version="\K[0-9.]+' aec_gui.manifest | head -1)
test "$man_ver" = "${VER}.0" || { echo "manifest version ($man_ver) != $VER" >&2; fail=1; }
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

# ============================================================
#  Code signing (optional, off by default) — runs BEFORE zipping
#  so the signature ships inside the zip (signing after would only
#  sign the staging copy nobody downloads).
#
#  The unsigned build draws AI-heuristic flags (unsigned PE +
#  bundled ML runtimes; standing: SecureAge APEX only — see
#  SECURITY.md). A code-signing certificate
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
    need_cmd signtool "AEC_SIGN=1 requires signtool on PATH"
    [ -n "${AEC_CERT_SHA1:-}" ] || { echo "AEC_SIGN=1 but AEC_CERT_SHA1 unset" >&2; exit 1; }
    signtool sign /v /sha1 "${AEC_CERT_SHA1}" \
        /t "${AEC_TIMESTAMP_URL:-http://timestamp.digicert.com}" \
        "$STAGE/aec_gui.exe"
    echo "signed aec_gui.exe"
fi

rm -f "${RELDIR}/AEC-Client-v${VER}-win64.zip"
(cd "$RELDIR" && zip -qr "AEC-Client-v${VER}-win64.zip" "AEC-Client-v${VER}-win64")

# ---- zip asserts ----
python3 -c "
import zipfile, re, sys
names = zipfile.ZipFile('release/v${VER}/AEC-Client-v${VER}-win64.zip').namelist()
assert len(names) > 15, f'zip suspiciously small ({len(names)})'
roots = sorted({n.split('/')[0] for n in names})
assert roots == ['AEC-Client-v${VER}-win64'], f'zip root layout wrong: {roots}'
pat = re.compile(r'releases/|imgui\.ini|aec_config|dfn|deepfilter|config\.txt|\.pdb$|\.dll\.a$|enrollment|embedding|\.wav$|\.mp3$|\.flac$|libabsl_|libprotobuf|libre2|libonnx\.dll|libutf8|libgomp', re.I)
bad = [n for n in names if pat.search(n)]
assert not bad, f'forbidden files in zip: {bad}'
print(f'zip entries: {len(names)}, single root OK, no strays')
"

echo "----"
du -h "release/v${VER}/AEC-Client-v${VER}-win64.zip"
sha256sum "release/v${VER}/AEC-Client-v${VER}-win64.zip"
echo "RELEASE OK: $STAGE + zip, no releases folder, no strays"
