#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Run the exporter wrapper and validate produced .knxprod files.

Uses full XSD validation when `xmlschema` is installed. If not, falls back to
strict structural checks for the KNX XML subset emitted by this exporter so the
test remains self-contained in minimal environments.
"""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import xmlschema
except Exception:
    xmlschema = None


ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parent.parent
WRAPPER = ROOT / 'run_exporter.sh'
SCHEMA = ROOT / 'schema' / 'knx_schema_version23.xsd'
OUT_DIR = Path(
    os.environ.get(
        'KNXPROD_EXPORTER_OUT_DIR',
        str(REPO_ROOT / 'build' / 'knxprod_exporter_artifacts')
    )
)
KNX_NS = 'http://knx.org/xml/project/23'
NS = {'knx': KNX_NS}


def require(elem, xpath: str):
    node = elem.find(xpath, NS)
    if node is None:
        raise AssertionError(f'missing required element: {xpath}')
    return node


def require_attrs(elem, names):
    missing = [name for name in names if elem.get(name) in (None, '')]
    if missing:
        raise AssertionError(f'missing required attributes {missing} on {elem.tag}')


def validate_knx_subset(path: Path):
    tree = ET.parse(path)
    root = tree.getroot()
    if root.tag != f'{{{KNX_NS}}}KNX':
        raise AssertionError(f'unexpected root tag: {root.tag}')

    schema_location = root.get('{http://www.w3.org/2001/XMLSchema-instance}schemaLocation', '')
    if 'http://knx.org/xml/project/23' not in schema_location:
        raise AssertionError('missing KNX schemaLocation')

    master = require(root, './knx:MasterData')
    require_attrs(master, ['Version', 'Signature', 'Id'])

    medium = require(master, './knx:MediumTypes/knx:MediumType')
    require_attrs(medium, ['Id', 'Number', 'Name', 'DomainAddressLength'])

    mask = require(master, './knx:MaskVersions/knx:MaskVersion')
    require_attrs(mask, ['Id', 'Name', 'MaskVersion', 'ManagementModel', 'MediumTypeRefId'])

    manufacturer = require(master, './knx:Manufacturers/knx:Manufacturer')
    require_attrs(manufacturer, ['Id', 'Name', 'KnxManufacturerId'])

    data_manufacturer = require(root, './knx:ManufacturerData/knx:Manufacturer')
    require_attrs(data_manufacturer, ['RefId'])
    if data_manufacturer.get('RefId') != manufacturer.get('Id'):
        raise AssertionError('ManufacturerData RefId does not match MasterData Manufacturer Id')

    app = require(data_manufacturer, './knx:ApplicationPrograms/knx:ApplicationProgram')
    require_attrs(
        app,
        [
            'Id',
            'ApplicationNumber',
            'ApplicationVersion',
            'ProgramType',
            'MaskVersion',
            'Name',
            'LoadProcedureStyle',
            'PeiType',
            'DefaultLanguage',
            'DynamicTableManagement',
            'Linkable',
        ],
    )
    if app.get('MaskVersion') != mask.get('Id'):
        raise AssertionError('ApplicationProgram MaskVersion does not reference declared mask version')

    com_objects = root.findall('.//knx:ComObject', NS)
    if not com_objects:
        raise AssertionError('expected at least one ComObject')

    required_comobject_attrs = [
        'Id',
        'Name',
        'Text',
        'Number',
        'FunctionText',
        'ObjectSize',
        'ReadFlag',
        'WriteFlag',
        'CommunicationFlag',
        'TransmitFlag',
        'UpdateFlag',
        'ReadOnInitFlag',
    ]
    for com_object in com_objects:
        require_attrs(com_object, required_comobject_attrs)

def run_wrapper():
    if not WRAPPER.exists():
        print('run_exporter.sh not found', file=sys.stderr)
        return 2
    print('Running exporter wrapper...')
    subprocess.check_call([str(WRAPPER)], cwd=REPO_ROOT)
    return 0


def validate_file(path: Path):
    if xmlschema is not None:
        print(f'Validating {path} against {SCHEMA}...')
        schema = xmlschema.XMLSchema(str(SCHEMA))
        try:
            schema.validate(str(path))
        except xmlschema.validators.exceptions.XMLSchemaValidationError as e:
            print('Validation failed:', e, file=sys.stderr)
            return False
        return True

    print(f'xmlschema not installed; running structural KNX validation for {path}...')
    try:
        validate_knx_subset(path)
    except (ET.ParseError, AssertionError) as e:
        print('Validation failed:', e, file=sys.stderr)
        return False
    return True


def main():
    run_wrapper()
    ok = True
    for name in ('pilot_a_export.knxprod.xml', 'pilot_b_export.knxprod.xml'):
        p = OUT_DIR / name
        if not p.exists():
            print(f'Expected output missing: {p}', file=sys.stderr)
            ok = False
            continue
        if not validate_file(p):
            ok = False

    if not ok:
        print('One or more validations failed', file=sys.stderr)
        return 2

    print('Both .knxprod files validated successfully')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
