#!/usr/bin/env python3
"""Test HUD sampling and composition without a Vulkan device."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Read production source from this Git revision')
args = parser.parse_args()
path = Path(__file__).resolve().parent
root = path.parents[2]
definition = runpy.run_path(str(path.parent / 'presenter/run.py'))['definition']


def read(name):
    if args.revision:
        return subprocess.check_output(['git', 'show', f'{args.revision}:{name}'],
                                       cwd=root, text=True)
    return (root / name).read_text()


header = read('src/dxvk/hud/dxvk_hud_item.h')
source = read('src/dxvk/hud/dxvk_hud_item.cpp')
production = definition(header, r'  class HudFpsData : public RcObject') + ';\n'
production += definition(source, r'  void HudFpsData::update\(')
blitter = read('src/dxvk/dxvk_swapchain_blitter.cpp')
composition = '\n'.join(definition(blitter, pattern) for pattern in (
    r'  void DxvkSwapchainBlitter::createHudImage\(',
    r'  void DxvkSwapchainBlitter::destroyHudImage\(',
    r'  bool DxvkSwapchainBlitter::needsComposition\('))
with tempfile.TemporaryDirectory(prefix='dxvk-hud-tests-') as temporary:
    output = Path(temporary)
    for name, code in [('fps', production), ('composition', composition)]:
        (output / f'{name}.h').write_text(code)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(output),
                        '-I', str(root / 'include/vulkan/include'), str(path / f'{name}.cpp'),
                        '-o', str(output / name)], check=True)
        subprocess.run([str(output / name)], check=True)
