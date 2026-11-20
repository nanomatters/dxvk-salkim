#!/usr/bin/env python3
"""Decode DXVK's reconstructed AV1 headers and tiles with dav1d."""

import os
from pathlib import Path
import subprocess
import tempfile

TEST_DIR = Path(__file__).resolve().parent
ROOT = TEST_DIR.parents[1]


def decode(path):
    return subprocess.check_output(["ffmpeg", "-nostdin", "-v", "error", "-xerror",
                                    "-c:v", "libdav1d", "-i", str(path),
                                    "-pix_fmt", "nv12", "-f", "rawvideo", "pipe:1"])


def main():
    with tempfile.TemporaryDirectory() as tmp:
        replay = Path(tmp) / "replay"
        rebuilt = Path(tmp) / "rebuilt.obu"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Isrc/d3d11",
                        "-Iinclude", "-Iinclude/vulkan/include", "-Iinclude/native/windows",
                        "-Iinclude/native/directx", TEST_DIR / "av1-headers.cpp",
                        "src/d3d11/d3d11_video_av1.cpp", "-o", replay], cwd=ROOT, check=True)
        with (TEST_DIR / "av1-headers.dxva").open("rb") as inputs, rebuilt.open("wb") as output:
            subprocess.run([replay], stdin=inputs, stdout=output, check=True)

        expected = decode(TEST_DIR / "av1-headers.ivf")
        actual = decode(rebuilt)
        frame_size = 256 * 128 * 3 // 2
        if len(expected) != 3 * frame_size or len(actual) != len(expected):
            raise RuntimeError("Expected three complete 256x128 NV12 pictures")
        for i in range(3):
            frame = slice(i * frame_size, (i + 1) * frame_size)
            if actual[frame] != expected[frame]:
                raise RuntimeError(f"Reconstructed AV1 picture {i} differs from the original")
        print("All three reconstructed AV1 pictures match the original with dav1d")


if __name__ == "__main__":
    main()
