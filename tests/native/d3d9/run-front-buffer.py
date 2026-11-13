#!/usr/bin/env python3
"""Test production D3D9 front-buffer selection without a Vulkan device."""

import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Read production source from this Git revision')
args = parser.parse_args()
directory = Path(__file__).resolve().parent
root = directory.parents[2]
definition = runpy.run_path(str(directory.parent / 'presenter/run.py'))['definition']


def read(name):
    if args.revision:
        return subprocess.check_output(['git', 'show', f'{args.revision}:{name}'],
                                       cwd=root, text=True)
    return (root / name).read_text()


source = read('src/d3d9/d3d9_device.cpp')
header = read('src/d3d9/d3d9_device.h')
production = '\n'.join(definition(source, pattern) for pattern in (
    r'  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFrontBufferData\(',
    r'  D3D9SwapChainEx\* D3D9DeviceEx::GetHeadSwapChain\('))
setter = definition(header, r'    void SetMostRecentlyUsedSwapchain\(')
with tempfile.TemporaryDirectory(prefix='dxvk-front-buffer-tests-') as temporary:
    output = Path(temporary)
    (output / 'production.h').write_text(production)
    (output / 'setter.h').write_text(setter)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(output),
                    str(directory / 'front-buffer.cpp'), '-o', str(output / 'front-buffer')],
                   check=True)
    subprocess.run([str(output / 'front-buffer')], check=True, timeout=15)
