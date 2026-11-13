#!/usr/bin/env python3
"""Test production D3D9 monitor lookup without Windows or a Vulkan device."""

import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile


directory = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=directory / '../../..',
                    help='DXVK source directory')
parser.add_argument('--revision', help='Read production source from this Git revision')
sanitizer = parser.add_mutually_exclusive_group()
sanitizer.add_argument('--sanitize', action='store_true')
sanitizer.add_argument('--thread-sanitize', action='store_true')
args = parser.parse_args()
root = args.source_root.resolve()
definition = runpy.run_path(str(root / 'tests/native/presenter/run.py'))['definition']
name = 'src/d3d9/d3d9_adapter.cpp'
if args.revision:
    source = subprocess.check_output(['git', 'show', f'{args.revision}:{name}'],
                                    cwd=root, text=True)
else:
    source = (root / name).read_text()
production = definition(source, r'  HMONITOR D3D9Adapter::GetMonitor\(')

with tempfile.TemporaryDirectory(prefix='dxvk-d3d9-monitor-') as temporary:
    output = Path(temporary)
    (output / 'monitor.h').write_text(production)
    command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
               '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(output),
               str(directory / 'monitor.cpp'), '-o', str(output / 'monitor')]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.thread_sanitize:
        command += ['-fsanitize=thread', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True)
    subprocess.run([str(output / 'monitor')], check=True, timeout=30)
