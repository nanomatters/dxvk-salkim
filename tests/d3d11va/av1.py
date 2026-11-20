#!/usr/bin/env python3
"""Check Windows D3D11VA AV1 decoding against software using synthetic 4:2:0 clips."""

import argparse
import hashlib
import subprocess

COMMON = ["-hide_banner", "-loglevel", "error", "-xerror"]


def run(command, data=None):
    return subprocess.run(command, input=data, stdout=subprocess.PIPE,
                          check=True, timeout=120).stdout


def check_clip(depth, tiles, grain, windows_ffmpeg, lookahead=False):
    planar, output = ("yuv420p10le", "p010le") if depth == 10 else ("yuv420p", "nv12")
    # This lookahead stream fills the decoder's eight logical reference slots
    # while some of their pictures still have to survive the next refresh.
    if lookahead:
        width, height, rate, frames = 510, 286, 30, 48
        encoder = ["-cpu-used", "5", "-crf", "30", "-lag-in-frames", "24", "-auto-alt-ref", "1", "-g", "32"]
    else:
        width, height, rate, frames = 384, 256, 24, 12
        encoder = ["-cpu-used", "6", "-crf", "32", "-lag-in-frames", "0", "-g", "8"]
    source = f"testsrc2=size={width}x{height}:rate={rate},format={planar}"
    grain_args = ["-aom-params", "film-grain-test=1"] if grain else []
    clip = run(["ffmpeg", *COMMON, "-f", "lavfi", "-i", source, "-frames:v", str(frames),
                "-c:v", "libaom-av1", "-threads", "4", "-b:v", "0", *encoder, "-tiles", tiles,
                *grain_args, "-f", "ivf", "pipe:1"])

    inputs = ["-f", "ivf", "-i", "pipe:0"]
    outputs = ["-pix_fmt", output, "-f", "rawvideo", "pipe:1"]
    types = run(["ffprobe", "-v", "error", *inputs, "-show_entries", "frame=pict_type",
                 "-of", "default=nk=1:nw=1"], clip).splitlines()
    if b"I" not in types or b"P" not in types:
        raise RuntimeError("Expected both key and inter pictures")

    expected = run(["ffmpeg", *COMMON, "-hwaccel", "none", *inputs, *outputs], clip)
    # The native AV1 decoder tries D3D11VA. hwdownload requires actual hardware surfaces.
    actual = run([*windows_ffmpeg, *COMMON, "-hwaccel", "d3d11va", "-hwaccel_output_format",
                  "d3d11", "-c:v", "av1", *inputs, "-vf", f"hwdownload,format={output}", *outputs], clip)

    label = f"AV1 Main {depth}-bit, {tiles} tiles" + (", film grain" if grain else "")
    if lookahead:
        label += ", alternate references"
    frame_size = width * height * 3 // 2 * (2 if depth == 10 else 1)
    if len(expected) != frame_size * frames or len(actual) != len(expected):
        raise RuntimeError(f"{label}: expected {frames} frames, got {len(actual)} bytes")
    if depth == 10 and not any(value & 0xc0 for value in expected[::2]):
        raise RuntimeError("P010 reference has no precision beyond 8 bits")
    for i in range(frames):
        bounds = slice(i * frame_size, (i + 1) * frame_size)
        if expected[bounds] != actual[bounds]:
            want = hashlib.sha256(expected[bounds]).hexdigest()
            got = hashlib.sha256(actual[bounds]).hexdigest()
            raise RuntimeError(f"{label}: frame {i} differs, expected {want}, got {got}")
    print(f"{label}: all {frames} frames match software decode", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--depth", type=int, choices=[8, 10])
    parser.add_argument("windows_ffmpeg", nargs="+", help="Windows FFmpeg command, e.g. wine /path/to/ffmpeg.exe")
    args = parser.parse_args()
    for depth in [args.depth] if args.depth else [8, 10]:
        for tiles in ["1x1", "2x2"]:
            check_clip(depth, tiles, False, args.windows_ffmpeg)
        check_clip(depth, "2x2", True, args.windows_ffmpeg)
    if args.depth != 10:
        check_clip(8, "1x1", False, args.windows_ffmpeg, lookahead=True)
