#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""The steps after the exporter: check, lock, pack and collect an ETS product.

knx_ets_product() (cmake/knx_ets_product.cmake) runs these from the build; on
the PC with ETS, `pack` is also run by hand. Standard library only.

    ets_product.py validate [--if-available] <name>.knxprod.xml
    ets_product.py lock --lock <product.lock> --name <name> [--mode check|warn] <name>.ets.xml
    ets_product.py pack [--packer auto|<backend>] [--if-available] <name>.ets.xml | <dir> ...
    ets_product.py check [--ets-xml <name>.ets.xml] <name>.knxprod
    ets_product.py dist --out <dir> --name <name> --product-dir <dir> [--file <f> ...]
                        [--source-dir <dir>] [--knstax-dir <dir>]
    ets_product.py hash <name>.ets.xml

The product lock records, for each product a firmware builds, the application
program, its version and a hash of the product file's content. ETS refuses a
changed program under a version it has already imported, so a build in which
the content changed and the version did not fails here, long before ETS.

A packer backend turns <name>.ets.xml (the product without MasterData) into a
signed <name>.knxprod. Signing needs the ETS libraries, so every backend runs
on a PC with ETS. After any backend, `check` reads the archive back.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

KNX_NS = 'http://knx.org/xml/project/23'
HERE = Path(__file__).resolve().parent
SCHEMA = HERE.parent / 'knxprod_exporter' / 'schema' / 'knx_schema_version23.xsd'
APP_ID = re.compile(r'^(M-[0-9A-F]{4})_A-([0-9A-F]{4})-([0-9A-F]{2})-([0-9A-F]{4})$')


class ProductError(Exception):
    """A finding that fails the step (or warns, in lock --mode warn)."""


def _say(message: str, stream=None):
    print(message, file=stream or sys.stdout, flush=True)


# ── The product file ─────────────────────────────────────────────────────────

def application_program(xml_path: Path):
    """(manufacturer, application, version) of the product's application program.

    application is 'M-xxxx_A-nnnn': the id without version and hash, which is
    what ETS keys a program by together with the version.
    """
    root = ET.parse(xml_path).getroot()
    app = root.find(f'.//{{{KNX_NS}}}ApplicationProgram')
    if app is None:
        raise ProductError(f'{xml_path}: no ApplicationProgram element')
    match = APP_ID.match(app.get('Id', ''))
    if not match:
        raise ProductError(f'{xml_path}: unexpected ApplicationProgram id {app.get("Id")!r}')
    manufacturer, number, version, _ = match.groups()
    return manufacturer, f'{manufacturer}_A-{number}', int(version, 16)


def content_hash(xml_path: Path) -> str:
    """SHA-256 of the canonical XML (C14N 2.0, text trimmed, no comments).

    Line endings, indentation and attribute order do not count; any change to
    an element, an attribute or a text does. The placeholder '0000' hash group
    in the ids is part of the content and is constant.
    """
    canonical = ET.canonicalize(from_file=str(xml_path), strip_text=True)
    return 'sha256:' + hashlib.sha256(canonical.encode('utf-8')).hexdigest()


def _file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 16), b''):
            digest.update(block)
    return 'sha256:' + digest.hexdigest()


def _write_json(path: Path, data):
    text = json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + '\n'
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ── validate ─────────────────────────────────────────────────────────────────

def cmd_validate(args) -> int:
    try:
        import xmlschema
    except ImportError:
        if args.if_available:
            _say('ets_product: xmlschema is not installed, the product file is not '
                 'validated against the KNX schema (pip install xmlschema)')
            return 0
        _say('ets_product: validate needs the xmlschema package', sys.stderr)
        return 1
    schema = xmlschema.XMLSchema(str(SCHEMA))
    errors = list(schema.iter_errors(str(args.xml)))
    for error in errors[:20]:
        _say(f'{args.xml}: {error.reason} at {error.path}', sys.stderr)
    if errors:
        _say(f'{args.xml}: {len(errors)} schema error(s)', sys.stderr)
        return 1
    return 0


# ── lock ─────────────────────────────────────────────────────────────────────

def check_lock(lock_path: Path, name: str, ets_xml: Path, mode: str = 'check') -> int:
    """Compare the product with its lock entry; update the entry when allowed.

    Returns 0 when the build may go on. Messages go to stdout (notes) and
    stderr (errors and warnings).
    """
    _, application, version = application_program(ets_xml)
    content = content_hash(ets_xml)
    lock = {}
    if lock_path.exists():
        with open(lock_path, encoding='utf-8') as f:
            lock = json.load(f)
    entry = lock.get(name)
    new_entry = {'application': application, 'version': version, 'content': content}
    where = f'ETS product {name} ({application}, version {version})'

    def record(note):
        lock[name] = new_entry
        _write_json(lock_path, lock)
        _say(f'{where}: {note}; {lock_path} updated')
        return 0

    if entry is None:
        return record('new product')
    if entry.get('application') != application:
        return record(f'application changed from {entry.get("application")}')

    locked_version = int(entry.get('version', 0))
    changed = entry.get('content') != content
    if not changed:
        if version == locked_version:
            return 0
        if version > locked_version:
            _say(f'warning: {where}: version raised from {locked_version} without a change '
                 'ETS would see', sys.stderr)
            return record('version raised')
        problem = (f'version {version} is lower than the locked {locked_version}')
    elif version > locked_version:
        return record(f'changed, version raised from {locked_version}')
    elif version == locked_version:
        problem = (f'the product changed but its application version is still {version}. '
                   'ETS refuses a changed program under a version it has imported '
                   '("the product has a different hash than the existing product"). '
                   'Raise the application version in the product definition')
    else:
        problem = (f'the product changed and its version {version} is lower than the '
                   f'locked {locked_version}')

    if mode == 'warn':
        _say(f'warning: {where}: {problem}. Not recorded in {lock_path} '
             '(lock mode warn).', sys.stderr)
        return 0
    _say(f'error: {where}: {problem}. (For iterations that never reach ETS: '
         '-DKNX_ETS_LOCK_MODE=warn.)', sys.stderr)
    return 1


def cmd_lock(args) -> int:
    return check_lock(Path(args.lock), args.name, Path(args.ets_xml), args.mode)


def cmd_hash(args) -> int:
    _, application, version = application_program(Path(args.ets_xml))
    _say(f'{application} version {version} {content_hash(Path(args.ets_xml))}')
    return 0


# ── check ────────────────────────────────────────────────────────────────────

def check_archive(archive: Path, ets_xml: Path = None) -> list:
    """What is wrong with a packed .knxprod; an empty list when nothing is."""
    problems = []
    try:
        z = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile) as e:
        return [f'not a ZIP archive: {e}']
    with z:
        names = set(z.namelist())
        signatures = [n for n in names if re.fullmatch(r'M-[0-9A-F]{4}\.signature', n)]
        if len(signatures) != 1:
            return problems + [f'expected one M-xxxx.signature at the top, found {signatures}']
        manufacturer = signatures[0][:-len('.signature')]
        if not z.read(signatures[0]).strip():
            problems.append(f'{signatures[0]} is empty')
        if 'knx_master.xml' not in names:
            problems.append('knx_master.xml is missing')
        for part in ('Catalog.xml', 'Hardware.xml'):
            if f'{manufacturer}/{part}' not in names:
                problems.append(f'{manufacturer}/{part} is missing')
        apps = [n for n in names
                if re.fullmatch(rf'{manufacturer}/{manufacturer}_A-[0-9A-F]{{4}}-[0-9A-F]{{2}}-[0-9A-F]{{4}}\.xml', n)]
        if len(apps) != 1:
            return problems + [f'expected one application program file, found {apps}']
        app_id = Path(apps[0]).stem
        if app_id.endswith('-0000'):
            problems.append(f'{apps[0]}: the id still carries the 0000 placeholder; it was not signed')
        app = ET.fromstring(z.read(apps[0])).find(f'.//{{{KNX_NS}}}ApplicationProgram')
        if app is None or app.get('Id') != app_id:
            problems.append(f'{apps[0]}: no ApplicationProgram with id {app_id}')
        elif not app.get('Hash'):
            problems.append(f'{apps[0]}: the ApplicationProgram has no Hash')
        if f'{manufacturer}/Hardware.xml' in names:
            hardware = ET.fromstring(z.read(f'{manufacturer}/Hardware.xml'))
            refs = [r.get('RefId') for r in hardware.iter(f'{{{KNX_NS}}}ApplicationProgramRef')]
            if app_id not in refs:
                problems.append(f'Hardware.xml does not refer to {app_id} (it refers to {refs})')
            for h2p in hardware.iter(f'{{{KNX_NS}}}Hardware2Program'):
                if not h2p.get('Hash'):
                    problems.append(f'Hardware2Program {h2p.get("Id")} has no Hash')
        if ets_xml is not None:
            want_manufacturer, application, version = application_program(ets_xml)
            have = f'{application}-{version:02X}-'
            if not app_id.startswith(have):
                problems.append(f'the archive holds {app_id}, the product file is {have}xxxx')
    return problems


def cmd_check(args) -> int:
    problems = check_archive(Path(args.archive), Path(args.ets_xml) if args.ets_xml else None)
    for p in problems:
        _say(f'{args.archive}: {p}', sys.stderr)
    if not problems:
        _say(f'{args.archive}: complete and signed')
    return 1 if problems else 0


# ── pack ─────────────────────────────────────────────────────────────────────

class OpenKnxProducer:
    """OpenKNXproducer (github.com/OpenKNX/OpenKNXproducer): signs through the
    ETS installation's own libraries, so it runs on Windows with ETS."""

    name = 'openknxproducer'
    # Versions a product packed by it has been imported into ETS6 with.
    TESTED = ('4.3.12',)

    def __init__(self):
        self.exe = None
        self.version = None

    def find(self) -> bool:
        candidates = [os.environ.get('OPENKNXPRODUCER'),
                      shutil.which('OpenKNXproducer'), shutil.which('OpenKNXproducer.exe')]
        self.exe = next((c for c in candidates if c and Path(c).is_file()), None)
        if self.exe is None:
            return False
        try:
            out = subprocess.run([self.exe, '--version'], capture_output=True, text=True,
                                 timeout=60).stdout
            match = re.search(r'OpenKNXproducer\s+(\S+)', out)
            self.version = match.group(1) if match else 'unknown'
        except (OSError, subprocess.SubprocessError):
            self.version = 'unknown'
        return True

    def describe(self) -> str:
        return f'OpenKNXproducer {self.version}'

    def pack(self, ets_xml: Path, output: Path):
        if self.version not in self.TESTED:
            _say(f'warning: OpenKNXproducer {self.version} has not been tested with KNstaX '
                 f'products (tested: {", ".join(self.TESTED)})', sys.stderr)
        # OpenKNXproducer deletes its output before it starts; doing it here too
        # means an archive that exists afterwards was written by this run.
        output.unlink(missing_ok=True)
        # -N: no XSD auto-search. It looks for an <?xml-model?> instruction the
        # exporter does not write, and then reports the working directory as a
        # missing schema file.
        result = subprocess.run([self.exe, 'knxprod', '-N', '-o', str(output), str(ets_xml)])
        if result.returncode != 0:
            if not output.exists():
                raise ProductError(f'OpenKNXproducer failed (exit {result.returncode})')
            # It writes the archive, then prints a banner for which it re-reads
            # the application id with a fixed pattern; a crash there says
            # nothing about the archive, which the check below reads back.
            _say(f'warning: OpenKNXproducer exited with {result.returncode} after writing '
                 f'{output}; the archive is checked below', sys.stderr)


BACKENDS = {cls.name: cls for cls in (OpenKnxProducer,)}


def find_packer(choice: str):
    names = list(BACKENDS) if choice == 'auto' else [choice]
    for name in names:
        if name not in BACKENDS:
            raise ProductError(f'unknown packer {name!r} (known: {", ".join(BACKENDS)})')
        backend = BACKENDS[name]()
        if backend.find():
            return backend
    return None


def _pack_inputs(paths):
    for p in map(Path, paths):
        if p.is_dir():
            yield from sorted(p.rglob('*.ets.xml'))
        else:
            yield p


def pack_one(backend, ets_xml: Path, output: Path = None) -> Path:
    if output is None:
        output = ets_xml.with_name(ets_xml.name[:-len('.ets.xml')] + '.knxprod')
    # The packer gets a copy in a directory of its own: some write work files
    # next to their input.
    with tempfile.TemporaryDirectory(prefix='ets_product_') as tmp:
        work = Path(tmp) / ets_xml.name
        shutil.copyfile(ets_xml, work)
        backend.pack(work, output.resolve())
    problems = check_archive(output, ets_xml)
    if problems:
        raise ProductError(f'{output}: ' + '; '.join(problems))
    manifest = output.parent / 'manifest.json'
    if manifest.exists():
        with open(manifest, encoding='utf-8') as f:
            data = json.load(f)
        data['knxprod'] = {'file': output.name, 'packer': backend.describe(),
                           'sha256': _file_hash(output)}
        _write_json(manifest, data)
    _say(f'Packed {output} with {backend.describe()}')
    return output


def cmd_pack(args) -> int:
    inputs = list(_pack_inputs(args.inputs))
    if not inputs:
        _say('ets_product: no *.ets.xml to pack', sys.stderr)
        return 1
    if args.output and len(inputs) != 1:
        _say('ets_product: --output takes one input', sys.stderr)
        return 2
    backend = find_packer(args.packer)
    if backend is None:
        names = ' '.join(p.name for p in inputs)
        hint = ('no packer found (OpenKNXproducer on PATH or in OPENKNXPRODUCER), so '
                f'{names} is not packed. On the PC with ETS: python '
                '<KNstaX>/tools/ets_product/ets_product.py pack <build>/dist')
        if args.if_available:
            _say(f'ets_product: {hint}')
            return 0
        _say(f'ets_product: {hint}', sys.stderr)
        return 1
    for ets_xml in inputs:
        pack_one(backend, ets_xml, Path(args.output) if args.output else None)
    return 0


# ── dist ─────────────────────────────────────────────────────────────────────

def _git_describe(directory):
    if not directory:
        return None
    try:
        result = subprocess.run(
            ['git', '-c', 'safe.directory=*', '-C', str(directory), 'describe', '--always',
             '--dirty', '--tags'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def cmd_dist(args) -> int:
    out = Path(args.out)
    product_dir = Path(args.product_dir)
    out.mkdir(parents=True, exist_ok=True)
    ets_xml = product_dir / f'{args.name}.ets.xml'
    _, application, version = application_program(ets_xml)
    files = [product_dir / f'{args.name}{suffix}'
             for suffix in ('.json', '.knxprod.xml', '.ets.xml', '.knxprod')]
    files = [f for f in files if f.exists()] + [Path(f) for f in args.file]
    previous = {}
    manifest_path = out / 'manifest.json'
    if manifest_path.exists():
        with open(manifest_path, encoding='utf-8') as f:
            previous = json.load(f)
    hashes = {}
    for f in files:
        if not f.exists():
            _say(f'ets_product: {f} does not exist', sys.stderr)
            return 1
        target = out / f.name
        if not target.exists() or _file_hash(target) != _file_hash(f):
            shutil.copyfile(f, target)
        hashes[f.name] = _file_hash(target)
    # A .knxprod packed in the dist directory from the current product stays,
    # with its record; one from an older product goes.
    knxprod = None
    packed = out / f'{args.name}.knxprod'
    if f'{args.name}.knxprod' not in hashes and packed.exists():
        if not check_archive(packed, ets_xml) and previous.get('content') == content_hash(ets_xml):
            knxprod = previous.get('knxprod')
            hashes[packed.name] = _file_hash(packed)
        else:
            packed.unlink()
    manifest = {
        'name': args.name,
        'application': application,
        'version': version,
        'content': content_hash(ets_xml),
        'firmware': _git_describe(args.source_dir),
        'knstax': _git_describe(args.knstax_dir),
        'files': hashes,
        'knxprod': knxprod,
    }
    if manifest != previous:
        _write_json(manifest_path, manifest)
    return 0


# ── main ─────────────────────────────────────────────────────────────────────

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('validate', help='validate a product file against the KNX schema')
    p.add_argument('--if-available', action='store_true',
                   help='skip with a note when xmlschema is not installed')
    p.add_argument('xml')
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser('lock', help='check the product against its lock entry')
    p.add_argument('--lock', required=True)
    p.add_argument('--name', required=True)
    p.add_argument('--mode', choices=['check', 'warn'], default='check')
    p.add_argument('ets_xml')
    p.set_defaults(func=cmd_lock)

    p = sub.add_parser('hash', help='print the application, version and content hash')
    p.add_argument('ets_xml')
    p.set_defaults(func=cmd_hash)

    p = sub.add_parser('pack', help='pack and sign *.ets.xml into .knxprod')
    p.add_argument('--packer', default='auto', help=f'auto or one of: {", ".join(BACKENDS)}')
    p.add_argument('--if-available', action='store_true',
                   help='without a packer, print how to pack and succeed')
    p.add_argument('--output', '-o')
    p.add_argument('inputs', nargs='+', help='<name>.ets.xml files or directories holding them')
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser('check', help='check a packed .knxprod')
    p.add_argument('--ets-xml')
    p.add_argument('archive')
    p.set_defaults(func=cmd_check)

    p = sub.add_parser('dist', help='collect a product and its firmware with a manifest')
    p.add_argument('--out', required=True)
    p.add_argument('--name', required=True)
    p.add_argument('--product-dir', required=True)
    p.add_argument('--file', action='append', default=[])
    p.add_argument('--source-dir')
    p.add_argument('--knstax-dir')
    p.set_defaults(func=cmd_dist)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ProductError as e:
        _say(f'error: {e}', sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
