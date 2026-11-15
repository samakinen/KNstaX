#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Resolve a KNstaX configuration and write it where the build can read it.

Inputs are a top-level Kconfig file, any number of defconfig fragments (applied
in the order given, a later one wins) and an output directory. Outputs:

  <out>/knx/autoconf.h   the CONFIG_KNX_* macros, as ESP-IDF's sdkconfig.h has
                         them (bool y -> 1, bool n -> absent, int/hex as
                         numbers, strings quoted)
  <out>/knx_config.cmake set(CONFIG_KNX_X ...) lines for CMake to include
  <out>/knx.config       the full resolved configuration, in .config format

A file that has not changed is not rewritten, so a re-run does not make the
build recompile everything.

The generator fails (exit status 1, naming the symbol and the fragment line)
on an unknown symbol, a malformed line, or an assignment that Kconfig does not
honour: out of range, or a symbol whose dependencies make it invisible. A
silent fallback to the default there would build something other than what the
fragment asked for.

Run it as `python3 -I tools/kconfig/knx_genconfig.py ...`. Isolated mode keeps
the user's site-packages and the current directory out of the import path, so
the script adds its own directory (where kconfiglib.py is vendored) itself.
"""
import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kconfiglib  # noqa: E402

CONFIG_PREFIX = "CONFIG_"

_SET_RE = re.compile(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$")
_UNSET_RE = re.compile(r"^#\s*CONFIG_([A-Za-z0-9_]+) is not set\s*$")


class GenError(Exception):
    pass


def parse_fragment(path):
    """Yield (name, value text or None for 'is not set', 'file:line')."""
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            where = f"{path}:{lineno}"
            if not line:
                continue
            m = _UNSET_RE.match(line)
            if m:
                yield m.group(1), None, where
                continue
            if line.startswith("#"):
                continue
            m = _SET_RE.match(line)
            if not m:
                raise GenError(f"{where}: not a CONFIG_X=value line: {line!r}")
            yield m.group(1), m.group(2).strip(), where


def _unquote(text, where):
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise GenError(f"{where}: a string value must be quoted: {text}")
    return text[1:-1].replace('\\"', '"').replace("\\\\", "\\")


def assign(sym, value, where):
    """Set the user value of sym from fragment text; returns the expected string."""
    t = sym.type
    name = CONFIG_PREFIX + sym.name
    if t == kconfiglib.BOOL:
        if value is None or value == "n":
            sym.set_value(0)
            return "n"
        if value == "y":
            sym.set_value(2)
            return "y"
        raise GenError(f"{where}: {name} is a bool, got {value!r} (use y, or '# {name} is not set')")
    if value is None:
        raise GenError(f"{where}: {name} is not a bool, '# ... is not set' does not apply")
    if t == kconfiglib.INT:
        if not re.fullmatch(r"-?[0-9]+", value):
            raise GenError(f"{where}: {name} is an int, got {value!r}")
        sym.set_value(value)
        return value
    if t == kconfiglib.HEX:
        if not re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
            raise GenError(f"{where}: {name} is a hex, got {value!r} (needs 0x)")
        sym.set_value(value)
        return value
    if t == kconfiglib.STRING:
        text = _unquote(value, where)
        sym.set_value(text)
        return text
    raise GenError(f"{where}: {name} has unsupported type {kconfiglib.TYPE_TO_STR[t]}")


def same_value(sym, expected):
    got = sym.str_value
    if sym.type in (kconfiglib.INT, kconfiglib.HEX):
        try:
            return int(got, 0) == int(expected, 0)
        except ValueError:
            return False
    return got == expected


def load(kconfig_path, srctree, fragments):
    os.environ["srctree"] = str(srctree)
    kconf = kconfiglib.Kconfig(str(kconfig_path), warn=True, warn_to_stderr=False)
    for frag in fragments:
        assigned = {}  # name -> (expected, where); the last assignment in the fragment wins
        for name, value, where in parse_fragment(frag):
            sym = kconf.syms.get(name)
            if sym is None or not sym.nodes:
                raise GenError(f"{where}: unknown symbol {CONFIG_PREFIX}{name}")
            assigned[name] = (assign(sym, value, where), where)
        # Checked once per fragment, when all of its lines are in: a fragment
        # may set a symbol before the symbol it depends on. A later fragment may
        # still turn the dependency off (an override that drops KNXnet/IP also
        # takes tunnelling with it), so only the fragment that asked is held to it.
        for name, (expected, where) in assigned.items():
            sym = kconf.syms[name]
            if not same_value(sym, expected):
                reason = ("not visible (a dependency is not met)" if sym.visibility == 0
                          else "out of range or conflicting")
                raise GenError(f"{where}: {CONFIG_PREFIX}{name}={expected!r} does not stick: "
                               f"it resolves to {sym.str_value!r}, {reason}")
    # A standalone build compiles several media itself (KNX_BUILD_*); the ESP-IDF
    # medium choice must stay at its default there. See Kconfig.standalone.
    if "KNX_BUILD_TP1" in kconf.syms and kconf.syms["KNX_BUILD_TP1"].nodes:
        medium_ip = kconf.syms.get("KNX_MEDIUM_IP")
        if medium_ip is not None and medium_ip.tri_value == 2:
            raise GenError("CONFIG_KNX_MEDIUM_IP is selected in a standalone configuration; "
                           "leave KNX_MEDIUM at TP1 and use KNX_BUILD_NETIP / KNX_BUILD_TP1")
    return kconf


def emitted(kconf):
    """The symbols that have a value to write, bools that are n included.

    Kconfiglib writes nothing for a symbol with no value to give (hidden, no
    default); those are left out.
    """
    return [sym for sym in kconf.unique_defined_syms if sym.config_string]


def render_header(kconf):
    lines = ["// Generated by tools/kconfig/knx_genconfig.py. Do not edit.", "#pragma once", ""]
    for sym in emitted(kconf):
        name = CONFIG_PREFIX + sym.name
        if sym.type == kconfiglib.BOOL:
            if sym.tri_value == 2:
                lines.append(f"#define {name} 1")
        elif sym.type in (kconfiglib.INT, kconfiglib.HEX):
            if sym.str_value != "":
                lines.append(f"#define {name} {sym.str_value}")
        elif sym.type == kconfiglib.STRING:
            esc = sym.str_value.replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'#define {name} "{esc}"')
    return "\n".join(lines) + "\n"


def render_cmake(kconf):
    lines = ["# Generated by tools/kconfig/knx_genconfig.py. Do not edit."]
    for sym in emitted(kconf):
        name = CONFIG_PREFIX + sym.name
        if sym.type == kconfiglib.BOOL:
            lines.append(f"set({name} {'y' if sym.tri_value == 2 else 'n'})")
        elif sym.type in (kconfiglib.INT, kconfiglib.HEX):
            if sym.str_value != "":
                lines.append(f"set({name} {sym.str_value})")
        elif sym.type == kconfiglib.STRING:
            esc = sym.str_value.replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'set({name} "{esc}")')
    return "\n".join(lines) + "\n"


def render_config(kconf):
    return ("# Generated by tools/kconfig/knx_genconfig.py. Do not edit.\n"
            + "".join(s.config_string for s in kconf.unique_defined_syms))


def write_if_changed(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8", newline="\n")


def main(argv=None):
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kconfig", required=True, help="top-level Kconfig file")
    ap.add_argument("--srctree", default=str(here.parent.parent),
                    help="base directory for `source` lines (default: the KNstaX root)")
    ap.add_argument("--fragment", action="append", default=[], metavar="FILE",
                    help="defconfig fragment; repeat, later ones win")
    ap.add_argument("--out-dir", required=True, help="where to write the outputs")
    args = ap.parse_args(argv)

    try:
        kconf = load(Path(args.kconfig).resolve(), Path(args.srctree).resolve(), args.fragment)
    except GenError as exc:
        print(f"knx_genconfig: error: {exc}", file=sys.stderr)
        return 1
    except kconfiglib.KconfigError as exc:
        print(f"knx_genconfig: error: {exc}", file=sys.stderr)
        return 1

    for warning in kconf.warnings:
        print(f"knx_genconfig: warning: {warning}", file=sys.stderr)

    out = Path(args.out_dir)
    write_if_changed(out / "knx" / "autoconf.h", render_header(kconf))
    write_if_changed(out / "knx_config.cmake", render_cmake(kconf))
    write_if_changed(out / "knx.config", render_config(kconf))
    print(f"knx_genconfig: {len(emitted(kconf))} symbols resolved from "
          f"{len(args.fragment)} fragment(s), {len(kconf.warnings)} warning(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
