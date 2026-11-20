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

The test creates the device with flags set to zero, as ANGLE does in its
regular D3D11 path. Pass `--video-support` to create the device with
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`.

Pass `--expect-unsupported` to check that DXVK exposes no H.264 decoder.
You can combine it with `--video-support` in either order. In normal mode,
a missing profile is a failure.

Pass `--recover-missing-reference` to test recovery on DXVK. This submits a P
frame with a missing reference after a valid decode and as the first frame on a
fresh decoder. For both cases the test checks that the failed frame leaves its
destination untouched, then verifies the NV12 hashes of a valid IDR and its
dependent P frame on fresh output slices of the same decoder.

Pass `--grow-dpb` to exercise decoded picture buffer growth on the same DXVK
decoder. Each round submits an IDR and P frame, then submits another IDR and P
frame with a larger reference limit on two separate output slices. It checks all
four NV12 hashes after submission and reuses the four slices in the second round.

The fixture is synthetic. Run `python3 generate-fixture.py` to reproduce the
slice bytes and expected NV12 hashes using x264 and FFmpeg. The P frame
references the IDR for 15 of its 16 macroblocks.
