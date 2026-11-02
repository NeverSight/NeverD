#!/usr/bin/env python3
"""Compare the workbench's commands, shortcuts and menus with IDA's.

tools/neverd-gui/app/IdaActions.def pairs each workbench command with the IDA
command that does the same, under IDA's default shortcut.  This script reads
that table with Actions.def and, given an IDA installation, IDA's own
configuration (cfg/idagui.cfg: the default "legacy" shortcut scheme and the
main menu layout).  It fails when a row names a command IDA does not have or a
key IDA does not give it, and reports, menu by menu, which of IDA's commands
the workbench has, plans and lacks, and which of its keys IDA spends on
another command.  Without an installation it reports the table alone.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
APP = REPOSITORY / "tools" / "neverd-gui" / "app"
STRING = re.compile(r'"([^"]*)"')
SHARED = re.compile(r'NEVERD_IDA_ACTION\(\s*"([^"]+)",\s*"([^"]*)",\s*(\w+)\)')
PLANNED = re.compile(r'NEVERD_IDA_PLANNED\(\s*"([^"]+)",\s*"([^"]*)",\s*"([^"]+)"\)')
ACTION = re.compile(
    r'NEVERD_(?:VIEW_)?ACTION\(\s*(\w+),\s*QT_TRANSLATE_NOOP\("Actions",\s*'
    r'((?:"[^"]*"\s*)+)\),\s*((?:"[^"]*"\s*)+),')
# The preprocessor symbols idagui.cfg tests, as IDA defines them on Linux.
DEFINED = frozenset({"__QT__", "__LINUX__", "__UNIX__", "__EA64__"})
# IDA's main menus, and the variants it shows while one kind of window has
# the focus (lists, folder trees, local types, registers).  Menus IDA fills at
# run time and those of features the workbench does not model (collaboration,
# the debugger, the metadata server, merging) are left out.
MAIN_MENU = re.compile(r"^(?:File|Default:\w+|View|Options|Windows|Help)$")
WINDOW_MENU = re.compile(r"^(?!Live:|Default:|diffmerge:)[\w ]+:\w+$")
# IDA's names for keys, against Qt's portable ones.
KEY_NAMES = {"Enter": "Return", "Escape": "Esc"}
MODIFIERS = ("Ctrl", "Shift", "Alt", "Meta")
# The platform keys ("sys(Name)") the table and IDA's menus use, on Linux.
SYSTEM_KEYS = {
    "Copy": "Ctrl+C", "Undo": "Ctrl+Z", "Redo": "Ctrl+Shift+Z",
    "ZoomIn": "Ctrl++", "ZoomOut": "Ctrl+-", "Find": "Ctrl+F",
    "SelectAll": "Ctrl+A", "HelpContents": "F1",
}


def portable(spelling: str) -> str:
    """IDA's spelling of a key ("Ctrl-Shift-Down", "sys(Copy)") in Qt's
    portable text, with the modifiers in Qt's order."""
    if spelling.startswith("sys(") and spelling.endswith(")"):
        return SYSTEM_KEYS.get(spelling[4:-1], spelling)
    held, rest = set(), spelling
    while True:
        for modifier in MODIFIERS:
            prefix = modifier + "-"
            if len(rest) > len(prefix) and rest.lower().startswith(prefix.lower()):
                held.add(modifier)
                rest = rest[len(prefix):]
                break
        else:
            break
    return "+".join([m for m in MODIFIERS if m in held] + [KEY_NAMES.get(rest, rest)])


def read_table():
    text = (APP / "IdaActions.def").read_text(encoding="utf-8")
    shared = [dict(ida=m[1], key=m[2], neverd=m[3]) for m in SHARED.finditer(text)]
    planned = [dict(ida=m[1], key=m[2], group=m[3]) for m in PLANNED.finditer(text)]
    actions = {}
    for match in ACTION.finditer((APP / "Actions.def").read_text(encoding="utf-8")):
        # QKeySequence's list format separates alternative bindings with "; ".
        spelling = "".join(STRING.findall(match[3]))
        keys = spelling.split("; ") if spelling else []
        actions[match[1]] = dict(text="".join(STRING.findall(match[2])).replace("&", ""),
                                 key=keys[0] if keys else "", keys=keys)
    return shared, planned, actions


def preprocess(text: str) -> str:
    """Keeps the lines idagui.cfg selects on Linux and drops comments."""
    kept, stack = [], []
    for line in text.splitlines():
        directive = line.strip()
        if directive.startswith("#ifdef ") or directive.startswith("#ifndef "):
            symbol = directive.split()[1]
            stack.append((symbol in DEFINED) == directive.startswith("#ifdef "))
        elif directive.startswith("#if "):
            stack.append("__IDAFREE__" in directive and directive.startswith("#if !"))
        elif directive.startswith("#else"):
            stack[-1] = not stack[-1]
        elif directive.startswith("#endif"):
            stack.pop()
        elif directive.startswith("#"):
            continue
        elif all(stack):
            kept.append(re.sub(r"\s*//.*$", "", line))
    return "\n".join(kept)


def read_ida(installation: Path):
    """IDA's default shortcuts, by command, and its static menus."""
    text = preprocess((installation / "cfg" / "idagui.cfg").read_text(encoding="latin-1"))
    legacy = text[text.index('"legacy" : {'):text.index('"new" : {')]
    shortcuts: dict[str, list[str]] = {}
    for match in re.finditer(r'^\s*"([^"]+)"\s*:\s*(\[[^\]]*\]|"[^"]*")', legacy, re.M):
        keys = shortcuts.setdefault(match[1], [])
        keys += [portable(key) for key in STRING.findall(match[2]) if key]
    start = text.index("MENUS_LAYOUT = [") + len("MENUS_LAYOUT = ")
    end = text.index("\n]", start) + 2
    layout = json.loads(re.sub(r",(\s*[\]}])", r"\1", text[start:end]))
    menus: dict[str, list[str]] = {}
    window_menus: dict[str, list[str]] = {}

    def walk(entry, path, into, nested=True):
        for item in entry.get("actions", []):
            if isinstance(item, dict):
                label = item.get("label", "").replace("&", "")
                walk(item, path + [label] if nested else path, into, nested)
            elif item:
                into.setdefault(" > ".join(path), []).append(item)

    for top in layout:
        name, label = top.get("name", ""), top.get("label", "").replace("&", "")
        if MAIN_MENU.match(name):
            walk(top, [label], menus)
        elif WINDOW_MENU.match(name):
            # One row per kind of window, whatever menu the command is in.
            walk(top, [f"`{name.split(':')[0]}` window"], window_menus, False)
    decompiler = b"".join(path.read_bytes() for path in
                          sorted((installation / "plugins").glob("hexx64.*")))
    return shortcuts, menus, window_menus, decompiler


def check(shared, planned, shortcuts, decompiler):
    """Rows that name a command IDA lacks, or a key IDA does not give it."""
    errors = []
    for row in shared + planned:
        if row["ida"].startswith("hx:"):
            if decompiler and row["ida"].encode() + b"\0" not in decompiler:
                errors.append(f"{row['ida']}: not a decompiler command")
        elif row["ida"] not in shortcuts:
            errors.append(f"{row['ida']}: not an IDA command")
        elif row["key"] and portable(row["key"]) not in shortcuts[row["ida"]]:
            errors.append(f"{row['ida']}: IDA's keys are {shortcuts[row['ida']] or 'none'}, "
                          f"the table says {row['key']}")
        elif not row["key"] and shortcuts[row["ida"]]:
            errors.append(f"{row['ida']}: IDA's keys are {shortcuts[row['ida']]}, the table has none")
    return errors


def menu_table(menus, by_ida, planned_ida, missing):
    """One row per menu: IDA's commands, and those the workbench has or
    plans.  Appends the others to missing, unless it is None."""
    lines = ["| IDA menu | IDA commands | The workbench has | Planned | Not yet |",
             "|---|---:|---:|---:|---:|"]
    totals = [0, 0, 0]
    for path, items in menus.items():
        unique = list(dict.fromkeys(items))
        have = [item for item in unique if item in by_ida]
        plan = [item for item in unique if item in planned_ida]
        lack = [item for item in unique if item not in by_ida and item not in planned_ida]
        totals = [totals[0] + len(unique), totals[1] + len(have), totals[2] + len(plan)]
        lines.append(f"| {path} | {len(unique)} | {len(have)} | {len(plan)} | {len(lack)} |")
        if lack and missing is not None:
            missing.append(f"- **{path}:** " + ", ".join(f"`{item}`" for item in lack))
    lines.append(f"| **All** | **{totals[0]}** | **{totals[1]}** | **{totals[2]}** | "
                 f"**{totals[0] - totals[1] - totals[2]}** |")
    return lines


def report(shared, planned, actions, shortcuts, menus, window_menus):
    by_ida = {row["ida"]: row for row in shared}
    planned_ida = {row["ida"] for row in planned}
    lines = ["# Workbench commands against IDA's", ""]
    own = sorted(set(actions) - {row["neverd"] for row in shared})
    lines += [f"The workbench has {len(actions)} commands. "
              f"{len(actions) - len(own)} of them are IDA commands ({len(shared)} rows of "
              f"IdaActions.def), under IDA's default keys, and {len(own)} are its own. "
              f"{len(planned)} more IDA commands are planned.", ""]
    if menus:
        missing = []
        lines += ["## IDA's main menus", ""] + menu_table(menus, by_ida, planned_ida, missing)
        lines += ["", "IDA's menus for one kind of window:", ""]
        lines += menu_table(window_menus, by_ida, planned_ida, None)
        lines += ["", "### Main-menu commands the workbench does not have yet", ""]
        lines += missing + [""]
        clashes = []
        for name in own:
            for key in actions[name]["keys"]:
                owners = sorted(command for command, keys in shortcuts.items() if key in keys)
                if owners:
                    clashes.append(f"- `{key}` runs {actions[name]['text']} ({name}); IDA gives it to "
                                   + ", ".join(f"`{owner}`" for owner in owners))
        lines += ["## Keys of the workbench's own commands that IDA gives another command", "",
                  "IDA scopes some keys to one kind of window, such as a list's quick filter.", ""]
        lines += clashes or ["None."]
        lines.append("")
    lines += ["## The workbench's own commands", "",
              "| Command | Key |", "|---|---|"]
    lines += [f"| {actions[name]['text']} | {', '.join(actions[name]['keys']) or '-'} |" for name in own]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ida", type=Path, help="an IDA installation directory")
    parser.add_argument("--output", type=Path, help="write the Markdown report here")
    args = parser.parse_args()
    shared, planned, actions = read_table()
    errors = [f"{row['neverd']}: not a workbench command" for row in shared
              if row["neverd"] not in actions]
    errors += [f"{row['neverd']}: has {actions[row['neverd']]['key'] or 'no key'}, IDA's "
               f"{row['ida']} has {portable(row['key']) if row['key'] else 'none'}"
               for row in shared if row["neverd"] in actions
               and actions[row["neverd"]]["key"] != (portable(row["key"]) if row["key"] else "")
               and not row["key"].startswith("sys(")]
    shortcuts, menus, window_menus = {}, {}, {}
    if args.ida:
        shortcuts, menus, window_menus, decompiler = read_ida(args.ida)
        errors += check(shared, planned, shortcuts, decompiler)
    text = report(shared, planned, actions, shortcuts, menus, window_menus)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
