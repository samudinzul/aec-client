#!/usr/bin/env bash
# aec-web starter (Linux/macOS) — ./web/start.sh
# Creates a local venv on first run, installs packages, starts the server.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -x .venv/bin/python ]; then
  echo "[aec-web] creating local Python environment (first run only)..."
  python3 -m venv .venv
fi
.venv/bin/pip install -q -r web/requirements.txt
echo "[aec-web] starting server — open http://localhost:8000"
exec .venv/bin/python -m web.server
