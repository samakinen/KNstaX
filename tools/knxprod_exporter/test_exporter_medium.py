#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""What the exported catalogue entry says about the bus the device sits on.

None of this is visible from firmware: the device runs and reports the right
mask version either way. It is ETS that reads the product file, decides which
lines the device may be placed on, and compares the declared mask against the
Device Descriptor before it will download. A product file that gets the medium
wrong therefore fails at the point where it is most expensive to discover.

The IP variant used to declare itself TP1 in three independent ways at once —
a hardcoded mask, a medium number that was really a C++ enum ordinal, and ids
that pointed at ETS's TP1 master data regardless of both.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from exporter import export_product_json_to_knxprod  # noqa: E402

NS = {'k': 'http://knx.org/xml/project/23'}

TP1 = {'code': 0, 'name': 'TP', 'maskVersion': 0x07B0, 'ipEnabled': False}
IP = {'code': 5, 'name': 'IP', 'maskVersion': 0x57B0, 'ipEnabled': True}


def _doc(medium, hardware_serial=1, hardware_version=1, app_number=1):
    return {
        'identity': {'profileKey': 't', 'productDisplayName': 'T', 'manufacturerId': 250,
                     'medium': medium, 'orderNumber': 'T1',
                     'hardwareSerialNumber': hardware_serial,
                     'hardwareVersion': hardware_version,
                     'applicationProgram': {'number': app_number, 'version': 1}},
        'features': {},
        # The table ceilings ETS reads as the device's association limit;
        # the exporter refuses to render a product without them.
        'capacities': {'addressTableMaxEntries': 256,
                       'associationTableMaxEntries': 512},
        'communicationObjects': [{
            'exportNumber': 0, 'logicalId': 0, 'key': 'k', 'displayName': 'K',
            'defaultAddress': 0, 'dpt_main': 1, 'dpt_sub': 1, 'valueType': 1,
            'readable': True, 'writable': False, 'transmit': True, 'receivable': False,
            'readOnInit': False, 'communication': True, 'persisted': False}],
        'parameters': [],
    }


def _root(medium, **kwargs):
    return ET.fromstring(export_product_json_to_knxprod(_doc(medium, **kwargs)))


def _one(root, tag):
    found = root.findall(f'.//k:{tag}', NS)
    assert len(found) == 1, f'expected exactly one <{tag}>, got {len(found)}'
    return found[0]


def _hardware(root):
    # <Hardware> is both the container and the entry inside it.
    found = root.findall('.//k:Hardware/k:Hardware', NS)
    assert len(found) == 1, f'expected exactly one hardware entry, got {len(found)}'
    return found[0]


def test_tp1_declares_the_tp_medium_and_the_tp_mask():
    root = _root(TP1)
    medium = _one(root, 'MediumType')
    assert medium.get('Number') == '0'
    assert medium.get('Id') == 'MT-0'
    mask = _one(root, 'MaskVersion')
    assert mask.get('MaskVersion') == '1968', 'System B on TP1 is 07B0h = 1968'


def test_ip_declares_the_ip_medium_and_the_ip_mask():
    root = _root(IP)
    medium = _one(root, 'MediumType')
    # 5, not 2. The C++ enum's ordinal for IP_Routing is 2, and KNX medium 2 is
    # RF — the exact confusion this test exists to prevent.
    assert medium.get('Number') == '5'
    assert medium.get('Id') == 'MT-5'
    mask = _one(root, 'MaskVersion')
    assert mask.get('MaskVersion') == '22448', 'System B on KNX IP is 57B0h = 22448'
    assert mask.get('Id') == 'MV-57B0'


def test_medium_ids_agree_with_the_values_they_carry():
    # ETS resolves MT-*/MV-* against its own master data by id, so an id that
    # disagrees with the number beside it silently wins and the product lands on
    # the wrong medium.
    for medium in (TP1, IP):
        root = _root(medium)
        medium_id = _one(root, 'MediumType').get('Id')
        assert medium_id == f"MT-{medium['code']}"
        assert _one(root, 'MaskVersion').get('MediumTypeRefId') == medium_id
        assert _one(root, 'Hardware2Program').get('MediumTypes') == medium_id
        assert _one(root, 'ApplicationProgram').get('MaskVersion') == \
            _one(root, 'MaskVersion').get('Id')


def test_neither_medium_claims_a_domain_address():
    # 16 bits is PL110's domain address. TP1 and IP have none, and declaring one
    # describes a device on a different bus entirely.
    for medium in (TP1, IP):
        assert _one(_root(medium), 'MediumType').get('DomainAddressLength') == '0'


def test_ip_hardware_is_marked_ip_enabled():
    # Hardware/@IsIPEnabled is what makes ETS offer the IP settings tab, without
    # which a KNXnet/IP device cannot be given an address assignment method.
    assert _hardware(_root(IP)).get('IsIPEnabled') == 'true'


def test_tp1_hardware_is_not_marked_ip_enabled():
    assert _hardware(_root(TP1)).get('IsIPEnabled') is None


def test_hardware_id_distinguishes_products_of_one_manufacturer():
    # Two products of the same manufacturer once shared the id 'M-00FA_H-0001'
    # while differing in content. ETS keys hardware by that id and rejects the
    # second import: "the product has a different hash than the existing
    # product."
    tp1 = _hardware(_root(TP1, hardware_serial=1))
    ip = _hardware(_root(IP, hardware_serial=2))
    assert tp1.get('Id') != ip.get('Id')
    assert tp1.get('Id') == 'M-00FA_H-0001-1'
    assert ip.get('Id') == 'M-00FA_H-0002-1'


def test_hardware_id_tracks_the_serial_and_version_it_declares():
    hardware = _hardware(_root(TP1, hardware_serial=7, hardware_version=3))
    assert hardware.get('SerialNumber') == '0007'
    assert hardware.get('VersionNumber') == '3'
    assert hardware.get('Id') == 'M-00FA_H-0007-3'


def test_both_media_agree_on_the_manufacturer_they_share():
    # MasterData/Manufacturer is shared by every product of one manufacturer.
    # The two files used to name it after whichever product they described, so
    # importing both told ETS two different things about M-00FA.
    path = './k:MasterData/k:Manufacturers/k:Manufacturer'
    tp1 = _root(TP1).find(path, NS)
    ip = _root(IP).find(path, NS)
    assert tp1.get('Id') == ip.get('Id')
    assert tp1.get('Name') == ip.get('Name')


def test_a_legacy_export_is_refused_rather_than_rendered():
    # A bare integer is the pre-fix shape, holding a C++ enum ordinal. Rendering
    # it would produce a plausible-looking product declaring the wrong bus.
    try:
        export_product_json_to_knxprod(_doc(2))
    except SystemExit as error:
        assert 'ordinal' in str(error)
    else:
        raise AssertionError('a legacy medium value must not be rendered')


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
        print(f'{failures} medium export test(s) failed')
        return 1
    print('medium export tests passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
