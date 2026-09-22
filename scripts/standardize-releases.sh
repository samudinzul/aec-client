#!/usr/bin/env bash
# standardize-releases.sh — align every GitHub release with release-template.md
# (gold standard: the v1.5.0 release).
#
#   Titles  -> "AEC Client vX.Y.Z" for every release.
#   Notes   -> only the current latest is rewritten to the template
#              (old releases keep their historical notes, per the template).
#
# Run from a shell where `gh auth login` has been done (MSYS2 UCRT64).
set -euo pipefail

REPO=samudinzul/aec-client

command -v gh >/dev/null 2>&1 || { echo "error: gh not found" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "error: not authenticated — run: gh auth login" >&2; exit 1; }

# ------------------------------------------------------------------
# Current latest: v1.6.0 — standard title + notes rewritten to template
# ------------------------------------------------------------------
# Notes go to a RELATIVE path: gh is a native Windows exe and cannot
# open POSIX mktemp paths like /tmp/... (MSYS path conversion skips
# them). A repo-relative file needs no conversion on any shell.
mkdir -p release
NOTES="release/.notes-v1.6.0.md"
trap 'rm -f "$NOTES"' EXIT

cat > "$NOTES" <<'EOF'
AEC Client v1.6.0 (Windows 10/11 64-bit).

Highlights since v1.5.0:
- Noise reduction checkbox cuts background hiss and fans on WebRTC AEC3 and NKF-AEC — off by default, toggles live
- SpeexDSP + LocalVQE removed after failing the double-talk voice test; old settings migrate to AEC3 automatically, three engines remain
- Owner-only voice gate removed — the standard voice gate (Push down silence) is unchanged
- AEC3 loud-speaker caveat documented: very loud speakers can clip your voice on AEC3 — lower them or switch to NKF
- Engine ranking renamed to outcomes: Voice preservation / Echo removal

Install:
1. Install VB-CABLE (https://vb-audio.com/Cable/, free) and reboot.
2. Download AEC-Client-v1.6.0-win64.zip from Assets below and extract it anywhere.
3. Run aec_gui.exe, pick your devices, click Start.
4. In Discord: input = CABLE Output, Input Profile = Voice Isolation.
   (Zoom/Teams: input = CABLE Output; turn off their echo cancellation.)

SmartScreen notice: this app is unsigned (no paid code-signing certificate), so Windows may show
"Windows protected your PC" on first launch. This is expected: click More info -> Run anyway.
Every release is built straight from public source (https://github.com/samudinzul/aec-client) —
audit it, rebuild it, or scan the ZIP on VirusTotal if unsure.

Full changelog in CHANGELOG.md.
Compare: https://github.com/samudinzul/aec-client/compare/v1.5.0...v1.6.0
EOF

echo "== titles: standardize all releases =="
for t in v1.0.0 v1.1.0 v1.1.1 v1.2.0 v1.2.1 v1.3.0 v1.3.1 v1.3.2 v1.4.0 v1.5.0; do
    gh release edit "$t" --repo "$REPO" --title "AEC Client $t" >/dev/null
    echo "   AEC Client $t"
done

echo "== v1.6.0: standard title + template notes =="
gh release edit v1.6.0 --repo "$REPO" \
    --title "AEC Client v1.6.0" \
    --notes-file "$NOTES" >/dev/null
echo "   AEC Client v1.6.0 (notes rewritten to release-template.md)"

echo
echo "done. verify with:"
echo "  gh release list -R $REPO"
