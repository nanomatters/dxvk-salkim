#!/usr/bin/env python3
"""Print the synthetic fixture's H.264 slice bytes and software NV12 hashes."""
import re
import subprocess
import tempfile
from pathlib import Path


def plane(size, values):
    block = size // 4
    return bytes(values[y // block * 4 + x // block]
                 for y in range(size) for x in range(size))


def frame(luma, chroma_u, chroma_v):
    return plane(64, luma) + plane(32, chroma_u) + plane(32, chroma_v)


def fnv1a32(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def main():
    luma = [40, 65, 94, 118, 52, 78, 107, 133, 73, 100, 134, 167, 104, 130, 162, 197]
    chroma_u = [160, 144, 122, 104, 157, 133, 109, 89, 128, 109, 91, 73, 103, 86, 72, 56]
    chroma_v = [82, 98, 113, 134, 93, 113, 133, 153, 111, 132, 155, 177, 135, 154, 181, 206]

    source = frame(luma, chroma_u, chroma_v)
    luma[-1], chroma_u[-1], chroma_v[-1] = 151, 102, 156
    source += frame(luma, chroma_u, chroma_v)

    with tempfile.TemporaryDirectory(prefix="dxvk-h264-fixture-") as directory:
        root = Path(directory)
        (root / "source.i420").write_bytes(source)
        subprocess.run([
            "x264", "--input-res", "64x64", "--input-csp", "i420", "--input-depth", "8",
            "--output-depth", "8", "--fps", "1", "--profile", "high", "--level", "3.0",
            "--preset", "medium", "--qp", "20", "--ref", "1", "--bframes", "0", "--no-weightb",
            "--weightp", "0", "--no-deblock", "--scenecut", "0", "--keyint", "250",
            "--min-keyint", "1", "--8x8dct", "--threads", "1", "--slices", "1", "--no-asm",
            "--output", "fixture.h264", "source.i420",
        ], cwd=root, check=True)
        reference = subprocess.check_output([
            "ffmpeg", "-v", "error", "-xerror", "-i", "fixture.h264", "-pix_fmt", "nv12",
            "-f", "rawvideo", "-",
        ], cwd=root)
        stream = (root / "fixture.h264").read_bytes()

    nals = re.split(rb"\x00\x00\x00?\x01", stream)
    slices = [nal for nal in nals if nal and nal[0] & 31 in (1, 5)]
    assert [nal[0] & 31 for nal in slices] == [5, 1]
    frame_size = 64 * 64 * 3 // 2
    assert len(reference) == frame_size * len(slices)

    for i, nal in enumerate(slices):
        data = b"\x00\x00\x01" + nal
        assert len(data) <= 128
        pixels = reference[i * frame_size:(i + 1) * frame_size]
        print(f"Slice {i}, {len(data)} bytes, NV12 FNV-1a32 0x{fnv1a32(pixels):08x}")
        for row in range(0, len(data), 12):
            print("  " + ", ".join(f"0x{byte:02x}" for byte in data[row:row+12]) + ",")


if __name__ == "__main__":
    main()
