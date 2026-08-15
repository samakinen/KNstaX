#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""Exporter behaviours that decide whether an integrator can configure the
product, and whether ETS can actually download it.

Each of these was previously wrong in a way that is invisible from firmware:
the device works, but the ETS side is unusable or the declared download does
not describe what has to happen.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from exporter import export_product_json_to_knxprod  # noqa: E402

NS = {'k': 'http://knx.org/xml/project/23'}


def _doc(parameters):
    return {
        'identity': {'profileKey': 't', 'productDisplayName': 'T', 'manufacturerId': 250,
                     'medium': 0, 'applicationProgram': {'number': 1, 'version': 1}},
        'features': {}, 'capacities': {},
        'communicationObjects': [{
            'exportNumber': 0, 'logicalId': 0, 'key': 'k', 'displayName': 'K',
            'defaultAddress': 0, 'dpt_main': 1, 'dpt_sub': 1, 'valueType': 1,
            'readable': True, 'writable': False, 'transmit': True, 'receivable': False,
            'readOnInit': False, 'communication': True, 'persisted': False}],
        'parameters': parameters,
    }


def _root(parameters):
    return ET.fromstring(export_product_json_to_knxprod(_doc(parameters)))


def _find(root, tag):
    return root.findall(f'.//k:{tag}', NS)


def test_enum_parameter_becomes_a_dropdown():
    root = _root([{'id': 0, 'key': 'mode', 'displayName': 'Mode', 'valueKind': 5,
                   'defaultValue': 1, 'required': False,
                   'options': [{'value': 0, 'label': 'Auto'}, {'value': 1, 'label': 'Comfort'}]}])
    enums = _find(root, 'Enumeration')
    assert len(enums) == 2, 'enum options must become <Enumeration> entries'
    assert [e.get('Text') for e in enums] == ['Auto', 'Comfort']
    # Without a restriction ETS renders a bare number box and the integrator has
    # to be told out of band what 1 means.
    assert _find(root, 'TypeRestriction'), 'enum must carry a TypeRestriction'


def test_parameter_groups_become_separate_blocks():
    root = _root([
        {'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3, 'defaultValue': 0,
         'required': False, 'group': 'Heating'},
        {'id': 1, 'key': 'b', 'displayName': 'B', 'valueKind': 3, 'defaultValue': 0,
         'required': False, 'group': 'Cooling'},
    ])
    blocks = _find(root, 'ParameterBlock')
    assert [b.get('Name') for b in blocks] == ['Heating', 'Cooling'], \
        'each declared group gets its own labelled block, in declaration order'


def test_ungrouped_parameters_share_one_block():
    root = _root([
        {'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3, 'defaultValue': 0, 'required': False},
        {'id': 1, 'key': 'b', 'displayName': 'B', 'valueKind': 3, 'defaultValue': 0, 'required': False},
    ])
    blocks = _find(root, 'ParameterBlock')
    assert len(blocks) == 1, 'a product that declares no groups must look exactly as before'


def test_visibility_condition_becomes_choose_when():
    root = _root([
        {'id': 0, 'key': 'cool_en', 'displayName': 'Cooling', 'valueKind': 5, 'defaultValue': 1,
         'required': False, 'options': [{'value': 0, 'label': 'Off'}, {'value': 1, 'label': 'On'}]},
        {'id': 1, 'key': 'cool_kp', 'displayName': 'Gain', 'valueKind': 8, 'defaultValue': 25,
         'required': False, 'visibleWhenParameterId': 0, 'visibleWhenValue': 1},
    ])
    chooses = _find(root, 'choose')
    assert len(chooses) == 1, 'a visibility condition must produce one <choose>'
    whens = _find(root, 'when')
    assert whens[0].get('test') == '1'
    # The conditional parameter must sit inside the <when>, not beside it.
    assert whens[0].findall('k:ParameterRefRef', NS), 'conditional parameter belongs inside <when>'


def test_group_objects_are_not_nested_under_a_channel():
    # A <Channel> is a node in the ETS group object tree, so every group address
    # sat one collapsed level deeper than on devices that declare no channels.
    root = _root([{'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3,
                   'defaultValue': 0, 'required': False}])
    assert not _find(root, 'Channel'), 'a single-function product declares no channel'
    top = root.find('.//k:Dynamic/k:ChannelIndependentBlock', NS)
    assert top is not None, 'blocks and object refs hang off a ChannelIndependentBlock'
    assert top.findall('k:ComObjectRefRef', NS), 'group objects must sit at the top level'


def _gated_section(**overrides):
    """An enable switch in one group and a two-parameter section gated on it."""
    gate = {'groupVisibleWhenParameterId': 0, 'groupVisibleWhenValue': 1}
    gate.update(overrides)
    return [
        {'id': 0, 'key': 'cool_en', 'displayName': 'Cooling', 'valueKind': 5, 'defaultValue': 0,
         'required': False, 'group': 'General',
         'options': [{'value': 0, 'label': 'Off'}, {'value': 1, 'label': 'On'}]},
        {'id': 1, 'key': 'cool_algo', 'displayName': 'Algorithm', 'valueKind': 5,
         'defaultValue': 0, 'required': False, 'group': 'Cooling',
         'options': [{'value': 0, 'label': 'Two-point'}, {'value': 1, 'label': 'PI'}], **gate},
        {'id': 2, 'key': 'cool_kp', 'displayName': 'Gain', 'valueKind': 8, 'defaultValue': 25,
         'required': False, 'group': 'Cooling',
         'visibleWhenParameterId': 1, 'visibleWhenValue': 1, **gate},
    ]


def test_gated_group_hides_the_whole_block():
    root = _root(_gated_section())
    # An always-visible block beside a <choose>-wrapped one: ETS drops the whole
    # section while the gate is off, instead of offering an empty heading.
    top = root.find('.//k:Dynamic/k:ChannelIndependentBlock', NS)
    assert len(top.findall('k:ParameterBlock', NS)) == 1, 'only the ungated block stays at top level'
    gated = top.find('k:choose/k:when/k:ParameterBlock', NS)
    assert gated is not None, 'the gated section must live inside <when>, not beside it'
    assert gated.get('Name') == 'Cooling'
    assert top.find('k:choose', NS).get('ParamRefId').endswith('_P-0_R-0')


def test_gated_group_keeps_conditions_on_its_own_parameters():
    # The section gate and a parameter-level condition compose: hiding the
    # section must not swallow the rule that PI terms need the PI algorithm.
    root = _root(_gated_section())
    gated = root.find('.//k:choose/k:when/k:ParameterBlock', NS)
    inner = gated.findall('k:choose', NS)
    assert len(inner) == 1, 'the PI-only parameter keeps its own <choose> inside the section'
    assert inner[0].get('ParamRefId').endswith('_P-1_R-1')
    assert [r.get('RefId').split('_')[-1] for r in gated.findall('k:ParameterRefRef', NS)] == ['R-1']


def test_partially_gated_group_is_refused():
    params = _gated_section()
    del params[2]['groupVisibleWhenParameterId']
    del params[2]['groupVisibleWhenValue']
    try:
        _root(params)
    except ValueError as exc:
        assert 'conflicting section visibility' in str(exc)
    else:
        raise AssertionError('a half-gated section would hide only some of its parameters')


def test_group_gated_on_its_own_member_is_refused():
    # Self-gating produces a section that can never be switched back on.
    params = _gated_section(groupVisibleWhenParameterId=1)
    try:
        _root(params)
    except ValueError as exc:
        assert 'member of that same group' in str(exc)
    else:
        raise AssertionError('a section must not be gated on a parameter it hides')


def test_load_procedure_drives_the_load_state_machine():
    # A bare <LdCtrlConnect/> declared a download that never told the device a
    # download had begun. Unload before allocation matters: the load state
    # machine only accepts a segment allocation from Unloaded, so without it a
    # second download of an already-commissioned device fails.
    root = _root([{'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3,
                   'defaultValue': 0, 'required': False}])
    procedure = _find(root, 'LoadProcedure')[0]
    steps = [child.tag.rsplit('}', 1)[-1] for child in procedure]
    assert steps == ['LdCtrlConnect', 'LdCtrlUnload', 'LdCtrlRelSegment',
                     'LdCtrlWriteRelMem', 'LdCtrlLoadCompleted', 'LdCtrlDisconnect'], steps


def test_load_procedure_size_matches_the_parameter_block():
    # u16 + dpt9 = 4 octets. A wrong size either truncates the download or
    # allocates memory the device does not have.
    root = _root([
        {'id': 0, 'key': 'a', 'displayName': 'A', 'valueKind': 3, 'defaultValue': 0, 'required': False},
        {'id': 1, 'key': 'b', 'displayName': 'B', 'valueKind': 8, 'defaultValue': 0, 'required': False},
    ])
    segment = _find(root, 'LdCtrlRelSegment')[0]
    write = _find(root, 'LdCtrlWriteRelMem')[0]
    assert segment.get('Size') == '4', segment.get('Size')
    assert write.get('Size') == '4'
    assert write.get('Verify') == '1', 'the device answers writes with a read-back; use it'


def test_parameterless_product_omits_the_memory_steps():
    root = _root([])
    procedure = _find(root, 'LoadProcedure')[0]
    steps = [child.tag.rsplit('}', 1)[-1] for child in procedure]
    assert 'LdCtrlRelSegment' not in steps, 'nothing to allocate when there are no parameters'
    assert 'LdCtrlWriteRelMem' not in steps
    assert steps[0] == 'LdCtrlConnect' and steps[-1] == 'LdCtrlDisconnect'


def main():
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith('test_') or not callable(fn):
            continue
        try:
            fn()
            print(f'[PASS] {name}')
        except AssertionError as exc:
            failures += 1
            print(f'[FAIL] {name}: {exc}')
    print(f'\n{"All exporter UX tests passed" if not failures else f"{failures} failed"}')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
