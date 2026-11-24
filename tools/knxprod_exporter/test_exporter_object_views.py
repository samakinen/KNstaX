#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Object views in the product file (knx/product/object_views.hpp).

A view tells ETS when to show a communication object and with which text and
datapoint type. These tests check that a port with views becomes one
ComObjectRef per view, each inside a <choose> on the controlling parameter of
the right block instance; that a port without views is exported as before;
and that views ETS could not use are refused.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from exporter import export_product_json_to_knxprod  # noqa: E402

NS = {'k': 'http://knx.org/xml/project/23'}
KIND_ENUM = 5

FUNCTION_OPTIONS = {'id': 0, 'sizeInBit': 8,
                    'options': [{'value': 0, 'label': 'Disabled'}, {'value': 1, 'label': 'Switch'},
                                {'value': 2, 'label': 'Shutter'}, {'value': 3, 'label': 'Value'}]}


def _co(number, key, main, sub, writable=False):
    return {'exportNumber': number, 'logicalId': number, 'key': key, 'displayName': key.upper(),
            'defaultAddress': 0, 'dpt_main': main, 'dpt_sub': sub, 'valueType': 1,
            'readable': True, 'writable': writable, 'transmit': True, 'receivable': False,
            'readOnInit': False, 'communication': True, 'persisted': False}


def _when(values, block=0, op='anyOf'):
    return {'block': block, 'parameter': 0, 'op': op, 'values': values}


def _doc(views, count=2):
    return {
        'identity': {'profileKey': 't', 'productDisplayName': 'T', 'manufacturerId': 250,
                     'medium': {'code': 0, 'name': 'TP', 'maskVersion': 0x07B0, 'ipEnabled': False},
                     'applicationProgram': {'number': 1, 'version': 1}},
        'features': {},
        'capacities': {'addressTableMaxEntries': 64, 'associationTableMaxEntries': 96},
        # Port 0: 1 bit, port 1: 1 byte, port 2: 1 bit without views.
        'communicationObjects': [_co(0, 'a', 1, 1, writable=True), _co(1, 'd', 5, 10), _co(2, 'plain', 1, 1)],
        'parameterImage': {'size': count, 'fingerprint': 1},
        'optionTypes': [FUNCTION_OPTIONS],
        'parameterBlocks': [
            {'index': 0, 'key': 'input', 'name': 'Input {n}', 'count': count, 'base': 0, 'stride': 1,
             'parameters': [{'id': 0, 'key': 'function', 'name': 'Function', 'valueKind': KIND_ENUM,
                             'sizeInBit': 8, 'offset': 0, 'default': 1, 'optionType': 0}]}],
        'objectViews': views,
    }


def _root(views, **kwargs):
    return ET.fromstring(export_product_json_to_knxprod(_doc(views, **kwargs)))


def _function_refs(root):
    """ParameterRef id of input n's Function parameter, by instance."""
    params = {p.get('Id'): p.get('Name') for p in root.findall('.//k:Parameter', NS)}
    refs = {}
    for ref in root.findall('.//k:ParameterRef', NS):
        name = params[ref.get('RefId')]
        refs[int(name[len('input'):].split('_')[0]) - 1] = ref.get('Id')
    return refs


def _dynamic_refrefs(root):
    """(ComObjectRef id, [(choose ParamRefId, when test), ...]) for every
    ComObjectRefRef in the Dynamic section, with the chooses around it."""
    out = []

    def walk(element, path):
        for child in element:
            tag = child.tag.split('}')[1]
            if tag == 'choose':
                for when in child:
                    walk(when, path + [(child.get('ParamRefId'), when.get('test'))])
            elif tag == 'ComObjectRefRef':
                out.append((child.get('RefId'), path))
            else:
                walk(child, path)

    walk(root.find('.//k:Dynamic', NS), [])
    return out


SWITCH_VIEWS = [
    {'logicalId': 0, 'instance': 1, 'function': 'Switch', 'dpt_main': 1, 'dpt_sub': 1, 'visible': _when([1])},
    {'logicalId': 0, 'instance': 1, 'function': 'Move', 'dpt_main': 1, 'dpt_sub': 8, 'visible': _when([2])},
    {'logicalId': 1, 'instance': 1, 'text': 'Value', 'dpt_main': 17, 'dpt_sub': 1, 'visible': _when([3])},
]


def test_a_port_with_views_has_one_reference_per_view_under_its_condition():
    root = _root(SWITCH_VIEWS)
    refs = {r.get('Id'): r for r in root.findall('.//k:ComObjectRef', NS)}
    a_refs = [r for r in refs.values() if r.get('RefId').endswith('_O-0')]
    assert [r.get('FunctionText') for r in a_refs] == ['Switch', 'Move']
    assert [r.get('DatapointType') for r in a_refs] == ['DPST-1-1', 'DPST-1-8']
    # The text stays the port's display name where the view gives none.
    assert all(r.get('Text') == 'A' for r in a_refs)

    function_of_input_2 = _function_refs(root)[1]
    placed = dict(_dynamic_refrefs(root))
    assert placed[a_refs[0].get('Id')] == [(function_of_input_2, '1')]
    assert placed[a_refs[1].get('Id')] == [(function_of_input_2, '2')]


def test_one_comobject_per_port_whatever_the_views():
    root = _root(SWITCH_VIEWS)
    numbers = [int(co.get('Number')) for co in root.findall('.//k:ComObject', NS)]
    assert numbers == [0, 1, 2]


def test_a_view_text_and_datapoint_type_replace_the_ports():
    root = _root(SWITCH_VIEWS)
    d_ref = [r for r in root.findall('.//k:ComObjectRef', NS) if r.get('RefId').endswith('_O-1')]
    assert len(d_ref) == 1
    assert d_ref[0].get('Text') == 'Value'
    assert d_ref[0].get('FunctionText') == 'Value'
    assert d_ref[0].get('DatapointType') == 'DPST-17-1'
    assert d_ref[0].get('ObjectSize') == '1 Byte'
    # Its subtype reaches the master data, or ETS cannot resolve the reference.
    subtypes = {s.get('Id') for s in root.findall('.//k:DatapointSubtype', NS)}
    assert 'DPST-17-1' in subtypes and 'DPST-1-8' in subtypes


def test_a_port_without_views_is_exported_as_before():
    root = _root(SWITCH_VIEWS)
    plain = [r for r in root.findall('.//k:ComObjectRef', NS) if r.get('RefId').endswith('_O-2')]
    assert len(plain) == 1
    assert plain[0].get('Id').endswith('_O-2_R-2')
    assert dict(_dynamic_refrefs(root))[plain[0].get('Id')] == []


def test_a_view_without_condition_is_always_shown():
    root = _root([{'logicalId': 1, 'instance': 0, 'function': 'Level', 'dpt_main': 5, 'dpt_sub': 1}])
    d_ref = [r for r in root.findall('.//k:ComObjectRef', NS) if r.get('RefId').endswith('_O-1')][0]
    assert dict(_dynamic_refrefs(root))[d_ref.get('Id')] == []
    assert d_ref.get('DatapointType') == 'DPST-5-1'


def test_not_equal_and_range_conditions_are_written_as_ets_tests():
    root = _root([
        {'logicalId': 0, 'instance': 0, 'visible': _when([0], op='notEqual')},
        {'logicalId': 1, 'instance': 0, 'visible': _when([1, 3], op='range')},
    ])
    tests = {ref: [test for _, test in path] for ref, path in _dynamic_refrefs(root) if path}
    assert sorted(tests.values()) == [['!=0'], ['>=1', '<=3']]


def test_a_view_of_another_size_is_refused():
    with pytest.raises(ValueError, match='not the size'):
        _root([{'logicalId': 0, 'instance': 0, 'dpt_main': 5, 'dpt_sub': 1, 'visible': _when([1])}])


def test_views_that_can_show_at_once_are_refused():
    with pytest.raises(ValueError, match='both shown for the value 1'):
        _root([{'logicalId': 0, 'instance': 0, 'visible': _when([1, 2])},
               {'logicalId': 0, 'instance': 0, 'visible': _when([1], op='range') | {'values': [0, 1]}}])
    with pytest.raises(ValueError, match='without a condition'):
        _root([{'logicalId': 0, 'instance': 0},
               {'logicalId': 0, 'instance': 0, 'visible': _when([1])}])


def test_views_on_different_instances_are_refused():
    with pytest.raises(ValueError, match='different parameters'):
        _root([{'logicalId': 0, 'instance': 0, 'visible': _when([1])},
               {'logicalId': 0, 'instance': 1, 'visible': _when([2])}])


def test_a_file_with_views_is_valid_against_the_knx_schema():
    xmlschema = pytest.importorskip('xmlschema')
    schema = xmlschema.XMLSchema(str(Path(__file__).resolve().parent / 'schema' / 'knx_schema_version23.xsd'))
    schema.validate(export_product_json_to_knxprod(_doc(SWITCH_VIEWS)))


def test_objects_below_an_octet_use_the_schemas_spelling():
    doc = _doc([])
    doc['communicationObjects'] += [_co(3, 'forced', 2, 1), _co(4, 'dim', 3, 7)]
    root = ET.fromstring(export_product_json_to_knxprod(doc))
    sizes = {int(co.get('Number')): co.get('ObjectSize') for co in root.findall('.//k:ComObject', NS)}
    assert sizes[3] == '2 Bit' and sizes[4] == '4 Bit'
    xmlschema = pytest.importorskip('xmlschema')
    schema = xmlschema.XMLSchema(str(Path(__file__).resolve().parent / 'schema' / 'knx_schema_version23.xsd'))
    schema.validate(export_product_json_to_knxprod(doc))


def test_a_view_of_an_unknown_port_or_instance_is_refused():
    with pytest.raises(ValueError, match='port 9'):
        _root([{'logicalId': 9, 'instance': 0}])
    with pytest.raises(ValueError, match='instance 3'):
        _root([{'logicalId': 0, 'instance': 2, 'visible': _when([1])}])
