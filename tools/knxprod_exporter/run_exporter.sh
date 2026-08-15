#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

# Wrapper: run C++ exporter to produce JSON, then run Python exporter
CPP_BIN="$REPO_ROOT/tools/product_exporter/product_exporter"
PY_EXPORTER="$SCRIPT_DIR/exporter.py"
OUT_DIR="${KNXPROD_EXPORTER_OUT_DIR:-$REPO_ROOT/build/knxprod_exporter_artifacts}"

if [ ! -x "$CPP_BIN" ]; then
  echo "C++ exporter not found or not built: $CPP_BIN" >&2
  echo "Build with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

"$CPP_BIN"
python3 "$PY_EXPORTER" --format knxprod --input pilot_a_export.json pilot_b_export.json
python3 "$PY_EXPORTER" --format kaenx --input pilot_a_export.json pilot_b_export.json
