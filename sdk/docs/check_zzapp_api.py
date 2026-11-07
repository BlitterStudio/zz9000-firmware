#!/usr/bin/env python3
"""Fail if zzapp-api.md lacks a heading for a public zzapp.h symbol.

Parses declarations. A word search of the header is not the check.
"""
# SPDX-License-Identifier: MIT

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "sdk" / "zzapp" / "include" / "zzapp.h"
DOC = ROOT / "sdk" / "docs" / "zzapp-api.md"

KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Bool", "_Complex",
    "_Imaginary",
}


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise SystemExit("unterminated block comment")
            out.append(" " * (end + 2 - i))
            i = end + 2
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                end = n
            out.append(" " * (end - i))
            i = end
            continue
        if text[i] in "\"'":
            quote = text[i]
            out.append(text[i])
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def continued_lines(text):
    lines = text.splitlines()
    merged = []
    buf = ""
    for line in lines:
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        merged.append(buf + line)
        buf = ""
    if buf:
        merged.append(buf)
    return merged


def zz_macros(text):
    found = []
    for line in continued_lines(strip_comments(text)):
        stripped = line.lstrip()
        if not stripped.startswith("#"):
            continue
        body = stripped[1:].strip()
        if not body.startswith("define"):
            continue
        rest = body[len("define"):].strip()
        match = re.match(r"(ZZ_[A-Za-z0-9_]+)\b", rest)
        if match:
            found.append(match.group(1))
    return found


def tokenize(code):
    tokens = []
    i = 0
    n = len(code)
    while i < n:
        ch = code[i]
        if ch.isspace():
            i += 1
            continue
        if ch == "#" :
            while i < n and code[i] != "\n":
                if code[i] == "\\":
                    i += 2
                    continue
                i += 1
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and code[i] != quote:
                i += 2 if code[i] == "\\" else 1
            i += 1
            tokens.append(("string", ""))
            continue
        if ch.isalpha() or ch == "_":
            start = i
            i += 1
            while i < n and (code[i].isalnum() or code[i] == "_"):
                i += 1
            tokens.append(("ident", code[start:i]))
            continue
        if ch.isdigit():
            start = i
            i += 1
            while i < n and (code[i].isalnum() or code[i] in ".xX"):
                i += 1
            tokens.append(("num", code[start:i]))
            continue
        if ch in "(){}[];,*":
            tokens.append(("punct", ch))
            i += 1
            continue
        tokens.append(("other", ch))
        i += 1
    return tokens


def drop_linkage_braces(tokens):
    """extern \"C\" { } is a linkage wrapper, not a type body."""
    out = []
    i = 0
    while i < len(tokens):
        if (i + 3 < len(tokens) and tokens[i] == ("ident", "extern")
                and tokens[i + 1][0] == "string"
                and tokens[i + 2] == ("punct", "{")):
            i += 3
            depth = 1
            while i < len(tokens) and depth:
                if tokens[i] == ("punct", "{"):
                    depth += 1
                    i += 1
                    continue
                if tokens[i] == ("punct", "}"):
                    depth -= 1
                    i += 1
                    if depth == 0:
                        break
                    continue
                out.append(tokens[i])
                i += 1
            continue
        out.append(tokens[i])
        i += 1
    return out


def zz_functions(text):
    tokens = drop_linkage_braces(tokenize(strip_comments(text)))
    found = []
    i = 0
    type_depth = 0
    pending_type = False
    decl = []
    while i < len(tokens):
        kind, value = tokens[i]
        if type_depth == 0 and kind == "ident" and value in ("struct", "union", "enum"):
            pending_type = True
        if kind == "punct" and value == "{":
            if pending_type or type_depth:
                type_depth += 1
            pending_type = False
            decl = []
            i += 1
            continue
        if kind == "punct" and value == "}":
            if type_depth:
                type_depth -= 1
            pending_type = False
            decl = []
            i += 1
            continue
        if type_depth:
            i += 1
            continue
        if kind == "punct" and value == ";":
            found.extend(function_names(decl))
            decl = []
            pending_type = False
            i += 1
            continue
        if kind == "ident" and value not in ("struct", "union", "enum"):
            pending_type = False
        decl.append(tokens[i])
        i += 1
    return found


def function_names(decl):
    names = []
    paren = 0
    for i, (kind, value) in enumerate(decl):
        if kind == "punct" and value == "(":
            if paren == 0 and i > 0 and decl[i - 1][0] == "ident":
                name = decl[i - 1][1]
                if name.startswith("zz_") and name not in KEYWORDS:
                    names.append(name)
            paren += 1
            continue
        if kind == "punct" and value == ")":
            paren = max(0, paren - 1)
    return names


def headings(text):
    found = set()
    for line in text.splitlines():
        match = re.match(r"#{1,6}\s+(\S+)", line)
        if match:
            found.add(match.group(1).rstrip("`"))
    return found


def missing(header_text, doc_text):
    required = zz_macros(header_text) + zz_functions(header_text)
    have = headings(doc_text)
    absent = []
    seen = set()
    for name in required:
        if name in seen:
            continue
        seen.add(name)
        if name not in have:
            absent.append(name)
    return absent


def prove_injection(header_text, doc_text):
    injected_fn = header_text + "\nint zz_not_documented(void);\n"
    injected_macro = header_text + "\n#define ZZ_NOT_DOCUMENTED 1\n"
    fn_miss = missing(injected_fn, doc_text)
    macro_miss = missing(injected_macro, doc_text)
    if "zz_not_documented" not in fn_miss:
        raise SystemExit("injection of zz_not_documented was not reported")
    if "ZZ_NOT_DOCUMENTED" not in macro_miss:
        raise SystemExit("injection of ZZ_NOT_DOCUMENTED was not reported")
    print("injection rejected: zz_not_documented, ZZ_NOT_DOCUMENTED")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--header", type=pathlib.Path, default=HEADER)
    parser.add_argument("--doc", type=pathlib.Path, default=DOC)
    args = parser.parse_args()
    header_text = args.header.read_text(encoding="utf-8")
    doc_text = args.doc.read_text(encoding="utf-8") if args.doc.exists() else ""
    absent = missing(header_text, doc_text)
    if absent:
        sys.stderr.write("zzapp-api.md missing headings:\n")
        for name in absent:
            sys.stderr.write("  %s\n" % name)
        return 1
    funcs = zz_functions(header_text)
    macros = zz_macros(header_text)
    print("covered %d zz_* functions and %d ZZ_* macros" %
          (len(set(funcs)), len(set(macros))))
    if args.self_test:
        prove_injection(header_text, doc_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
