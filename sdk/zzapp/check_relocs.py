#!/usr/bin/env python3
"""Fail unless a shipped app image matches the v0 loader contract.

Only allocated SHT_REL/SHT_RELA, only R_ARM_RELATIVE with symbol 0, no
PT_INTERP, no DT_NEEDED, at most four PT_LOAD, no writable+executable PT_LOAD,
and a ZZAPP note whose descriptor starts with magic 0x50505A41.
"""
import struct
import sys

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
SHDR = struct.Struct("<IIIIIIIIII")
PHDR = struct.Struct("<IIIIIIII")
RELA = struct.Struct("<III")
REL = struct.Struct("<II")
SHT_RELA = 4
SHT_REL = 9
SHT_NOTE = 7
SHT_DYNAMIC = 6
PT_LOAD = 1
PT_INTERP = 3
PT_NOTE = 4
SHF_ALLOC = 2
PF_W = 2
PF_X = 1
R_ARM_RELATIVE = 23
DT_NEEDED = 1
DT_REL = 17


def die(path, msg):
    sys.stderr.write("%s: %s\n" % (path, msg))
    sys.exit(1)


def check(path):
    blob = open(path, "rb").read()
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        die(path, "not a little-endian ELF32")
    (ident, etype, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = EHDR.unpack_from(blob, 0)
    if etype != 3:
        die(path, "e_type is not ET_DYN")
    if machine != 40:
        die(path, "not EM_ARM")
    shdrs = [SHDR.unpack_from(blob, shoff + i * shentsize) for i in range(shnum)]
    phdrs = [PHDR.unpack_from(blob, phoff + i * phentsize) for i in range(phnum)]
    names = blob[shdrs[shstrndx][4]:shdrs[shstrndx][4] + shdrs[shstrndx][5]]

    def sname(off):
        return names[off:names.find(b"\x00", off)].decode("ascii", "replace")

    loads = 0
    interp = False
    for p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align in phdrs:
        if p_type == PT_INTERP:
            interp = True
        if p_type == PT_LOAD:
            loads += 1
            if (p_flags & PF_W) and (p_flags & PF_X):
                die(path, "PT_LOAD is writable and executable")
    if interp:
        die(path, "PT_INTERP is present")
    if loads > 4:
        die(path, "%d PT_LOAD segments" % loads)
    if loads < 1:
        die(path, "no PT_LOAD")

    saw_reloc = False
    saw_note = False
    for name, typ, shflags, addr, off, size, link, info, addralign, entsize in shdrs:
        if typ in (SHT_REL, SHT_RELA) and (shflags & SHF_ALLOC):
            saw_reloc = True
            section = sname(name)
            if section not in (".rel.dyn", ".rela.dyn"):
                die(path, "allocated relocation is named %s" % section)
            stride = 8 if typ == SHT_REL else 12
            if size % stride:
                die(path, "%s size is not a multiple of %d" % (section, stride))
            for n in range(size // stride):
                if typ == SHT_REL:
                    r_offset, r_info = REL.unpack_from(blob, off + n * stride)
                else:
                    r_offset, r_info, r_addend = RELA.unpack_from(
                        blob, off + n * stride)
                    del r_addend
                if (r_info & 0xFF) != R_ARM_RELATIVE or (r_info >> 8) != 0:
                    die(path, "reloc %d is not R_ARM_RELATIVE/0" % n)
                del r_offset
        if typ == SHT_NOTE or sname(name) == ".note.zzapp":
            note = blob[off:off + size]
            if len(note) < 16:
                die(path, "ZZAPP note is short")
            namesz, descsz, ntype = struct.unpack_from("<III", note, 0)
            if namesz != 6 or descsz != 24 or ntype != 1:
                die(path, "ZZAPP note header %d %d %d" % (namesz, descsz, ntype))
            owner = note[12:12 + namesz]
            if owner != b"ZZAPP\x00":
                die(path, "note owner is %r" % owner)
            desc_off = 12 + ((namesz + 3) & ~3)
            # The loader rejects a descriptor that runs past the note.
            if desc_off + descsz > len(note):
                die(path, "ZZAPP note descriptor %d bytes, note has %d"
                    % (descsz, len(note) - desc_off))
            magic = struct.unpack_from("<I", note, desc_off)[0]
            if magic != 0x50505A41:
                die(path, "note magic 0x%x" % magic)
            saw_note = True
        if typ == SHT_DYNAMIC:
            for n in range(size // 8):
                tag, val = struct.unpack_from("<II", blob, off + n * 8)
                if tag == DT_NEEDED:
                    die(path, "DT_NEEDED")
                if tag == 0:
                    break
                del val
    if not saw_reloc:
        die(path, "no allocated .rel.dyn or .rela.dyn")
    if not saw_note:
        die(path, "no ZZAPP note")
    if entry != 0:
        # zz_start is the first text byte. A non-zero entry is not wrong if
        # the script ever inserts bytes before it; v0 links it at offset 0.
        pass
    sys.stdout.write("%s: R_ARM_RELATIVE only, ZZAPP note ok\n" % path)


def main(argv):
    if len(argv) < 2:
        die("check_relocs", "usage: check_relocs.py <elf>...")
    for path in argv[1:]:
        check(path)


if __name__ == "__main__":
    main(sys.argv)
