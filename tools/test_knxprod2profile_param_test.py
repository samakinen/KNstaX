# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

import os
import subprocess


def test_generator_emits_prop_type_literal(tmp_path):
    repo_root = os.path.dirname(os.path.dirname(__file__))
    input_knxprod = os.path.join(repo_root, 'tools', 'examples', 'sample_with_params.knxprod.xml')
    template = os.path.join(repo_root, 'templates', 'profile_header.tpl')
    dpt_catalog = os.path.join(repo_root, 'include', 'knx', 'application', 'dpt_catalog.inc')
    pdt_catalog = os.path.join(repo_root, 'include', 'knx', 'application', 'pdt_catalog.inc')
    out_path = tmp_path / 'generated_product_profile.hpp'

    cmd = [
        'python3',
        os.path.join(repo_root, 'tools', 'knxprod2profile.py'),
        input_knxprod,
        '--output', str(out_path),
        '--dpt-catalog', dpt_catalog,
        '--pdt-catalog', pdt_catalog,
        '--template', template,
    ]
    subprocess.check_call(cmd)

    text = out_path.read_text(encoding='utf-8')

    # Expect at least one makeExportParameterDescriptor invocation with a propType argument
    assert 'makeExportParameterDescriptor' in text
    # Prop type literal should appear as either knx::application::PropertyDataType:: or a static_cast
    assert 'PropertyDataType::' in text or 'static_cast<knx::application::PropertyDataType>' in text
