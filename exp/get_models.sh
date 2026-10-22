#!/usr/bin/env bash
# Downloads experiment weights into exp/models/ (gitignored, never shipped).
# Run from the repo root:  bash exp/get_models.sh
set -euo pipefail
OUT="$(dirname "$0")/models"
mkdir -p "$OUT"

have() { test -s "$1" && echo "keep: $1" || return 1; }

# GTCRN streaming NS (~0.5 MB, MIT) — official Xiaobin-Rong/gtcrn graph.
have "$OUT/gtcrn_simple.onnx" || curl -L -o "$OUT/gtcrn_simple.onnx" \
  "https://raw.githubusercontent.com/Xiaobin-Rong/gtcrn/main/stream/onnx_models/gtcrn_simple.onnx"

# LocalVQE v1.4-AEC echo-only GGUF (~3 MB, Apache-2.0) — reference weights.
have "$OUT/localvqe-v1.4-aec-200K-f32.gguf" || curl -L -o "$OUT/localvqe-v1.4-aec-200K-f32.gguf" \
  "https://huggingface.co/LocalAI-io/LocalVQE/resolve/main/localvqe-v1.4-aec-200K-f32.gguf"

# LocalVQE v1.4-AEC portable ONNX mask + frontend (~1 MB + JSON, Apache-2.0).
have "$OUT/LocalVQEAECResidualMask.onnx" || curl -L -o "$OUT/LocalVQEAECResidualMask.onnx" \
  "https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32/resolve/main/LocalVQEAECResidualMask.onnx"
have "$OUT/LocalVQEAECFrontend.json" || curl -L -o "$OUT/LocalVQEAECFrontend.json" \
  "https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32/resolve/main/LocalVQEAECFrontend.json"
have "$OUT/LocalVQEAECConfig.json" || curl -L -o "$OUT/LocalVQEAECConfig.json" \
  "https://huggingface.co/soniqo/LocalVQE-v1.4-AEC-200K-ONNX-FP32/resolve/main/config.json"

# NovaSR super-resolution source weights (~53 KB, Apache-2.0). The desktop
# harness runs the baked ONNX (exp/novasr/novasr.onnx, exported + ORT
# parity-checked 1.000000, ~11x CPU RTF) — this .bin is the refresh
# source if the upstream checkpoint ever moves. Provenance:
# https://huggingface.co/YatharthS/NovaSR
# Code: https://github.com/ysharma3501/NovaSR
have "$OUT/pytorch_model_v1.bin" || curl -L -o "$OUT/pytorch_model_v1.bin" \
  "https://huggingface.co/YatharthS/NovaSR/resolve/main/pytorch_model_v1.bin"

echo "---"; ls -lh "$OUT"; sha256sum "$OUT"/* 2>/dev/null | sed 's#.*/##' || true
