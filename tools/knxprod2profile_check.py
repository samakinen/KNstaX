#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
import argparse
import os
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('header', help='Header file to check (path)')
    args = parser.parse_args()

    header = args.header
    if not os.path.exists(header):
        print('Header not found:', header)
        return 2

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    tmp = 'tools/_knxprod_check_tmp.cpp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write('#include "' + os.path.basename(header) + '\"\n')
        f.write('int main() { return 0; }\n')

    include_dir = os.path.dirname(header) or '.'
    cmd = ['g++', '-std=c++23', '-fsyntax-only', '-I', include_dir, '-I', os.path.join(repo_root, 'include'), tmp]
    print('Running:', ' '.join(cmd))
    proc = subprocess.run(cmd)
    os.remove(tmp)
    return proc.returncode

if __name__ == '__main__':
    raise SystemExit(main())
