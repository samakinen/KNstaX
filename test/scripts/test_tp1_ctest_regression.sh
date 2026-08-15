#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/ci/tp1_ctest_regression.sh"

if [[ ! -x "$SCRIPT" ]]; then
  echo "Missing or non-executable script: $SCRIPT" >&2
  exit 1
fi

if [[ ! -d "$ROOT_DIR/build" ]]; then
  echo "Build directory not found for regression test: $ROOT_DIR/build" >&2
  exit 1
fi

"$SCRIPT" --build-dir "$ROOT_DIR/build" \
  --require-test test_tp1_frame_codec \
  --require-test test_tp1_frame_golden \
  --require-test test_tp1_datalink_frame_pool \
  --require-test test_tpuart_medium_backend_adapter \
  --require-test test_bitbang_mac_physical \
  --require-test test_tpuart_mac_physical \
  --require-test test_physical_factory_tp1 \
  --require-test test_tp1_mac_controller \
  --require-test test_tp1_integration \
  --require-test test_tp1_to_ip_routing_coupler \
  --require-test test_ip_tp1_routing \
  --require-test test_ip_routing_physical \
  --require-test test_tp1_ack_diagnostics_adapter_mapping_check_script \
  --require-test test_tp1_benchmark_gate_script \
  --preflight-only

if "$SCRIPT" --build-dir "$ROOT_DIR/build" --require-test test_tp1_nonexistent_required_gate --preflight-only >/dev/null 2>&1; then
  echo "Expected TP1 required-test gate failure did not occur for nonexistent required test" >&2
  exit 1
fi

echo "tp1_ctest_regression gate script test: PASS"
