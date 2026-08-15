#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

# Run TP1-labeled tests from CTest.
# Usage:
#   ./scripts/ci/tp1_ctest_regression.sh [--build-dir build]
#   ./scripts/ci/tp1_ctest_regression.sh [--build-dir build] [--require-test <name>]...
#   ./scripts/ci/tp1_ctest_regression.sh [--build-dir build] [--preflight-only]

build_dir="build"
preflight_only=0
required_tests=(
  "test_tp1_frame_codec"
  "test_tp1_frame_golden"
  "test_tp1_datalink_frame_pool"
  "test_tpuart_medium_backend_adapter"
  "test_bitbang_mac_physical"
  "test_tpuart_mac_physical"
  "test_physical_factory_tp1"
  "test_tp1_mac_controller"
  "test_tp1_integration"
  "test_tp1_to_ip_routing_coupler"
  "test_ip_tp1_routing"
  "test_ip_routing_physical"
  "test_tp1_ack_diagnostics_adapter_mapping_check_script"
  "test_tp1_benchmark_gate_script"
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --require-test)
      required_tests+=("${2:-}")
      shift 2
      ;;
    --preflight-only)
      preflight_only=1
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--build-dir <dir>] [--require-test <name>]... [--preflight-only]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -d "$build_dir" ]]; then
  echo "Build directory not found: $build_dir" >&2
  exit 2
fi

listed_tests=$(ctest --test-dir "$build_dir" -N -L tp1 2>/dev/null || true)
for required_test in "${required_tests[@]}"; do
  if ! grep -q "$required_test" <<< "$listed_tests"; then
    echo "Required TP1 regression test missing from tp1 label set: $required_test" >&2
    exit 1
  fi
done

if [[ "$preflight_only" == "1" ]]; then
  echo "PASS: TP1 regression preflight checks"
  exit 0
fi

"$(dirname "$0")/ctest_with_backtraces.sh" --test-dir "$build_dir" --output-on-failure -L tp1
