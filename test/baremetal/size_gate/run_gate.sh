#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
#
# Bare-metal size gate, start to finish:
#   1. link the reference device with LTO and read flash and static RAM from the map;
#   2. link the emulator variant, run it under QEMU and read the heap high-water mark;
#   3. check both against size_budget.json (scripts/ci/knstax_size_gate.py).
#
# Needs arm-none-eabi-gcc, cmake, ninja and python3. Step 2 also needs
# qemu-system-arm; without it the gate checks flash and static RAM only and
# says so. Exit status: 0 within budget, 1 over budget, 2 the gate could not run.
#
# Usage: run_gate.sh <build-dir>
#
# Inside the h8i8o toolchain image (add qemu-system-arm to it for the heap step):
#   MSYS_NO_PATHCONV=1 docker run --rm -v <KNstaX>:/src -v <out>:/out <image> \
#       sh /src/test/baremetal/size_gate/run_gate.sh /out

set -u

BUILD=${1:?usage: run_gate.sh <build-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TOOLCHAIN="$ROOT/cmake/toolchains/arm-none-eabi.cmake"

cmake -S "$HERE" -B "$BUILD/gate" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" >/dev/null \
    && cmake --build "$BUILD/gate" || { echo "error: the reference device did not link" >&2; exit 2; }

HEAP_ARG=""
if command -v qemu-system-arm >/dev/null 2>&1; then
    cmake -S "$HERE" -B "$BUILD/gate_qemu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
          -DKNX_SIZE_GATE_SEMIHOST=ON >/dev/null \
        && cmake --build "$BUILD/gate_qemu" >/dev/null \
        || { echo "error: the emulator variant did not link" >&2; exit 2; }
    RUN=$(timeout 60 qemu-system-arm -M mps2-an385 -cpu cortex-m3 -nographic \
          -semihosting-config enable=on,target=native -kernel "$BUILD/gate_qemu/size_gate.elf" 2>&1 | tr -d '\r')
    echo "$RUN"
    HEAP=$(echo "$RUN" | sed -n 's/^heap_high_water=\([0-9][0-9]*\).*/\1/p')
    [ -n "$HEAP" ] || { echo "error: the emulator run reported no heap figure" >&2; exit 2; }
    HEAP_ARG="--heap-high-water $HEAP"
else
    echo "note: qemu-system-arm not found; the heap is not measured and RAM is checked for static data only"
fi

# shellcheck disable=SC2086
python3 -I "$ROOT/scripts/ci/knstax_size_gate.py" \
    --map "$BUILD/gate/size_gate.elf.map" --budget "$HERE/size_budget.json" $HEAP_ARG
