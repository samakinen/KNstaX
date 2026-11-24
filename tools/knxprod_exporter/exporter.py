#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""
Simple Python Kaenx/ETS exporter that consumes JSON produced by the
`tools/product_exporter/product_exporter` binary and emits a minimal
Kaenx-style XML file per product.

Usage:
  python3 exporter.py --input pilot_a_export.json --output pilot_a_export.kaenx.xml

If no input is provided the script will look for `pilot_a_export.json` and
`pilot_b_export.json` in the current working directory.
"""
import argparse
import json
import sys
import xml.etree.ElementTree as ET
from xml.dom import minidom


def prettify(elem: ET.Element) -> str:
    rough = ET.tostring(elem, 'utf-8')
    parsed = minidom.parseString(rough)
    return parsed.toprettyxml(indent='  ')


def export_product_json_to_kaenx(json_obj) -> str:
    root = ET.Element('KNXProductExport')
    product = ET.SubElement(root, 'Product')

    identity = json_obj.get('identity', {})
    ET.SubElement(product, 'ProfileKey').text = identity.get('profileKey', '')
    ET.SubElement(product, 'DisplayName').text = identity.get('productDisplayName', '')
    ET.SubElement(product, 'ManufacturerId').text = str(identity.get('manufacturerId', ''))
    medium_code, medium_name, mask_version, ip_enabled = _medium_facts(identity)
    medium_elem = ET.SubElement(product, 'Medium')
    medium_elem.text = medium_name
    medium_elem.set('Code', str(medium_code))
    medium_elem.set('MaskVersion', str(mask_version))
    medium_elem.set('IpEnabled', str(ip_enabled).lower())

    app = ET.SubElement(product, 'ApplicationProgram')
    applicationProgram = identity.get('applicationProgram', {})
    ET.SubElement(app, 'Number').text = str(applicationProgram.get('number', 0))
    ET.SubElement(app, 'Version').text = str(applicationProgram.get('version', 0))

    features = json_obj.get('features', {})
    feats = ET.SubElement(product, 'Features')
    for key in ('persistenceEnabled', 'securityCapable', 'readResponsesEnabled', 'diagnosticsEnabled'):
        ET.SubElement(feats, key).text = str(features.get(key, False)).lower()

    caps = json_obj.get('capacities', {})
    capacities = ET.SubElement(product, 'Capacities')
    for k in ('groupObjectCount', 'addressTableEntries', 'associationEntries',
              'addressTableMaxEntries', 'associationTableMaxEntries', 'maxApduLength',
              'autoResponseQueueCapacity', 'transmissionOutcomeQueueCapacity'):
        ET.SubElement(capacities, k).text = str(caps.get(k, 0))

    comms = ET.SubElement(product, 'CommunicationObjects')
    for c in json_obj.get('communicationObjects', []):
        co = ET.SubElement(comms, 'CommunicationObject')
        if 'exportNumber' in c:
            co.set('ExportNumber', str(c['exportNumber']))
        if 'logicalId' in c:
            co.set('LogicalId', str(c['logicalId']))

        ET.SubElement(co, 'Key').text = c.get('key', '')
        ET.SubElement(co, 'DisplayName').text = c.get('displayName', '')
        ET.SubElement(co, 'DefaultAddressEncoded').text = str(c.get('defaultAddress', ''))
        if 'dpt_main' in c:
            ET.SubElement(co, 'DPT').text = f"{c.get('dpt_main')}" + (f".{c.get('dpt_sub')}" if c.get('dpt_sub') not in (None, 0) else '')
        else:
            dpt = c.get('dpt')
            if isinstance(dpt, dict) and 'main' in dpt:
                text = str(dpt.get('main')) + (f".{dpt.get('sub')}" if dpt.get('sub') else '')
                ET.SubElement(co, 'DPT').text = text

        ET.SubElement(co, 'ValueType').text = str(c.get('valueType', ''))
        for flag in ('readable', 'writable', 'transmit', 'receivable', 'persisted'):
            ET.SubElement(co, flag).text = str(c.get(flag, False)).lower()

    return prettify(root)


def _dpt_object_size(dpt_main: int) -> str:
    """Return the KNX ObjectSize string for a given DPT main type number."""
    # The schema spells sizes below an octet in the singular: '2 Bit', '4 Bit'.
    if dpt_main == 1:
        return '1 Bit'
    if dpt_main in (2,):
        return '2 Bit'
    if dpt_main in (3,):
        return '4 Bit'
    if dpt_main in (4, 5, 6):
        return '1 Byte'
    if dpt_main in (7, 8, 9):
        return '2 Bytes'
    if dpt_main in (10, 11):
        return '3 Bytes'
    if dpt_main in (12, 13, 14):
        return '4 Bytes'
    if dpt_main in (20, 21):  # N8 HVAC enums, B8 status words
        return '1 Byte'
    if dpt_main in (22,):     # B16 status words (e.g. 22.101 StatusRHCC)
        return '2 Bytes'
    return '1 Byte'  # safe fallback


def _dpt_size_in_bit(dpt_main: int) -> int:
    """Return the DPT size in bits for the <DatapointType SizeInBit> attribute."""
    size_map = {1: 1, 2: 2, 3: 4, 4: 8, 5: 8, 6: 8,
                7: 16, 8: 16, 9: 16,
                10: 24, 11: 24,
                12: 32, 13: 32, 14: 32,
                20: 8, 21: 8, 22: 16}
    return size_map.get(dpt_main, 8)


# Parameter valueKind: ExportParameterValueKind in export_descriptor.hpp. The
# width of a value is not derived from it here; the export carries sizeInBit.
_PARAM_KIND_NONE = 0
_PARAM_KIND_BOOLEAN = 1
_PARAM_KIND_UNSIGNED8 = 2
_PARAM_KIND_UNSIGNED16 = 3
_PARAM_KIND_SIGNED16 = 4
_PARAM_KIND_ENUM = 5
_PARAM_KIND_TEXT = 6
_PARAM_KIND_FLOAT = 7
_PARAM_KIND_FLOAT_DPT9 = 8

def _param_default_text(kind: int, value) -> str:
    """Format a parameter default for the knxprod Value attribute.

    Integral values are emitted WITHOUT a decimal separator (so "22", never
    "22.0") for both float kinds: separator-free values survive the
    locale-sensitive parsers in third-party knxprod tooling unchanged."""
    if value is None:
        value = 0
    if kind in (_PARAM_KIND_FLOAT, _PARAM_KIND_FLOAT_DPT9):
        f = float(value)
        if f == int(f):
            return str(int(f))
        return repr(f)
    return str(int(round(float(value))))


def _condition_tests(condition):
    """The <when test="..."> texts for one visibility condition, outermost first.

    ETS tests one controlling parameter per <choose>: a value, a list of values
    separated by spaces, or a comparison. A range has no single test, so it is
    two nested ones."""
    op = condition.get('op', 'anyOf')
    values = [int(v) for v in condition.get('values', [])]
    if op == 'anyOf' and values:
        return [' '.join(str(v) for v in values)]
    if op == 'notEqual' and len(values) == 1:
        return [f'!={values[0]}']
    if op == 'range' and len(values) == 2:
        return [f'>={values[0]}', f'<={values[1]}']
    raise ValueError(f'visibility condition {condition!r} is not one the exporter knows')


def _condition_holds(condition, value: int) -> bool:
    """Whether the controlling parameter holding `value` satisfies `condition`."""
    op = condition.get('op', 'anyOf')
    values = [int(v) for v in condition.get('values', [])]
    if op == 'anyOf':
        return value in values
    if op == 'notEqual':
        return value != values[0]
    if op == 'range':
        return values[0] <= value <= values[1]
    raise ValueError(f'visibility condition {condition!r} is not one the exporter knows')


def _under_condition(parent, condition, ref_ids, block_counts, own_block, instance):
    """The element to put content in so that ETS shows it only while the
    condition holds. A parameter of `own_block` is the one of `instance`; a
    parameter of any other block is that block's only instance."""
    if not condition:
        return parent
    controlling_block = int(condition['block'])
    controlling_instance = instance if controlling_block == own_block else 0
    if controlling_block not in block_counts:
        raise ValueError(f'a condition names parameter block {controlling_block}, which the product does not have')
    if controlling_instance >= block_counts[controlling_block]:
        raise ValueError(f'a condition names instance {controlling_instance + 1} of parameter block '
                         f'{controlling_block}, which has {block_counts[controlling_block]}')
    key = (controlling_block, controlling_instance, int(condition['parameter']))
    if key not in ref_ids:
        raise ValueError(f'a condition names parameter {condition["parameter"]} of block {controlling_block}, '
                         'which the block does not have')
    for test in _condition_tests(condition):
        choose = ET.SubElement(parent, 'choose')
        choose.set('ParamRefId', ref_ids[key])
        parent = ET.SubElement(choose, 'when')
        parent.set('test', test)
    return parent


def _check_object_views(cos, views):
    """Refuse views ETS could not use: of an unknown port, of a datapoint type
    of another size than the port's, or two views of one port that could be
    shown at once (ETS would list the object twice).

    Two views of one port must test the same parameter instance, and no value
    of it may satisfy both. The values are tried over 0..255, the range of the
    selection parameters that choose a function."""
    by_id = {int(c['logicalId']): c for c in cos if 'logicalId' in c}
    by_port = {}
    for view in views:
        port = int(view['logicalId'])
        co = by_id.get(port)
        if co is None:
            raise ValueError(f'an object view names port {port}, which the product does not have')
        if 'dpt_main' in view and co.get('dpt_main') is not None:
            if _dpt_size_in_bit(int(view['dpt_main'])) != _dpt_size_in_bit(int(co['dpt_main'])):
                raise ValueError(
                    f"object view of port {port} ({co.get('key')}) shows DPT {view['dpt_main']}.{view.get('dpt_sub', 0)}, "
                    f"which is not the size of the port's DPT {co['dpt_main']}")
        by_port.setdefault(port, []).append(view)
    for port, port_views in by_port.items():
        for i, first in enumerate(port_views):
            for second in port_views[i + 1:]:
                a, b = first.get('visible'), second.get('visible')
                name = by_id[port].get('key', port)
                if not a or not b:
                    raise ValueError(f'port {name} has a view without a condition and another view: '
                                     'ETS would show the object twice')
                same = (int(a['block']), int(first.get('instance', 0)), int(a['parameter'])) == \
                       (int(b['block']), int(second.get('instance', 0)), int(b['parameter']))
                if not same:
                    raise ValueError(f'two views of port {name} test different parameters, '
                                     'so the exporter cannot tell that they never show at once')
                both = [v for v in range(256) if _condition_holds(a, v) and _condition_holds(b, v)]
                if both:
                    raise ValueError(f'two views of port {name} are both shown for the value {both[0]}')
    return by_port


def _numeric_parameter_type(kind: int, size_in_bit: int, p):
    """(tag, attrs, name) of the ParameterType for a parameter without options."""
    fmt = lambda v: str(int(v)) if float(v) == int(v) else repr(float(v))
    has_bounds = 'min' in p and 'max' in p
    if kind in (_PARAM_KIND_FLOAT, _PARAM_KIND_FLOAT_DPT9):
        # Bounds are written without a decimal separator where they are whole:
        # some tooling (Kaenx-Creator) parses these strings with the system
        # locale, and separator-free integers read the same under every one.
        # The full DPT 9 range is ±670760.96.
        tag = 'TypeFloat'
        if kind == _PARAM_KIND_FLOAT_DPT9:
            attrs = {'Encoding': 'DPT 9', 'minInclusive': '-670760', 'maxInclusive': '670760'}
        else:
            attrs = {'Encoding': 'IEEE-754 Single',
                     'minInclusive': '-3.40282e+38', 'maxInclusive': '3.40282e+38'}
        if has_bounds:
            attrs['minInclusive'] = fmt(p['min'])
            attrs['maxInclusive'] = fmt(p['max'])
        name = ('dpt9' if kind == _PARAM_KIND_FLOAT_DPT9 else 'float')
        return tag, attrs, f"{name}_{attrs['minInclusive']}_{attrs['maxInclusive']}"
    if kind == _PARAM_KIND_BOOLEAN:
        # A yes/no value without option texts is a check box, not a number box.
        return ('TypeNumber',
                {'SizeInBit': '8', 'Type': 'unsignedInt', 'minInclusive': '0', 'maxInclusive': '1',
                 'UIHint': 'CheckBox'},
                'bool')
    signed = kind == _PARAM_KIND_SIGNED16
    low = -(1 << (size_in_bit - 1)) if signed else 0
    high = (1 << (size_in_bit - 1)) - 1 if signed else (1 << size_in_bit) - 1
    if has_bounds:
        low, high = int(p['min']), int(p['max'])
    return ('TypeNumber',
            {'SizeInBit': str(size_in_bit), 'Type': 'signedInt' if signed else 'unsignedInt',
             'minInclusive': str(low), 'maxInclusive': str(high)},
            f"{'s' if signed else 'u'}{size_in_bit}_{low}_{high}")


def _emit_parameter_blocks(static_elem, dynamic_channel, app_ref: str, rs_ref: str, json_obj):
    """Fill ParameterTypes/Parameters/ParameterRefs and the Dynamic section from
    the `parameterBlocks` of the product (docs/design/parameter-model.md).

    A block that repeats is written out once per instance: its parameters, and
    one ParameterBlock named after the instance. Every memory offset is
    base + instance × stride + offset, all three from the C++ layout; nothing
    about the layout is derived here. Returns the size of the image, which is
    what ETS must download, the ParameterRef id of every (block, instance,
    parameter), and the repeat count of every block: what a condition on an
    object needs."""
    blocks = json_obj.get('parameterBlocks', [])
    image_size = int(json_obj.get('parameterImage', {}).get('size', 0))
    if not blocks:
        return 0, {}, {}

    # The layout is the firmware's; this only refuses a document that
    # contradicts itself, so a broken export cannot become a product file.
    end = 0
    for block in blocks:
        base, stride, count = int(block['base']), int(block['stride']), int(block['count'])
        if base != end:
            raise ValueError(f"parameter block {block.get('key')!r} starts at {base}, expected {end}")
        for parameter in block['parameters']:
            if int(parameter['offset']) + int(parameter['sizeInBit']) // 8 > stride:
                raise ValueError(
                    f"parameter {parameter.get('key')!r} of block {block.get('key')!r} "
                    f"does not fit the block's {stride} octets")
        end = base + stride * count
    if end != image_size:
        raise ValueError(f'the parameter blocks end at {end}, the image is {image_size} octets')

    types_elem = ET.SubElement(static_elem, 'ParameterTypes')
    params_elem = ET.SubElement(static_elem, 'Parameters')
    refs_elem = ET.SubElement(static_elem, 'ParameterRefs')

    # Option lists first: one ParameterType each, shared by every parameter and
    # every instance that uses the list.
    option_type_ids = {}
    for option_type in json_obj.get('optionTypes', []):
        type_id = f"{app_ref}_PT-opt{int(option_type['id'])}"
        option_type_ids[int(option_type['id'])] = type_id
        pt = ET.SubElement(types_elem, 'ParameterType')
        pt.set('Id', type_id)
        pt.set('Name', type_id.rsplit('_', 1)[-1])
        restriction = ET.SubElement(pt, 'TypeRestriction',
                                    {'Base': 'Value', 'SizeInBit': str(int(option_type['sizeInBit']))})
        for index, option in enumerate(option_type.get('options', [])):
            value = int(option.get('value', 0))
            enum = ET.SubElement(restriction, 'Enumeration')
            enum.set('Text', str(option.get('label', value)))
            enum.set('Value', str(value))
            enum.set('Id', f'{type_id}_EN-{value}')
            enum.set('DisplayOrder', str(index))

    numeric_type_ids = {}

    def numeric_type_for(parameter):
        tag, attrs, name = _numeric_parameter_type(int(parameter['valueKind']),
                                                   int(parameter['sizeInBit']), parameter)
        key = (tag, tuple(sorted(attrs.items())))
        existing = numeric_type_ids.get(key)
        if existing is not None:
            return existing
        type_id = f'{app_ref}_PT-n{len(numeric_type_ids) + 1}'
        pt = ET.SubElement(types_elem, 'ParameterType')
        pt.set('Id', type_id)
        pt.set('Name', name)
        ET.SubElement(pt, tag, attrs)
        numeric_type_ids[key] = type_id
        return type_id

    # Pass 1: the parameters and their refs, instance by instance, in image
    # order. Conditions may point forwards, so the Dynamic part comes after.
    ref_ids = {}
    number = 0
    for block in blocks:
        block_index = int(block['index'])
        count = int(block['count'])
        for instance in range(count):
            instance_tag = f'{instance + 1}' if count > 1 else ''
            for parameter in block['parameters']:
                number += 1
                kind = int(parameter['valueKind'])
                if 'optionType' in parameter:
                    type_id = option_type_ids[int(parameter['optionType'])]
                else:
                    type_id = numeric_type_for(parameter)

                param_id = f'{app_ref}_P-{number}'
                pe = ET.SubElement(params_elem, 'Parameter')
                pe.set('Id', param_id)
                pe.set('Name', f"{block.get('key', 'block')}{instance_tag}_{parameter.get('key')}")
                pe.set('ParameterType', type_id)
                pe.set('Text', parameter.get('name') or parameter.get('key', ''))
                pe.set('Value', _param_default_text(kind, parameter.get('default')))
                if parameter.get('unit'):
                    pe.set('SuffixText', parameter['unit'])
                mem = ET.SubElement(pe, 'Memory')
                mem.set('CodeSegment', rs_ref)
                mem.set('Offset', str(int(block['base']) + instance * int(block['stride'])
                                      + int(parameter['offset'])))
                mem.set('BitOffset', '0')

                ref_id = f'{param_id}_R-{number}'
                pref = ET.SubElement(refs_elem, 'ParameterRef')
                pref.set('Id', ref_id)
                pref.set('RefId', param_id)
                ref_ids[(block_index, instance, int(parameter['id']))] = ref_id

    block_counts = {int(block['index']): int(block['count']) for block in blocks}

    def under_condition(parent, condition, own_block, instance):
        return _under_condition(parent, condition, ref_ids, block_counts, own_block, instance)

    # Pass 2: the pages. A block instance is one page; a section of the block
    # is a page of its own after it. A block whose parameters are all in
    # sections has no page of its own.
    page_number = 0
    for block in blocks:
        block_index = int(block['index'])
        count = int(block['count'])
        name = block.get('name') or block.get('key') or 'Settings'
        sections = sorted(block.get('sections') or [], key=lambda section: int(section['index']))
        for instance in range(count):
            if '{n}' in name:
                text = name.replace('{n}', str(instance + 1))
            else:
                text = f'{name} {instance + 1}' if count > 1 else name
            page_name = f"{block.get('key', 'block')}{instance + 1 if count > 1 else ''}"
            holder = under_condition(dynamic_channel, block.get('visible'), block_index, instance)

            def emit_page(parent, page_key, page_text, members):
                nonlocal page_number
                page_number += 1
                element = ET.SubElement(parent, 'ParameterBlock')
                element.set('Id', f'{app_ref}_PB-{page_number}')
                element.set('Name', page_key)
                element.set('Text', page_text)
                for parameter in members:
                    target = under_condition(element, parameter.get('visible'), block_index, instance)
                    ET.SubElement(target, 'ParameterRefRef').set(
                        'RefId', ref_ids[(block_index, instance, int(parameter['id']))])

            own = [parameter for parameter in block['parameters'] if 'section' not in parameter]
            if own or not sections:
                emit_page(holder, page_name, text, own)
            for section in sections:
                members = [parameter for parameter in block['parameters']
                           if parameter.get('section') == section['index']]
                # A block that appears once and has no page of its own is
                # only a container: its sections carry their own names.
                section_text = section.get('name') or text
                if count > 1 or own:
                    section_text = f"{text}: {section_text}"
                section_holder = under_condition(holder, section.get('visible'), block_index, instance)
                emit_page(section_holder, f"{page_name}_s{int(section['index']) + 1}", section_text, members)

    return image_size, ref_ids, block_counts


# Load State Machine index of the Application Program interface object, as this
# stack registers its interface objects: 0 = Device, 1 = Address Table,
# 2 = Association Table, 3 = Group Object Table, 4 = Application Program.
# ETS addresses load-state transitions by this index, so it must match the
# registration order in bau.cpp.
_LSM_APPLICATION_PROGRAM = 4


# The mask versions whose ETS master data load procedures this exporter knows
# the merge points of. In MV-07B0 (TP1) and MV-57B0 (IP), every procedure that
# loads the Application Program ("all", "par", "par,grp", "ap1") runs
# "Unload 4 ... Load 4, Merge 2" and later "Merge 4" just before it writes the
# tables with LdCtrlWriteRelMem. Another mask may number its merge points
# differently, so it is refused rather than guessed.
_MERGE_ALLOCATE_SEGMENT = 2
_MERGE_WRITE_SEGMENT = 4
_MASKS_WITH_KNOWN_MERGE_POINTS = (0x07B0, 0x57B0)


def _emit_load_procedures(static, mask_version: int, param_bytes: int):
    """Add the product's part of the download: the parameter segment.

    The application program is LoadProcedureStyle="MergedProcedure": ETS runs
    the load procedure of the mask version from its master data (unload, load
    and write the group tables, load-completed, restart) and inserts each
    product <LoadProcedure MergeId="n"> at the LdCtrlMerge step with the same
    id. A <LoadProcedure> without a MergeId has no place in that procedure and
    ETS ignores it, which is why the parameters were once never written.

    Two pieces, at the points the master data provides for them:

      MergeId 2, right after "Load 4": allocate the Application Program's
        relative segment. Mode 0 is the plain data segment; Fill 0 zeroes what
        the write does not cover.
      MergeId 4, before the tables are written: write the parameter image into
        that segment, verified by read-back.

    Unload, load and load-completed of the Application Program are already in
    the master procedure. A product without parameters adds nothing, and the
    element is left out.
    """
    if param_bytes <= 0:
        return
    if mask_version not in _MASKS_WITH_KNOWN_MERGE_POINTS:
        raise SystemExit(
            f'mask version {mask_version:04X}h: the merge points of its ETS load '
            'procedure are not known to this exporter, so the parameter segment '
            'cannot be placed in the download. Supported: '
            + ', '.join(f'{m:04X}h' for m in _MASKS_WITH_KNOWN_MERGE_POINTS) + '.')

    load_procs = ET.SubElement(static, 'LoadProcedures')

    allocate = ET.SubElement(load_procs, 'LoadProcedure')
    allocate.set('MergeId', str(_MERGE_ALLOCATE_SEGMENT))
    segment = ET.SubElement(allocate, 'LdCtrlRelSegment')
    segment.set('LsmIdx', str(_LSM_APPLICATION_PROGRAM))
    segment.set('Size', str(param_bytes))
    segment.set('Mode', '0')
    segment.set('Fill', '0')

    write_proc = ET.SubElement(load_procs, 'LoadProcedure')
    write_proc.set('MergeId', str(_MERGE_WRITE_SEGMENT))
    write = ET.SubElement(write_proc, 'LdCtrlWriteRelMem')
    write.set('ObjIdx', str(_LSM_APPLICATION_PROGRAM))
    write.set('Offset', '0')
    write.set('Size', str(param_bytes))
    write.set('Verify', 'true')


def _dpt_id(dpt_main: int, dpt_sub) -> str:
    """Return the KNX-canonical datapoint-type reference.

    A specific sub-type uses 'DPST-<main>-<sub>' (the 'DPST' prefix marks it as
    a sub-type; the sub number is NOT zero-padded, e.g. DPST-9-1 not DPST-9-001).
    A sub of 0/None means "generic main type", referenced as bare 'DPT-<main>'.
    Emitting any other form (e.g. DPT-9-001, or DPST for a non-existent sub) makes
    importers fall back to the generic main type or reject the product outright."""
    if not dpt_sub:  # 0 or None → generic main type
        return f'DPT-{dpt_main}'
    return f'DPST-{dpt_main}-{dpt_sub}'


# Length of the domain address on each medium, in bits (KNX master data). Open
# media address a domain, TP1 and IP do not.
_MEDIUM_DOMAIN_ADDRESS_LENGTH = {0: 0, 1: 16, 2: 48, 5: 0}


def _medium_facts(identity):
    """The medium as KNX numbers it, plus the mask version that goes with it.

    Returns (code, name, mask_version, ip_enabled). Every one of these comes
    from the C++ product definition rather than being decided here: the medium
    decides which lines ETS will let the device sit on, and the mask version is
    cross-checked against the Device Descriptor the device reports, so neither
    is the renderer's to guess.

    Exports older than that carried `medium` as a bare integer holding the C++
    enum's ordinal — 2 for IP_Routing, which KNX reads as RF. Rejected outright:
    silently rendering it produced a plausible-looking product declaring the
    wrong bus."""
    medium = identity.get('medium', {})
    if not isinstance(medium, dict):
        raise SystemExit(
            'identity.medium is a bare number, so this export predates the '
            'medium fix: the value is a C++ enum ordinal, not a KNX medium '
            'code. Regenerate the JSON with the current product exporter.')
    return (int(medium.get('code', 0)),
            medium.get('name', 'TP'),
            int(medium.get('maskVersion', 0x07B0)),
            bool(medium.get('ipEnabled', False)))


def _ets_id_part(text: str, fallback: str = 'PRODUCT') -> str:
    """Encode a free-form name as an ETS identifier fragment.

    ETS ids are uppercase alphanumeric; every other character is escaped as
    '.XX' with the hex of its byte, so a profile key 'room_tp1' becomes
    'ROOM.5FTP1'. Deriving these from the product identity rather than
    hardcoding them is what keeps the exporter product-agnostic."""
    out = []
    for ch in (text or '').upper():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
        else:
            out.extend(f'.{b:02X}' for b in ch.encode('utf-8'))
    return ''.join(out) or fallback


def export_product_json_to_knxprod(json_obj) -> str:
    # Produce a minimal KNX v2.3 compatible XML structure expected by ETS XSD.
    # Root element: KNX, with MasterData and ManufacturerData/ApplicationPrograms.
    root = ET.Element('KNX')
    root.set('xmlns', 'http://knx.org/xml/project/23')
    root.set('xmlns:xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    root.set('xsi:schemaLocation', 'http://knx.org/xml/project/23 schema/knx_schema_version23.xsd')

    identity = json_obj.get('identity', {})
    cos = json_obj.get('communicationObjects', [])
    app_program = identity.get('applicationProgram', {})
    manufacturer_id = int(identity.get('manufacturerId', 0))
    app_number = int(app_program.get('number', 0))
    app_version = int(app_program.get('version', 0))

    # OpenKNX/Kaenx importer expects ETS-style IDs with hex-encoded parts.
    manu_ref = f'M-{manufacturer_id:04X}'
    # ETS spells an application program id 'M-xxxx_A-<number:4 hex>-<version:2
    # hex>-<hash:4 hex>'. The application number is a 16-bit field, so it is
    # four digits, not six: tools that parse the id back out (OpenKNXproducer
    # reads the version from fixed offsets) fall over on any other width.
    # The trailing group is a placeholder — the ETS signer replaces it with the
    # hash it computes over the program.
    if not 0 <= app_number <= 0xFFFF:
        raise SystemExit(
            f'applicationProgram.number {app_number} does not fit the 16-bit '
            'ETS application number field')
    app_ref = f'{manu_ref}_A-{app_number:04X}-{app_version:02X}-0000'
    # ETS keys hardware by manufacturer + serial + version, and its ids encode
    # that. A fixed 'H-0001' made every product of one manufacturer collide:
    # importing the second one hit "the product has a different hash than the
    # existing product", because the id already named something else.
    hardware_serial = int(identity.get('hardwareSerialNumber', 1))
    hardware_version = int(identity.get('hardwareVersion', 1))
    hardware_ref = f'{manu_ref}_H-{hardware_serial:04d}-{hardware_version}'
    # Derived from the product's own identity: two products from one
    # manufacturer must not collide, and no product name belongs in this file.
    product_display_name = identity.get('productDisplayName') or identity.get('profileKey', '')
    profile_id = _ets_id_part(identity.get('profileKey', ''))
    order_id = _ets_id_part(identity.get('orderNumber', ''), profile_id)
    product_ref = f'{hardware_ref}_P-{profile_id}'
    # Same shape as the application program id it points at, placeholder hash
    # included — that group is what the ETS hardware signer rewrites.
    hardware2prog_ref = f'{hardware_ref}_HP-{app_number:04X}-{app_version:02X}-0000'
    catalog_section_ref = f'{manu_ref}_CS-{profile_id}'
    catalog_item_ref = f'{manu_ref}_CI-{order_id}'
    # The medium and its mask version come from the C++ product definition, in
    # the numbering KNX uses (MediumType/@Number, 03/05/01 Table 7). ETS
    # resolves MT-*/MV-* against its own master data by *id*, so the ids have to
    # follow the values — writing MT-0 with Number=5 would silently keep the
    # product on TP1.
    # Hardware/@IsIPEnabled below: the device has a KNXnet/IP interface, which
    # is what makes ETS offer the IP configuration tab. Distinct from the
    # medium — an IP router is TP1 medium and IP-enabled both.
    medium_code, medium_name, mask_version, ip_enabled = _medium_facts(identity)
    medium_type_id = f'MT-{medium_code}'
    mask_version_id = f'MV-{mask_version:04X}'

    # Object views: when ETS shows an object, and as what. Checked before
    # anything is written, so a contradictory export never becomes a file.
    object_views = json_obj.get('objectViews') or []
    views_by_port = _check_object_views(cos, object_views)

    # Collect unique DPTs used across all COs and views (preserve insertion order)
    seen_dpts: dict[tuple, None] = {}
    for c in cos:
        main = c.get('dpt_main')
        sub = c.get('dpt_sub', 1)
        if main is not None:
            seen_dpts[(main, sub)] = None
    for view in object_views:
        if 'dpt_main' in view:
            seen_dpts[(int(view['dpt_main']), int(view.get('dpt_sub', 0)))] = None

    # MasterData
    master = ET.SubElement(root, 'MasterData')
    master.set('Version', '1')
    master.set('Signature', 'AA==')
    master.set('Id', 'master1')

    # DatapointTypes — one entry per unique DPT main type used, with sub-entries
    # grouped by main type so ETS can resolve "DPST-9-1" style references.
    dpt_types_elem = ET.SubElement(master, 'DatapointTypes')
    by_main: dict[int, list] = {}
    for (main, sub) in seen_dpts:
        by_main.setdefault(main, []).append(sub)
    for main in sorted(by_main):
        dt = ET.SubElement(dpt_types_elem, 'DatapointType')
        dt.set('Id', f'DPT-{main}')
        dt.set('Number', str(main))
        dt.set('Name', f'DPT Main {main}')
        dt.set('SizeInBit', str(_dpt_size_in_bit(main)))
        # A sub of 0 means the main type is referenced generically (DPT-<main>);
        # it has no DatapointSubtype element of its own.
        real_subs = sorted(s for s in by_main[main] if s)
        # DatapointSubtypes is optional, but must hold at least one
        # DatapointSubtype when present — omit it for generic-only mains.
        if not real_subs:
            continue
        subs_elem = ET.SubElement(dt, 'DatapointSubtypes')
        for sub in real_subs:
            ds = ET.SubElement(subs_elem, 'DatapointSubtype')
            ds.set('Id', _dpt_id(main, sub))
            ds.set('Number', str(sub))
            ds.set('Name', f'DPT {main}.{sub:03d}')
            ds.set('Text', f'DPT {main}.{sub:03d}')
            ds.set('Default', 'true' if sub == real_subs[0] else 'false')

    medium_types = ET.SubElement(master, 'MediumTypes')
    mt = ET.SubElement(medium_types, 'MediumType')
    mt.set('Id', medium_type_id)
    mt.set('Number', str(medium_code))
    mt.set('Name', medium_name)
    mt.set('DomainAddressLength', str(_MEDIUM_DOMAIN_ADDRESS_LENGTH.get(medium_code, 0)))

    mask_versions = ET.SubElement(master, 'MaskVersions')
    mv = ET.SubElement(mask_versions, 'MaskVersion')
    mv.set('Id', mask_version_id)
    mv.set('Name', mask_version_id)
    mv.set('MaskVersion', str(mask_version))  # decimal, e.g. 1968 = 0x07B0
    mv.set('ManagementModel', 'SystemB')
    mv.set('MediumTypeRefId', medium_type_id)

    manufacturers = ET.SubElement(master, 'Manufacturers')
    m = ET.SubElement(manufacturers, 'Manufacturer')
    m.set('Id', manu_ref)
    # A manufacturer, not a product: this entry is master data shared by every
    # product of M-xxxx, and two files that name the same id differently are
    # describing one entity two ways. It used to carry the product's display
    # name, so the TP1 and IP files disagreed. ETS shows its own registered name
    # for the manufacturer id regardless; what the integrator browses by is the
    # catalogue section and item below, which do carry the product name.
    m.set('Name', f'Manufacturer {manufacturer_id:04X}')
    m.set('KnxManufacturerId', str(manufacturer_id))

    # ManufacturerData / Manufacturer / ApplicationPrograms / ApplicationProgram
    manuf_data = ET.SubElement(root, 'ManufacturerData')
    manuf = ET.SubElement(manuf_data, 'Manufacturer')
    manuf.set('RefId', manu_ref)

    # Catalog section required by Kaenx import path.
    catalog = ET.SubElement(manuf, 'Catalog')
    cat_section = ET.SubElement(catalog, 'CatalogSection')
    cat_section.set('Id', catalog_section_ref)
    cat_section.set('Name', product_display_name)
    cat_section.set('Number', '1')
    cat_item = ET.SubElement(cat_section, 'CatalogItem')
    cat_item.set('Id', catalog_item_ref)
    cat_item.set('Name', product_display_name)
    cat_item.set('Number', '1')
    cat_item.set('VisibleDescription', product_display_name)
    cat_item.set('ProductRefId', product_ref)
    cat_item.set('Hardware2ProgramRefId', hardware2prog_ref)

    # Manufacturer's children are a fixed sequence: Catalog, ApplicationPrograms,
    # Baggages, Hardware, Languages — so the container is parented here even
    # though it is populated after the Hardware section below.
    app_programs = ET.SubElement(manuf, 'ApplicationPrograms')

    # Hardware section required by Kaenx import path.
    hardware = ET.SubElement(manuf, 'Hardware')
    hard = ET.SubElement(hardware, 'Hardware')
    hard.set('Id', hardware_ref)
    hard.set('Name', product_display_name)
    # Must match what the device reports in PID_HARDWARE_TYPE / PID_VERSION:
    # ETS checks the catalogue entry against the device before downloading.
    hard.set('SerialNumber', '%04d' % hardware_serial)
    hard.set('VersionNumber', str(hardware_version))
    hard.set('BusCurrent', '10')
    hard.set('HasApplicationProgram', 'true')
    hard.set('HasIndividualAddress', 'true')
    if ip_enabled:
        # Without this ETS never shows the device's IP settings, so a KNXnet/IP
        # device cannot be given an address assignment method or additional
        # individual addresses.
        hard.set('IsIPEnabled', 'true')

    products = ET.SubElement(hard, 'Products')
    product = ET.SubElement(products, 'Product')
    product.set('Id', product_ref)
    product.set('Text', identity.get('productDisplayName') or identity.get('profileKey', ''))
    # Mirrors the device's PID_ORDER_INFO.
    product.set('OrderNumber', identity.get('orderNumber') or identity.get('profileKey', ''))
    product.set('DefaultLanguage', 'en-US')
    product.set('IsRailMounted', 'false')
    # ETS refuses to import a product it considers unregistered unless the
    # installation holds a manufacturer test licence: "No valid license found to
    # test the unregistered product(s) in this file." The registration state is
    # not something the tool can prove, it is simply declared here, and both the
    # product and the hardware-to-program link have to declare it.
    ET.SubElement(product, 'RegistrationInfo').set('RegistrationStatus', 'Registered')

    hard2progs = ET.SubElement(hard, 'Hardware2Programs')
    hard2prog = ET.SubElement(hard2progs, 'Hardware2Program')
    hard2prog.set('Id', hardware2prog_ref)
    hard2prog.set('MediumTypes', medium_type_id)
    # No MaskVersionRefId here: Hardware2Program_t does not declare that
    # attribute. The mask linkage is carried by ApplicationProgram/@MaskVersion.
    app_ref_elem = ET.SubElement(hard2prog, 'ApplicationProgramRef')
    app_ref_elem.set('RefId', app_ref)
    # Must follow ApplicationProgramRef: Hardware2Program_t is a sequence.
    # RegistrationNumber is '<4 digits>/<digits>' — the ETS registration id and
    # the hardware/application version pair it was registered for, which is what
    # lets ETS offer an update to an already installed device rather than a
    # second catalogue entry.
    registration = ET.SubElement(hard2prog, 'RegistrationInfo')
    registration.set('RegistrationStatus', 'Registered')
    registration.set('RegistrationNumber', f'0001/{hardware_version}{app_version}')

    app = ET.SubElement(app_programs, 'ApplicationProgram')
    app.set('Id', app_ref)
    app.set('ApplicationNumber', str(app_number))
    app.set('ApplicationVersion', str(app_version))
    app.set('ProgramType', 'ApplicationProgram')
    app.set('MaskVersion', mask_version_id)
    app.set('Name', identity.get('productDisplayName', app_ref))
    # MergedProcedure: ETS runs the mask version's own load procedure (the
    # tables, the load states, the restart) and merges in the product's steps
    # for the parameter segment at the merge points (_emit_load_procedures).
    # DefaultProcedure would ignore those steps; ProductProcedure would have
    # to describe the whole download.
    app.set('LoadProcedureStyle', 'MergedProcedure')
    app.set('PeiType', '0')
    app.set('DefaultLanguage', 'en-US')
    app.set('DynamicTableManagement', 'false')
    app.set('Linkable', 'false')

    # KNX Data Secure.  ApplicationProgram/@IsSecureEnabled is optional with a
    # schema default of false, so omitting it is an active declaration that the
    # device is a plain one — ETS then hides every Secure option regardless of
    # what the firmware can actually do.  The MaxSecurity*Entries attributes
    # tell ETS how many key-table slots it may allocate; without them a
    # Secure-enabled device has nowhere to put the keys it negotiates.
    security = json_obj.get('security', {})
    security_requirement = security.get('groupObjectRequirement', 'None')
    if security.get('dataSecureCapable', False):
        app.set('IsSecureEnabled', 'true')
        app.set('MaxSecurityIndividualAddressEntries',
                str(security.get('individualAddressEntries', 1)))
        app.set('MaxSecurityGroupKeyTableEntries',
                str(security.get('groupKeyTableEntries', len(cos))))
        app.set('MaxSecurityP2PKeyTableEntries',
                str(security.get('p2pKeyTableEntries', 1)))
    else:
        # Never advertise a per-object requirement on a non-secure device.
        security_requirement = 'None'

    static = ET.SubElement(app, 'Static')

    # The RelativeSegment is the ETS-visible parameter memory: Parameter
    # <Memory> entries reference it, and on the device it is the product's
    # parameter image, exactly as long. A product without parameters has no
    # image and declares no segment.
    #
    # ETS parses a code segment id rather than treating it as opaque: it is
    # '<application program id>_RS-<load state machine:2 digits>-<offset:5
    # digits>', and a download of anything else fails with "ID '..._RS-0000' is
    # not a valid id ID". Build it from the same values the attributes carry so
    # the two cannot drift apart.
    #
    # Load state machine index == the interface object INDEX of the loadable
    # part that owns the segment. This is not the object *type*: the
    # Application Program object's type is 3, but in the canonical System B
    # ordering its index is 4 (0=Device, 1=GA table, 2=Assoc table,
    # 3=GO table, 4=App program) — the same ordering bau.cpp binds and the same
    # index the load procedure below already uses for LsmIdx/ObjIdx.
    if json_obj.get('parameters'):
        raise SystemExit('the export carries a flat "parameters" list, which this exporter no longer '
                         'reads; regenerate the JSON from a product with a parameter layout')
    parameter_blocks = json_obj.get('parameterBlocks') or []
    rs_load_state_machine = _LSM_APPLICATION_PROGRAM
    rs_offset = 0
    rs_ref = f'{app_ref}_RS-{rs_load_state_machine:02d}-{rs_offset:05d}'
    if parameter_blocks:
        code = ET.SubElement(static, 'Code')
        rs = ET.SubElement(code, 'RelativeSegment')
        rs.set('Id', rs_ref)
        rs.set('Name', 'Parameters')
        rs.set('LoadStateMachine', str(rs_load_state_machine))
        rs.set('Offset', str(rs_offset))
        rs.set('Size', str(int(json_obj.get('parameterImage', {}).get('size', 0))))

    # Dynamic section is created before parameter emission so the parameter
    # blocks can be attached to it; XML sibling order within <ApplicationProgram>
    # (Static then Dynamic) is fixed by the creation order of the parents above.
    #
    # ChannelIndependentBlock, not Channel: a <Channel> is a real node in the ETS
    # group object tree, so every group address sat one collapsed level deeper
    # than on devices that declare no channels. These products have a single
    # fixed function, not N repeated ones, so there is no channel to name.
    dynamic = ET.SubElement(app, 'Dynamic')
    dyn_root = ET.SubElement(dynamic, 'ChannelIndependentBlock')

    param_bytes, param_ref_ids, block_counts = _emit_parameter_blocks(static, dyn_root, app_ref, rs_ref, json_obj)

    com_objects_parent = ET.SubElement(static, 'ComObjectTable')
    com_refs_parent = ET.SubElement(static, 'ComObjectRefs')
    for c in cos:
        co = ET.SubElement(com_objects_parent, 'ComObject')
        export_number = int(c.get('exportNumber', 0))
        co_id = f'{app_ref}_O-{export_number}'
        co.set('Id', co_id)
        name = c.get('displayName', c.get('key', ''))
        co.set('Name', name)
        co.set('Text', name)
        co.set('Number', str(export_number))
        co.set('FunctionText', name)

        # ObjectSize derived from DPT main type
        dpt_main = c.get('dpt_main')
        dpt_sub = c.get('dpt_sub', 1)
        co.set('ObjectSize', _dpt_object_size(dpt_main) if dpt_main is not None else '1 Byte')

        # Communication flags.  These map one-to-one onto knx::application::
        # GroupObjectFlags and onto the Group Object Descriptor the device
        # serves from PID_TABLE, so what ETS shows is what the firmware
        # enforces.  Emitting a flag the runtime ignores would be worse than
        # not offering it: the integrator would tick a box that does nothing.
        readable = c.get('readable', False)
        writable = c.get('writable', False)
        transmit = c.get('transmit', False)
        receivable = c.get('receivable', False)
        read_on_init = c.get('readOnInit', False)
        communication = c.get('communication', True)
        co.set('ReadFlag', 'Enabled' if readable else 'Disabled')
        co.set('WriteFlag', 'Enabled' if writable else 'Disabled')
        co.set('CommunicationFlag', 'Enabled' if communication else 'Disabled')
        co.set('TransmitFlag', 'Enabled' if transmit else 'Disabled')
        # UpdateFlag is KNX "Response-Update enable": an A_GroupValue_Response
        # from another device updates this object.  Distinct from WriteFlag.
        co.set('UpdateFlag', 'Enabled' if receivable else 'Disabled')
        co.set('ReadOnInitFlag', 'Enabled' if read_on_init else 'Disabled')

        # DatapointType reference → must match an Id defined in MasterData/DatapointTypes
        if dpt_main is not None:
            co.set('DatapointType', _dpt_id(dpt_main, dpt_sub))

        # 'None' is the schema default, so leave the attribute off entirely
        # rather than writing it out on every object of a plain device.
        if security_requirement != 'None':
            co.set('SecurityRequired', security_requirement)

        # ComObjectRef and Dynamic linkage required by Kaenx importer. A port
        # without views has one reference, always shown. A port with views
        # has one per view, each shown while its condition holds: the
        # alternatives of one object number (object_views.hpp).
        port_views = views_by_port.get(int(c.get('logicalId', -1)), [])
        references = port_views if port_views else [None]
        for index, view in enumerate(references):
            cref = ET.SubElement(com_refs_parent, 'ComObjectRef')
            cref_id = f'{co_id}_R-{export_number}' if view is None else f'{co_id}_R-{index + 1}'
            text = (view or {}).get('text') or name
            cref.set('Id', cref_id)
            cref.set('RefId', co_id)
            cref.set('Text', text)
            cref.set('FunctionText', (view or {}).get('function') or text)
            cref.set('ReadFlag', co.get('ReadFlag'))
            cref.set('WriteFlag', co.get('WriteFlag'))
            cref.set('CommunicationFlag', co.get('CommunicationFlag'))
            cref.set('TransmitFlag', co.get('TransmitFlag'))
            cref.set('UpdateFlag', co.get('UpdateFlag'))
            cref.set('ReadOnInitFlag', co.get('ReadOnInitFlag'))
            cref.set('ObjectSize', co.get('ObjectSize'))
            if view is not None and 'dpt_main' in view:
                cref.set('DatapointType', _dpt_id(int(view['dpt_main']), int(view.get('dpt_sub', 0))))
            elif co.get('DatapointType') is not None:
                cref.set('DatapointType', co.get('DatapointType'))
            if co.get('SecurityRequired') is not None:
                cref.set('SecurityRequired', co.get('SecurityRequired'))

            holder = dyn_root
            if view is not None and view.get('visible'):
                condition = view['visible']
                holder = _under_condition(dyn_root, condition, param_ref_ids, block_counts,
                                          int(condition['block']), int(view.get('instance', 0)))
            ET.SubElement(holder, 'ComObjectRefRef').set('RefId', cref_id)

    # How many group addresses and associations the device's tables hold. ETS
    # takes a missing declaration as zero and refuses the first link with "The
    # maximum associations of the device are been reached", so these are not
    # optional in practice. Both elements sit between ComObjectRefs and
    # LoadProcedures in the Static sequence.
    capacities = json_obj.get('capacities', {})
    for element_name, capacity_key in (('AddressTable', 'addressTableMaxEntries'),
                                       ('AssociationTable', 'associationTableMaxEntries')):
        max_entries = capacities.get(capacity_key)
        if max_entries is None:
            raise SystemExit(
                f'capacities.{capacity_key} is missing, so this export predates '
                'the table-capacity fix and ETS would refuse to link a group '
                'address. Regenerate the JSON with the current product exporter.')
        ET.SubElement(static, element_name).set('MaxEntries', str(int(max_entries)))

    # LoadProcedures comes last in the Static sequence.
    _emit_load_procedures(static, mask_version, param_bytes)

    # Data Secure is loaded through the extended memory services: ETS writes the
    # Security interface object key tables with A_MemoryExtended_Write, which the
    # 16-bit A_Memory_* services cannot address.  Kaenx-Creator refuses to build
    # a .knxprod for a Secure device without this flag, and ETS would fail the
    # download later anyway.  Gated on dataSecureCapable so a plain device never
    # claims services it has no reason to advertise.
    #
    # <Options> is last in the ApplicationProgramStatic_t sequence, so it must be
    # appended after LoadProcedures — the schema enforces the order.
    # Profiles v02.01.01 §9.1.2.3.1 lists both Extended Memory services and
    # Extended Property services as mandatory (M) for the KNX Data Security
    # profile, so a Secure product declares both or neither.
    if security.get('dataSecureCapable', False):
        options = ET.SubElement(static, 'Options')
        options.set('SupportsExtendedMemoryServices', 'true')
        options.set('SupportsExtendedPropertyServices', 'true')

    # Optional language section used by importer for translated labels.
    languages = ET.SubElement(manuf, 'Languages')
    language = ET.SubElement(languages, 'Language')
    language.set('Identifier', 'en-US')

    tu_app = ET.SubElement(language, 'TranslationUnit')
    tu_app.set('RefId', app_ref)
    te_app_name = ET.SubElement(tu_app, 'TranslationElement')
    te_app_name.set('RefId', app_ref)
    tet_app_name = ET.SubElement(te_app_name, 'Translation')
    tet_app_name.set('AttributeName', 'Name')
    tet_app_name.set('Text', product_display_name)

    tu_product = ET.SubElement(language, 'TranslationUnit')
    tu_product.set('RefId', product_ref)
    te_product_text = ET.SubElement(tu_product, 'TranslationElement')
    te_product_text.set('RefId', product_ref)
    tet_product_text = ET.SubElement(te_product_text, 'Translation')
    tet_product_text.set('AttributeName', 'Text')
    tet_product_text.set('Text', product_display_name)

    return prettify(root)


def main(argv=None):
    parser = argparse.ArgumentParser(description='KNX Kaenx XML exporter from JSON')
    parser.add_argument('--input', '-i', nargs='+', help='Input JSON file(s)', default=None)
    parser.add_argument('--output', '-o', nargs='+', help='Output XML file(s) (must match inputs count)', default=None)
    parser.add_argument('--format', '-f', choices=['kaenx', 'knxprod'], default='kaenx', help='Output XML format')
    args = parser.parse_args(argv)

    inputs = args.input or ['pilot_a_export.json', 'pilot_b_export.json']
    if args.output:
        outputs = args.output
    else:
        if args.format == 'knxprod':
            outputs = [inp.rsplit('.', 1)[0] + '.knxprod.xml' for inp in inputs]
        else:
            outputs = [inp.rsplit('.', 1)[0] + '.kaenx.xml' for inp in inputs]

    if len(outputs) != len(inputs):
        print('Number of outputs must match inputs', file=sys.stderr)
        return 2

    for inp, out in zip(inputs, outputs):
        with open(inp, 'r', encoding='utf-8') as f:
            data = json.load(f)
        if args.format == 'knxprod':
            xml_text = export_product_json_to_knxprod(data)
        else:
            xml_text = export_product_json_to_kaenx(data)
        with open(out, 'w', encoding='utf-8') as fo:
            fo.write(xml_text)
        print(f'Wrote {out}')

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
