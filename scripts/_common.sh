#!/usr/bin/env bash
# _common.sh — shared usage/flag helpers for the scripts/ tools.
#
# Sourced by every script in this folder; never executed directly.
#
# FLAG CONVENTIONS (one vocabulary across all tools — a tool only
# accepts the flags its own header documents, everything else is a
# usage error):
#   -h, --help      print the tool's header block and exit 0
#   -v, --verbose   stream full tool output / trace every command
#   --fresh         wipe generated state before running (build tools)
#   --run           build, then launch the app (build tools)
#   --exp           build only aec_exp.exe (desktop build)
#   --with-smoke    also build the smoke tools (desktop build)
#   --native-only   run only the native suite (test.sh)
#   --web-only      run only the web suites (test.sh)
#   --              end of flags; later args are positional
#   [version]       release version, positional (release tools)
#
# EXIT CODES: 0 success, 1 runtime failure, 2 usage error. The split
# lets CI tell "you called it wrong" from "the build failed".
#
# HELP TEXT is generated from each tool's own header comments
# (line 2 up to — but excluding — `set -euo pipefail`), so editing a
# header can never truncate or desync --help again.

# usage_from_header <script-path>: print the header comment block with
# the leading "# " stripped. No hardcoded line ranges.
usage_from_header() {
    sed -n '2,/^set -euo pipefail$/p' "$1" | sed '$d' | sed -E 's/^# ?//'
}

# die <message>: runtime failure (bad state, missing model, build gone
# wrong). Exit 1.
die() {
    printf '%s\n' "$1" >&2
    exit 1
}

# usage_error <message>: command-line misuse (unknown flag, too many
# arguments). Exit 2, and always points at --help.
usage_error() {
    printf '%s (see --help)\n' "$1" >&2
    exit 2
}

# require_file <path>: the tool must run from the repo root.
require_file() {
    [ -f "$1" ] || die "run from the repo root (missing file: $1)"
}

# require_dir <path>: the tool must run from the repo root.
require_dir() {
    [ -d "$1" ] || die "run from the repo root (missing dir: $1)"
}

# need_cmd <command> [install hint]: standard missing-tool failure.
need_cmd() {
    command -v "$1" >/dev/null 2>&1 \
        || die "$1 not found${2:+ ($2)}"
}
