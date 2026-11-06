#!/usr/bin/env python3
"""Feed known-bad ELFs to zzapp-check. No pytest."""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "zzapp"))
CHECK = os.path.join(ROOT, "tools", "zzapp-check")
NOTE_MAGIC = 0x50505A41


def note_bytes(flags, stack, magic=NOTE_MAGIC, abi_major=0):
    name = b"ZZAPP\x00\x00\x00"
    desc = struct.pack("<IIIIII", magic, abi_major, 0, stack, flags, 0)
    return struct.pack("<III", 6, 24, 1) + name + desc


def _shdr(name, typ, flags, addr, off, size, align, entsize):
    return struct.pack("<IIIIIIIIII", name, typ, flags, addr, off, size, 0, 0,
                       align, entsize)


def elf(flags=0, stack=0, reloc_info=23, include_note=True, magic=NOTE_MAGIC,
        abi_major=0):
    note = note_bytes(flags, stack, magic, abi_major)
    phnum = 2 if include_note else 1
    phoff = 52
    cursor = phoff + phnum * 32
    note_off = cursor
    if include_note:
        cursor += len(note)
    rel_off = cursor
    rel = struct.pack("<II", rel_off, reloc_info)
    cursor += len(rel)
    shstr_off = cursor
    shstr = b"\x00.rel.dyn\x00.note.zzapp\x00.shstrtab\x00"
    cursor += len(shstr)
    shoff = (cursor + 3) & ~3
    shnum = 4 if include_note else 3
    shstrndx = shnum - 1
    blob = bytearray(shoff + shnum * 40)
    blob[0:4] = b"\x7fELF"
    blob[4] = 1
    blob[5] = 1
    blob[6] = 1
    struct.pack_into("<HHIIIIIHHHHHH", blob, 16,
                     3, 40, 1, 0, phoff, shoff, 0x400,
                     52, 32, phnum, 40, shnum, shstrndx)
    struct.pack_into("<IIIIIIII", blob, phoff,
                     1, rel_off, rel_off, rel_off, 8, 8, 5, 4)
    if include_note:
        struct.pack_into("<IIIIIIII", blob, phoff + 32,
                         4, note_off, note_off, note_off, len(note), len(note),
                         4, 4)
        blob[note_off:note_off + len(note)] = note
    blob[rel_off:rel_off + len(rel)] = rel
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    shdrs = [
        _shdr(0, 0, 0, 0, 0, 0, 0, 0),
        _shdr(1, 9, 2, rel_off, rel_off, 8, 4, 8),
    ]
    if include_note:
        shdrs.append(_shdr(10, 7, 2, note_off, note_off, len(note), 4, 0))
    shdrs.append(_shdr(22, 3, 0, 0, shstr_off, len(shstr), 1, 0))
    blob[shoff:shoff + shnum * 40] = b"".join(shdrs)
    return bytes(blob)


def section(blob, want):
    if blob[:4] != b"\x7fELF":
        raise SystemExit("not an ELF")
    (etype, machine, version, entry, phoff, shoff, flags, ehsize, phentsize,
     phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH",
                                                            blob, 16)
    del etype, machine, version, entry, phoff, flags, ehsize, phentsize, phnum
    shdrs = [struct.unpack_from("<IIIIIIIIII", blob, shoff + i * shentsize)
             for i in range(shnum)]
    names_off, names_size = shdrs[shstrndx][4], shdrs[shstrndx][5]
    names = blob[names_off:names_off + names_size]
    for name, typ, shflags, addr, off, size, link, info, align, entsize in shdrs:
        del typ, shflags, addr, link, info, align, entsize
        end = names.find(b"\x00", name)
        if names[name:end] == want:
            return blob[off:off + size]
    raise SystemExit("section %s missing" % want)


def run_check(blob):
    fd, path = tempfile.mkstemp(suffix=".elf")
    try:
        os.write(fd, blob)
        os.close(fd)
        proc = subprocess.run([sys.executable, CHECK, path],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        return proc.returncode, proc.stdout, proc.stderr
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def expect_fail(blob, needle):
    code, out, err = run_check(blob)
    if code == 0 or needle not in err:
        raise SystemExit("expected failure containing %r\ncode=%s\nstdout=%s\nstderr=%s"
                         % (needle, code, out, err))


def expect_ok(blob):
    code, out, err = run_check(blob)
    if code != 0 or "ZZAPP note ok" not in out:
        raise SystemExit("expected success\ncode=%s\nstdout=%s\nstderr=%s"
                         % (code, out, err))


def unit_tests():
    if not os.path.isfile(CHECK):
        raise SystemExit("missing %s" % CHECK)
    expect_ok(elf())
    expect_ok(elf(flags=7, stack=1 << 20))
    expect_ok(elf(flags=1, stack=64 << 10))
    expect_fail(elf(reloc_info=2), "reloc 0 is not R_ARM_RELATIVE/0")
    expect_fail(elf(include_note=False), "no ZZAPP note")
    expect_fail(elf(magic=0x11111111), "note magic")
    expect_fail(elf(abi_major=1), "note abi major 1 is not 0")
    expect_fail(elf(flags=0x8), "note flags 0x8 has unknown bits")
    expect_fail(elf(stack=4), "note stack 4 is not 0")
    expect_fail(elf(stack=(1 << 20) + 8), "note stack")
    code, out, err = run_check(b"not an elf")
    if code == 0 or "not a little-endian ELF32" not in err:
        raise SystemExit("expected a non-ELF rejection\n%s" % err)
    del out
    print("zzapp-check unit tests passed")


def expect_note(path, flags, stack):
    blob = open(path, "rb").read()
    got = section(blob, b".note.zzapp")
    want = note_bytes(flags, stack)
    if got != want:
        raise SystemExit("%s note mismatch\n got %s\nwant %s"
                         % (path, got.hex(), want.hex()))
    print("%s: .note.zzapp matches flags=%d stack=%d" % (path, flags, stack))


def main(argv):
    if len(argv) >= 2 and argv[1] == "--expect-note":
        if len(argv) != 5:
            raise SystemExit("usage: zzapp_check_test.py --expect-note ELF FLAGS STACK")
        expect_note(argv[2], int(argv[3], 0), int(argv[4], 0))
        return
    if len(argv) != 1:
        raise SystemExit("usage: zzapp_check_test.py [--expect-note ELF FLAGS STACK]")
    unit_tests()


if __name__ == "__main__":
    main(sys.argv)
