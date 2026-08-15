#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Flash-size budget for the KNstaX component in a linked ESP-IDF image.

Why this exists: a TP1-only firmware silently linked ~31 KB of KNXnet/IP code
for a long time, and the datapoint-type registry pulled in every codec whether
the product used it or not. Neither showed up as a build failure — they were
visible only by reading the map file, which nobody does routinely. This gate
makes footprint a build-breaking property instead of an archaeology exercise.

Usage:
  knstax_size_budget.py --map build/app.map [--budget BYTES] [--forbid SUBSTR]...

--forbid is the sharper tool: it fails when a named object reaches the link at
all. Use it to assert that a medium switched off in Kconfig is genuinely absent
rather than merely small.
"""
import argparse
import re
import sys

# Only the region after this marker counts. The archive-member listing above it
# repeats sizes for members that were never placed.
MAP_BODY_MARKER = 'Linker script and memory map'
CONTRIBUTION = re.compile(
    r'^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+\S*libKNstaX\.a\(([^)]+)\)')


def collect(map_path):
    sizes = {}
    started = False
    with open(map_path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            if not started:
                started = line.startswith(MAP_BODY_MARKER)
                continue
            match = CONTRIBUTION.match(line)
            if match:
                sizes[match.group(2)] = sizes.get(match.group(2), 0) + int(match.group(1), 16)
    return sizes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--map', required=True)
    parser.add_argument('--budget', type=int, default=0)
    parser.add_argument('--forbid', action='append', default=[])
    args = parser.parse_args()

    sizes = collect(args.map)
    if not sizes:
        print(f'error: no libKNstaX.a contributions found in {args.map}', file=sys.stderr)
        print('       (wrong map file, or the component was not linked)', file=sys.stderr)
        return 2

    total = sum(sizes.values())
    ranked = sorted(sizes.items(), key=lambda kv: -kv[1])

    print(f'KNstaX linked size: {total} bytes across {len(sizes)} objects\n')
    print('Largest contributors:')
    for name, size in ranked[:8]:
        print(f'  {size:>8}  {name}')
    print()

    failed = False

    if args.forbid:
        print('Checking forbidden objects:')
        for pattern in args.forbid:
            hits = [f'{n} ({s} B)' for n, s in ranked if pattern in n]
            if hits:
                print(f"  FAIL: '{pattern}' reached the link but should have been gated out:")
                for hit in hits:
                    print(f'        {hit}')
                failed = True
            else:
                print(f"  ok: '{pattern}' absent")
        print()

    if args.budget > 0:
        if total > args.budget:
            print(f'FAIL: {total} bytes exceeds the {args.budget} byte budget '
                  f'by {total - args.budget}.')
            print('      Either justify the growth and raise the budget deliberately,')
            print('      or find what started reaching the linker that did not before.')
            failed = True
        else:
            print(f'ok: {total} bytes is within the {args.budget} byte budget '
                  f'({args.budget - total} to spare).')

    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
