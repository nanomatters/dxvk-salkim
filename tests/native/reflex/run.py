#!/usr/bin/env python3
"""Test production Reflex sleep and frame reports without a Vulkan device."""

import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile


directory = Path(__file__).resolve().parent
root = directory.parents[2]
definition = runpy.run_path(str(directory.parent / 'presenter/run.py'))['definition']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Read production methods from a Git revision')
parser.add_argument('--case', choices=('sleep', 'reports', 'stats', 'all'), default='all')
args = parser.parse_args()


def read(name):
    if args.revision:
        return subprocess.check_output(['git', 'show', f'{args.revision}:{name}'],
                                       cwd=root, text=True)
    return (root / name).read_text()


presenter = read('src/dxvk/dxvk_presenter.cpp')
device = read('src/d3d11/d3d11_device.cpp')
interfaces = (root / 'src/dxgi/dxgi_interfaces.h').read_text()
production = definition(presenter, r'  void Presenter::latencySleep\(')
production += '\n' + definition(presenter, r'  PresenterLatencyInfo Presenter::queryLatencyStats\(')
production += '\n' + definition(device, r'  HRESULT STDMETHODCALLTYPE D3D11ReflexDevice::GetLatencyInfo\(')
reports = '\n'.join(definition(interfaces, pattern) + ';' for pattern in (
    r'struct D3D_LOW_LATENCY_FRAME_REPORT\b', r'struct D3D_LOW_LATENCY_RESULTS\b'))

with tempfile.TemporaryDirectory(prefix='dxvk-reflex-tests-') as temporary:
    output = Path(temporary)
    (output / 'production.h').write_text(production)
    (output / 'reports.h').write_text(reports)
    (output / 'stats.h').write_text(definition(read('src/dxvk/dxvk_presenter.h'),
        r'  struct PresenterLatencyInfo\b') + ';\n')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(output), '-I', str(root / 'include/vulkan/include'),
                    str(directory / 'reflex.cpp'), '-o', str(output / 'reflex')], check=True)
    subprocess.run([str(output / 'reflex'), args.case], check=True, timeout=15)
