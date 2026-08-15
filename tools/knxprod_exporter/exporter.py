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
    ET.SubElement(product, 'Medium').text = str(identity.get('medium', ''))

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
    for k in ('groupObjectCount', 'addressTableEntries', 'associationEntries', 'autoResponseQueueCapacity', 'transmissionOutcomeQueueCapacity'):
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
    if dpt_main == 1:
        return '1 Bit'
    if dpt_main in (2,):
        return '2 Bits'
    if dpt_main in (3,):
        return '4 Bits'
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


# Parameter valueKind (ExportParameterValueKind in export_descriptor.hpp) →
# serialised byte width in the device's ProgramData / RS-0000 code segment.
# Must stay consistent with exportParameterValueByteWidth() in
# endpoint_compiler.hpp and ParameterState::applyFromBytes (declaration order,
# big-endian): that is the layout ETS memory-writes into RS-0000 and the device
# decodes back into parameter values.
_PARAM_KIND_NONE = 0
_PARAM_KIND_BOOLEAN = 1
_PARAM_KIND_UNSIGNED8 = 2
_PARAM_KIND_UNSIGNED16 = 3
_PARAM_KIND_SIGNED16 = 4
_PARAM_KIND_ENUM = 5
_PARAM_KIND_TEXT = 6
_PARAM_KIND_FLOAT = 7
_PARAM_KIND_FLOAT_DPT9 = 8

_PARAM_KIND_WIDTH = {
    _PARAM_KIND_BOOLEAN: 1,
    _PARAM_KIND_UNSIGNED8: 1,
    _PARAM_KIND_UNSIGNED16: 2,
    _PARAM_KIND_SIGNED16: 2,
    _PARAM_KIND_ENUM: 2,
    _PARAM_KIND_FLOAT: 4,
    _PARAM_KIND_FLOAT_DPT9: 2,
}


def _param_bounds(p):
    """Return {'minInclusive','maxInclusive'} when the parameter declares real
    bounds, else None.  Both-zero means "unset" on the C++ side, so an
    all-zero pair is not a 0..0 range."""
    lo = p.get('minValue')
    hi = p.get('maxValue')
    if lo is None or hi is None or (lo == 0 and hi == 0):
        return None
    fmt = lambda v: str(int(v)) if float(v) == int(v) else repr(float(v))
    return {'minInclusive': fmt(lo), 'maxInclusive': fmt(hi)}


def _enum_param_type(app_ref: str, param_id: int, width_bits: int, options):
    """Build a per-parameter enumerated ParameterType.

    ETS renders a <TypeRestriction> as a drop-down.  Without one an enum
    parameter is a bare number box and the integrator has to be told out of
    band that 2 means Standby, which is how a product ends up with labels like
    "Mode (0=Auto,1=Comfort,2=Standby)".  Enumerations cannot be shared across
    parameters the way plain numeric types are, so each gets its own id.
    """
    type_id = f'{app_ref}_PT-enum-{param_id}'
    return type_id, 'TypeRestriction', {'Base': 'Value', 'SizeInBit': str(width_bits)}


def _param_type_for_kind(app_ref: str, kind: int):
    """Return (type_id, element_tag, element_attrs) for a ParameterType of the
    given value kind. One shared ParameterType per kind keeps the XML small."""
    if kind == _PARAM_KIND_FLOAT_DPT9:
        # KNX-native 2-byte half-float. Bounds are integers on purpose: some
        # tooling (Kaenx-Creator) round-trips these strings through a
        # locale-sensitive parser, and separator-free integers parse
        # identically under every locale. Full DPT9 range is ±670760.96.
        return (f'{app_ref}_PT-dpt9',
                'TypeFloat',
                {'Encoding': 'DPT 9',
                 'minInclusive': '-670760', 'maxInclusive': '670760'})
    if kind == _PARAM_KIND_FLOAT:
        return (f'{app_ref}_PT-float',
                'TypeFloat',
                {'Encoding': 'IEEE-754 Single',
                 'minInclusive': '-3.40282e+38', 'maxInclusive': '3.40282e+38'})
    if kind == _PARAM_KIND_BOOLEAN:
        return (f'{app_ref}_PT-bool',
                'TypeNumber',
                {'SizeInBit': '8', 'Type': 'unsignedInt',
                 'minInclusive': '0', 'maxInclusive': '1'})
    if kind == _PARAM_KIND_UNSIGNED8:
        return (f'{app_ref}_PT-u8',
                'TypeNumber',
                {'SizeInBit': '8', 'Type': 'unsignedInt',
                 'minInclusive': '0', 'maxInclusive': '255'})
    if kind == _PARAM_KIND_SIGNED16:
        return (f'{app_ref}_PT-s16',
                'TypeNumber',
                {'SizeInBit': '16', 'Type': 'signedInt',
                 'minInclusive': '-32768', 'maxInclusive': '32767'})
    # Unsigned16 and Enum (serialised as u16) share the same numeric shape.
    return (f'{app_ref}_PT-u16',
            'TypeNumber',
            {'SizeInBit': '16', 'Type': 'unsignedInt',
             'minInclusive': '0', 'maxInclusive': '65535'})


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


def _group_visibility_conditions(serialisable):
    """Map group name -> (controlling parameter id, value) for groups whose ETS
    section is shown or hidden as a whole.

    A group qualifies when every one of its parameters declares the same
    `groupVisibleWhen*` pair. Anything less is refused rather than guessed at:
    a half-gated section would hide some of its parameters and leave the rest
    under a heading that no longer describes them."""
    declared = {}
    members = {}
    for p in serialisable:
        key = p.get('group') or ''
        controlling = p.get('groupVisibleWhenParameterId')
        condition = (None if controlling is None
                     else (int(controlling), int(p.get('groupVisibleWhenValue', 0))))
        if key in declared:
            if declared[key] != condition:
                raise ValueError(
                    f'parameter group {key!r} declares conflicting section visibility '
                    f'conditions ({declared[key]} vs {condition}); every parameter of a '
                    f'gated group must name the same controlling parameter and value')
        else:
            declared[key] = condition
        members.setdefault(key, []).append(int(p.get('id', 0)))

    conditions = {}
    for key, condition in declared.items():
        if condition is None:
            continue
        if condition[0] in members[key]:
            # The switch would hide itself the moment it was turned off, and no
            # ETS user could ever turn it back on.
            raise ValueError(
                f'parameter group {key!r} is gated on parameter {condition[0]}, which is a '
                f'member of that same group; the controlling parameter must live outside '
                f'the section it shows or hides')
        conditions[key] = condition
    return conditions


def _emit_parameters(static_elem, dynamic_channel, app_ref: str, rs_ref: str, params):
    """Fill ParameterTypes/Parameters/ParameterRefs and a Dynamic ParameterBlock
    from the JSON `parameters` array. Sequential memory offsets in declaration
    order mirror the device-side ParameterState/ProgramData byte layout, so an
    ETS partial download memory-writes exactly the block the firmware decodes.
    Non-serialisable kinds (Text, None) are skipped, matching applyFromBytes."""
    serialisable = [p for p in params if int(p.get('valueKind', 0)) in _PARAM_KIND_WIDTH]
    # Each of these containers requires at least one child when present, so a
    # product without serialisable parameters must omit them entirely.
    if not serialisable:
        return 0

    types_elem = ET.SubElement(static_elem, 'ParameterTypes')
    params_elem = ET.SubElement(static_elem, 'Parameters')
    refs_elem = ET.SubElement(static_elem, 'ParameterRefs')

    # A group whose every parameter declares the same section-level condition is
    # gated as a whole: ETS hides the section instead of showing a heading that
    # opens onto nothing. An empty block reads as a broken product, and it gives
    # the integrator no hint that some other parameter switches it on.
    group_conditions = _group_visibility_conditions(serialisable)

    # One ParameterBlock per declared group, in first-appearance order, so a
    # product with thirty parameters presents as a handful of labelled sections
    # instead of one flat list. Parameters without a group share a default
    # block, which keeps simple products exactly as they were.
    blocks = {}

    def block_for(group_name):
        key = group_name or ''
        existing = blocks.get(key)
        if existing is not None:
            return existing
        condition = group_conditions.get(key)
        if condition is None:
            parent = dynamic_channel
        else:
            # <choose> sits at channel level here, wrapping the whole block;
            # the parameter-level form below wraps a single ParameterRefRef.
            controlling_id, controlling_value = condition
            choose = ET.SubElement(dynamic_channel, 'choose')
            choose.set('ParamRefId', f'{app_ref}_P-{controlling_id}_R-{controlling_id}')
            parent = ET.SubElement(choose, 'when')
            parent.set('test', str(controlling_value))
        element = ET.SubElement(parent, 'ParameterBlock')
        element.set('Id', f'{app_ref}_PB-{len(blocks) + 1}')
        element.set('Name', key or 'Settings')
        element.set('Text', key or 'Settings')
        blocks[key] = element
        return element

    emitted_types = {}
    offset = 0
    for p in serialisable:
        kind = int(p.get('valueKind', 0))
        width = _PARAM_KIND_WIDTH[kind]
        raw_id = int(p.get('id', 0))
        options = p.get('options') or []

        if options:
            # An enumerated parameter gets its own type carrying the value
            # list, so ETS shows named choices instead of a raw number.
            type_id, tag, attrs = _enum_param_type(app_ref, raw_id, width * 8, options)
            if type_id not in emitted_types:
                pt = ET.SubElement(types_elem, 'ParameterType')
                pt.set('Id', type_id)
                pt.set('Name', type_id.rsplit('_', 1)[-1])
                restriction = ET.SubElement(pt, tag, attrs)
                for index, opt in enumerate(options):
                    enum = ET.SubElement(restriction, 'Enumeration')
                    label = str(opt.get('label', opt.get('value', '')))
                    value = int(opt.get('value', 0))
                    enum.set('Text', label)
                    enum.set('Value', str(value))
                    # xs:ID, so it must be unique across the whole document and
                    # start with a letter — hence the type id as the prefix.
                    # The schema marks it required; ETS refuses to import a
                    # TypeRestriction whose Enumerations have no Id.
                    enum.set('Id', f'{type_id}_EN-{value}')
                    # DisplayOrder keeps ETS from re-sorting the list; authors
                    # order options meaningfully (Auto before Comfort, ...).
                    enum.set('DisplayOrder', str(index))
                emitted_types[type_id] = True
        else:
            type_id, tag, attrs = _param_type_for_kind(app_ref, kind)
            # Author-supplied bounds narrow the shared numeric type, so they
            # need a parameter-specific id; without them the shared one is
            # reused and the XML stays small.
            bounds = _param_bounds(p)
            if bounds is not None:
                attrs = dict(attrs)
                attrs.update(bounds)
                type_id = f'{type_id}-{raw_id}'
            if type_id not in emitted_types:
                pt = ET.SubElement(types_elem, 'ParameterType')
                pt.set('Id', type_id)
                pt.set('Name', type_id.rsplit('_', 1)[-1])
                ET.SubElement(pt, tag, attrs)
                emitted_types[type_id] = True

        param_id = f'{app_ref}_P-{raw_id}'
        pe = ET.SubElement(params_elem, 'Parameter')
        pe.set('Id', param_id)
        pe.set('Name', p.get('key', param_id))
        pe.set('ParameterType', type_id)
        pe.set('Text', p.get('displayName', p.get('key', '')))
        pe.set('Value', _param_default_text(kind, p.get('defaultValue')))
        unit = p.get('unit')
        if unit:
            pe.set('SuffixText', unit)
        mem = ET.SubElement(pe, 'Memory')
        mem.set('CodeSegment', rs_ref)
        mem.set('Offset', str(offset))
        mem.set('BitOffset', '0')
        offset += width

        ref_id = f'{param_id}_R-{int(p.get("id", 0))}'
        pref = ET.SubElement(refs_elem, 'ParameterRef')
        pref.set('Id', ref_id)
        pref.set('RefId', param_id)

        target_block = block_for(p.get('group'))

        condition = p.get('visibleWhenParameterId')
        if condition is not None:
            # ETS renders <choose>/<when> as conditional visibility: the
            # parameter appears only while the controlling parameter holds the
            # given value. Showing settings that currently do nothing is a
            # standing source of misconfiguration, so this is worth the extra
            # nesting.
            controlling_ref = f'{app_ref}_P-{int(condition)}_R-{int(condition)}'
            choose = ET.SubElement(target_block, 'choose')
            choose.set('ParamRefId', controlling_ref)
            when = ET.SubElement(choose, 'when')
            when.set('test', str(int(p.get('visibleWhenValue', 0))))
            prefref = ET.SubElement(when, 'ParameterRefRef')
        else:
            prefref = ET.SubElement(target_block, 'ParameterRefRef')
        prefref.set('RefId', ref_id)

    # The accumulated offset is exactly the size of the memory block ETS must
    # download, which the load procedure needs in order to allocate and write it.
    return offset



# Load State Machine index of the Application Program interface object, as this
# stack registers its interface objects: 0 = Device, 1 = Address Table,
# 2 = Association Table, 3 = Group Object Table, 4 = Application Program.
# ETS addresses load-state transitions by this index, so it must match the
# registration order in bau.cpp.
_LSM_APPLICATION_PROGRAM = 4


def _emit_load_procedure(load_proc, param_bytes: int):
    """Emit the download sequence ETS performs for this product.

    Previously this was a bare <LdCtrlConnect/>. Parameter download still
    worked, because parameters are memory-mapped and ETS writes them directly,
    but the product declared a procedure that did not describe what actually had
    to happen — in particular it never drove the Application Program object's
    load state machine, so the device was never told a download had begun or
    completed.

    The sequence below is the standard System B application download:

      connect → unload → allocate the segment → write it → load-completed
      → disconnect

    Unload before Load matters: the load state machine only accepts a segment
    allocation from the Unloaded state, so skipping it leaves a device that was
    already commissioned refusing the second download.
    """
    ET.SubElement(load_proc, 'LdCtrlConnect')

    # Drive the Application Program object to Unloaded, then back through
    # Loading to Loaded around the memory write.
    unload = ET.SubElement(load_proc, 'LdCtrlUnload')
    unload.set('LsmIdx', str(_LSM_APPLICATION_PROGRAM))

    if param_bytes > 0:
        # Allocate the relative code segment the parameters live in, then write
        # it. Mode 0 = the plain data segment; Fill 0 = zero the remainder, so a
        # shortened parameter block cannot leave stale bytes from a previous
        # download behind.
        segment = ET.SubElement(load_proc, 'LdCtrlRelSegment')
        segment.set('LsmIdx', str(_LSM_APPLICATION_PROGRAM))
        segment.set('Size', str(param_bytes))
        segment.set('Mode', '0')
        segment.set('Fill', '0')

        write = ET.SubElement(load_proc, 'LdCtrlWriteRelMem')
        write.set('ObjIdx', str(_LSM_APPLICATION_PROGRAM))
        write.set('Offset', '0')
        write.set('Size', str(param_bytes))
        # Verify: the device answers A_Memory_Write with a read-back, so asking
        # ETS to check it costs nothing and catches a truncated download.
        write.set('Verify', '1')

    completed = ET.SubElement(load_proc, 'LdCtrlLoadCompleted')
    completed.set('LsmIdx', str(_LSM_APPLICATION_PROGRAM))

    ET.SubElement(load_proc, 'LdCtrlDisconnect')

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
    app_ref = f'{manu_ref}_A-{app_number:06X}-{app_version:02X}-0000'
    hardware_ref = f'{manu_ref}_H-0001'
    # Derived from the product's own identity: two products from one
    # manufacturer must not collide, and no product name belongs in this file.
    product_display_name = identity.get('productDisplayName') or identity.get('profileKey', '')
    profile_id = _ets_id_part(identity.get('profileKey', ''))
    order_id = _ets_id_part(identity.get('orderNumber', ''), profile_id)
    product_ref = f'{hardware_ref}_P-{profile_id}'
    hardware2prog_ref = f'{hardware_ref}_HP-0001-{app_number:04X}-{app_version:02X}'
    catalog_section_ref = f'{manu_ref}_CS-{profile_id}'
    catalog_item_ref = f'{manu_ref}_CI-{order_id}'
    mask_version_id = 'MV-07B0'
    medium_type_id = 'MT-0'

    # Collect unique DPTs used across all COs (preserve insertion order)
    seen_dpts: dict[tuple, None] = {}
    for c in cos:
        main = c.get('dpt_main')
        sub = c.get('dpt_sub', 1)
        if main is not None:
            seen_dpts[(main, sub)] = None

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
    mt.set('Number', str(identity.get('medium', 0)))
    mt.set('Name', 'MT-0')
    mt.set('DomainAddressLength', '16')

    mask_versions = ET.SubElement(master, 'MaskVersions')
    mv = ET.SubElement(mask_versions, 'MaskVersion')
    mv.set('Id', mask_version_id)
    mv.set('Name', 'Default')
    mv.set('MaskVersion', '1968')  # 0x07B0 decimal
    mv.set('ManagementModel', 'SystemB')
    mv.set('MediumTypeRefId', medium_type_id)

    manufacturers = ET.SubElement(master, 'Manufacturers')
    m = ET.SubElement(manufacturers, 'Manufacturer')
    m.set('Id', manu_ref)
    m.set('Name', identity.get('productDisplayName', 'Manufacturer'))
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
    hard.set('SerialNumber', '%04d' % int(identity.get('hardwareSerialNumber', 1)))
    hard.set('VersionNumber', str(int(identity.get('hardwareVersion', 1))))
    hard.set('BusCurrent', '10')
    hard.set('HasApplicationProgram', 'true')
    hard.set('HasIndividualAddress', 'true')

    products = ET.SubElement(hard, 'Products')
    product = ET.SubElement(products, 'Product')
    product.set('Id', product_ref)
    product.set('Text', identity.get('productDisplayName') or identity.get('profileKey', ''))
    # Mirrors the device's PID_ORDER_INFO.
    product.set('OrderNumber', identity.get('orderNumber') or identity.get('profileKey', ''))
    product.set('DefaultLanguage', 'en-US')
    product.set('IsRailMounted', 'false')

    hard2progs = ET.SubElement(hard, 'Hardware2Programs')
    hard2prog = ET.SubElement(hard2progs, 'Hardware2Program')
    hard2prog.set('Id', hardware2prog_ref)
    hard2prog.set('MediumTypes', medium_type_id)
    # No MaskVersionRefId here: Hardware2Program_t does not declare that
    # attribute. The mask linkage is carried by ApplicationProgram/@MaskVersion.
    app_ref_elem = ET.SubElement(hard2prog, 'ApplicationProgramRef')
    app_ref_elem.set('RefId', app_ref)

    app = ET.SubElement(app_programs, 'ApplicationProgram')
    app.set('Id', app_ref)
    app.set('ApplicationNumber', str(app_number))
    app.set('ApplicationVersion', str(app_version))
    app.set('ProgramType', 'ApplicationProgram')
    app.set('MaskVersion', mask_version_id)
    app.set('Name', identity.get('productDisplayName', app_ref))
    # MergedProcedure, not DefaultProcedure: with "DefaultProcedure" ETS
    # synthesizes the whole download from the mask version and IGNORES the
    # product's <LoadProcedures>. That is exactly what was observed — ETS ran
    # its built-in table download (0x4000/0x4400/0x5000) but never executed our
    # LdCtrlRelSegment/LdCtrlWriteRelMem for the Application Program object, so
    # parameters were never sent and the device kept firmware defaults.
    # "Merged" keeps ETS's default handling for the tables and additionally runs
    # the product steps below, which only describe the parameter segment.
    # ("ProductProcedure" would replace the default entirely and would then have
    # to describe the table download too, which these steps do not.)
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
    # <Memory> entries reference it, and the device maps it to the application
    # program load segment it decodes into ParameterState on LoadCompleted.
    code = ET.SubElement(static, 'Code')
    rs_ref = f'{app_ref}_RS-0000'
    rs = ET.SubElement(code, 'RelativeSegment')
    rs.set('Id', rs_ref)
    rs.set('Name', 'RS-0000')
    # Load state machine index == the interface object INDEX of the loadable
    # part that owns the segment. This is not the object *type*: the
    # Application Program object's type is 3, but in the canonical System B
    # ordering its index is 4 (0=Device, 1=GA table, 2=Assoc table,
    # 3=GO table, 4=App program) — the same ordering bau.cpp binds and the same
    # index the load procedure below already uses for LsmIdx/ObjIdx.
    # Declaring 3 here pointed the segment at the group object table's load
    # state machine, so ETS never allocated a parameter segment.
    rs.set('LoadStateMachine', str(_LSM_APPLICATION_PROGRAM))
    rs.set('Offset', '0')
    rs.set('Size', '256')

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

    param_bytes = _emit_parameters(static, dyn_root, app_ref, rs_ref, json_obj.get('parameters', []))

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

        # ComObjectRef and Dynamic linkage required by Kaenx importer.
        cref = ET.SubElement(com_refs_parent, 'ComObjectRef')
        cref_id = f'{co_id}_R-{export_number}'
        cref.set('Id', cref_id)
        cref.set('RefId', co_id)
        cref.set('Text', name)
        cref.set('FunctionText', name)
        cref.set('ReadFlag', co.get('ReadFlag'))
        cref.set('WriteFlag', co.get('WriteFlag'))
        cref.set('CommunicationFlag', co.get('CommunicationFlag'))
        cref.set('TransmitFlag', co.get('TransmitFlag'))
        cref.set('UpdateFlag', co.get('UpdateFlag'))
        cref.set('ReadOnInitFlag', co.get('ReadOnInitFlag'))
        cref.set('ObjectSize', co.get('ObjectSize'))
        if co.get('DatapointType') is not None:
            cref.set('DatapointType', co.get('DatapointType'))
        if co.get('SecurityRequired') is not None:
            cref.set('SecurityRequired', co.get('SecurityRequired'))

        crefref = ET.SubElement(dyn_root, 'ComObjectRefRef')
        crefref.set('RefId', cref_id)

    # Kaenx publish check requires a non-empty load procedure for masks
    # that are not marked as ProcedureTypes.Default. LoadProcedures comes last
    # in the Static sequence, and each LdCtrl* step is wrapped in a
    # LoadProcedure element.
    load_procs = ET.SubElement(static, 'LoadProcedures')
    load_proc = ET.SubElement(load_procs, 'LoadProcedure')
    _emit_load_procedure(load_proc, param_bytes)

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
