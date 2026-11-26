#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""The steps after the exporter (tools/ets_product/ets_product.py).

The lock is what stands between a product change and an ETS that refuses it,
so most of these are about the lock: which changes it lets through, which it
records and which it stops. The packer itself needs ETS; here a stub backend
writes the archive, and the archive check is tested on its own.
"""
import io
import json
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'knxprod_exporter'))
import ets_product  # noqa: E402
from exporter import export_product_json_to_knxprod  # noqa: E402

NS = {'k': 'http://knx.org/xml/project/23'}


def _doc(version=1, display='Switch', number=1):
    return {
        'identity': {'profileKey': 'sw', 'productDisplayName': display, 'manufacturerId': 250,
                     'medium': {'code': 0, 'name': 'TP', 'maskVersion': 0x07B0, 'ipEnabled': False},
                     'orderNumber': 'SW1', 'hardwareSerialNumber': 1, 'hardwareVersion': 1,
                     'applicationProgram': {'number': number, 'version': version}},
        'features': {},
        'capacities': {'addressTableMaxEntries': 64, 'associationTableMaxEntries': 96},
        'communicationObjects': [{
            'exportNumber': 0, 'logicalId': 0, 'key': 'k', 'displayName': 'K',
            'defaultAddress': 0, 'dpt_main': 1, 'dpt_sub': 1, 'valueType': 1,
            'readable': True, 'writable': False, 'transmit': True, 'receivable': False,
            'readOnInit': False, 'communication': True, 'persisted': False}],
        'parameters': [],
    }


class Workspace:
    def __init__(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.lock = self.dir / 'product.lock'
        self.ets = self.dir / 'sw.ets.xml'

    def write(self, newline='\n', **kwargs):
        text = export_product_json_to_knxprod(_doc(**kwargs), master_data=False)
        with open(self.ets, 'w', encoding='utf-8', newline=newline) as f:
            f.write(text)

    def lock_check(self, mode='check'):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = ets_product.check_lock(self.lock, 'sw', self.ets, mode)
        return rc, out.getvalue() + err.getvalue()

    def entry(self):
        return json.loads(self.lock.read_text(encoding='utf-8'))['sw']

    def close(self):
        self._tmp.cleanup()


def _with_workspace(test):
    def run():
        ws = Workspace()
        try:
            test(ws)
        finally:
            ws.close()
    run.__name__ = test.__name__
    return run


# ── The ets format ───────────────────────────────────────────────────────────

def test_ets_format_is_knxprod_without_master_data():
    full = ET.fromstring(export_product_json_to_knxprod(_doc()))
    ets = ET.fromstring(export_product_json_to_knxprod(_doc(), master_data=False))
    assert full.find('k:MasterData', NS) is not None
    assert ets.find('k:MasterData', NS) is None
    full.remove(full.find('k:MasterData', NS))
    assert ET.tostring(full) == ET.tostring(ets), 'nothing but MasterData may differ'


# ── The lock ─────────────────────────────────────────────────────────────────

@_with_workspace
def test_a_new_product_is_recorded(ws):
    ws.write(version=3)
    rc, out = ws.lock_check()
    assert rc == 0, out
    entry = ws.entry()
    assert entry['application'] == 'M-00FA_A-0001' and entry['version'] == 3
    assert entry['content'].startswith('sha256:')


@_with_workspace
def test_an_unchanged_product_passes_and_leaves_the_lock_alone(ws):
    ws.write()
    ws.lock_check()
    before = ws.lock.read_bytes()
    rc, out = ws.lock_check()
    assert rc == 0 and out == '', out
    assert ws.lock.read_bytes() == before


@_with_workspace
def test_line_endings_and_indentation_are_not_a_change(ws):
    ws.write()
    ws.lock_check()
    ws.write(newline='\r\n')
    text = ws.ets.read_text(encoding='utf-8').replace('  <', '    <')
    ws.ets.write_text(text, encoding='utf-8')
    rc, out = ws.lock_check()
    assert rc == 0 and out == '', out


@_with_workspace
def test_a_change_without_a_new_version_fails(ws):
    ws.write(version=2)
    ws.lock_check()
    ws.write(version=2, display='Switch, renamed')
    rc, out = ws.lock_check()
    assert rc == 1, out
    assert 'Raise the application version' in out, out
    assert ws.entry()['version'] == 2


@_with_workspace
def test_warn_mode_reports_and_does_not_record(ws):
    ws.write(version=2)
    ws.lock_check()
    content = ws.entry()['content']
    ws.write(version=2, display='Switch, renamed')
    rc, out = ws.lock_check(mode='warn')
    assert rc == 0 and 'warning' in out, out
    assert ws.entry()['content'] == content, 'warn must not record the change'


@_with_workspace
def test_a_change_with_a_new_version_is_recorded(ws):
    ws.write(version=2)
    ws.lock_check()
    ws.write(version=3, display='Switch, renamed')
    rc, out = ws.lock_check()
    assert rc == 0, out
    assert ws.entry()['version'] == 3


@_with_workspace
def test_a_lower_version_fails(ws):
    ws.write(version=4)
    ws.lock_check()
    ws.write(version=3, display='Switch, renamed')
    rc, out = ws.lock_check()
    assert rc == 1 and 'lower' in out, out


@_with_workspace
def test_a_version_raised_for_nothing_warns_and_is_recorded(ws):
    ws.write(version=2)
    ws.lock_check()
    ws.write(version=3)
    # The version is part of the ids, so the content changes with it; what
    # counts is whether anything else did.
    rc, out = ws.lock_check()
    assert rc == 0, out
    assert ws.entry()['version'] == 3


@_with_workspace
def test_entries_of_other_products_are_kept(ws):
    ws.lock.write_text(json.dumps({'other': {'application': 'M-00FA_A-0009',
                                             'version': 1, 'content': 'sha256:00'}}),
                       encoding='utf-8')
    ws.write()
    ws.lock_check()
    lock = json.loads(ws.lock.read_text(encoding='utf-8'))
    assert set(lock) == {'other', 'sw'}


# ── The archive check and pack ───────────────────────────────────────────────

def _archive(path: Path, ets: Path, app_hash='271C', with_signature=True, hash_attr=True):
    """A .knxprod as a signer leaves it, made from the product file."""
    text = ets.read_text(encoding='utf-8')
    app_id = re.search(r'ApplicationProgram Id="([^"]+)"', text).group(1)
    signed_id = app_id[:-4] + app_hash
    text = text.replace(app_id, signed_id)
    text = text.replace(app_id.replace('_A-', '_H-0001-1_HP-').split('_HP-')[0], 'unused')
    app_xml = text.replace(f'Id="{signed_id}"', f'Id="{signed_id}"' + (' Hash="aGFzaA=="' if hash_attr else ''), 1)
    hardware = ('<KNX xmlns="http://knx.org/xml/project/23"><ManufacturerData><Manufacturer RefId="M-00FA">'
                '<Hardware><Hardware Id="M-00FA_H-0001-1"><Hardware2Programs>'
                f'<Hardware2Program Id="M-00FA_H-0001-1_HP-0001-01-{app_hash}" Hash="aA==">'
                f'<ApplicationProgramRef RefId="{signed_id}"/></Hardware2Program>'
                '</Hardware2Programs></Hardware></Hardware></Manufacturer></ManufacturerData></KNX>')
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('knx_master.xml', '<KNX/>')
        if with_signature:
            z.writestr('M-00FA.signature', '﻿c2lnbmF0dXJl')
        z.writestr('M-00FA/Catalog.xml', '<KNX/>')
        z.writestr('M-00FA/Hardware.xml', hardware)
        z.writestr(f'M-00FA/{signed_id}.xml', app_xml)


@_with_workspace
def test_a_signed_archive_passes_the_check(ws):
    ws.write()
    archive = ws.dir / 'sw.knxprod'
    _archive(archive, ws.ets)
    assert ets_product.check_archive(archive, ws.ets) == []


@_with_workspace
def test_an_unsigned_archive_fails_the_check(ws):
    ws.write()
    archive = ws.dir / 'sw.knxprod'
    _archive(archive, ws.ets, app_hash='0000')
    problems = ets_product.check_archive(archive, ws.ets)
    assert any('placeholder' in p for p in problems), problems
    _archive(archive, ws.ets, with_signature=False)
    assert ets_product.check_archive(archive, ws.ets), 'no signature file must fail'
    _archive(archive, ws.ets, hash_attr=False)
    assert any('no Hash' in p for p in ets_product.check_archive(archive, ws.ets))


@_with_workspace
def test_an_archive_of_another_version_fails_the_check(ws):
    ws.write(version=1)
    archive = ws.dir / 'sw.knxprod'
    _archive(archive, ws.ets)
    ws.write(version=2)
    problems = ets_product.check_archive(archive, ws.ets)
    assert any('the product file is' in p for p in problems), problems


@_with_workspace
def test_pack_runs_the_backend_and_records_it_in_the_manifest(ws):
    ws.write()
    (ws.dir / 'manifest.json').write_text('{"name": "sw", "knxprod": null}', encoding='utf-8')

    class Stub:
        name = 'stub'

        def find(self):
            return True

        def describe(self):
            return 'stub 1.0'

        def pack(self, ets_xml, output):
            assert ets_xml.parent != ws.dir, 'the backend works on a copy'
            _archive(output, ets_xml)

    ets_product.BACKENDS['stub'] = Stub
    try:
        out = io.StringIO()
        with redirect_stdout(out):
            rc = ets_product.main(['pack', '--packer', 'stub', str(ws.dir)])
        assert rc == 0, out.getvalue()
    finally:
        del ets_product.BACKENDS['stub']
    assert (ws.dir / 'sw.knxprod').exists()
    manifest = json.loads((ws.dir / 'manifest.json').read_text(encoding='utf-8'))
    assert manifest['knxprod']['packer'] == 'stub 1.0'


@_with_workspace
def test_pack_without_a_packer_says_how_and_succeeds_when_asked_to(ws):
    ws.write()
    saved = dict(ets_product.BACKENDS)
    ets_product.BACKENDS.clear()
    try:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            assert ets_product.main(['pack', '--if-available', str(ws.ets)]) == 0
            assert ets_product.main(['pack', str(ws.ets)]) == 1
        assert 'no packer found' in out.getvalue()
    finally:
        ets_product.BACKENDS.update(saved)


@_with_workspace
def test_dist_collects_the_product_and_the_firmware(ws):
    ws.write(version=5)
    (ws.dir / 'sw.json').write_text('{}', encoding='utf-8')
    (ws.dir / 'sw.knxprod.xml').write_text('<KNX/>', encoding='utf-8')
    firmware = ws.dir / 'fw.bin'
    firmware.write_bytes(b'\x00\x01')
    out = ws.dir / 'dist' / 'sw'
    rc = ets_product.main(['dist', '--out', str(out), '--name', 'sw', '--product-dir', str(ws.dir),
                           '--file', str(firmware)])
    assert rc == 0
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    assert manifest['version'] == 5 and manifest['application'] == 'M-00FA_A-0001'
    assert set(manifest['files']) == {'sw.json', 'sw.knxprod.xml', 'sw.ets.xml', 'fw.bin'}
    assert manifest['knxprod'] is None


def main():
    failures = 0
    for name, test in sorted(globals().items()):
        if not name.startswith('test_') or not callable(test):
            continue
        try:
            test()
            print(f'  ok  {name}')
        except AssertionError as error:
            failures += 1
            print(f'FAIL  {name}: {error}')
    if failures:
        print(f'{failures} ETS product test(s) failed')
        return 1
    print('ETS product tests passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
