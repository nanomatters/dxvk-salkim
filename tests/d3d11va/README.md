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

## HEVC

Build `hevc-caps.cpp` with the same MinGW flags as `h264.cpp`. Pass each profile
you expect the selected driver to expose, e.g. `hevc-caps.exe main main10`.
With no arguments the test expects neither profile. It checks profile enumeration,
output formats, configurations and decoder creation independently for each profile.

`hevc.py` generates two progressive HEVC clips with libx265. Each contains
I, P and B pictures and dimensions that require cropping. Main uses two temporal
layers. Main 10 uses two slices per picture and enables scaling lists. The fixtures
signal their short-term reference sets in slice headers. The Main test also plays a five-frame
NVENC fixture whose SPS declares one short-term reference set. All four inter
pictures define their own sets in their slice headers. DXVA omits the contents
of SPS reference tables, so pictures that select an SPS short-term set or use it
for inter-set prediction are unsupported. An SPS with long-term reference
pictures is also unsupported. The test runs the
Windows FFmpeg D3D11VA client, requires hardware surfaces, and compares every
visible byte of every NV12 and P010 frame against FFmpeg software decoding.
Install host FFmpeg with libx265 and ffprobe, and put the DXVK DLLs next to a
Windows FFmpeg executable built with D3D11VA.

```sh
WINEDLLOVERRIDES='d3d11,dxgi=n' python3 hevc.py wine /path/to/ffmpeg.exe
```

Use `--profile main` or `--profile main10` before the Wine command to test only
the indicated profile.

The NVENC fixture is included so running this test only requires a compatible
decoder. To regenerate it from FFmpeg's synthetic test pattern on an encoder
that supports NVENC, run the following command in this directory.

```sh
ffmpeg -f lavfi -i testsrc2=size=160x96:rate=30 -frames:v 5 -c:v hevc_nvenc \
  -preset p4 -profile:v main -bf 3 -g 30 -rc constqp -qp 35 -f hevc unused-sps-rps.hevc
```

When regenerating the clip, check FFmpeg's `trace_headers` output. The SPS
must have `num_short_term_ref_pic_sets = 1`. Each non-IDR slice must have
`short_term_ref_pic_set_sps_flag = 0` and `inter_ref_pic_set_prediction_flag = 0`.

`cra-dpb-growth.hevc` contains four pictures from a synthetic x265 Main stream.
The initial IDR picture uses an SPS with a two-picture decoded picture buffer.
The CRA picture switches to an SPS with a four-picture buffer and an empty
reference set. A P picture and a B picture follow in decode order. Comparing
these pictures with software decoding checks playback across the SPS change.
Windows FFmpeg creates a new D3D11VA decoder when the SPS changes.

For the same SPS change on a single D3D11 decoder, build and run the direct
test from this directory with the matching DXVK DLLs next to the executable.

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -static \
  hevc-dpb.cpp -ld3d11 -o "$test_dir/hevc-dpb.exe"
WINEDLLOVERRIDES='d3d11,dxgi=n' wine "$test_dir/hevc-dpb.exe" cra-dpb-growth.hevc
```

Each run submits IDR, CRA, P and B pictures on one decoder, then repeats on a
second decoder with a BLA_N_LP in place of the CRA. Both random-access
pictures have an empty reference set. The final B picture needs both new
references. The test checks all four NV12 output hashes against software in
each case, including the IDR submitted before the reference buffer grew.

## AV1

`av1.py` compares Windows FFmpeg's D3D11VA output with software decoding,
byte for byte. It exercises AV1 Main in NV12 and P010, single-tile and
multi-tile pictures, and film-grain synthesis. It also checks a stream with
lookahead and alternate reference frames. The hardware path uses
`hwdownload` so a software fallback fails the test.

Use host FFmpeg with libaom and an AV1 software decoder. Put the matching
DXVK DLLs next to the Windows FFmpeg executable, as for HEVC.

```sh
WINEDLLOVERRIDES='d3d11,dxgi=n' python3 av1.py wine /path/to/ffmpeg.exe
```

Pass `--depth 8` or `--depth 10` before the Wine command to test one
output format. D3D11 uses the same AV1 Profile 0 GUID for both, and
`CheckVideoDecoderFormat` reports the available depths individually.
