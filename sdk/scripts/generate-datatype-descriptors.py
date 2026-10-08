#!/usr/bin/env python3
# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile Amiga DataTypes descriptor metadata into FORM DTYP files."""

import argparse
import pathlib
import struct
import sys

_REQUIRED_KEYS = {
    "FileName",
    "Version",
    "DTName",
    "ID",
    "Recog",
    "Pattern",
    "Flags",
    "Install",
}


class DescriptorError(ValueError):
    pass


def _error(path, message):
    raise DescriptorError("%s: %s" % (path, message))


def _parse_source(path):
    values = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as exc:
        _error(path, "metadata is not UTF-8 (%s)" % exc)

    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in line:
            _error(path, "line %u is not key=value" % line_number)
        key, value = line.split("=", 1)
        if key not in _REQUIRED_KEYS:
            _error(path, "line %u has unknown key %r" % (line_number, key))
        if key in values:
            _error(path, "line %u repeats %s" % (line_number, key))
        if not value:
            _error(path, "line %u has an empty %s" % (line_number, key))
        values[key] = value

    missing = _REQUIRED_KEYS.difference(values)
    if missing:
        _error(path, "missing " + ", ".join(sorted(missing)))
    return values


def _latin1(path, label, text):
    try:
        return text.encode("latin-1")
    except UnicodeEncodeError:
        _error(path, "%s must be Latin-1" % label)


def _c_string(path, label, text):
    encoded = _latin1(path, label, text)
    if b"\0" in encoded:
        _error(path, "%s must not contain NUL" % label)
    return encoded


def _fourcc(path, label, value):
    value = value.replace("<nul>", "\0")
    encoded = _latin1(path, label, value)
    if len(encoded) != 4:
        _error(path, "%s must encode to exactly four bytes" % label)
    return encoded


def _parse_recognition(path, value):
    fields = value.split()
    if not fields:
        _error(path, "Recog must contain at least one hexadecimal byte")
    if len(fields) > 32767:
        _error(path, "Recog exceeds the signed DTHD mask length")
    recognition = []
    for field in fields:
        if field == "??":
            # dth_Mask is a WORD array; -1 is the DataTypes wildcard.
            recognition.append(-1)
            continue
        if len(field) != 2 or any(c not in "0123456789abcdefABCDEF" for c in field):
            _error(path, "Recog byte %r is not two hexadecimal digits or ??" % field)
        recognition.append(int(field, 16))
    return recognition


def _parse_version(path, value):
    parts = value.split(".")
    if len(parts) != 2 or any(not part.isdecimal() for part in parts):
        _error(path, "Version must be major.minor")
    if any(int(part, 10) > 65535 for part in parts):
        _error(path, "Version component exceeds 65535")


def _parse_flags(path, value):
    fields = value.split(",")
    if len(fields) != 3 or fields[0] != "Binary" or fields[1] != "n":
        _error(path, "Flags must be Binary,n,<priority>")
    try:
        priority = int(fields[2], 10)
    except ValueError:
        _error(path, "Flags priority is not an integer")
    if not 0 <= priority <= 65535:
        _error(path, "Flags priority must be in 0..65535")
    return 0, priority


def _compile(path):
    values = _parse_source(path)
    _parse_version(path, values["Version"])

    expected_file_name = "Storage/DataTypes/%s" % path.stem
    if values["FileName"] != expected_file_name:
        _error(path, "FileName must be %s for inactive descriptors" % expected_file_name)
    if values["Install"] != "inactive":
        _error(path, "Install must be inactive")

    try:
        descriptor_name, base_name = values["DTName"].split(",", 1)
    except ValueError:
        _error(path, "DTName must be descriptor-name,base-name")
    if not descriptor_name or not base_name:
        _error(path, "DTName must name both descriptor and base")
    if descriptor_name != path.stem:
        _error(path, "DTName descriptor name must match the source filename")

    try:
        group, identifier = values["ID"].split(",", 1)
    except ValueError:
        _error(path, "ID must be group,identifier")

    descriptor_name_bytes = _c_string(path, "DTName descriptor name", descriptor_name)
    base_name_bytes = _c_string(path, "DTName base name", base_name)
    pattern = _c_string(path, "Pattern", values["Pattern"])
    if not pattern:
        _error(path, "Pattern must not be empty")
    flags, priority = _parse_flags(path, values["Flags"])
    recognition = _parse_recognition(path, values["Recog"])

    # The NDK defines dth_Mask as WORD[]. Values 0..255 match a byte;
    # -1 is the wildcard representation accepted by DataTypes.
    masks = b"".join(struct.pack(">h", value) for value in recognition)
    dth_name_offset = 32 + len(masks)
    base_name_offset = dth_name_offset + len(descriptor_name_bytes) + 1
    pattern_offset = base_name_offset + len(base_name_bytes) + 1
    dthd = struct.pack(
        ">IIII4s4sHHHH",
        dth_name_offset,
        base_name_offset,
        pattern_offset,
        32,
        _fourcc(path, "ID group", group),
        _fourcc(path, "ID identifier", identifier),
        len(recognition),
        0,
        flags,
        priority,
    )
    dthd += masks + descriptor_name_bytes + b"\0" + base_name_bytes + b"\0"
    dthd += pattern + b"\0"

    def chunk(chunk_id, payload):
        padding = b"\0" if len(payload) & 1 else b""
        return chunk_id + struct.pack(">I", len(payload)) + payload + padding

    body = b"DTYP" + chunk(b"NAME", descriptor_name_bytes) + chunk(b"DTHD", dthd)
    return b"FORM" + struct.pack(">I", len(body)) + body


def generate(source_dir, output_dir):
    sources = sorted(source_dir.glob("*.dtid"))
    if not sources:
        raise DescriptorError("%s: no .dtid descriptor sources" % source_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    for source in sources:
        (output_dir / source.stem).write_bytes(_compile(source))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    if not args.source_dir.is_dir():
        parser.error("--source-dir is not a directory: %s" % args.source_dir)
    try:
        generate(args.source_dir, args.output_dir)
    except DescriptorError as exc:
        print("generate-datatype-descriptors: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
