#!/usr/bin/env python3
"""Check that the NeverD Agent's window shows text as text.

The window (tools/neverd-agent/ui) shows names from project files, paths, and
text a model wrote about a binary. Qt Quick reads a label whose format is left
to it as styled text when the text looks like markup, and styled text names
images that Qt then fetches. So every Label and Text element states
`textFormat: Text.PlainText`, rich text is confined to the one view whose
document loads no resources, and an Image's source is a literal path in the
application's own resources. A control that draws its own label (a button, a
menu item, a tool tip) either lays it out itself with such a Text or shows
only words written in the source."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
UI = REPOSITORY / "tools" / "neverd-agent" / "ui"

# Controls whose style draws `text` (or a Dialog's `title`) in a label of its
# own, in Qt's default format.
CONTROLS = ("Button", "ToolButton", "RoundButton", "DelayButton", "TabButton", "CheckBox",
            "RadioButton", "Switch", "MenuItem", "ItemDelegate", "CheckDelegate",
            "RadioDelegate", "SwitchDelegate", "SwipeDelegate", "ToolTip", "GroupBox", "Dialog")
ELEMENT = re.compile(r"(?<![\w.])(Label|Text|Image|" + "|".join(CONTROLS) + r")\s*\{")
RICH = re.compile(r"\b(?:Text|TextEdit)\.(?:RichText|StyledText|AutoText|MarkdownText)\b")
PLAIN = re.compile(r"\btextFormat\s*:\s*Text\.PlainText\b")
# The conversation's Markdown is laid out by MarkdownDocument, whose text
# document answers every resource request with nothing.
RICH_ALLOWED = {"MarkdownView.qml": "TextEdit.RichText"}


def skip(source: str, index: int) -> int | None:
    """The index after a string or comment that starts at index, if one does."""
    char = source[index]
    if char in "\"'`":
        end = index + 1
        while end < len(source) and source[end] != char:
            end += 2 if source[end] == "\\" else 1
        return end + 1
    if source.startswith("//", index):
        end = source.find("\n", index)
        return len(source) if end < 0 else end
    if source.startswith("/*", index):
        end = source.find("*/", index)
        return len(source) if end < 0 else end + 2
    return None


def code(source: str) -> str:
    """The source with its comments blanked, so only code is searched."""
    out, index = [], 0
    while index < len(source):
        after = skip(source, index)
        if after is None:
            out.append(source[index])
            index += 1
        else:
            text = source[index:after]
            out.append(text if text[0] in "\"'`" else re.sub(r"[^\n]", " ", text))
            index = after
    return "".join(out)


def own_body(source: str, brace: int) -> str:
    """What an element declares itself: its body without its children's."""
    out, depth, index = [], 0, brace
    while index < len(source):
        after = skip(source, index)
        if after is not None:
            if depth == 1:
                out.append(source[index:after])
            index = after
            continue
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                break
        elif depth == 1:
            out.append(char)
        index += 1
    return "".join(out)


LITERAL = re.compile(r'(?:qsTr\(\s*)?"(?:[^"\\]|\\.)*"\s*\)?')


def written(expression: str) -> bool:
    """Whether a binding can only yield words written in the source: a string,
    a qsTr of one, or a choice between such."""
    expression = expression.strip()
    if LITERAL.fullmatch(expression):
        return expression.startswith("qsTr(") == expression.endswith(")")
    depth, index, question, nested = 0, 0, -1, 0
    while index < len(expression):
        after = skip(expression, index)
        if after is not None:
            index = after
            continue
        char = expression[index]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif depth == 0 and char == "?":
            if question < 0:
                question = index
            else:
                nested += 1
        elif depth == 0 and char == ":" and question >= 0:
            if nested == 0:
                return (written(expression[question + 1:index])
                        and written(expression[index + 1:]))
            nested -= 1
        index += 1
    return False


def binding(body: str, name: str) -> str | None:
    """The expression an element's own body binds to a property, if any."""
    found = re.search(r"(?<![\w.])" + name + r"\s*:\s*(.+)", body)
    if not found:
        return None
    value, depth, index = found.group(1), 0, 0
    while index < len(value):
        after = skip(value, index)
        if after is not None:
            index = after
            continue
        if value[index] in "([{":
            depth += 1
        elif value[index] in ")]}":
            depth -= 1
        elif value[index] == ";" and depth == 0:
            break
        index += 1
    return value[:index].strip()


def check_text(name: str, source: str) -> tuple[list[str], int, int]:
    """Problems in one QML file, and how many labels and images it has."""
    problems, labels, images = [], 0, 0
    source = code(source)
    for match in RICH.finditer(source):
        if RICH_ALLOWED.get(name) != match.group(0):
            line = source.count("\n", 0, match.start()) + 1
            problems.append(f"{name}:{line}: {match.group(0)} lays out markup; "
                            "show the text with Text.PlainText")
    for match in ELEMENT.finditer(source):
        line = source.count("\n", 0, match.start()) + 1
        body = own_body(source, match.end() - 1)
        if match.group(1) == "Image":
            images += 1
            value = binding(body, "source") or ""
            if not re.fullmatch(r'"qrc:/[^"]*"', value):
                problems.append(f"{name}:{line}: an Image's source is a literal qrc:/ "
                                f"path, not {value or 'unset'}")
            continue
        if match.group(1) in CONTROLS:
            shown, own = ("title", "header") if match.group(1) == "Dialog" else ("text", "contentItem")
            value = binding(body, shown)
            if value is not None and binding(body, own) is None and not written(value):
                problems.append(f"{name}:{line}: {match.group(1)} draws {shown} in Qt's "
                                f"default format; give it a {own} with a plain Text")
            continue
        labels += 1
        if not PLAIN.search(body):
            problems.append(f"{name}:{line}: {match.group(1)} needs "
                            "textFormat: Text.PlainText")
    return problems, labels, images


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=UI,
                        help="QML directory to check (default: the Agent's ui)")
    args = parser.parse_args()
    files = sorted(args.directory.glob("*.qml"))
    problems, labels, images = [], 0, 0
    for path in files:
        found, own_labels, own_images = check_text(path.name, path.read_text(encoding="utf-8"))
        problems += found
        labels += own_labels
        images += own_images
    if not files:
        problems.append(f"No QML files in {args.directory}")
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print(f"Agent window text: {labels} labels in {len(files)} files are plain text; "
          f"{images} images come from the application's resources.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
