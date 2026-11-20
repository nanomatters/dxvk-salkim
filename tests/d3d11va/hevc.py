#!/usr/bin/env python3
"""Compare HEVC frames from a Windows D3D11VA decoder against FFmpeg's software decoder."""

import argparse
import hashlib
import subprocess
from pathlib import Path

WIDTH, HEIGHT, FRAMES = 318, 178, 24


def run(command, data=None):
    return subprocess.run(command, input=data, stdout=subprocess.PIPE,
                          check=True, timeout=120).stdout


def generate_clip(profile):
    ten_bit = profile == "main10"
    planar = "yuv420p10le" if ten_bit else "yuv420p"
    source = f"testsrc2=size={WIDTH}x{HEIGHT}:rate=24,format={planar}"
    common = ["-hide_banner", "-loglevel", "error", "-xerror"]

    # Exercise both the single-slice path and decoding multiple slices with scaling lists.
    params = "frame-threads=1:keyint=12:scenecut=0:open-gop=0:bframes=3:b-adapt=0"
    params += ":pools=2:wpp=1:slices=2:scaling-list=default" if ten_bit else ":pools=none:temporal-layers=2"

    return run(["ffmpeg", *common, "-f", "lavfi", "-i", source,
                "-frames:v", str(FRAMES), "-c:v", "libx265", "-profile:v", profile,
                "-x265-params", "log-level=error:" + params, "-f", "hevc", "pipe:1"])


def compare_frames(label, output, width, height, frame_count, clip, windows_ffmpeg):
    ten_bit = output == "p010le"
    common = ["-hide_banner", "-loglevel", "error", "-xerror"]
    types = run(["ffprobe", "-v", "error", "-f", "hevc", "-i", "pipe:0",
                 "-show_entries", "frame=pict_type", "-of", "default=nk=1:nw=1"], clip)
    if not {b"I", b"P", b"B"}.issubset(types.splitlines()):
        raise RuntimeError(f"{label}: expected I, P and B pictures")

    input_args = ["-f", "hevc", "-i", "pipe:0"]
    output_args = ["-pix_fmt", output, "-f", "rawvideo", "pipe:1"]
    expected = run(["ffmpeg", *common, "-hwaccel", "none", *input_args, *output_args], clip)
    # hwdownload requires actual hardware surfaces. A software fallback must fail the test.
    actual = run([*windows_ffmpeg, *common, "-hwaccel", "d3d11va",
                  "-hwaccel_output_format", "d3d11", *input_args,
                  "-vf", f"hwdownload,format={output}", *output_args], clip)

    frame_size = width * height * 3 // 2 * (2 if ten_bit else 1)
    if len(actual) != frame_size * frame_count or len(expected) != len(actual):
        raise RuntimeError(f"{label}: incomplete output ({len(expected)} reference, {len(actual)} decoded bytes)")
    if ten_bit and not any(byte & 0xc0 for byte in expected[::2]):
        raise RuntimeError("Main 10 reference has no precision beyond 8 bits")
    for i in range(frame_count):
        start = i * frame_size
        ref = expected[start:start + frame_size]
        got = actual[start:start + frame_size]
        if got != ref:
            raise RuntimeError(f"{label}: frame {i} differs, expected {hashlib.sha256(ref).hexdigest()}, "
                               f"got {hashlib.sha256(got).hexdigest()}")
    print(f"{label}: all {frame_count} {output} I/P/B frames match software decode")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=["main", "main10", "both"], default="both")
    parser.add_argument("windows_ffmpeg", nargs="+", help="Windows FFmpeg command, e.g. wine /path/to/ffmpeg.exe")
    args = parser.parse_args()
    for profile in ("main", "main10") if args.profile == "both" else (args.profile,):
        output = "p010le" if profile == "main10" else "nv12"
        compare_frames(profile, output, WIDTH, HEIGHT, FRAMES, generate_clip(profile), args.windows_ffmpeg)
        if profile == "main":
            clip = Path(__file__).with_name("unused-sps-rps.hevc").read_bytes()
            compare_frames("unused SPS reference set", output, 160, 96, 5, clip, args.windows_ffmpeg)
            clip = Path(__file__).with_name("cra-dpb-growth.hevc").read_bytes()
            compare_frames("CRA after SPS change", output, 128, 96, 4, clip, args.windows_ffmpeg)
