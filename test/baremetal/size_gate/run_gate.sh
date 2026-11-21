#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
#
# Bare-metal size gate for one reference device, start to finish:
#   1. link the reference device with LTO and read flash and static RAM from the map;
#   2. on the Cortex-M3 and Cortex-M4 configurations, link the emulator variant, run it under QEMU
#      and read the heap high-water mark (Cortex-M0+ reports "heap not measured");
#   3. check the figures against the budget file of the configuration
#      (scripts/ci/knstax_size_gate.py), printing the resolved knx.config first.
#
# The reference devices (small-target-profile.md section 7) and their budgets:
#
#   profile       variant  core           budget file                         heap run
#   tp1-small     -        cortex-m3      budgets/tp1-small-m3.json           QEMU mps2-an385
#   tp1-standard  -        cortex-m4      budgets/tp1-standard-m4.json        QEMU mps2-an386
#   tp1-small     -        cortex-m0plus  budgets/tp1-small-m0plus.json       no
#   tp1-small     secure   cortex-m3      budgets/tp1-small-secure-m3.json    QEMU mps2-an385
#
# A variant applies variants/<variant>.defconfig over the profile (secure: Data
# Secure with the software AES and APDUs of 55 octets).
#
# Needs arm-none-eabi-gcc, cmake, ninja and python3. The heap run also needs
# qemu-system-arm; without it the gate checks flash and static RAM only and
# says so. Exit status: 0 within budget, 1 over budget, 2 the gate could not run.
#
# Usage: run_gate.sh <build-dir> [profile [cpu [variant]]]
#   profile   default $KNX_SIZE_GATE_PROFILE, else tp1-small
#   cpu       default $KNX_ARM_CPU, else cortex-m3 (the toolchain file's default)
#   variant   default $KNX_SIZE_GATE_VARIANT, else none
#   $KNX_SIZE_GATE_BUDGET       a budget file to use instead of the one named from profile and cpu
#   $KNX_SIZE_GATE_KNSTAX_ROOT  measure another KNstaX tree (CI: the base of a pull request)
#   $KNX_SIZE_GATE_BASELINE     the gate_report.json of an earlier run; the change is printed
#
# Writes <build-dir>/gate_report.txt (what is printed) and <build-dir>/gate_report.json,
# and leaves the map and ELF in <build-dir>/gate/.
#
# Inside the toolchain image (needs qemu-system-arm for the heap step):
#   MSYS_NO_PATHCONV=1 docker run --rm -v <KNstaX>:/src -v <out>:/out <image> \
#       sh /src/test/baremetal/size_gate/run_gate.sh /out tp1-standard cortex-m4

set -u

BUILD=${1:?usage: run_gate.sh <build-dir> [profile [cpu [variant]]]}
PROFILE=${2:-${KNX_SIZE_GATE_PROFILE:-tp1-small}}
CPU=${3:-${KNX_ARM_CPU:-cortex-m3}}
VARIANT=${4:-${KNX_SIZE_GATE_VARIANT:-}}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TOOLCHAIN="$ROOT/cmake/toolchains/arm-none-eabi.cmake"

case "$CPU" in
    cortex-m3) CORE=m3 ;;
    cortex-m4) CORE=m4 ;;
    cortex-m0plus) CORE=m0plus ;;
    *) CORE=$CPU ;;
esac
BUDGET=${KNX_SIZE_GATE_BUDGET:-$HERE/budgets/$PROFILE${VARIANT:+-$VARIANT}-$CORE.json}
[ -f "$BUDGET" ] || { echo "error: no budget file $BUDGET for $PROFILE${VARIANT:+ ($VARIANT)} on $CPU" >&2; exit 2; }

mkdir -p "$BUILD" || exit 2
CONFIGURE_ARGS="-DKNX_SIZE_GATE_PROFILE=$PROFILE -DKNX_SIZE_GATE_VARIANT=$VARIANT -DKNX_ARM_CPU=$CPU"
[ -z "${KNX_SIZE_GATE_KNSTAX_ROOT:-}" ] || CONFIGURE_ARGS="$CONFIGURE_ARGS -DKNX_SIZE_GATE_KNSTAX_ROOT=$KNX_SIZE_GATE_KNSTAX_ROOT"

# shellcheck disable=SC2086
cmake -S "$HERE" -B "$BUILD/gate" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" $CONFIGURE_ARGS >/dev/null \
    && cmake --build "$BUILD/gate" || { echo "error: the reference device did not link" >&2; exit 2; }

# Code placed in RAM (KNX_ISR_RAM) keeps running while flash is erased only if it
# never calls into flash. The linker reaches flash from RAM through a veneer it
# places next to the caller, so a veneer at a RAM address is such a call.
RAM_VENEERS=$(arm-none-eabi-nm -C "$BUILD/gate/size_gate.elf" \
    | awk '$1 >= "20000000" && $1 < "40000000" && /_veneer/ {print $3}')
if [ -n "$RAM_VENEERS" ]; then
    echo "error: code placed in RAM calls into flash: $RAM_VENEERS" >&2
    exit 1
fi

HEAP_ARG=""
case "$CPU" in
    cortex-m3) QEMU_MACHINE=mps2-an385 ;;
    cortex-m4) QEMU_MACHINE=mps2-an386 ;;
    *) QEMU_MACHINE= ;;
esac
if [ -z "$QEMU_MACHINE" ]; then
    echo "note: heap not measured for $CPU (the emulator run covers Cortex-M3 and Cortex-M4; QEMU has no Cortex-M0+ machine in the set used here)"
elif command -v qemu-system-arm >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    cmake -S "$HERE" -B "$BUILD/gate_qemu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" $CONFIGURE_ARGS \
          -DKNX_SIZE_GATE_SEMIHOST=ON >/dev/null \
        && cmake --build "$BUILD/gate_qemu" >/dev/null \
        || { echo "error: the emulator variant did not link" >&2; exit 2; }
    RUN=$(timeout 60 qemu-system-arm -M "$QEMU_MACHINE" -cpu "$CPU" -nographic \
          -semihosting-config enable=on,target=native -kernel "$BUILD/gate_qemu/size_gate.elf" 2>&1 | tr -d '\r')
    echo "$RUN"
    HEAP=$(echo "$RUN" | sed -n 's/^heap_high_water=\([0-9][0-9]*\).*/\1/p')
    [ -n "$HEAP" ] || { echo "error: the emulator run reported no heap figure" >&2; exit 2; }
    # A heap figure from a run in which the scripted ETS download did not take effect says
    # nothing about the download, so it is not a result.
    echo "$RUN" | grep -q '^download_applied=1' \
        || { echo "error: the emulator run's ETS download did not take effect (see download_* above)" >&2; exit 2; }
    HEAP_ARG="--heap-high-water $HEAP"
else
    echo "note: qemu-system-arm not found; the heap is not measured and RAM is checked for static data only"
fi

CONFIG="$BUILD/gate/knx-config/knx.config"
[ -f "$CONFIG" ] || CONFIG=$(find "$BUILD/gate" -name knx.config 2>/dev/null | head -n 1)

BASELINE_ARG=""
[ -z "${KNX_SIZE_GATE_BASELINE:-}" ] || BASELINE_ARG="--baseline $KNX_SIZE_GATE_BASELINE"

# shellcheck disable=SC2086
python3 -I "$ROOT/scripts/ci/knstax_size_gate.py" \
    --map "$BUILD/gate/size_gate.elf.map" --budget "$BUDGET" --elf "$BUILD/gate/size_gate.elf" \
    --profile "$PROFILE${VARIANT:+-$VARIANT}" --cpu "$CPU" --config "$CONFIG" --json "$BUILD/gate_report.json" \
    $HEAP_ARG $BASELINE_ARG > "$BUILD/gate_report.txt"
STATUS=$?
cat "$BUILD/gate_report.txt"
exit $STATUS
