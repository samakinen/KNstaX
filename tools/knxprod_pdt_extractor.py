#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Extract PDT metadata from include/knx/application/property.hpp and print a JSON mapping to stdout.
This tool no longer writes a persistent cache file.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROP_HEADER = ROOT / 'include' / 'knx' / 'application' / 'property.hpp'
# Do not write a persistent cache file anymore; output mapping to stdout.

if not PROP_HEADER.exists():
    print('property.hpp not found at', PROP_HEADER, file=sys.stderr)
    sys.exit(2)

text = PROP_HEADER.read_text(encoding='utf-8')

# Extract enum entries and their PDT comments ("///< PDT_CONTROL")
enum_block_match = re.search(r'enum class PropertyDataType [^{]*\{([^}]*)\}', text, re.S)
if not enum_block_match:
    print('PropertyDataType enum not found', file=sys.stderr)
    sys.exit(2)

enum_block = enum_block_match.group(1)
# parse lines like: "    Control = 0x00,           ///< PDT_CONTROL"
entry_re = re.compile(r"\s*(?P<name>\w+)\s*(?:=[^,]+)?,?\s*(?:///<|///<)?\s*(?P<pdt>PDT_[A-Z0-9_\-]+)?")
# fallback: sometimes comment uses '///< PDT_CONTROL' exactly; capture more broadly
entries = {}
for line in enum_block.splitlines():
    m = re.match(r"\s*(?P<name>\w+)\s*(?:=[^,]+)?,?\s*(?:/\*\*<|///<?)?\s*(?P<pdt>PDT_[A-Z0-9_\-]+)?", line)
    if m:
        name = m.group('name')
        pdt = m.group('pdt') or None
        entries[name] = {'pdt': pdt}

# parse getElementSize switch cases
size_match = re.search(r'uint8_t getElementSize\([^)]*\) const \{([^}]*)\}', text, re.S)
case_map = {}
if size_match:
    body = size_match.group(1)
    # find case blocks: case PropertyDataType::Control:
    for case in re.finditer(r'case\s+PropertyDataType::(?P<ename>\w+):\s*(?:return\s+(?P<size>\d+);)', body):
        ename = case.group('ename')
        size = int(case.group('size'))
        case_map[ename] = size
    # also check grouped cases that return same size spanning multiple lines
    for m in re.finditer(r'(case\s+PropertyDataType::(?P<ename>\w+)\s*,?\s*)+\s*return\s+(?P<size>\d+);', body):
        pass

# Build mapping keyed by PDT token when available, else enum name
mapping = {}
for ename, info in entries.items():
    pdt = info.get('pdt')
    size = case_map.get(ename, None)
    mapping_key = pdt if pdt else f'PropertyDataType::{ename}'
    mapping[mapping_key] = {
        'enum_name': ename,
        'element_size': size if size is not None else 0,
        'variable_length': size == 0,
    }

# Print JSON to stdout so callers can decide whether to persist it.
sys.stdout.write(json.dumps(mapping, indent=2))
