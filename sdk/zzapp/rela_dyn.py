#!/usr/bin/env python3
"""Rewrite allocated SHT_REL into an allocated SHT_RELA .rela.dyn.

Arm GNU ld 13.2 emits .rel.dyn (SHT_REL). The loader accepts SHT_RELA only and
applies R_ARM_RELATIVE as place = r_addend + load_bias. The REL addend is the
word already stored at the place; this copies it into r_addend.

Existing PT_LOAD bytes are not shifted. The new section is a new PT_LOAD
(RX, align 4) appended to the file. DT_REL* tags in .dynamic are rewritten
to DT_RELA* in place. Any reloc other than R_ARM_RELATIVE with symbol 0 is
an error.
"""
import struct
import sys

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
SHDR = struct.Struct("<IIIIIIIIII")
PHDR = struct.Struct("<IIIIIIII")
REL = struct.Struct("<II")
RELA = struct.Struct("<III")
DYN = struct.Struct("<II")

SHT_PROGBITS = 1
SHT_STRTAB = 3
SHT_RELA = 4
SHT_REL = 9
PT_LOAD = 1
SHF_ALLOC = 2
R_ARM_RELATIVE = 23

DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_RELACOUNT = 0x6FFFFFF9
DT_RELCOUNT = 0x6FFFFFFa


def die(msg):
    sys.stderr.write("rela_dyn: %s\n" % msg)
    sys.exit(1)


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


def main(argv):
    if len(argv) != 2:
        die("usage: rela_dyn.py <elf>")
    path = argv[1]
    blob = bytearray(open(path, "rb").read())
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        die("not a little-endian ELF32")

    (ident, etype, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = EHDR.unpack_from(blob, 0)
    if machine != 40:
        die("not EM_ARM")
    if shentsize != SHDR.size or phentsize != PHDR.size or ehsize < 52:
        die("unexpected ELF header sizes")

    shdrs = [list(SHDR.unpack_from(blob, shoff + i * shentsize)) for i in range(shnum)]
    phdrs = [list(PHDR.unpack_from(blob, phoff + i * phentsize)) for i in range(phnum)]

    def va_word(va):
        for p_type, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _flags, _align in phdrs:
            if p_type != PT_LOAD:
                continue
            if p_vaddr <= va < p_vaddr + p_filesz:
                return struct.unpack_from("<I", blob, p_offset + (va - p_vaddr))[0]
        die("place 0x%x is not in a PT_LOAD filesz" % va)

    relas = bytearray()
    count = 0
    for sh in shdrs:
        name, typ, shflags, addr, off, size, link, info, addralign, entsize = sh
        if typ != SHT_REL or not (shflags & SHF_ALLOC):
            continue
        if size % 8:
            die("allocated SHT_REL size is not a multiple of 8")
        for n in range(size // 8):
            r_offset, r_info = REL.unpack_from(blob, off + n * 8)
            if (r_info & 0xFF) != R_ARM_RELATIVE or (r_info >> 8) != 0:
                die("reloc type %d sym %d, want R_ARM_RELATIVE/0" %
                    (r_info & 0xFF, r_info >> 8))
            relas += RELA.pack(r_offset, r_info, va_word(r_offset))
            count += 1
        sh[1] = SHT_PROGBITS
        sh[6] = 0
        sh[7] = 0
        sh[9] = 0

    for sh in shdrs:
        if sh[1] == SHT_RELA and (sh[2] & SHF_ALLOC):
            die("image already has allocated SHT_RELA")

    str_off = shdrs[shstrndx][4]
    str_size = shdrs[shstrndx][5]
    old_str = bytes(blob[str_off:str_off + str_size])
    extra = b".rela.dyn\x00"
    new_str = old_str + extra
    name_off = len(old_str)

    rela_va = 0
    for sh in shdrs:
        if sh[2] & SHF_ALLOC:
            rela_va = max(rela_va, sh[3] + sh[5])
    rela_va = align_up(rela_va, 4)

    rela_file = align_up(len(blob), 4)
    if rela_file > len(blob):
        blob += b"\x00" * (rela_file - len(blob))
    blob += relas
    str_file = align_up(len(blob), 4)
    if str_file > len(blob):
        blob += b"\x00" * (str_file - len(blob))
    blob += new_str

    rela_sh = [name_off, SHT_RELA, SHF_ALLOC, rela_va, rela_file, len(relas),
               0, 0, 4, 12]
    str_sh = [0, SHT_STRTAB, 0, 0, str_file, len(new_str), 0, 0, 1, 0]
    shdrs.append(rela_sh)
    shdrs.append(str_sh)
    new_strndx = len(shdrs) - 1

    load_count = sum(1 for p in phdrs if p[0] == PT_LOAD)
    if load_count >= 4:
        die("already 4 PT_LOAD segments")
    phdr_end = phoff + phnum * phentsize
    first_payload = min(
        (sh[4] for sh in shdrs[:-2] if sh[5] and sh[4] > phdr_end),
        default=len(blob))
    if phdr_end + phentsize > first_payload:
        die("no spare file bytes for another program header")
    phdrs.append([PT_LOAD, rela_file, rela_va, rela_va, len(relas), len(relas), 4, 4])

    patch_dynamic(blob, shdrs, rela_va, len(relas), count)

    new_shoff = align_up(len(blob), 4)
    if new_shoff > len(blob):
        blob += b"\x00" * (new_shoff - len(blob))
    for sh in shdrs:
        blob += SHDR.pack(*sh)
    for i, ph in enumerate(phdrs):
        PHDR.pack_into(blob, phoff + i * phentsize, *ph)
    EHDR.pack_into(
        blob, 0, ident, etype, machine, version, entry, phoff, new_shoff, flags,
        ehsize, phentsize, len(phdrs), shentsize, len(shdrs), new_strndx)
    open(path, "wb").write(blob)


def patch_dynamic(blob, shdrs, rela_va, rela_size, count):
    dyn = None
    for sh in shdrs:
        if sh[1] == 6 and (sh[2] & SHF_ALLOC):  # SHT_DYNAMIC
            dyn = sh
            break
    if dyn is None:
        if count:
            die("relocations present but no .dynamic section")
        return
    off, size = dyn[4], dyn[5]
    if size % 8:
        die(".dynamic size is not a multiple of 8")
    replaced = False
    for n in range(size // 8):
        at = off + n * 8
        tag, val = DYN.unpack_from(blob, at)
        if tag == DT_REL:
            DYN.pack_into(blob, at, DT_RELA, rela_va)
            replaced = True
        elif tag == DT_RELSZ:
            DYN.pack_into(blob, at, DT_RELASZ, rela_size)
        elif tag == DT_RELENT:
            DYN.pack_into(blob, at, DT_RELAENT, 12)
        elif tag == DT_RELCOUNT:
            DYN.pack_into(blob, at, DT_RELACOUNT, count)
    if count and not replaced:
        die(".dynamic has no DT_REL tag to rewrite")


if __name__ == "__main__":
    main(sys.argv)
