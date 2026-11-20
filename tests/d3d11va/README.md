# D3D11VA H.264 test

This Windows console program submits a progressive H.264 IDR frame and a
dependent P frame through D3D11VA. It queries the decoder profile, NV12 support
and an unencrypted short-slice configuration. It decodes into two NV12 array
slices and checks both planes of each frame after a staging readback.

Build with MinGW from this directory.

```sh
test_dir=$(mktemp -d)
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -static \
  h264.cpp -ld3d11 -o "$test_dir/h264.exe"
```

Use `i686-w64-mingw32-g++` to build the 32-bit version. Copy the matching
DXVK `d3d11.dll` and `dxgi.dll` next to the executable. Run in a private
Wine prefix with native overrides. To test a specific Vulkan driver, set
`VK_DRIVER_FILES` to its ICD manifest.

```sh
WINEPREFIX="$test_dir/prefix" WINEDLLOVERRIDES='d3d11,dxgi=n' wine "$test_dir/h264.exe"
```

On the current baseline, pass `--expect-unsupported` to check that DXVK exposes
no H.264 decoder. In normal mode, a missing profile is a failure.

The fixture is synthetic. Run `python3 generate-fixture.py` to reproduce the
slice bytes and expected NV12 hashes using x264 and FFmpeg. The P frame
references the IDR for 15 of its 16 macroblocks.
