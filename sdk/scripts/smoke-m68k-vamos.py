#!/usr/bin/env python3
"""Run packaged m68k CLI binaries under Vamos and reject startup CPU faults.

Usage:
  python smoke-m68k-vamos.py <package-dir> [--extra <binary>]...

Run after package-m68k-amigaos.sh. The package must contain C/. Every regular
file in C/ except .info files is run without arguments. --extra adds binaries
outside C/, for example ab/ZZPlay.nolra. Normal Amiga library-load failures
are allowed because this smoke only rejects a startup CPU fault.
"""

import argparse
from pathlib import Path
import subprocess
import sys


CPU_ERROR_MARKERS = (
    "error in cpu run",
    "invalidmemoryaccesserror",
    "guru",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_dir", type=Path)
    parser.add_argument("--extra", action="append", default=[], type=Path)
    parser.add_argument("--vamos", default="vamos.EXE")
    parser.add_argument("--timeout", type=float, default=20.0)
    return parser.parse_args()


def run_binary(vamos: str, binary: Path, timeout: float) -> tuple[bool, str]:
    try:
        result = subprocess.run(
            [vamos, "-c", "", "--", binary.name],
            cwd=binary.parent,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or "") + (error.stderr or "")
        return False, f"timed out after {timeout:g}s\n{output}"
    output = result.stdout + result.stderr
    if any(marker in output.lower() for marker in CPU_ERROR_MARKERS):
        return False, output
    return True, output


def main() -> int:
    args = parse_args()
    package_dir = args.package_dir.resolve()
    c_dir = package_dir / "C"
    if not c_dir.is_dir():
        raise SystemExit(f"missing package C directory: {c_dir}")
    binaries = {path.resolve() for path in c_dir.iterdir()
                if path.is_file() and path.suffix.lower() != ".info"}
    binaries.update(path.resolve() for path in args.extra)
    binaries = sorted(binaries)
    if not binaries:
        raise SystemExit("no binaries to smoke")
    failures = []
    for binary in binaries:
        if not binary.is_file():
            failures.append((binary, "missing binary"))
            continue
        ok, output = run_binary(args.vamos, binary, args.timeout)
        if ok:
            print(f"PASS {binary.name}")
        else:
            print(f"FAIL {binary.name}")
            failures.append((binary, output))
    if failures:
        for binary, output in failures:
            print(f"\n--- {binary} ---\n{output}", file=sys.stderr)
        return 1
    print(f"PASS {len(binaries)} binaries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
