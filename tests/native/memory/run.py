#!/usr/bin/env python3
"""Test production allocator synchronization without a Vulkan device."""

import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="Read production source from this Git revision")
    parser.add_argument("--source", type=Path, help="Read production source from another tree")
    parser.add_argument("--sanitize", choices=("address", "thread"))
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = directory.parents[2]
    definition = runpy.run_path(str(directory.parent / "presenter/run.py"))["definition"]

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=root, text=True)
        return ((args.source or root) / path).read_text()

    source = read("src/dxvk/dxvk_memory.cpp")
    production = "\n".join(definition(source, pattern) for pattern in (
        r"  void DxvkMemoryAllocator::evictResources\(",
        r"  void DxvkMemoryAllocator::moveDefragChunk\(",
        r"  void DxvkMemoryAllocator::registerResource\(",
        r"  void DxvkMemoryAllocator::unregisterResource\(",
    ))

    with tempfile.TemporaryDirectory(prefix="dxvk-memory-tests-") as temporary:
        output = Path(temporary)
        (output / "production.h").write_text(production)
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(output),
                   str(directory / "eviction.cpp"), "-o", str(output / "eviction")]
        if args.sanitize:
            sanitizer = "address,undefined" if args.sanitize == "address" else "thread"
            command.extend([f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer"])
        subprocess.run(command, check=True)
        subprocess.run([str(output / "eviction")], check=True, timeout=30)


if __name__ == "__main__":
    main()
