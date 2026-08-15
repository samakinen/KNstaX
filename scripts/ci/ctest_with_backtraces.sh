#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 --test-dir <build-dir> [ctest-args...]" >&2
    exit 2
fi

build_dir=""
ctest_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test-dir)
            build_dir="${2:-}"
            shift 2
            ;;
        *)
            ctest_args+=("$1")
            shift
            ;;
    esac
done

if [[ -z "$build_dir" ]]; then
    echo "missing --test-dir <build-dir>" >&2
    exit 2
fi

if [[ ! -d "$build_dir" ]]; then
    echo "build directory not found: $build_dir" >&2
    exit 2
fi

set +e
ctest --test-dir "$build_dir" "${ctest_args[@]}"
ctest_status=$?
set -e

if [[ $ctest_status -eq 0 ]]; then
    exit 0
fi

failed_log="$build_dir/Testing/Temporary/LastTestsFailed.log"
if [[ ! -f "$failed_log" ]]; then
    echo "No failed test log found at $failed_log" >&2
    exit "$ctest_status"
fi

mapfile -t failed_tests < <(awk -F: 'NF >= 2 { print $2 }' "$failed_log" | sed '/^$/d')
if [[ ${#failed_tests[@]} -eq 0 ]]; then
    exit "$ctest_status"
fi

python3 - "$build_dir" "${failed_tests[@]}" <<'PY'
import os
import pathlib
import re
import shlex
import subprocess
import sys

build_dir = pathlib.Path(sys.argv[1])
failed_tests = sys.argv[2:]

ctest_file = build_dir / "CTestTestfile.cmake"
if not ctest_file.exists():
    print(f"No CTestTestfile.cmake found at {ctest_file}", file=sys.stderr)
    sys.exit(0)

content = ctest_file.read_text(encoding="utf-8", errors="replace")
launcher = pathlib.Path("/workspaces/KNstaX/scripts/ci/run_test_with_gdb_backtrace.sh")

for test_name in failed_tests:
    match = re.search(rf'add_test\(\[=\[{re.escape(test_name)}\]=\]\s+(.*?)\)', content)
    if not match:
        print(f"Unable to locate command for failed test: {test_name}", file=sys.stderr)
        continue

    command = shlex.split(match.group(1))
    if not command:
        continue

    print(f"\n==> Replaying failed test under backtrace launcher: {test_name}")
    env = os.environ.copy()
    env.setdefault("KNX_GDB_BACKTRACE_TIMEOUT_SECONDS", "120")

    if len(command) == 1 and os.access(command[0], os.X_OK):
        subprocess.run([str(launcher), command[0]], cwd=build_dir, env=env, check=False)
        continue

    subprocess.run(command, cwd=build_dir, env=env, check=False)
PY

exit "$ctest_status"