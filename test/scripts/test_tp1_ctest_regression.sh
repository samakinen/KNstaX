#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/ci/tp1_ctest_regression.sh"
# The build tree to check; CTest passes its own, so the gate is checked against
# the tests this build actually registered rather than whatever an in-tree
# build/ directory last held.
BUILD_DIR="${1:-$ROOT_DIR/build}"

if [[ ! -x "$SCRIPT" ]]; then
  echo "Missing or non-executable script: $SCRIPT" >&2
  exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Build directory not found for regression test: $BUILD_DIR" >&2
  exit 1
fi
# A test that needs a feature this build has configured out is not registered
# (knx_add_test NEEDS ...), so it is not required either; knx_tests_not_registered.txt
# in the build tree lists what a profile leaves out.
REQUIRED_TESTS=(
  test_tp1_frame_golden
  test_tp1_datalink_frame_pool
  test_bitbang_mac_physical
  test_tp1_mac_physical
  test_tp1_integration
  test_tp1_to_ip_routing_coupler
  test_ip_tp1_routing
  test_ip_routing_physical
  test_tp1_ack_diagnostics_adapter_mapping_check_script
  test_tp1_benchmark_gate_script
)
REQUIRE_ARGS=()
for test_name in "${REQUIRED_TESTS[@]}"; do
  if [[ -f "$BUILD_DIR/knx_tests_not_registered.txt" ]] \
     && grep -q "^${test_name} " "$BUILD_DIR/knx_tests_not_registered.txt"; then
    continue
  fi
  REQUIRE_ARGS+=(--require-test "$test_name")
done

"$SCRIPT" --build-dir "$BUILD_DIR" "${REQUIRE_ARGS[@]}" --preflight-only

if "$SCRIPT" --build-dir "$BUILD_DIR" --require-test test_tp1_nonexistent_required_gate --preflight-only >/dev/null 2>&1; then
  echo "Expected TP1 required-test gate failure did not occur for nonexistent required test" >&2
  exit 1
fi

echo "tp1_ctest_regression gate script test: PASS"
