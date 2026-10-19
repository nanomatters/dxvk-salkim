#!/usr/bin/env python3
"""Test the production FPS sampler without a Vulkan device."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

path = Path(__file__).resolve().parent
root = path.parents[2]
definition = runpy.run_path(str(path.parent / 'presenter/run.py'))['definition']
header = (root / 'src/dxvk/hud/dxvk_hud_item.h').read_text()
source = (root / 'src/dxvk/hud/dxvk_hud_item.cpp').read_text()
production = definition(header, r'  class HudFpsData : public RcObject') + ';\n'
production += definition(source, r'  void HudFpsData::update\(')
with tempfile.TemporaryDirectory(prefix='dxvk-hud-tests-') as temporary:
    output = Path(temporary)
    (output / 'fps.h').write_text(production)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(output), str(path / 'fps.cpp'),
                    '-o', str(output / 'fps')], check=True)
    subprocess.run([str(output / 'fps')], check=True)
