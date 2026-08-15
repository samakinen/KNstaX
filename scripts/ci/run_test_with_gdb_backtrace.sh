#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

set -euo pipefail

timeout_seconds="${KNX_GDB_BACKTRACE_TIMEOUT_SECONDS:-120}"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <test-binary> [args...]" >&2
    exit 2
fi

test_binary="$1"
shift

if command -v gdb >/dev/null 2>&1; then
    gdb_args=(
        --batch
        --return-child-result
        --quiet
        -ex 'set pagination off'
        -ex 'set print thread-events off'
        -ex 'set confirm off'
        -ex 'handle SIGINT stop nopass'
        -ex run
        -ex 'thread apply all bt full'
        -ex quit
    )

    if [[ "$timeout_seconds" =~ ^[0-9]+$ ]] && [[ "$timeout_seconds" -gt 0 ]]; then
        exec timeout --signal=SIGINT --kill-after=10s "${timeout_seconds}s" gdb "${gdb_args[@]}" --args "$test_binary" "$@"
    fi

    exec gdb "${gdb_args[@]}" --args "$test_binary" "$@"
fi

echo "gdb not found; running test directly without automatic backtrace" >&2
exec "$test_binary" "$@"