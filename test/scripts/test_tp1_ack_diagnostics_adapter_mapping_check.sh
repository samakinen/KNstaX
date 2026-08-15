#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/scripts/ci/tp1_ack_diagnostics_adapter_mapping_check.sh"

if [[ ! -x "$SCRIPT" ]]; then
  echo "Missing or non-executable script: $SCRIPT" >&2
  exit 1
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

pass_src="$tmp_dir/bitbang_medium_backend_adapter_pass.cpp"
cat > "$pass_src" <<'EOF'
void f() {
  event.type = Tp1RxEventType::AckDiagnosticsSnapshot;
  event.ackDiagnostics.windowOpenedNoDecisionCount = stats.windowOpenedNoDecisionCount;
  event.ackDiagnostics.decisionLatchedLateCount = stats.decisionLatchedLateCount;
  event.ackDiagnostics.responseEmittedCount = stats.responseEmittedCount;
  event.ackDiagnostics.deadlineMissCount = stats.deadlineMissCount;
  event.ackDiagnostics.overflowErrorCount = stats.overflowErrorCount;
  event.ackDiagnostics.rxAckObservedCount = stats.rxAckObservedCount;
  event.ackDiagnostics.unsupportedRawIngressCount = stats.unsupportedRawIngressCount;
}
EOF

"$SCRIPT" --source-file "$pass_src"

fail_src="$tmp_dir/bitbang_medium_backend_adapter_fail.cpp"
cat > "$fail_src" <<'EOF'
void f() {
  event.type = Tp1RxEventType::AckDiagnosticsSnapshot;
  event.ackDiagnostics.windowOpenedNoDecisionCount = stats.windowOpenedNoDecisionCount;
  event.ackDiagnostics.decisionLatchedLateCount = stats.decisionLatchedLateCount;
  event.ackDiagnostics.responseEmittedCount = stats.responseEmittedCount;
  event.ackDiagnostics.deadlineMissCount = stats.deadlineMissCount;
  event.ackDiagnostics.overflowErrorCount = stats.overflowErrorCount;
  event.ackDiagnostics.rxAckObservedCount = stats.rxAckObservedCount;
}
EOF

if "$SCRIPT" --source-file "$fail_src" >/dev/null 2>&1; then
  echo "Expected diagnostics adapter mapping check failure did not occur for fail fixture" >&2
  exit 1
fi

echo "tp1_ack_diagnostics_adapter_mapping_check regression test: PASS"
