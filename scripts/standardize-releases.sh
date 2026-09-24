#!/usr/bin/env bash
# standardize-releases.sh — align every GitHub release title with
# release-template.md ("AEC Client vX.Y.Z"), and optionally rewrite
# the NOTES of one release (normally the current latest) to the template.
#
#   Usage:
#     scripts/standardize-releases.sh              # titles only
#     scripts/standardize-releases.sh v1.7.2       # titles + rewrite that tag's notes
#     scripts/standardize-releases.sh v1.7.2 path/to/notes.md
#
#   Notes file is optional when VERSION is given: without it the script
#   only fixes the title for that tag. With it, notes are rewritten from
#   the file (use scripts/release-template.md + CHANGELOG for content).
#
#   Old releases keep their historical notes (per the template).
#   Run from a shell where `gh auth login` has been done (MSYS2 UCRT64).
set -euo pipefail

REPO=samudinzul/aec-client
VERSION="${1:-}"
NOTES_SRC="${2:-}"

command -v gh >/dev/null 2>&1 || { echo "error: gh not found" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "error: not authenticated — run: gh auth login" >&2; exit 1; }

# Normalize bare "1.7.2" → "v1.7.2"
if [ -n "$VERSION" ]; then
    case "$VERSION" in
        v*) ;;
        *) VERSION="v${VERSION}" ;;
    esac
fi

echo "== titles: standardize all releases =="
# Every tag that exists on the remote (sorted)
while IFS= read -r t; do
    [ -n "$t" ] || continue
    gh release edit "$t" --repo "$REPO" --title "AEC Client $t" >/dev/null
    echo "   AEC Client $t"
done < <(gh release list --repo "$REPO" --limit 100 \
            | awk -F'\t' '{print $NF}' \
            | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' \
            | sort -V)

if [ -n "$VERSION" ]; then
    if [ -z "$NOTES_SRC" ]; then
        echo "== ${VERSION}: title only (no notes file given) =="
        gh release edit "$VERSION" --repo "$REPO" \
            --title "AEC Client ${VERSION}" >/dev/null
        echo "   AEC Client ${VERSION} (notes left as-is)"
    else
        test -f "$NOTES_SRC" || { echo "error: notes file not found: $NOTES_SRC" >&2; exit 1; }
        # Repo-relative notes path — gh (native Windows) cannot open POSIX /tmp/...
        mkdir -p release
        NOTES="release/.notes-${VERSION}.md"
        cp "$NOTES_SRC" "$NOTES"
        trap 'rm -f "$NOTES"' EXIT
        echo "== ${VERSION}: standard title + notes from ${NOTES_SRC} =="
        gh release edit "$VERSION" --repo "$REPO" \
            --title "AEC Client ${VERSION}" \
            --notes-file "$NOTES" >/dev/null
        echo "   AEC Client ${VERSION} (notes rewritten)"
    fi
fi

echo
echo "done. verify with:"
echo "  gh release list -R $REPO"
