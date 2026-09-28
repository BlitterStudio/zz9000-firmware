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


def protect_guide_references(text):
    """Replace complete @{"..." link ...} references with whitespace-free tokens."""
    references = []
    output = []
    index = 0
    while index < len(text):
        if not text.startswith('@{"', index):
            output.append(text[index])
            index += 1
            continue
        depth = 1
        end = index + 2
        while end < len(text) and depth:
            if text.startswith("@{", end):
                depth += 1
                end += 2
            elif text[end] == "}":
                depth -= 1
                end += 1
            else:
                end += 1
        if depth:
            output.append(text[index])
            index += 1
            continue
        references.append(text[index:end])
        output.append("\x00{}\x00".format(len(references) - 1))
        index = end
    return "".join(output), references

def display_text(text):
    """Return the text MultiView displays, without AmigaGuide control markup."""
    text = re.sub(r'@\{"(.*?)"\s+link\s+(?:"[^"]+"|[^}]+)\}', r"\1", text)
    return re.sub(r"@\{(?:b|ub|i|ui)\}", "", text)




def wrap_flow(text, width, first_prefix="", following_prefix=None):
    """Wrap rendered flowing text without splitting words or guide references."""
    if width < 1:
        raise ValueError("width must be positive")
    following_prefix = first_prefix if following_prefix is None else following_prefix
    protected, references = protect_guide_references(text)
    words = protected.split()
    if not words:
        return []

    def visible_width(value):
        for index, reference in enumerate(references):
            value = value.replace("\x00{}\x00".format(index), reference)
        return len(display_text(value))

    lines = []
    prefix = first_prefix
    line = prefix
    for word in words:
        separator = "" if line == prefix else " "
        if line != prefix and visible_width(line + separator + word) > width:
            lines.append(line)
            prefix = following_prefix
            line = prefix + word
        else:
            line += separator + word
    lines.append(line)
    for index, reference in enumerate(references):
        token = "\x00{}\x00".format(index)
        lines = [line.replace(token, reference) for line in lines]
    return lines


def longest_token(text):
    protected, references = protect_guide_references(text)
    tokens = protected.split()
    return max([len(display_text(token)) for token in tokens] +
               [len(display_text(reference)) for reference in references] + [1])


def distribute_widths(rows, width):
    columns = len(rows[0])
    available = max(1, width - 3 * (columns - 1))
    minimums = [max(longest_token(row[column]) for row in rows) for column in range(columns)]
    maximums = [max(len(display_text(row[column])) for row in rows) for column in range(columns)]
    widths = list(minimums)
    remaining = available - sum(widths)
    while remaining > 0:
        candidates = [column for column in range(columns) if widths[column] < maximums[column]]
        if not candidates:
            break
        weights = [max(1, maximums[column] - widths[column]) for column in candidates]
        total = sum(weights)
        changed = False
        for column, weight in zip(candidates, weights):
            share = max(1, remaining * weight // total)
            addition = min(share, maximums[column] - widths[column], remaining)
            if addition:
                widths[column] += addition
                remaining -= addition
                changed = True
            if not remaining:
                break
        if not changed:
            break
    return widths

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


def render_table(rows, alignment, anchor_map, siblings, line_numbers, width):
    rendered = [[render_inline(cell, anchor_map, siblings, number) for cell in row]
                for row, number in zip(rows, line_numbers)]
    columns = max(len(row) for row in rendered)
    for row in rendered:
        row.extend([""] * (columns - len(row)))
    if columns == 1:
        return [wrap_flow(row[0], width) for row in rendered]
    widths = distribute_widths(rendered, width)
    modes = []
    for column in range(columns):
        marker = alignment[column] if alignment and column < len(alignment) else ""
        modes.append("center" if marker.startswith(":") and marker.endswith(":") else
                     "right" if marker.endswith(":") else "left")

    def padded(value, cell_width, mode):
        padding = max(0, cell_width - len(display_text(value)))
        if mode == "right":
            return " " * padding + value
        if mode == "center":
            left = padding // 2
            return " " * left + value + " " * (padding - left)
        return value + " " * padding

    def row_lines(row):
        cells = [wrap_flow(value, widths[column]) or [""] for column, value in enumerate(row)]
        height = max(len(cell) for cell in cells)
        return [" | ".join(padded(cells[column][line] if line < len(cells[column]) else "",
                                  widths[column], modes[column])
                            for column in range(columns)) for line in range(height)]

    header = ["@{b}" + line + "@{ub}" for line in row_lines(rendered[0])]
    table_width = sum(widths) + 3 * (columns - 1)
    units = [header + ["-" * table_width]]
    units.extend(row_lines(row) for row in rendered[1:])
    return units


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
                base_indent = len(indent.expandtabs(2))
                item_lines = [item]
                line_number = index + 1
                index += 1
                while index < len(lines):
                    candidate = lines[index]
                    if not candidate.strip() or re.match(r"^(\s*)(?:[-*]|\d+[.)])\s+", candidate):
                        break
                    candidate_indent = len(candidate) - len(candidate.lstrip(" \t"))
                    if candidate_indent <= base_indent:
                        break
                    item_lines.append(candidate.strip())
                    index += 1
                marker = "-" if marker in ("-", "*") else marker + "."
                entries.append((level, marker, item_lines, line_number))
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


def guide_text(source, input_name, title=None, name=None, version="1.0", date=None, siblings=(), width=75):
    nodes, contents, anchors = parse_markdown(source)
    guide_name = ascii_text((name or pathlib.Path(input_name).stem).removesuffix(".guide"))
    document_title = title or nodes[0]["title"] or guide_name
    version = ascii_text(version)
    date = ascii_text(date) if date else "{}.{}.{}".format(_datetime.date.today().day, _datetime.date.today().month, _datetime.date.today().year)
    sibling_map = sibling_guides(siblings)
    output = ["@database {}.guide".format(guide_name), "@$VER: {}.guide {} ({})".format(guide_name, version, date), ""]

    def emit_unit(lines):
        output.extend(lines)
        output.append("")

    def emit_block(kind, value, number):
        if kind == "paragraph":
            text = render_inline(" ".join(text for text, _ in value), anchors, sibling_map, number)
            emit_unit(wrap_flow(text, width))
        elif kind == "line":
            emit_unit(wrap_flow(ascii_text(value, number), width))
        elif kind == "rule":
            emit_unit(["-" * 40])
        elif kind == "code":
            if output[-1] != "":
                output.append("")
            for line, line_number in value:
                content = ascii_text(line, line_number)
                output.append("@" + content if content.startswith("@") else content)
            output.append("")
        elif kind == "quote":
            text = render_inline(" ".join(line for line, _ in value), anchors, sibling_map, number)
            emit_unit(wrap_flow(text, width, "> "))
        elif kind == "list":
            for level, marker, item_lines, line_number in value:
                text = render_inline(" ".join(item_lines), anchors, sibling_map, line_number)
                indent = "  " * level
                emit_unit(wrap_flow(text, width, indent + marker + " ",
                                    indent + " " * (len(marker) + 1)))
        elif kind == "table":
            rows, alignment, row_numbers = value
            for table_unit in render_table(rows, alignment, anchors, sibling_map, row_numbers, width):
                emit_unit(table_unit)

    output.append('@node Main "{}"'.format(ascii_text(document_title)))
    for kind, value, number in nodes[0]["blocks"]:
        emit_block(kind, value, number)
    if contents:
        emit_unit(wrap_flow("Contents", width))
        for section_title, section_name in contents:
            emit_unit(['@{{"{}" link {}}}'.format(ascii_text(section_title), section_name)])
    output.append("@endnode")
    for node in nodes[1:]:
        output.extend(["", '@node {} "{}"'.format(node["name"], ascii_text(node["title"]))])
        for kind, value, number in node["blocks"]:
            emit_block(kind, value, number)
        if node["parent"]:
            emit_unit(['@{"Back" link ' + node["parent"] + "}"])
        emit_unit(['@{"Contents" link Main}'])
        output.append("@endnode")
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
    width = 42
    fixture = """# Demo — Guide
<!-- ignored -->
Intro wraps onto one line with [local](#part), [play](zzplay.md),
[library](../docs/zz9k-library.md), [readme](../README.md),
[missing](missing.md), [site](https://example.test), ![logo](logo.png),
`code`, **bold**, and *italic*.

This **bold phrase
continues over source lines** without literal Markdown markers.

## Part
> quoted **line**
> next line

- A list item begins here and continues
  over a source line without becoming another block unit
- second item
  - nested item
1. first
2. second

| Left Column | Right Value |
| :--- | ---: |
| a long value that wraps | 12345678 |
| more text | 12 |

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
    result = guide_text(fixture, "demo.md", date="1.2.2003", width=width,
                        siblings=["ZZPlay.guide", "zz9k-library.guide", "README.guide"])
    expected = [
        "@database demo.guide",
        "@$VER: demo.guide 1.0 (1.2.2003)",
        '@node Main "Demo - Guide"',
        '@node part "Part"', '> quoted @{b}line@{ub}',
        "@{b}Small heading@{ub}", "----------------------------------------",
        "@@command", '@{"Back" link part}', '@{"Contents" link Main}',
        '@{"play" link "ZZPlay.guide/Main"}', '@{"library" link "zz9k-library.guide/Main"}',
        "../README.md", "missing.md", "https://example.test", "logo.png",
        "@{b}code@{ub}", "@{b}bold@{ub}", "@{i}italic@{ui}",
        "An unknown snowman ? and an arrow ->.", "5us ~", "exact.",
        "| +--+ >",
    ]
    for value in expected:
        if value not in result:
            raise AssertionError("missing {!r}".format(value))
    if "**bold phrase" in result or "source lines**" in result:
        raise AssertionError("cross-line bold was not rendered")
    if '@{"local" link part}' not in result:
        raise AssertionError("missing local link form")
    intro = result.index("Intro wraps")
    contents = result.index("\nContents\n")
    toc = result.index('@{"Part" link part}')
    if not intro < contents < toc:
        raise AssertionError("contents does not follow Main intro")
    lines = result.splitlines()
    list_start = next(index for index, line in enumerate(lines) if line.startswith("- A list item"))
    list_end = next(index for index in range(list_start + 1, len(lines)) if not lines[index])
    if list_end == list_start + 1 or not lines[list_start + 1].startswith("  "):
        raise AssertionError("list item was split into multiple block units")
    if not all(lines[index].strip() for index in range(list_start, list_end)):
        raise AssertionError("blank line inside list item")
    second_item = next(index for index, line in enumerate(lines) if line == "- second item")
    if lines[second_item - 1] != "":
        raise AssertionError("list items lack separating blank lines")
    for line in lines:
        if line.startswith("@") and not line.startswith("@{b}"):
            continue
        visual = display_text(line)
        if line and len(visual) > width and len(visual.split()) > 1:
            raise AssertionError("flowing line exceeds width: {!r}".format(line))
    table_lines = [display_text(line) for line in lines
                   if " | " in line or (line and set(line) == {"-"})]
    if any(len(line) > width for line in table_lines):
        raise AssertionError("table exceeds width")
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
    parser.add_argument("--width", type=int, default=75)
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
        if arguments.width < 1:
            parser.error("--width must be positive")
        if not arguments.input:
            parser.error("INPUT.md is required")
        source = pathlib.Path(arguments.input).read_text(encoding="utf-8")
        result = guide_text(source, arguments.input, arguments.title, arguments.name, arguments.version, arguments.date,
                            arguments.sibling, arguments.width)
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
