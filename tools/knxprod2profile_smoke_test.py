#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
import pathlib
import subprocess
import sys
import tempfile


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SAMPLE_INPUT = REPO_ROOT / 'tools' / 'examples' / 'sample.knxprod.xml'
GENERATOR = REPO_ROOT / 'tools' / 'knxprod2profile.py'
CHECKER = REPO_ROOT / 'tools' / 'knxprod2profile_check.py'
TEMPLATE = REPO_ROOT / 'templates' / 'profile_header.tpl'


def main():
    with tempfile.TemporaryDirectory(prefix='knxprod2profile_') as temp_dir:
        output = pathlib.Path(temp_dir) / 'generated_product_profile.hpp'

        subprocess.run(
            [
                sys.executable,
                str(GENERATOR),
                str(SAMPLE_INPUT),
                '--output',
                str(output),
                '--template',
                str(TEMPLATE),
            ],
            check=True,
            cwd=REPO_ROOT,
        )

        text = output.read_text(encoding='utf-8')
        required_fragments = [
            'namespace knx::product::pilot_a_tp1_actuator',
            'using DatapointDescriptor = knx::product::BasicDatapointDescriptor<ObjectId>;',
            'internal::StaticProductDefinition<ObjectId, Capacities::kDatapointCount, 0> kDefinition',
            'using DeclarativeProfile = knx::product::internal::BasicProductProfile<',
            'ProductDefinition::kDefinition.datapoints[0].metadata.exportNumber',
            'knx::application::dptids::Switch',
            'StaticExportDescriptor<ProductDefinition::kDefinition.kDatapointCount, 0>',
        ]
        for fragment in required_fragments:
            if fragment not in text:
                raise AssertionError(f'missing generated fragment: {fragment}')

        subprocess.run(
            [sys.executable, str(CHECKER), str(output)],
            check=True,
            cwd=REPO_ROOT,
        )

    print('knxprod2profile smoke test passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())