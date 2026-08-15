#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile

KNX_NS = 'http://knx.org/xml/project/23'
NS = {'knx': KNX_NS}
CATALOG_ENTRY_RE = re.compile(
    r'^KNX_DPT_CATALOG_ENTRY\('
    r'(?P<symbol>[^,]+),\s*'
    r'(?P<main_type>[^,]+),\s*'
    r'(?P<sub_type>[^,]+),\s*'
    r'(?P<value_type>[^,]+),\s*'
    r'(?P<cpp_type>[^,]+),\s*'
    r'(?P<codec_type>[^,]+),\s*'
    r'"(?P<short_name>[^"]+)",\s*'
    r'"(?P<description>[^"]*)"\)$'
)

PDT_ENTRY_RE = re.compile(r'^KNX_PDT_CATALOG_ENTRY\(\s*(?P<enum>[^,]+)\s*,\s*(?P<token>[^,]+)\s*,\s*(?P<value>[^,]+)\s*,\s*(?P<size>[^,]+)\s*,\s*"(?P<desc>[^\"]*)"\s*\)')

NON_ALNUM_RE = re.compile(r'[^0-9A-Za-z]+')


def normalize_dpt(dpt_text: str) -> str:
    if not dpt_text:
        return ''

    s = dpt_text.strip().upper()
    if s.startswith('DPT-'):
        s = s[4:]

    if '.' in s:
        parts = s.split('.', 1)
        if parts[0].isdigit() and parts[1].isdigit():
            return f'{int(parts[0])}.{int(parts[1]):03d}'

    if s.isdigit():
        return str(int(s))

    return dpt_text.strip()


def load_dpt_catalog(path):
    catalog = {}
    main_catalog = {}
    with open(path, 'r', encoding='utf-8') as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith('/*') or line.startswith('//'):
                continue

            match = CATALOG_ENTRY_RE.match(line)
            if not match:
                continue

            short_name = normalize_dpt(match.group('short_name'))
            symbol = match.group('symbol').strip()
            value_type = match.group('value_type').strip()
            if short_name:
                entry = {
                    'symbol': symbol,
                    'tag_literal': f'knx::application::dpttags::{symbol}',
                    'id_literal': f'knx::application::dptids::{symbol}',
                    'value_type_literal': f'knx::application::DptValue::Type::{value_type}',
                    'encode_literal': f'&knx::application::DptTraits<knx::application::dpttags::{symbol}>::encodeDynamic',
                    'decode_literal': f'&knx::application::DptTraits<knx::application::dpttags::{symbol}>::decodeDynamic',
                }
                catalog[short_name] = entry

                main_key = short_name.split('.', 1)[0]
                if '.' not in short_name and main_key not in main_catalog:
                    main_catalog[main_key] = entry

    return catalog, main_catalog


def load_pdt_catalog(path):
    mapping = {}
    if not os.path.exists(path):
        return mapping
    with open(path, 'r', encoding='utf-8') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('/*') or line.startswith('//'):
                continue
            m = PDT_ENTRY_RE.match(line)
            if not m:
                continue
            enum = m.group('enum').strip()
            token = m.group('token').strip()
            val_raw = m.group('value').strip()
            try:
                val = int(val_raw, 0)
            except Exception:
                val = None
            try:
                size = int(m.group('size'))
            except Exception:
                size = 0
            desc = m.group('desc').strip()
            mapping[token] = {'enum_name': enum, 'element_size': size, 'description': desc, 'value': val}
            mapping[enum] = {'enum_name': enum, 'element_size': size, 'description': desc, 'value': val}
    return mapping


def load_knx_root(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            xml_names = [name for name in archive.namelist() if name.lower().endswith('.xml')]
            for name in xml_names:
                data = archive.read(name)
                try:
                    return ET.fromstring(data)
                except ET.ParseError:
                    continue
        raise ValueError(f'No parseable XML found in archive: {path}')

    return ET.parse(path).getroot()


def find_comobjects(root):
    # Support both older 'ComObjects' and 'ComObjectTable' variants
    cos = []
    for table_tag in ('ComObject',):
        # search both ComObjects/ComObjectTable containers
        for parent_tag in ('ComObjects', 'ComObjectTable'):
            xpath = f".//{{{KNX_NS}}}{parent_tag}/{{{KNX_NS}}}{table_tag}"
            for co in root.findall(xpath):
                cos.append(co)
    # also try direct ComObject occurrences
    for co in root.findall(f'.//{{{KNX_NS}}}ComObject'):
        if co not in cos:
            cos.append(co)
    return cos


def first_application_program(root):
    return root.find('.//knx:ManufacturerData/knx:Manufacturer/knx:ApplicationPrograms/knx:ApplicationProgram', NS)


def text_of(elem, name):
    child = elem.find(f'knx:{name}', NS)
    if child is not None and child.text:
        return child.text.strip()
    return None


def attr(elem, name, default=''):
    v = elem.get(name)
    return v if v is not None else default


def bool_flag(elem, name):
    return attr(elem, name, '').lower() == 'enabled'


def sanitize_identifier(text, default='GeneratedProduct'):
    collapsed = NON_ALNUM_RE.sub('_', text or '').strip('_')
    if not collapsed:
        collapsed = default
    if collapsed[0].isdigit():
        collapsed = f'_{collapsed}'
    return collapsed


def to_pascal_case(text, default='GeneratedObject'):
    parts = [part for part in NON_ALNUM_RE.split(text or '') if part]
    if not parts:
        return default
    result = ''.join(part[:1].upper() + part[1:] for part in parts)
    if result[0].isdigit():
        result = f'Object{result}'
    return result


def to_snake_case(text, default='object'):
    ident = sanitize_identifier(text, default=default).lower()
    return ident or default


def unique_names(values, fallback_factory):
    seen = {}
    result = []
    for index, value in enumerate(values):
        base = value or fallback_factory(index)
        count = seen.get(base, 0)
        seen[base] = count + 1
        result.append(base if count == 0 else f'{base}_{count + 1}')
    return result


def medium_literal_for(root):
    medium = root.find('.//knx:MasterData/knx:MediumTypes/knx:MediumType', NS)
    number = attr(medium, 'Number', '0') if medium is not None else '0'
    mapping = {
        '0': 'knx::product::Medium::TP1',
        '1': 'knx::product::Medium::IP_Tunneling',
        '2': 'knx::product::Medium::IP_Routing',
    }
    return mapping.get(number, 'knx::product::Medium::TP1')


def parse_group_address_expr(co):
    encoded = attr(co, 'DefaultAddressEncoded', '') or text_of(co, 'DefaultAddressEncoded') or ''
    if encoded.isdigit():
        return f'GroupAddress({int(encoded)})'

    formatted = attr(co, 'DefaultAddress', '') or text_of(co, 'DefaultAddressFormatted') or text_of(co, 'DefaultAddress') or ''
    if formatted and '/' in formatted:
        parts = [part.strip() for part in formatted.split('/')]
        if len(parts) == 3 and all(part.isdigit() for part in parts):
            return f'GroupAddress({int(parts[0])}, {int(parts[1])}, {int(parts[2])})'
        if len(parts) == 2 and all(part.isdigit() for part in parts):
            return f'GroupAddress({int(parts[0])}, {int(parts[1])})'

    return 'GroupAddress()'


def build_datapoint_type_index(root):
    index = {}

    for dpt in root.findall('.//knx:DatapointType', NS):
        dpt_id = dpt.get('Id')
        dpt_number = dpt.get('Number')
        if dpt_id and dpt_number:
            index[dpt_id] = normalize_dpt(dpt_number)

        for subtype in dpt.findall('knx:DatapointSubtypes/knx:DatapointSubtype', NS):
            subtype_id = subtype.get('Id')
            subtype_number = subtype.get('Number')
            if dpt_number and subtype_id and subtype_number:
                index[subtype_id] = normalize_dpt(f'{dpt_number}.{subtype_number}')

    return index


def resolve_com_object_dpt(co, datapoint_type_index):
    raw_dpt = attr(co, 'DPT', '')
    if raw_dpt:
        return normalize_dpt(raw_dpt)

    dpt_elem = co.find('knx:DPT', NS)
    if dpt_elem is not None and dpt_elem.text:
        return normalize_dpt(dpt_elem.text)

    datapoint_type_refs = attr(co, 'DatapointType', '').split()
    for ref in datapoint_type_refs:
        resolved = datapoint_type_index.get(ref)
        if resolved:
            return resolved

    return ''


def dpt_support_for(norm_dpt, exact_catalog, main_catalog):
    unsupported = {
        'tag_literal': 'knx::application::dpttags::Bool',
        'dpt_literal': 'knx::application::DptId{}',
        'value_type_literal': 'knx::application::DptValue::Type::Unsupported',
        'encode_literal': 'nullptr',
        'decode_literal': 'nullptr',
        'runtime_supported': False,
        'comment': 'missing DPT metadata',
    }

    if not norm_dpt:
        return unsupported

    exact = exact_catalog.get(norm_dpt)
    if exact:
        return {
            'tag_literal': exact['tag_literal'],
            'dpt_literal': exact['id_literal'],
            'value_type_literal': exact['value_type_literal'],
            'encode_literal': exact['encode_literal'],
            'decode_literal': exact['decode_literal'],
            'runtime_supported': True,
            'comment': '',
        }

    parts = norm_dpt.split('.', 1)
    if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
        main = parts[0]
        fallback = main_catalog.get(main)
        if fallback:
            return {
                'tag_literal': fallback['tag_literal'],
                'dpt_literal': f'knx::application::makeDptId({int(parts[0])}, {int(parts[1])})',
                'value_type_literal': fallback['value_type_literal'],
                'encode_literal': fallback['encode_literal'],
                'decode_literal': fallback['decode_literal'],
                'runtime_supported': True,
                'comment': f'fallback to main-family codec for DPT {norm_dpt}',
            }

        return {
            'dpt_literal': f'knx::application::makeDptId({int(parts[0])}, {int(parts[1])})',
            'value_type_literal': 'knx::application::DptValue::Type::Unsupported',
            'encode_literal': 'nullptr',
            'decode_literal': 'nullptr',
            'runtime_supported': False,
            'comment': f'unsupported DPT {norm_dpt}',
        }

    if len(parts) == 1 and parts[0].isdigit():
        fallback = exact_catalog.get(parts[0]) or main_catalog.get(parts[0])
        if fallback:
            return {
                'tag_literal': fallback['tag_literal'],
                'dpt_literal': f'knx::application::makeDptId({int(parts[0])})',
                'value_type_literal': fallback['value_type_literal'],
                'encode_literal': fallback['encode_literal'],
                'decode_literal': fallback['decode_literal'],
                'runtime_supported': True,
                'comment': '',
            }

        return {
            'dpt_literal': f'knx::application::makeDptId({int(parts[0])})',
            'value_type_literal': 'knx::application::DptValue::Type::Unsupported',
            'encode_literal': 'nullptr',
            'decode_literal': 'nullptr',
            'runtime_supported': False,
            'comment': f'unsupported DPT {norm_dpt}',
        }

    return unsupported


def cpp_string_literal(text):
    return json.dumps(text or '')


def extract_product_model(root, exact_catalog, main_catalog):
    app = first_application_program(root)
    product_name = attr(app, 'Name', '') if app is not None else ''
    profile_key = attr(app, 'Id', product_name) if app is not None else product_name

    manufacturer = root.find('.//knx:MasterData/knx:Manufacturers/knx:Manufacturer', NS)
    manufacturer_id = int(attr(manufacturer, 'KnxManufacturerId', '0') or '0') if manufacturer is not None else 0
    if not product_name and manufacturer is not None:
        product_name = attr(manufacturer, 'Name', 'GeneratedProduct')

    datapoint_type_index = build_datapoint_type_index(root)
    com_objects = find_comobjects(app if app is not None else root)

    enum_bases = [to_pascal_case(attr(co, 'Name', attr(co, 'Text', attr(co, 'Id', 'Object'))), default=f'Object{index + 1}')
                  for index, co in enumerate(com_objects)]
    enum_names = unique_names(enum_bases, lambda index: f'Object{index + 1}')

    key_bases = [to_snake_case(attr(co, 'Name', attr(co, 'Text', attr(co, 'Id', 'object'))), default=f'object_{index + 1}')
                 for index, co in enumerate(com_objects)]
    keys = unique_names(key_bases, lambda index: f'object_{index + 1}')

    objects = []
    for index, co in enumerate(com_objects):
        name = attr(co, 'Name', attr(co, 'Text', attr(co, 'Id', f'Object {index + 1}')))
        dpt = resolve_com_object_dpt(co, datapoint_type_index)
        support = dpt_support_for(dpt, exact_catalog, main_catalog)
        export_number = int(attr(co, 'Number', str(index + 1)) or (index + 1))
        readable = bool_flag(co, 'ReadFlag')
        writable = bool_flag(co, 'WriteFlag')
        transmit = bool_flag(co, 'TransmitFlag')
        receivable = writable
        persisted = False
        objects.append({
            'enum_name': enum_names[index],
            'logical_id': index,
            'export_number': export_number,
            'key': keys[index],
            'display_name': name,
            'address_expr': parse_group_address_expr(co),
            'dpt': dpt,
            'support': support,
            'readable': readable,
            'writable': writable,
            'transmit': transmit,
            'receivable': receivable,
            'persisted': persisted,
        })

    # Build property lookup (Id -> numeric PropertyID, data type and description) for parameter resolution
    def resolve_prop_type_name(prop_type_str: str) -> str:
            if not prop_type_str:
                return ''
            s = prop_type_str.strip()
            # direct common form (PDT_...)
            if s in PDT_CATALOG:
                return s
            # strip namespace-like prefixes
            if s.startswith('PDT_'):
                if s in PDT_CATALOG:
                    return s
            # try enum form PropertyDataType::Name or just enum name
            if '::' in s:
                parts = s.split('::', 1)
                key = parts[1]
                if key in PDT_CATALOG:
                    return key
            if s in PDT_CATALOG:
                return s
            return s

    def map_prop_type_to_export_kind(prop_type_str: str) -> str:
            if not prop_type_str:
                return 'knx::product::ExportParameterValueKind::None'
            s = prop_type_str.strip()
            # prefer standardized PDT catalog entries (single source of truth)
            std = resolve_prop_type_name(prop_type_str)
            entry = None
            if std and std in PDT_CATALOG:
                entry = PDT_CATALOG.get(std)
            if entry:
                ename = entry.get('enum_name', '')
                size = entry.get('element_size', 0) or 0
                # Map using enum name and size
                if ename in ('Control', 'Enum8'):
                    return 'knx::product::ExportParameterValueKind::Enum'
                if ename in ('Char', 'CharBlock', 'ShortCharBlock', 'Utf8'):
                    return 'knx::product::ExportParameterValueKind::Text'
                if ename in ('UnsignedChar',):
                    return 'knx::product::ExportParameterValueKind::Unsigned8'
                if ename in ('UnsignedInt', 'UnsignedLong') or size == 2:
                    return 'knx::product::ExportParameterValueKind::Unsigned16'
                if ename in ('Int', 'Long', 'Float', 'KnxFloat') or size == 2:
                    return 'knx::product::ExportParameterValueKind::Signed16'
                # default fallback
                return 'knx::product::ExportParameterValueKind::None'
            # original heuristic fallback (case-insensitive)
            s = prop_type_str.strip().upper()
            if s in ('PDT_CONTROL',):
                return 'knx::product::ExportParameterValueKind::Enum'
            if s in ('PDT_CHAR', 'PDT_CHAR_BLOCK', 'PDT_SHORT_CHAR_BLOCK', 'PDT_UTF-8', 'PDT_VARIABLE_LENGTH', 'PDT_DATE', 'PDT_TIME', 'PDT_DATE_TIME', 'PDT_VERSION'):
                return 'knx::product::ExportParameterValueKind::Text'
            if s in ('PDT_UNSIGNED_CHAR', 'PDT_BITSET8'):
                return 'knx::product::ExportParameterValueKind::Unsigned8'
            if s in ('PDT_UNSIGNED_INT', 'PDT_UNSIGNED_LONG'):
                return 'knx::product::ExportParameterValueKind::Unsigned16'
            if s in ('PDT_INT', 'PDT_LONG', 'PDT_SCALING'):
                return 'knx::product::ExportParameterValueKind::Signed16'
            if s in ('PDT_ENUM8',):
                return 'knx::product::ExportParameterValueKind::Enum'
            # default fallback
            return 'knx::product::ExportParameterValueKind::None'

    property_index = {}
    for prop in root.findall('.//knx:Property', NS):
        prop_id = prop.get('Id')
        prop_pid = prop.get('PropertyID') or prop.get('PropertyId')
        prop_type = prop.get('PropertyDataType')
        desc = text_of(prop, 'Description') or prop.get('Description') or prop.get('Name') or prop.get('Text')
        try:
            pid_num = int(prop_pid) if prop_pid is not None and prop_pid != '' else None
        except Exception:
            pid_num = None
        export_kind = map_prop_type_to_export_kind(prop_type)
        # detect if property is flagged required/mandatory in the .knxprod
        prop_required = False
        for req_attr in ('Required', 'required', 'Mandatory', 'mandatory', 'IsRequired', 'isRequired'):
            v = prop.get(req_attr)
            if v is not None and str(v).strip().lower() in ('1', 'true', 'yes', 'enabled'):
                prop_required = True
                break
        if prop_id:
            property_index[prop_id] = {
                'property_id': pid_num,
                'description': desc,
                'prop_type': prop_type,
                'export_kind': export_kind,
                'required': prop_required,
            }

    # Collect functional-block parameters (if any)
    parameters = []
    assigned_param_ids = set()
    next_generated_param_id = 1
    for fb in root.findall('.//knx:FunctionalBlocks/knx:FunctionalBlock', NS):
        for param_block in fb.findall('knx:Parameters/knx:Parameter', NS):
            prop_ref = attr(param_block, 'Property', '')
            if not prop_ref:
                continue
            prop_entry = property_index.get(prop_ref, {})
            pid = prop_entry.get('property_id')
            if pid is None:
                # allocate a generated id that's not conflicting with any declared ones
                while next_generated_param_id in assigned_param_ids:
                    next_generated_param_id += 1
                pid = next_generated_param_id
                assigned_param_ids.add(pid)
                next_generated_param_id += 1
            else:
                assigned_param_ids.add(pid)

            key = sanitize_identifier(prop_ref, default=f'param_{pid}').lower()
            display = prop_entry.get('description') or prop_ref
            value_kind_literal = prop_entry.get('export_kind', 'knx::product::ExportParameterValueKind::None')
            # resolve canonical PDT token / enum if available
            prop_type_raw = prop_entry.get('prop_type', '')
            prop_type_std = ''
            prop_type_literal = 'static_cast<knx::application::PropertyDataType>(0)'
            if prop_type_raw:
                prop_type_std = resolve_prop_type_name(prop_type_raw)
                if prop_type_std and prop_type_std in PDT_CATALOG:
                    entry = PDT_CATALOG.get(prop_type_std)
                    enum_name = entry.get('enum_name') if entry else None
                    val = entry.get('value') if entry else None
                    if enum_name:
                        prop_type_literal = f'knx::application::PropertyDataType::{enum_name}'
                    elif val is not None:
                        prop_type_literal = f'static_cast<knx::application::PropertyDataType>({int(val)})'
                else:
                    # try to interpret raw token as numeric
                    try:
                        v = int(prop_type_raw, 0)
                        prop_type_literal = f'static_cast<knx::application::PropertyDataType>({v})'
                    except Exception:
                        pass
            # parameter may be flagged required either on the Parameter element or in the referenced Property
            required = False
            # check common attribute names on the Parameter element
            for req_attr in ('Required', 'required', 'Mandatory', 'mandatory', 'IsRequired', 'isRequired'):
                v = param_block.get(req_attr)
                if v is not None and str(v).strip().lower() in ('1', 'true', 'yes', 'enabled'):
                    required = True
                    break
            # check for an explicit child element <Required>true</Required>
            if not required:
                child_req = param_block.find('knx:Required', NS)
                if child_req is not None and (child_req.text or '').strip().lower() in ('1', 'true', 'yes', 'enabled'):
                    required = True
            # fallback to property-level required flag if available
            if not required:
                required = bool(prop_entry.get('required'))
            parameters.append({
                'id': pid,
                'key': key,
                'display_name': display,
                'value_kind_literal': value_kind_literal,
                'required': required,
                'prop_type_std': prop_type_std,
                'prop_type_literal': prop_type_literal,
            })

    # Collect function names per communication object (FunctionText), fallback to display name
    function_names = [attr(co, 'FunctionText', '') or co.get('Function') or co.get('FunctionText') or obj['display_name']
                      for co, obj in zip(com_objects, objects)]

    application_number = int(attr(app, 'ApplicationNumber', '0') or '0') if app is not None else 0
    application_version = int(attr(app, 'ApplicationVersion', '0') or '0') if app is not None else 0
    namespace_name = sanitize_identifier(profile_key or product_name, default='generated_product_profile').lower()

    return {
        'product_name': product_name or 'GeneratedProduct',
        'profile_key': profile_key or product_name or 'generated_product',
        'manufacturer_id': manufacturer_id,
        'application_number': application_number,
        'application_version': application_version,
        'medium_literal': medium_literal_for(root),
        'namespace': namespace_name,
        'persistence_namespace': namespace_name,
        'objects': objects,
        'parameters': parameters,
        'function_names': function_names,
    }


def generate_header(model, template_text, output_path):
    header_guard = os.path.basename(output_path).upper().replace('.', '_').replace('-', '_')
    object_enum_lines = []
    object_descriptor_lines = []
    export_object_lines = []

    for index, obj in enumerate(model['objects']):
        object_enum_lines.append(f'    {obj["enum_name"]} = {obj["logical_id"]},')

        metadata_literal = (
            'knx::product::makeProductDatapointMetadata('
            f'{cpp_string_literal(obj["key"])}, '
            f'{cpp_string_literal(obj["display_name"])}, '
            f'{obj["export_number"]})'
        )

        descriptor_line = (
            f'        DatapointDescriptor{{ObjectId::{obj["enum_name"]}, '
            f'{obj["address_expr"]}, '
            f'{obj["support"]["dpt_literal"]}, '
            f'{obj["support"]["value_type_literal"]}, '
            f'{obj["support"]["encode_literal"]}, '
            f'{obj["support"]["decode_literal"]}, '
            f'{str(obj["readable"]).lower()}, '
            f'{str(obj["writable"]).lower()}, '
            f'{str(obj["transmit"]).lower()}, '
            f'{str(obj["receivable"]).lower()}, '
            f'{str(obj["persisted"]).lower()}, '
            f'{metadata_literal}}},'
        )
        if obj['support']['comment']:
            descriptor_line = f'        // {obj["support"]["comment"]}\n' + descriptor_line
        object_descriptor_lines.append(descriptor_line)

        export_object_lines.append(
            '            knx::product::makeExportCommunicationObjectDescriptor('
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].metadata.exportNumber,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].id,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].metadata.key,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].metadata.displayName,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].primaryAddress,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].dpt,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].valueType,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].readable,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].writable,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].transmit,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].receivable,'
            f'\n                ProductDefinition::kDefinition.datapoints[{index}].persisted),'
        )

    primary_object = model['objects'][0] if model['objects'] else {
        'enum_name': 'Object0',
        'support': {'tag_literal': 'knx::application::dpttags::Bool'},
    }
    secondary_object = model['objects'][1] if len(model['objects']) > 1 else primary_object

    out = template_text.replace('{{HEADER_GUARD}}', header_guard)
    out = out.replace('{{PRODUCT_DISPLAY_NAME}}', model['product_name'])
    out = out.replace('{{PROFILE_KEY}}', model['profile_key'])
    out = out.replace('{{MANUFACTURER_ID}}', str(model['manufacturer_id']))
    out = out.replace('{{NAMESPACE}}', model['namespace'])
    out = out.replace('{{MEDIUM_LITERAL}}', model['medium_literal'])
    out = out.replace('{{APPLICATION_NUMBER}}', str(model['application_number']))
    out = out.replace('{{APPLICATION_VERSION}}', str(model['application_version']))
    out = out.replace('{{GROUP_OBJECT_COUNT}}', str(len(model['objects'])))
    out = out.replace('{{ADDRESS_TABLE_ENTRIES}}', str(len(model['objects'])))
    out = out.replace('{{ASSOCIATION_ENTRIES}}', str(len(model['objects'])))
    out = out.replace('{{AUTO_RESPONSE_QUEUE_CAPACITY}}', str(len(model['objects'])))
    out = out.replace('{{SEND_OUTCOME_QUEUE_CAPACITY}}', '4')
    out = out.replace('{{PERSISTENCE_NAMESPACE}}', model['persistence_namespace'])
    out = out.replace('{{OBJECT_ENUMS}}', '\n'.join(object_enum_lines))
    out = out.replace('{{OBJECT_DESCRIPTORS}}', '\n'.join(object_descriptor_lines))
    out = out.replace('{{EXPORT_OBJECTS}}', '\n'.join(export_object_lines))
    out = out.replace('{{PRIMARY_OBJECT_ENUM}}', primary_object['enum_name'])
    out = out.replace('{{SECONDARY_OBJECT_ENUM}}', secondary_object['enum_name'])
    out = out.replace('{{PRIMARY_TAG_LITERAL}}', primary_object['support'].get('tag_literal', 'knx::application::dpttags::Bool'))
    out = out.replace('{{SECONDARY_TAG_LITERAL}}', secondary_object['support'].get('tag_literal', 'knx::application::dpttags::Bool'))

    # Parameters: build parameter descriptors for export descriptor
    parameter_descriptor_lines = []
    parameters = model.get('parameters', []) or []
    param_count = len(parameters)
    for p in parameters:
        kind_lit = p.get('value_kind_literal', 'knx::product::ExportParameterValueKind::None')
        std_type = p.get('prop_type_std', '')
        prop_type_lit = p.get('prop_type_literal', 'static_cast<knx::application::PropertyDataType>(0)')

        parameter_descriptor_lines.append(
            '            knx::product::makeExportParameterDescriptor('\
            + f'{int(p["id"])}, '\
            + f'{cpp_string_literal(p["key"])}, '\
            + f'{cpp_string_literal(p["display_name"])}, '\
            + f'{kind_lit}, '\
            + f'{str(p.get("required", False)).lower()}, '\
            + f'{prop_type_lit}),' 
        )
        if std_type:
            parameter_descriptor_lines[-1] += f' // propType: {std_type}'

    # Function name literals for simple per-object function metadata
    function_name_literals = []
    for fn in model.get('function_names', []):
        function_name_literals.append(f'{cpp_string_literal(fn)}')

    out = out.replace('{{PARAMETER_COUNT}}', str(param_count))
    out = out.replace('{{PARAMETER_DESCRIPTORS}}', '\n'.join(parameter_descriptor_lines))
    out = out.replace('{{FUNCTION_NAMES}}', ',\n'.join(function_name_literals))

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(out)
    print(f'Wrote {output_path}')


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('input', help='Input .knxprod XML file (or extracted xml)')
    parser.add_argument('--output', '-o', help='Output header path', default='build/generated_product_profile.hpp')
    parser.add_argument('--dpt-catalog', help='Path to KNstaX DPT catalog', default='include/knx/application/dpt_catalog.inc')
    parser.add_argument('--pdt-catalog', help='Path to KNstaX PDT catalog', default='include/knx/application/pdt_catalog.inc')
    parser.add_argument('--template', help='Template file', default='templates/profile_header.tpl')
    args = parser.parse_args(argv)

    if not os.path.exists(args.input):
        print('Input file not found:', args.input, file=sys.stderr)
        return 2

    root = load_knx_root(args.input)

    # load template
    if not os.path.exists(args.template):
        print('Template not found:', args.template, file=sys.stderr)
        return 2
    with open(args.template, 'r', encoding='utf-8') as f:
        tpl = f.read()

    if not os.path.exists(args.dpt_catalog):
        print('DPT catalog not found:', args.dpt_catalog, file=sys.stderr)
        return 2
    exact_catalog, main_catalog = load_dpt_catalog(args.dpt_catalog)
    global PDT_CATALOG
    PDT_CATALOG = load_pdt_catalog(args.pdt_catalog)
    model = extract_product_model(root, exact_catalog, main_catalog)

    generate_header(model, tpl, args.output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
