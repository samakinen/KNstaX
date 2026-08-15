#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

# Run the TP1 benchmark executable and verify that the expected benchmark rows
# and non-zero metrics are present.
# Usage:
#   ./scripts/ci/tp1_benchmark_gate.sh [--build-dir build]
#   ./scripts/ci/tp1_benchmark_gate.sh [--benchmark ./build/benchmark_tp1]

build_dir="build"
benchmark_path=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --benchmark)
      benchmark_path="${2:-}"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [--build-dir <dir>] [--benchmark <path>]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$benchmark_path" ]]; then
  benchmark_path="$build_dir/benchmark_tp1"
fi

if [[ ! -x "$benchmark_path" ]]; then
  echo "Benchmark executable not found or not executable: $benchmark_path" >&2
  exit 2
fi

output="$($benchmark_path)"
printf '%s\n' "$output"

required_rows=(
  "TP1 MAC RX enqueue + dequeue"
  "TP1 MAC overflow retain-latest"
)

for row in "${required_rows[@]}"; do
  if ! grep -F -q "$row" <<< "$output"; then
    echo "Missing required TP1 benchmark row: $row" >&2
    exit 1
  fi
done

awk '
  /TP1 MAC RX enqueue \+ dequeue|TP1 MAC overflow retain-latest/ {
    ops = $(NF-2) + 0;
    us_per_op = $(NF-1) + 0;
    ops_per_sec = $NF + 0;
    if (ops <= 0 || us_per_op <= 0 || ops_per_sec <= 0) {
      printf("Non-positive TP1 benchmark metric in row: %s\n", $0) > "/dev/stderr";
      exit 1;
    }

    # Minimum throughput thresholds — set conservatively (~10% of baseline on a
    # Linux dev container) so routine CI variance does not trip the gate, but
    # algorithmic regressions (O(n²) loops, accidental global locks, etc.) are
    # caught before they reach production.
    #
    # Baseline (2026-05-01, Linux x86-64 dev container):
    #   TP1 MAC RX enqueue + dequeue    : ~1,672,000 ops/sec
    #   TP1 MAC overflow retain-latest  :   ~167,000 ops/sec
    if (/TP1 MAC RX enqueue \+ dequeue/ && ops_per_sec < 100000) {
      printf("FAIL: TP1 MAC RX enqueue+dequeue throughput %.0f ops/sec is below minimum 100000 ops/sec\n", ops_per_sec) > "/dev/stderr";
      exit 1;
    }
    if (/TP1 MAC overflow retain-latest/ && ops_per_sec < 10000) {
      printf("FAIL: TP1 MAC overflow retain-latest throughput %.0f ops/sec is below minimum 10000 ops/sec\n", ops_per_sec) > "/dev/stderr";
      exit 1;
    }

    matched++;
  }
  END {
    if (matched < 2) {
      printf("Expected 2 TP1 benchmark rows, found %d\n", matched) > "/dev/stderr";
      exit 1;
    }
  }
' <<< "$output"

echo "PASS: TP1 benchmark gate"