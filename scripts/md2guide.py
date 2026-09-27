#!/usr/bin/env python3
"""Convert the SDK's small Markdown subset to AmigaGuide."""

import argparse
import datetime as _datetime
import pathlib
import re
import sys
import tempfile
from collections import defaultdict

TRANSLITERATIONS = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"',
    "–": "-", "—": "-", "…": "...", "→": "->", "←": "<-", "↔": "<->",
    "×": "x", "°": "deg", "\u00a0": " ", "│": "|", "─": "-", "├": "+",
    "└": "+", "┌": "+", "┘": "+", "┬": "+", "┴": "+", "►": ">", "▼": "v",
    "◄": "<", "▲": "^", "µ": "u", "≈": "~", "≤": "<=", "≥": ">=", "±": "+/-",
    "·": ".", "•": "*", "✓": "[x]", "✗": "[ ]",
}


def _warn_character(character, line_number):
    print("md2guide: line {}: replaced {!r} with '?'".format(line_number, character), file=sys.stderr)


def ascii_text(text, line_number=0):
    """Return text encodable as ASCII, warning once per replaced character."""
    output = []
    for character in text:
        if character in TRANSLITERATIONS:
            output.append(TRANSLITERATIONS[character])
        else:
            try:
                character.encode("ascii")
            except UnicodeEncodeError:
                _warn_character(character, line_number)
                output.append("?")
            else:
                output.append(character)
    return "".join(output)


def plain_text(text):
    """Remove the inline Markdown syntax needed for node labels and slugs."""
    text = re.sub(r"!\[([^]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = text.replace("**", "")
    return re.sub(r"(?<!\*)\*([^*]+)\*", r"\1", text).strip()


def slugify(text):
    """Produce AmigaGuide node identifiers using the documented ASCII rule."""
    ascii_value = ascii_text(plain_text(text))
    return re.sub(r"-+", "-", re.sub(r"[^a-z0-9]+", "-", ascii_value.lower())).strip("-") or "section"



def sibling_guides(names):
    """Map case-insensitive guide stems to the caller's exact guide names."""
    guides = {}
    for name in names:
        filename = name.replace("\\", "/").rsplit("/", 1)[-1]
        if filename.lower().endswith(".guide"):
            guides[filename[:-6].casefold()] = name
    return guides


def render_inline(text, anchor_map, siblings, line_number):
    """Render inline syntax while protecting code spans from later substitutions."""
    protected = []

    def hold(value):
        protected.append(value)
        return "\x00{}\x00".format(len(protected) - 1)

    text = re.sub(r"`([^`]*)`", lambda match: hold("@{b}" + match.group(1) + "@{ub}"), text)

    def image(match):
        alt, url = match.group(1), match.group(2)
        return "{} (image: {})".format(alt, url) if alt else "(image: {})".format(url)

    text = re.sub(r"!\[([^]]*)\]\(([^)]+)\)", image, text)

    def link(match):
        label, url = match.group(1), match.group(2)
        if url.startswith("#"):
            target = anchor_map.get(url[1:], slugify(url[1:]))
            return '@{{"{}" link {}}}'.format(label, target)
        target, separator, _anchor = url.partition("#")
        relative = not target.startswith("/") and not re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target)
        if not separator and relative and target.lower().endswith(".md"):
            filename = target.replace("\\", "/").rsplit("/", 1)[-1]
            stem = filename[:-3]
            sibling = siblings.get(stem.casefold())
            if sibling and stem.casefold() != "readme":
                return '@{{"{}" link "{}/Main"}}'.format(label, sibling)
        return "{} ({})".format(label, url)

    text = re.sub(r"(?<!!)\[([^]]+)\]\(([^)]+)\)", link, text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"@{b}\1@{ub}", text)
    text = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"@{i}\1@{ui}", text)
    text = ascii_text(text, line_number)
    for index, value in enumerate(protected):
        text = text.replace("\x00{}\x00".format(index), ascii_text(value, line_number))
    return text


def is_table_line(line):
    return "|" in line and line.strip().startswith("|")


def table_cells(line):
    cells = line.strip().strip("|").split("|")
    return [cell.strip() for cell in cells]


def table_alignment(cells):
    return all(re.fullmatch(r":?-{3,}:?", cell.strip()) for cell in cells)


def render_table(rows, alignment, anchor_map, siblings, line_numbers):
    rendered = [[render_inline(cell, anchor_map, siblings, number) for cell in row]
                for row, number in zip(rows, line_numbers)]
    columns = max(len(row) for row in rendered)
    for row in rendered:
        row.extend([""] * (columns - len(row)))
    widths = [max(len(row[column]) for row in rendered) for column in range(columns)]
    modes = []
    for column in range(columns):
        marker = alignment[column] if alignment and column < len(alignment) else ""
        modes.append("center" if marker.startswith(":") and marker.endswith(":") else
                     "right" if marker.endswith(":") else "left")

    def padded(value, width, mode):
        if mode == "right":
            return value.rjust(width)
        if mode == "center":
            return value.center(width)
        return value.ljust(width)

    output = [" | ".join(padded(value, widths[column], modes[column])
                           for column, value in enumerate(row)) for row in rendered]
    separator = "+".join("-" * width for width in widths)
    return [output[0], separator] + output[1:]


def node_name(title, used):
    base = slugify(title)
    used[base] += 1
    return base if used[base] == 1 else "{}-{}".format(base, used[base])


def parse_markdown(source):
    """Parse block-level Markdown into Main and H2/H3 guide node records."""
    source = re.sub(r"<!--[\s\S]*?-->", "", source)
    lines = source.splitlines()
    used = defaultdict(int)
    nodes = [{"name": "Main", "title": "", "blocks": [], "parent": None}]
    current = nodes[0]
    h2_name = None
    anchors = {"main": "Main"}
    contents = []
    index = 0

    def add_block(kind, value, number):
        current["blocks"].append((kind, value, number))

    while index < len(lines):
        line = lines[index]
        number = index + 1
        heading = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", line)
        if heading:
            level, title = len(heading.group(1)), heading.group(2)
            if level == 1:
                nodes[0]["title"] = plain_text(title)
                anchors[slugify(title)] = "Main"
            elif level in (2, 3):
                name = node_name(title, used)
                parent = h2_name if level == 3 else None
                current = {"name": name, "title": plain_text(title), "blocks": [], "parent": parent}
                nodes.append(current)
                contents.append((current["title"], name))
                anchors[slugify(title)] = name
                if level == 2:
                    h2_name = name
            else:
                add_block("line", "@{b}" + plain_text(title) + "@{ub}", number)
            index += 1
            continue
        if not line.strip():
            index += 1
            continue
        fence = re.match(r"^\s*(```+|~~~+)", line)
        if fence:
            marker = fence.group(1)[0]
            code = []
            index += 1
            while index < len(lines) and not re.match(r"^\s*" + re.escape(marker) + r"{3,}", lines[index]):
                code.append((lines[index], index + 1))
                index += 1
            if index < len(lines):
                index += 1
            add_block("code", code, number)
            continue
        if re.fullmatch(r"\s*(?:---|\*\*\*|___)\s*", line):
            add_block("rule", "", number)
            index += 1
            continue
        if is_table_line(line) and index + 1 < len(lines) and is_table_line(lines[index + 1]) and table_alignment(table_cells(lines[index + 1])):
            rows = [table_cells(line)]
            row_numbers = [number]
            alignment = table_cells(lines[index + 1])
            index += 2
            while index < len(lines) and is_table_line(lines[index]):
                rows.append(table_cells(lines[index]))
                row_numbers.append(index + 1)
                index += 1
            add_block("table", (rows, alignment, row_numbers), number)
            continue
        if re.match(r"^\s*>", line):
            quote = []
            while index < len(lines) and re.match(r"^\s*>", lines[index]):
                quote.append((re.sub(r"^\s*>\s?", "", lines[index]), index + 1))
                index += 1
            add_block("quote", quote, number)
            continue
        list_match = re.match(r"^(\s*)([-*])\s+(.+)$", line) or re.match(r"^(\s*)(\d+)[.)]\s+(.+)$", line)
        if list_match:
            entries = []
            while index < len(lines):
                match = re.match(r"^(\s*)([-*])\s+(.+)$", lines[index]) or re.match(r"^(\s*)(\d+)[.)]\s+(.+)$", lines[index])
                if not match:
                    break
                indent, marker, item = match.group(1), match.group(2), match.group(3)
                level = len(indent.expandtabs(2)) // 2
                marker = "-" if marker in ("-", "*") else marker + "."
                entries.append((level, marker, item, index + 1))
                index += 1
            add_block("list", entries, number)
            continue
        paragraph = [(line.strip(), number)]
        index += 1
        while index < len(lines):
            candidate = lines[index]
            if not candidate.strip() or re.match(r"^(#{1,6})\s+", candidate) or re.match(r"^\s*(```+|~~~+)", candidate):
                break
            if re.fullmatch(r"\s*(?:---|\*\*\*|___)\s*", candidate) or re.match(r"^\s*>", candidate):
                break
            if re.match(r"^(\s*)(?:[-*]|\d+[.)])\s+", candidate):
                break
            if is_table_line(candidate) and index + 1 < len(lines) and is_table_line(lines[index + 1]):
                break
            paragraph.append((candidate.strip(), index + 1))
            index += 1
        add_block("paragraph", paragraph, number)
    return nodes, contents, anchors


def guide_text(source, input_name, title=None, name=None, version="1.0", date=None, siblings=()):
    nodes, contents, anchors = parse_markdown(source)
    guide_name = ascii_text((name or pathlib.Path(input_name).stem).removesuffix(".guide"))
    document_title = title or nodes[0]["title"] or guide_name
    version = ascii_text(version)
    date = ascii_text(date) if date else "{}.{}.{}".format(_datetime.date.today().day, _datetime.date.today().month, _datetime.date.today().year)
    sibling_map = sibling_guides(siblings)
    output = ["@database {}.guide".format(guide_name), "@$VER: {}.guide {} ({})".format(guide_name, version, date), ""]

    def emit_block(kind, value, number):
        if kind == "paragraph":
            output.append(render_inline(" ".join(text for text, _ in value), anchors, sibling_map, number))
        elif kind == "line":
            output.append(ascii_text(value, number))
        elif kind == "rule":
            output.append("-" * 40)
        elif kind == "code":
            output.append("")
            for line, line_number in value:
                content = ascii_text(line, line_number)
                output.append("@" + content if content.startswith("@") else content)
            output.append("")
        elif kind == "quote":
            output.extend("> " + render_inline(line, anchors, sibling_map, line_number) for line, line_number in value)
        elif kind == "list":
            for level, marker, item, line_number in value:
                output.append("  " * level + marker + " " + render_inline(item, anchors, sibling_map, line_number))
            output.append("")
        elif kind == "table":
            rows, alignment, row_numbers = value
            output.extend(render_table(rows, alignment, anchors, sibling_map, row_numbers))

    output.append('@node Main "{}"'.format(ascii_text(document_title)))
    for section_title, section_name in contents:
        output.append('@{{"{}" link {}}}'.format(ascii_text(section_title), section_name))
    if contents:
        output.append("")
    for kind, value, number in nodes[0]["blocks"]:
        emit_block(kind, value, number)
    output.append("@endnode")
    for node in nodes[1:]:
        output.extend(["", '@node {} "{}"'.format(node["name"], ascii_text(node["title"])), ""])
        for kind, value, number in node["blocks"]:
            emit_block(kind, value, number)
        if node["parent"]:
            output.extend(["", '@{"Back" link ' + node["parent"] + "}"])
        output.extend(["", '@{"Contents" link Main}', "@endnode"])
    return "\n".join(output).rstrip() + "\n"


def check_guide(path):
    try:
        data = pathlib.Path(path).read_bytes()
        text = data.decode("ascii")
        if text.encode("ascii") != data:
            raise ValueError("does not round-trip as ASCII")
    except (OSError, UnicodeError, ValueError) as error:
        print("md2guide: {}: {}".format(path, error), file=sys.stderr)
        return False
    errors = []
    lines = text.splitlines()
    first_node = next((number for number, line in enumerate(lines, 1) if line.startswith("@node ")), len(lines) + 1)
    if not any(line.startswith("@database ") for line in lines[:first_node - 1]):
        errors.append("missing @database in header")
    nodes = set()
    depth = 0
    links = []
    for number, line in enumerate(lines, 1):
        match = re.match(r'^@node\s+(\S+)\s+".*"\s*$', line)
        if match:
            if depth:
                errors.append("line {}: nested @node".format(number))
            name = match.group(1)
            if name in nodes:
                errors.append("line {}: duplicate node {}".format(number, name))
            nodes.add(name)
            depth += 1
        elif line == "@endnode":
            if not depth:
                errors.append("line {}: unmatched @endnode".format(number))
            else:
                depth -= 1
        elif line.startswith("@"):
            if not (line.startswith("@database ") or line.startswith("@$VER:") or line.startswith("@{") or line.startswith("@@")):
                errors.append("line {}: unknown AmigaGuide command".format(number))
        links.extend(re.findall(r'@\{"[^"\n]*"\s+link\s+([^}"\s]+)\}', line))
    if depth:
        errors.append("missing @endnode")
    for target in links:
        if target not in nodes:
            errors.append("link target {} does not name a node".format(target))
    for error in errors:
        print("md2guide: {}: {}".format(path, error), file=sys.stderr)
    return not errors


def selftest():
    fixture = """# Demo — Guide
<!-- ignored -->
Intro wraps
onto one line with [local](#part), [play](zzplay.md), [library](../docs/zz9k-library.md), [readme](../README.md), [missing](missing.md), [site](https://example.test), ![logo](logo.png), `code`, **bold**, and *italic*.

## Part
> quoted **line**
> next line

- one
  - nested
* two
1. first
2. second

| Left | Right |
| :--- | ---: |
| a | 12 |

#### Small heading

---

```text
@command
plain
│ ├──└ ►
```

### Child
An unknown snowman ☃ and an arrow →. A timing is 5µs ≈ exact.
"""
    result = guide_text(fixture, "demo.md", date="1.2.2003",
                        siblings=["ZZPlay.guide", "zz9k-library.guide", "README.guide"])
    expected = [
        "@database demo.guide",
        "@$VER: demo.guide 1.0 (1.2.2003)",
        '@node Main "Demo - Guide"',
        '@{"Part" link part}', '@{"Child" link child}',
        '@node part "Part"', '> quoted @{b}line@{ub}',
        "- one", "  - nested", "1. first", "2. second",
        "Left | Right", "----+-----", "a    |    12",
        "@{b}Small heading@{ub}", "----------------------------------------",
        "@@command", '@{"Back" link part}', '@{"Contents" link Main}',
        '@{"play" link "ZZPlay.guide/Main"}', '@{"library" link "zz9k-library.guide/Main"}',
        "readme (../README.md)", "missing (missing.md)", "site (https://example.test)", "logo (image: logo.png)",
        "@{b}code@{ub}", "@{b}bold@{ub}", "@{i}italic@{ui}",
        "An unknown snowman ? and an arrow ->. A timing is 5us ~ exact.",
        "| +--+ >",
    ]
    for value in expected:
        if value not in result:
            raise AssertionError("missing {!r}".format(value))
    if '@{"local" link part}' not in result:
        raise AssertionError("missing local link form")
    with tempfile.TemporaryDirectory() as directory:
        guide = pathlib.Path(directory) / "demo.guide"
        guide.write_bytes(result.encode("ascii"))
        if not check_guide(guide):
            raise AssertionError("generated guide failed validation")
        guide.write_text("@database bad.guide\n@node Main \"Bad\"\n@{\"bad\" link Missing}\n@endnode\n", encoding="ascii")
        if check_guide(guide):
            raise AssertionError("broken guide passed validation")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title")
    parser.add_argument("--name")
    parser.add_argument("--version", default="1.0")
    parser.add_argument("--date")
    parser.add_argument("--check", metavar="FILE.guide")
    parser.add_argument("--sibling", metavar="NAME.guide", action="append", default=[])
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("input", nargs="?")
    parser.add_argument("output", nargs="?")
    arguments = parser.parse_args(argv)
    try:
        if arguments.selftest:
            selftest()
            print("PASS")
            return 0
        if arguments.check:
            if arguments.input or arguments.output:
                parser.error("--check does not accept input or output paths")
            return 0 if check_guide(arguments.check) else 1
        if not arguments.input:
            parser.error("INPUT.md is required")
        source = pathlib.Path(arguments.input).read_text(encoding="utf-8")
        result = guide_text(source, arguments.input, arguments.title, arguments.name, arguments.version, arguments.date,
                            arguments.sibling)
        if arguments.output:
            pathlib.Path(arguments.output).write_bytes(result.encode("ascii"))
        else:
            sys.stdout.buffer.write(result.encode("ascii"))
        return 0
    except (OSError, UnicodeError, AssertionError) as error:
        print("md2guide: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
