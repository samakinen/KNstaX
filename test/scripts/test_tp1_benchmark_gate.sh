#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/ci/tp1_benchmark_gate.sh"

if [[ ! -f "$SCRIPT" ]]; then
  echo "Missing script: $SCRIPT" >&2
  exit 1
fi

if [[ ! -x "$ROOT_DIR/build/benchmark_tp1" ]]; then
  echo "TP1 benchmark executable not found for gate test: $ROOT_DIR/build/benchmark_tp1" >&2
  exit 1
fi

bash "$SCRIPT" --benchmark "$ROOT_DIR/build/benchmark_tp1"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

bad_benchmark="$tmp_dir/bad_benchmark_tp1.sh"
cat > "$bad_benchmark" <<'EOF'
#!/usr/bin/env bash
cat <<'OUT'
========================================================================================
TP1 Performance Benchmarks
========================================================================================

Operation                                             ops           µs/op         ops/sec
----------------------------------------------------------------------------------------
TP1 MAC RX enqueue + dequeue                            0           0.000             0.0
TP1 MAC overflow retain-latest                         72           1.000             0.0

========================================================================================
OUT
EOF
chmod +x "$bad_benchmark"

if bash "$SCRIPT" --benchmark "$bad_benchmark" >/dev/null 2>&1; then
  echo "Expected TP1 benchmark gate failure did not occur for invalid metrics fixture" >&2
  exit 1
fi

echo "tp1_benchmark_gate regression test: PASS"