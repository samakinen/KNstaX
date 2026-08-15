#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

# Enforce that BitBang medium backend diagnostics snapshot mapping includes required counters.
# Usage:
#   ./scripts/ci/tp1_ack_diagnostics_adapter_mapping_check.sh \
#     [--source-file src/physical/bitbang_medium_backend_adapter.cpp]

source_file="src/physical/bitbang_medium_backend_adapter.cpp"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-file)
      source_file="${2:-}"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [--source-file <path>]"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -f "$source_file" ]]; then
  echo "Source file not found: $source_file" >&2
  exit 2
fi

required_patterns=(
  'event\.type = Tp1RxEventType::AckDiagnosticsSnapshot;'
  'event\.ackDiagnostics\.windowOpenedNoDecisionCount = stats\.windowOpenedNoDecisionCount;'
  'event\.ackDiagnostics\.decisionLatchedLateCount = stats\.decisionLatchedLateCount;'
  'event\.ackDiagnostics\.responseEmittedCount = stats\.responseEmittedCount;'
  'event\.ackDiagnostics\.deadlineMissCount = stats\.deadlineMissCount;'
  'event\.ackDiagnostics\.overflowErrorCount = stats\.overflowErrorCount;'
  'event\.ackDiagnostics\.rxAckObservedCount = stats\.rxAckObservedCount;'
  'event\.ackDiagnostics\.unsupportedRawIngressCount = stats\.unsupportedRawIngressCount;'
)

for pattern in "${required_patterns[@]}"; do
  if ! grep -E -q "$pattern" "$source_file"; then
    echo "FAIL: required diagnostics mapping pattern missing: $pattern" >&2
    echo "  file: $source_file" >&2
    exit 1
  fi
done

echo "PASS: TP1 diagnostics adapter mapping patterns are present ($source_file)"
