#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Parameter blocks in the product file (docs/design/parameter-model.md §3.8).

A product with a parameter layout exports `parameterBlocks`: blocks with a
base, a stride and a repeat count. These tests check what the exporter makes
of them: one set of parameters and one page per instance, offsets taken from
the layout, one ParameterType per shared option list, and the visibility
conditions ETS understands.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from exporter import export_product_json_to_knxprod  # noqa: E402

NS = {'k': 'http://knx.org/xml/project/23'}

KIND_BOOL, KIND_U8, KIND_U16, KIND_ENUM = 1, 2, 3, 5


def _doc(blocks, option_types=(), image_size=None):
    if image_size is None:
        image_size = sum(b['stride'] * b['count'] for b in blocks)
    return {
        'identity': {'profileKey': 't', 'productDisplayName': 'T', 'manufacturerId': 250,
                     'medium': {'code': 0, 'name': 'TP', 'maskVersion': 0x07B0, 'ipEnabled': False},
                     'applicationProgram': {'number': 1, 'version': 1}},
        'features': {},
        'capacities': {'addressTableMaxEntries': 256, 'associationTableMaxEntries': 512},
        'communicationObjects': [{
            'exportNumber': 0, 'logicalId': 0, 'key': 'k', 'displayName': 'K',
            'defaultAddress': 0, 'dpt_main': 1, 'dpt_sub': 1, 'valueType': 1,
            'readable': True, 'writable': False, 'transmit': True, 'receivable': False,
            'readOnInit': False, 'communication': True, 'persisted': False}],
        'parameters': [],
        'parameterImage': {'size': image_size, 'fingerprint': 1},
        'optionTypes': list(option_types),
        'parameterBlocks': blocks,
    }


def _root(*args, **kwargs):
    return ET.fromstring(export_product_json_to_knxprod(_doc(*args, **kwargs)))


def _find(root, tag):
    return root.findall(f'.//k:{tag}', NS)


FUNCTION_OPTIONS = {'id': 0, 'sizeInBit': 8,
                    'options': [{'value': 0, 'label': 'Disabled'}, {'value': 1, 'label': 'Switch'},
                                {'value': 2, 'label': 'Dimming'}]}
ACTION_OPTIONS = {'id': 1, 'sizeInBit': 8,
                  'options': [{'value': 0, 'label': 'Nothing'}, {'value': 1, 'label': 'On'}]}


def _device_block():
    return {'index': 0, 'key': 'device', 'name': 'Device', 'count': 1, 'base': 0, 'stride': 2,
            'parameters': [
                {'id': 0, 'key': 'delay', 'name': 'Delay', 'valueKind': KIND_U8, 'sizeInBit': 8,
                 'offset': 0, 'default': 2, 'min': 0, 'max': 60, 'unit': 's'},
                {'id': 1, 'key': 'expert', 'name': 'Expert', 'valueKind': KIND_BOOL, 'sizeInBit': 8,
                 'offset': 1, 'default': 0}]}


def _input_block(count=3):
    return {'index': 1, 'key': 'input', 'name': 'Input {n}', 'count': count, 'base': 2, 'stride': 5,
            'parameters': [
                {'id': 0, 'key': 'function', 'name': 'Function', 'valueKind': KIND_ENUM, 'sizeInBit': 8,
                 'offset': 0, 'default': 1, 'optionType': 0},
                {'id': 1, 'key': 'closing', 'name': 'On closing', 'valueKind': KIND_ENUM, 'sizeInBit': 8,
                 'offset': 1, 'default': 1, 'optionType': 1,
                 'visible': {'block': 1, 'parameter': 0, 'op': 'anyOf', 'values': [1]}},
                {'id': 2, 'key': 'opening', 'name': 'On opening', 'valueKind': KIND_ENUM, 'sizeInBit': 8,
                 'offset': 2, 'default': 0, 'optionType': 1,
                 'visible': {'block': 1, 'parameter': 0, 'op': 'anyOf', 'values': [1, 2]}},
                {'id': 3, 'key': 'long_ms', 'name': 'Long press', 'valueKind': KIND_U16, 'sizeInBit': 16,
                 'offset': 3, 'default': 500, 'min': 300, 'max': 3000, 'unit': 'ms',
                 'visible': {'block': 1, 'parameter': 0, 'op': 'notEqual', 'values': [0]}}]}


def test_a_repeated_block_is_written_once_per_instance():
    root = _root([_device_block(), _input_block(3)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    parameters = _find(root, 'Parameter')
    assert len(parameters) == 2 + 3 * 4
    assert [p.get('Name') for p in parameters[:6]] == [
        'device_delay', 'device_expert', 'input1_function', 'input1_closing', 'input1_opening', 'input1_long_ms']
    blocks = _find(root, 'ParameterBlock')
    assert [b.get('Text') for b in blocks] == ['Device', 'Input 1', 'Input 2', 'Input 3']
    ids = [p.get('Id') for p in parameters] + [b.get('Id') for b in blocks]
    assert len(ids) == len(set(ids)), 'every instance needs ids of its own'


def test_offsets_are_base_plus_instance_times_stride_plus_offset():
    root = _root([_device_block(), _input_block(3)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    offsets = [int(m.get('Offset')) for m in root.findall('.//k:Parameter/k:Memory', NS)]
    assert offsets == [0, 1,
                       2, 3, 4, 5,
                       7, 8, 9, 10,
                       12, 13, 14, 15]


def test_the_segment_and_the_download_are_the_size_of_the_image():
    root = _root([_device_block(), _input_block(3)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    assert _find(root, 'RelativeSegment')[0].get('Size') == '17'
    assert _find(root, 'LdCtrlRelSegment')[0].get('Size') == '17'
    assert _find(root, 'LdCtrlWriteRelMem')[0].get('Size') == '17'


def test_a_shared_option_list_is_one_parameter_type():
    root = _root([_device_block(), _input_block(3)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    restrictions = _find(root, 'TypeRestriction')
    assert len(restrictions) == 2, 'two option lists, however many parameters and instances use them'
    types = {p.get('Name'): p.get('ParameterType') for p in _find(root, 'Parameter')}
    assert types['input1_closing'] == types['input1_opening'] == types['input3_closing']
    assert types['input1_function'] != types['input1_closing']
    enumeration_ids = [e.get('Id') for e in _find(root, 'Enumeration')]
    assert len(enumeration_ids) == len(set(enumeration_ids)) == 5


def test_equal_numeric_ranges_share_a_type_and_a_bool_is_a_check_box():
    root = _root([_device_block(), _input_block(2)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    numbers = _find(root, 'TypeNumber')
    assert len(numbers) == 3, 'delay 0..60, the check box, long press 300..3000'
    by_range = {(n.get('minInclusive'), n.get('maxInclusive')): n for n in numbers}
    assert by_range[('0', '60')].get('SizeInBit') == '8'
    assert by_range[('300', '3000')].get('SizeInBit') == '16'
    assert by_range[('0', '1')].get('UIHint') == 'CheckBox'
    units = {p.get('Name'): p.get('SuffixText') for p in _find(root, 'Parameter')}
    assert units['device_delay'] == 's' and units['input2_long_ms'] == 'ms'


def _when_of(root, parameter_name):
    """(controlling parameter name, test) pairs wrapped around a parameter, outermost first."""
    names = {p.get('Id'): p.get('Name') for p in _find(root, 'Parameter')}
    ref_to_param = {r.get('Id'): r.get('RefId') for r in _find(root, 'ParameterRef')}
    target = next(ref for ref, param in ref_to_param.items() if names[param] == parameter_name)

    def walk(element, chain):
        for child in element:
            tag = child.tag.rsplit('}', 1)[-1]
            if tag == 'ParameterRefRef' and child.get('RefId') == target:
                return chain
            if tag == 'choose':
                controlling = names[ref_to_param[child.get('ParamRefId')]]
                for when in child:
                    found = walk(when, chain + [(controlling, when.get('test'))])
                    if found is not None:
                        return found
            elif tag in ('ParameterBlock', 'ChannelIndependentBlock'):
                found = walk(child, chain)
                if found is not None:
                    return found
        return None

    return walk(_find(root, 'Dynamic')[0], [])


def test_a_condition_names_the_parameter_of_the_same_instance():
    root = _root([_device_block(), _input_block(3)], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    assert _when_of(root, 'input1_closing') == [('input1_function', '1')]
    assert _when_of(root, 'input3_closing') == [('input3_function', '1')]
    assert _when_of(root, 'input2_function') == []


def test_condition_forms():
    block = _input_block(1)
    block['parameters'].append(
        {'id': 4, 'key': 'ranged', 'name': 'Ranged', 'valueKind': KIND_U8, 'sizeInBit': 8,
         'offset': 5, 'default': 0,
         'visible': {'block': 1, 'parameter': 0, 'op': 'range', 'values': [1, 2]}})
    block['stride'] = 6
    root = _root([_device_block(), block], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    assert _when_of(root, 'input_opening') == [('input_function', '1 2')]
    assert _when_of(root, 'input_long_ms') == [('input_function', '!=0')]
    assert _when_of(root, 'input_ranged') == [('input_function', '>=1'), ('input_function', '<=2')]


def test_a_block_condition_hides_the_whole_page():
    inputs = _input_block(2)
    inputs['visible'] = {'block': 0, 'parameter': 1, 'op': 'anyOf', 'values': [1]}
    root = _root([_device_block(), inputs], [FUNCTION_OPTIONS, ACTION_OPTIONS])
    # Both pages hang under a test of the one device parameter, and the
    # condition of a parameter inside still names its own instance.
    assert _when_of(root, 'input2_function') == [('device_expert', '1')]
    assert _when_of(root, 'input2_closing') == [('device_expert', '1'), ('input2_function', '1')]
    channel = _find(root, 'ChannelIndependentBlock')[0]
    assert [child.tag.rsplit('}', 1)[-1] for child in channel][:3] == ['ParameterBlock', 'choose', 'choose']


def test_a_document_that_contradicts_itself_is_refused():
    for breaker in ('gap', 'size', 'overflow'):
        device, inputs = _device_block(), _input_block(2)
        size = None
        if breaker == 'gap':
            inputs['base'] = 3
        elif breaker == 'size':
            size = 99
        else:
            inputs['parameters'][3]['offset'] = 4
        try:
            export_product_json_to_knxprod(_doc([device, inputs], [FUNCTION_OPTIONS, ACTION_OPTIONS], size))
        except ValueError:
            continue
        raise AssertionError(f'a layout with a wrong {breaker} must not become a product file')


def test_a_product_has_one_parameter_model():
    document = _doc([_device_block()])
    document['parameters'] = [{'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3,
                               'defaultValue': 0, 'required': False}]
    try:
        export_product_json_to_knxprod(document)
    except SystemExit:
        return
    raise AssertionError('both parameter models in one export must be refused')


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith('test_') and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f'ok   {name}')
        except AssertionError as error:
            failed += 1
            print(f'FAIL {name}: {error}')
    print(f'{len(tests) - failed}/{len(tests)} passed')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
