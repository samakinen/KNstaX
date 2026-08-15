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
    # "M-<mfr>_A-<appnr>-<version>-..." (the KNX XSD rejects free-form ids);
    # the internal profile key only appears in the JSON/kaenx outputs.
    expected_id_prefix = f"M-{expected['manufacturer_id']:04X}_A-"
    app_id = app.get('Id') or ''
    if not app_id.startswith(expected_id_prefix):
        raise SystemExit(
            f'ApplicationProgram Id format mismatch in {path}: '
            f"expected prefix {expected_id_prefix!r}, got {app_id!r}")

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
