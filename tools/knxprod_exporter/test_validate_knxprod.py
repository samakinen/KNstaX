#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""
Smoke test for the knxprod exporter wrapper.
Runs the wrapper, parses the generated JSON, Kaenx XML, and .knxprod.xml files,
and verifies that the end-to-end export workflow preserves the expected product metadata.
"""
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

KNX_NS = 'http://knx.org/xml/project/23'
NS = {'knx': KNX_NS}
REPO_ROOT = Path(__file__).resolve().parents[2]
WRAPPER = REPO_ROOT / 'tools' / 'knxprod_exporter' / 'run_exporter.sh'
OUT_DIR = Path(
    os.environ.get(
        'KNXPROD_EXPORTER_OUT_DIR',
        str(REPO_ROOT / 'build' / 'knxprod_exporter_artifacts')
    )
)

EXPECTED_EXPORTS = {
    'pilot_a': {
        'profile_key': 'pilot_a_tp1_actuator',
        'display_name': 'Pilot A TP1 actuator',
        'manufacturer_id': 250,
        'group_object_count': 2,
        'features': {
            'persistenceEnabled': True,
            'securityCapable': False,
            'readResponsesEnabled': True,
            'diagnosticsEnabled': False,
        },
        'capacities': {
            'groupObjectCount': 2,
            'addressTableEntries': 2,
            'associationEntries': 2,
            # Stack-wide table ceilings, not per-product counts: what ETS reads
            # as the number of group addresses and associations the device can
            # take. They come from the address/association table domains.
            'addressTableMaxEntries': 256,
            'associationTableMaxEntries': 512,
            'autoResponseQueueCapacity': 2,
            'transmissionOutcomeQueueCapacity': 4,
        },
        'communication_objects': [
            {
                'exportNumber': 1,
                'logicalId': 0,
                'key': 'command',
                'displayName': 'Command',
                'defaultAddress': 2305,
                'dpt': '1.1',
            },
            {
                'exportNumber': 2,
                'logicalId': 1,
                'key': 'status',
                'displayName': 'Status',
                'defaultAddress': 2306,
                'dpt': '1.1',
            },
        ],
    },
    'pilot_b': {
        'profile_key': 'pilot_b_tp1_signal_indicator',
        'display_name': 'Pilot B TP1 signal indicator',
        'manufacturer_id': 251,
        'group_object_count': 2,
        'features': {
            'persistenceEnabled': True,
            'securityCapable': False,
            'readResponsesEnabled': True,
            'diagnosticsEnabled': False,
        },
        'capacities': {
            'groupObjectCount': 2,
            'addressTableEntries': 2,
            'associationEntries': 2,
            # Stack-wide table ceilings, not per-product counts: what ETS reads
            # as the number of group addresses and associations the device can
            # take. They come from the address/association table domains.
            'addressTableMaxEntries': 256,
            'associationTableMaxEntries': 512,
            'autoResponseQueueCapacity': 2,
            'transmissionOutcomeQueueCapacity': 4,
        },
        'communication_objects': [
            {
                'exportNumber': 1,
                'logicalId': 0,
                'key': 'command',
                'displayName': 'Command',
                'defaultAddress': 2570,
                'dpt': '1',
            },
            {
                'exportNumber': 2,
                'logicalId': 1,
                'key': 'state',
                'displayName': 'State',
                'defaultAddress': 2571,
                'dpt': '1',
            },
        ],
    },
}


def require(elem, xpath):
    node = elem.find(xpath, NS)
    if node is None:
        raise SystemExit(f'Missing required element: {xpath}')
    return node


def expect_equal(actual, expected, context):
    if actual != expected:
        raise SystemExit(f'{context}: expected {expected!r}, got {actual!r}')


def check_json_file(path, expected):
    with open(path, 'r', encoding='utf-8') as handle:
        data = json.load(handle)

    identity = data.get('identity', {})
    expect_equal(identity.get('profileKey'), expected['profile_key'], f'ProfileKey mismatch in {path}')
    expect_equal(identity.get('productDisplayName'), expected['display_name'], f'DisplayName mismatch in {path}')
    expect_equal(identity.get('manufacturerId'), expected['manufacturer_id'], f'ManufacturerId mismatch in {path}')

    features = data.get('features', {})
    expect_equal(features, expected['features'], f'FeatureFlags mismatch in {path}')

    capacities = data.get('capacities', {})
    expect_equal(capacities, expected['capacities'], f'Capacities mismatch in {path}')

    communication_objects = data.get('communicationObjects', [])
    expect_equal(len(communication_objects), expected['group_object_count'], f'CommunicationObject count mismatch in {path}')
    for index, expected_object in enumerate(expected['communication_objects']):
        actual_object = communication_objects[index]
        for key in ('exportNumber', 'logicalId', 'key', 'displayName', 'defaultAddress'):
            expect_equal(actual_object.get(key), expected_object[key], f'{key} mismatch in {path} object #{index + 1}')

        actual_dpt = str(actual_object.get('dpt_main'))
        dpt_sub = actual_object.get('dpt_sub')
        if dpt_sub not in (None, 0):
            actual_dpt += f'.{dpt_sub}'
        expect_equal(actual_dpt, expected_object['dpt'], f'DPT mismatch in {path} object #{index + 1}')


def check_kaenx_file(path, expected):
    tree = ET.parse(path)
    root = tree.getroot()
    product = root.find('./Product')
    if product is None:
        raise SystemExit(f'Missing Product element in {path}')

    expect_equal(product.findtext('./ProfileKey'), expected['profile_key'], f'ProfileKey mismatch in {path}')
    expect_equal(product.findtext('./DisplayName'), expected['display_name'], f'DisplayName mismatch in {path}')
    expect_equal(product.findtext('./ManufacturerId'), str(expected['manufacturer_id']), f'ManufacturerId mismatch in {path}')

    for key, value in expected['features'].items():
        expect_equal(product.findtext(f'./Features/{key}'), str(value).lower(), f'Feature {key} mismatch in {path}')

    for key, value in expected['capacities'].items():
        expect_equal(product.findtext(f'./Capacities/{key}'), str(value), f'Capacity {key} mismatch in {path}')

    communication_objects = product.findall('./CommunicationObjects/CommunicationObject')
    expect_equal(len(communication_objects), expected['group_object_count'], f'CommunicationObject count mismatch in {path}')
    for index, expected_object in enumerate(expected['communication_objects']):
        actual_object = communication_objects[index]
        expect_equal(actual_object.get('ExportNumber'), str(expected_object['exportNumber']), f'ExportNumber mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.get('LogicalId'), str(expected_object['logicalId']), f'LogicalId mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.findtext('./Key'), expected_object['key'], f'Key mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.findtext('./DisplayName'), expected_object['displayName'], f'DisplayName mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.findtext('./DefaultAddressEncoded'), str(expected_object['defaultAddress']), f'DefaultAddress mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.findtext('./DPT'), expected_object['dpt'], f'DPT mismatch in {path} object #{index + 1}')


def check_knxprod_file(path, expected):
    tree = ET.parse(path)
    root = tree.getroot()
    manufacturer = require(root, './knx:MasterData/knx:Manufacturers/knx:Manufacturer')
    expect_equal(manufacturer.get('KnxManufacturerId'), str(expected['manufacturer_id']), f'ManufacturerId mismatch in {path}')

    app = require(
        root,
        './knx:ManufacturerData/knx:Manufacturer/knx:ApplicationPrograms/knx:ApplicationProgram',
    )
    # The knxprod ApplicationProgram Id must use the ETS-conformant format
    # "M-<mfr>_A-<appnr:4 hex>-<version:2 hex>-<hash:4 hex>" (the KNX XSD
    # rejects free-form ids); the internal profile key only appears in the
    # JSON/kaenx outputs. The widths are not cosmetic: the application number
    # is a 16-bit field, and tools read the version back out of the id by
    # offset, so a six-digit number silently shifts every later group.
    expected_id = (f"M-{expected['manufacturer_id']:04X}"
                   r'_A-[0-9A-F]{4}-[0-9A-F]{2}-0000$')
    app_id = app.get('Id') or ''
    if not re.match(expected_id, app_id):
        raise SystemExit(
            f'ApplicationProgram Id format mismatch in {path}: '
            f'expected {expected_id!r}, got {app_id!r}')

    # Without a declared registration state ETS treats the product as one it may
    # only import under a manufacturer test licence, and refuses the file with
    # "No valid license found to test the unregistered product(s) in this file."
    for element_path in ('.//knx:Product', './/knx:Hardware2Program'):
        registration = require(root, element_path + '/knx:RegistrationInfo')
        expect_equal(registration.get('RegistrationStatus'), 'Registered',
                     f'{element_path} is not declared registered in {path}')
    registration_number = require(
        root, './/knx:Hardware2Program/knx:RegistrationInfo').get('RegistrationNumber') or ''
    if not re.fullmatch(r'\d{4}/\d+', registration_number):
        raise SystemExit(
            f'Hardware2Program RegistrationNumber malformed in {path}: '
            f'{registration_number!r}')

    # ETS reads the load state machine and the offset back out of the code
    # segment's id, and rejects the download of anything else ("is not a valid
    # id ID"), so the id has to agree with the attributes beside it.
    segment = require(root, './/knx:ApplicationProgram/knx:Static/knx:Code/knx:RelativeSegment')
    expected_segment_id = '{0}_RS-{1:02d}-{2:05d}'.format(
        app_id, int(segment.get('LoadStateMachine')), int(segment.get('Offset')))
    expect_equal(segment.get('Id'), expected_segment_id,
                 f'RelativeSegment Id does not match its own attributes in {path}')

    # The device's table ceilings have to reach the knxprod, or ETS links no
    # group address at all ("The maximum associations of the device are been
    # reached"). They come from the stack, so they are compared against what the
    # JSON reported rather than against a number written here twice.
    for element_name, capacity_key in (('AddressTable', 'addressTableMaxEntries'),
                                       ('AssociationTable', 'associationTableMaxEntries')):
        table = require(root, f'.//knx:ApplicationProgram/knx:Static/knx:{element_name}')
        expect_equal(table.get('MaxEntries'), str(expected['capacities'][capacity_key]),
                     f'{element_name} MaxEntries mismatch in {path}')

    communication_objects = root.findall('.//knx:ComObject', NS)
    expect_equal(len(communication_objects), expected['group_object_count'], f'GroupObject count mismatch in {path}')
    for index, expected_object in enumerate(expected['communication_objects']):
        actual_object = communication_objects[index]
        expect_equal(actual_object.get('Number'), str(expected_object['exportNumber']), f'ComObject number mismatch in {path} object #{index + 1}')
        expect_equal(actual_object.get('Name'), expected_object['displayName'], f'ComObject name mismatch in {path} object #{index + 1}')


def main():
    # Run the wrapper to regenerate outputs
    subprocess.run([str(WRAPPER)], check=True, cwd=REPO_ROOT)

    for prefix, expected in EXPECTED_EXPORTS.items():
        check_json_file(OUT_DIR / f'{prefix}_export.json', expected)
        check_kaenx_file(OUT_DIR / f'{prefix}_export.kaenx.xml', expected)
        check_knxprod_file(OUT_DIR / f'{prefix}_export.knxprod.xml', expected)

    print('knxprod export validation passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
