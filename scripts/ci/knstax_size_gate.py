#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Flash and RAM budget gate for a bare-metal KNstaX image (K7 step 7).

Reads the linker map of a linked Cortex-M image and splits what it contains
into two shares:

  knstax   the application image: KNstaX itself plus the few lines of startup
           and `main` that make it a device. With LTO the compiler merges all
           of it into anonymous partitions, so KNstaX cannot be told apart from
           that glue; the gate counts both, which only over-reports.
  runtime  the C and C++ runtime: members of libc, libstdc++, libgcc and the
           like. These are never LTO objects, so the split stays exact.

Flash is code, read-only data and the load image of initialised data. RAM is
initialised data plus zero-initialised data. The heap is not in the map: pass
the high-water mark a run measured with --heap-high-water and it is added to
the RAM share.

Usage:
  knstax_size_gate.py --map size_gate.elf.map --budget size_budget.json
                      [--heap-high-water BYTES] [--top N] [--json FILE]
                      [--elf size_gate.elf [--nm arm-none-eabi-nm]] [--forbid PATTERN]...

Forbidden features: the budget file may carry a "forbid" list of substrings
(also --forbid, repeatable). With --elf each is looked up in the demangled
symbol names of the image (`nm -C`) and the map's member names; any match fails
the gate. This is how the gate proves that a feature switched off in Kconfig is
absent from the image rather than merely small, which the map alone cannot say
under LTO.

Exit status: 0 within budget and nothing forbidden linked, 1 over budget or a
forbidden pattern found, 2 the map or the ELF could not be read.

Why a new script rather than knstax_size_budget.py: that one reads ESP-IDF
maps and counts one archive. This one reads a Cortex-M map, has two budgets
(flash and RAM) and has to cope with an LTO link.
"""
import argparse
import json
import re
import subprocess
import sys

MAP_BODY_MARKER = 'Linker script and memory map'

# A section header line: ` .text.foo` (the rest follows on the next line when the
# name is long) or ` .text.foo  0x08000100  0x24  file.o` on a single line.
SECTION_ONLY = re.compile(r'^ (\.\S+)\s*$')
SECTION_INLINE = re.compile(r'^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
CONTINUATION = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')

RUNTIME_ARCHIVES = re.compile(r'(libc|libc_nano|libm|libgcc|libstdc\+\+|libstdc\+\+_nano|libsupc\+\+|libnosys|libg)\w*\.a\(')
KNSTAX_ARCHIVE = re.compile(r'libknx_core\.a\(([^)]+)\)')


def kind_of(section):
    """Return 'flash', 'data' or 'bss' for an input section name, else None."""
    if section.startswith(('.text', '.rodata', '.init', '.fini', '.preinit_array', '.init_array',
                           '.fini_array', '.ARM')):
        return 'flash'
    if section.startswith('.data'):
        return 'data'
    if section.startswith(('.bss', 'COMMON')) or section == 'COMMON':
        return 'bss'
    return None


def owner_of(origin):
    """Classify the object file an input section came from."""
    if RUNTIME_ARCHIVES.search(origin):
        return 'runtime', origin.split('(')[-1].rstrip(')') if '(' in origin else origin
    match = KNSTAX_ARCHIVE.search(origin)
    if match:
        return 'knstax', match.group(1)
    return 'knstax', origin.split('/')[-1]


def parse_map(path):
    """Return {(owner, member): {'flash': n, 'data': n, 'bss': n}} from a GNU ld map."""
    totals = {}
    started = False
    pending = None
    with open(path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            if not started:
                started = line.startswith(MAP_BODY_MARKER)
                continue
            inline = SECTION_INLINE.match(line)
            if inline:
                section, size, origin = inline.group(1), int(inline.group(3), 16), inline.group(4)
                pending = None
            else:
                only = SECTION_ONLY.match(line)
                if only:
                    pending = only.group(1)
                    continue
                cont = CONTINUATION.match(line)
                if not cont or pending is None:
                    pending = None
                    continue
                section, size, origin = pending, int(cont.group(2), 16), cont.group(3)
                pending = None
            kind = kind_of(section)
            if kind is None or size == 0:
                continue
            key = owner_of(origin.strip())
            entry = totals.setdefault(key, {'flash': 0, 'data': 0, 'bss': 0})
            entry[kind] += size
    return totals


def share(totals, owner):
    flash = sum(v['flash'] + v['data'] for (o, _), v in totals.items() if o == owner)
    ram_static = sum(v['data'] + v['bss'] for (o, _), v in totals.items() if o == owner)
    return flash, ram_static


def forbidden_symbols(elf, nm, patterns, totals):
    """Return {pattern: [name, ...]} for every pattern that reached the link.

    A pattern is a plain substring, matched against the demangled names of the
    defined symbols of the ELF (`nm -C`) and against the archive-member names of
    the map. The symbols are what survives an LTO link, where the members are
    anonymous partitions and say nothing about which sources were built; a
    feature that was compiled out has no symbol, a feature that merely got
    small still has one.
    """
    names = sorted({member for (_, member) in totals})
    if elf:
        proc = subprocess.run([nm, '-C', '--defined-only', elf], capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise RuntimeError(f'{nm} failed on {elf}: {proc.stderr.strip()}')
        for line in proc.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3:
                names.append(parts[2])
    return {pattern: sorted({n for n in names if pattern in n})[:6] for pattern in patterns}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--map', required=True)
    parser.add_argument('--budget', required=True, help='JSON file with flash_bytes and ram_bytes')
    parser.add_argument('--heap-high-water', type=int, default=None,
                        help='peak heap use in bytes, measured by running the image')
    parser.add_argument('--elf', help='the linked ELF; with it, the "forbid" patterns of the budget are checked against its symbols')
    parser.add_argument('--nm', default='arm-none-eabi-nm', help='nm of the target toolchain (default %(default)s)')
    parser.add_argument('--forbid', action='append', default=[], help='one more forbidden pattern (repeatable)')
    parser.add_argument('--top', type=int, default=12)
    parser.add_argument('--json', help='write the figures to this file')
    args = parser.parse_args()

    totals = parse_map(args.map)
    if not totals:
        print(f'error: no placed input sections found in {args.map}', file=sys.stderr)
        return 2
    with open(args.budget, encoding='utf-8') as handle:
        budget = json.load(handle)

    flash, ram_static = share(totals, 'knstax')
    runtime_flash, runtime_ram = share(totals, 'runtime')
    heap = args.heap_high_water
    ram = ram_static + (heap or 0)

    print(f'KNstaX share      flash {flash:>8} B ({flash / 1024:6.1f} KiB)   '
          f'RAM static {ram_static:>6} B')
    print(f'C/C++ runtime     flash {runtime_flash:>8} B ({runtime_flash / 1024:6.1f} KiB)   '
          f'RAM static {runtime_ram:>6} B')
    if heap is None:
        print('Heap high-water   not measured (pass --heap-high-water); RAM check covers static data only')
    else:
        print(f'Heap high-water   {heap:>8} B')
    print()

    ranked = sorted(((v['flash'] + v['data'], v['bss'], o, m) for (o, m), v in totals.items()),
                    reverse=True)
    print(f'Largest contributors (flash B, bss B):')
    for flash_b, bss_b, owner, member in ranked[:args.top]:
        print(f'  {flash_b:>8} {bss_b:>6}  [{owner}] {member}')
    print()

    failed = False

    def check(name, value, limit):
        nonlocal failed
        if limit is None:
            return
        status = 'ok' if value <= limit else 'FAIL'
        if value > limit:
            failed = True
        print(f'{status:>4}: {name} {value} B against a budget of {limit} B '
              f'({value - limit:+d})')

    check('KNstaX flash', flash, budget.get('flash_bytes'))
    check('KNstaX RAM' + (' (static + heap)' if heap is not None else ' (static only)'),
          ram, budget.get('ram_bytes'))
    check('C/C++ runtime flash', runtime_flash, budget.get('runtime_flash_bytes'))

    # Features that are off must be absent, not merely small: a pattern in the
    # budget's "forbid" list (or given with --forbid) that matches a symbol of
    # the image means a source that should not have been built got linked.
    patterns = list(budget.get('forbid', [])) + list(args.forbid)
    if patterns:
        if not args.elf:
            print('error: forbid patterns given but no --elf to read symbols from', file=sys.stderr)
            return 2
        try:
            found = forbidden_symbols(args.elf, args.nm, patterns, totals)
        except (OSError, RuntimeError) as error:
            print(f'error: cannot read the symbols: {error}', file=sys.stderr)
            return 2
        print('Forbidden (gated-out) features:')
        for pattern in patterns:
            if found[pattern]:
                failed = True
                print(f'FAIL: "{pattern}" reached the link: {", ".join(found[pattern][:3])}')
            else:
                print(f'  ok: "{pattern}" absent')
        print()

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as handle:
            json.dump({
                'knstax_flash': flash, 'knstax_ram_static': ram_static,
                'runtime_flash': runtime_flash, 'runtime_ram_static': runtime_ram,
                'heap_high_water': heap,
                'members': [{'owner': o, 'member': m, **v} for (o, m), v in sorted(totals.items())],
            }, handle, indent=2)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
