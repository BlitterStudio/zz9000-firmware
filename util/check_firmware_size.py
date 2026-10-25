#!/usr/bin/env python3
"""Fail a firmware build when a linker-managed DDR region exceeds its budget."""
import argparse
import pathlib
import re
import subprocess
import sys

REGIONS = {
    "low vectors/MMU": (0x00100000, 0x001FC000),
    "high image": (0x08200000, 0x08600000),
    "newlib heap/stacks": (0x08800000, 0x09000000),
    "boot ROM image": (0x3FEF0000, 0x3FEF2000),
}
SECTION = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)")


def parse_sections(objdump, elf):
    output = subprocess.check_output([objdump, "-h", str(elf)], text=True)
    result = []
    for line in output.splitlines():
        match = SECTION.match(line)
        if match:
            result.append((match.group(1), int(match.group(2), 16), int(match.group(3), 16)))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("boot_bin", type=pathlib.Path)
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = parser.parse_args()
    if not args.elf.is_file() or not args.boot_bin.is_file():
        raise SystemExit("firmware size gate needs the built ELF and BOOT.bin")

    usage = {name: 0 for name in REGIONS}
    for section, size, address in parse_sections(args.objdump, args.elf):
        if size == 0:
            continue
        end = address + size
        for name, (base, limit) in REGIONS.items():
            if base <= address and end <= limit:
                usage[name] += size
                break
        else:
            if address < 0x40000000:
                print(f"unbudgeted DDR section: {section} 0x{address:08x}-0x{end:08x}", file=sys.stderr)
                return 1

    failed = False
    for name, (base, limit) in REGIONS.items():
        budget = limit - base
        used = usage[name]
        free = budget - used
        print(f"{name:20} used {used:8d} / {budget:8d} bytes; free {free:8d}")
        if free < 0:
            failed = True
    print(f"BOOT.bin             size {args.boot_bin.stat().st_size:8d} bytes")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
