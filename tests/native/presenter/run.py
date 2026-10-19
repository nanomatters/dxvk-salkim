#!/usr/bin/env python3
"""Compile the production presenter worker with deterministic Vulkan dispatch.

Run from any directory. --revision REV tests historical source without changing
the checkout. No Vulkan loader, GPU or DXVK build is required.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def definition(source, pattern):
    match = re.search(pattern, source)
    if not match:
        raise RuntimeError(f"Production definition not found: {pattern}")
    opening = source.index("{", match.end())
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():token.end()]
    raise RuntimeError("Unterminated production definition")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=("mode", "all"), default="all")
    parser.add_argument("--revision", help="Read production source from this Git revision")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = directory.parents[2]

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=root, text=True)
        return (root / path).read_text()

    source = read("src/dxvk/dxvk_presenter.cpp")
    header = read("src/dxvk/dxvk_presenter.h")
    frame = definition(header, r"  struct PresenterFrame\b") + ";\n"
    worker = definition(source, r"  void Presenter::runFrameThread\(\)")
    wait = definition(source, r"  bool Presenter::waitUntilFrameTargetTime\(")

    with tempfile.TemporaryDirectory(prefix="dxvk-presenter-tests-") as temporary:
        temporary = Path(temporary)
        (temporary / "frame.h").write_text(frame)
        (temporary / "worker.h").write_text(worker + "\n" + wait + "\n")
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                   "-pthread", "-I", str(temporary),
                   "-I", str(root / "include/vulkan/include"), str(directory / "presenter.cpp"),
                   "-o", str(temporary / "presenter")]
        if args.sanitize:
            command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run(command, check=True)
        subprocess.run([str(temporary / "presenter"), args.case], check=True)


if __name__ == "__main__":
    main()
