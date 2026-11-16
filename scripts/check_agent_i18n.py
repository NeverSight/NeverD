#!/usr/bin/env python3
"""Validate the NeverD Agent catalogs: the desktop Agent and its CLI
(tools/neverd-agent/i18n) and the workbench conversation plugin
(plugins/neverd-agent-ui/i18n). Each ships the workbench's locales with
complete coverage, matching placeholders and line breaks, and an English
catalog equal to its sources."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from check_docs_i18n import LOCALES
from check_gui_i18n import PLACEHOLDER, messages

REPOSITORY = Path(__file__).resolve().parents[1]
AGENT = REPOSITORY / "tools" / "neverd-agent"
PLUGIN = REPOSITORY / "plugins" / "neverd-agent-ui"


def agent_sources():
    """The Agent's QML and C++: the window, its client and the CLI runtime."""
    patterns = ("*.cpp", "ui/*.qml", "app/*.cpp", "bridge/*.cpp", "client/*.cpp",
                "models/*.cpp", "platform/*.cpp", "project/*.cpp", "runtime/*.cpp")
    return [str(path) for pattern in patterns for path in sorted(AGENT.glob(pattern))]


def plugin_sources():
    """The plugin's sources and the Agent client code it compiles."""
    return [*(str(path) for path in sorted(PLUGIN.glob("*.cpp"))),
            str(AGENT / "client" / "AgentProcess.cpp"),
            str(AGENT / "platform" / "AppPaths.cpp")]


CATALOGS = (
    ("Agent", AGENT / "i18n", "neverd-agent", agent_sources),
    ("Agent plugin", PLUGIN / "i18n", "neverd-agent-ui", plugin_sources),
)


def check(name, directory, stem, sources, lupdate, errors):
    locales = ("en", *(locale.replace("-", "_") for locale in LOCALES))
    expected = {f"{stem}_{locale}.ts" for locale in locales}
    actual = {path.name for path in directory.glob("*.ts")}
    if expected != actual:
        errors.append(f"{name}: locale files missing {sorted(expected - actual)}, "
                      f"extra {sorted(actual - expected)}")
    try:
        _, reference = messages(directory / f"{stem}_en.ts")
    except (OSError, ET.ParseError, ValueError) as error:
        errors.append(f"{name}: {error}")
        return 0
    if not reference:
        errors.append(f"{name}: English source catalog is empty")
    for locale in locales:
        path = directory / f"{stem}_{locale}.ts"
        if not path.exists():
            continue
        try:
            root, entries = messages(path)
        except (OSError, ET.ParseError, ValueError) as error:
            errors.append(str(error))
            continue
        if root.get("language") != locale:
            errors.append(f"{path.name}: expected language={locale}")
        if set(entries) != set(reference):
            errors.append(f"{path.name}: missing {sorted(set(reference) - set(entries))!r}; "
                          f"extra {sorted(set(entries) - set(reference))!r}")
        for key, translation in entries.items():
            source = key[1]
            if translation is None or translation.get("type") == "unfinished":
                errors.append(f"{path.name}: unfinished {key[:2]!r}")
                continue
            forms = translation.findall("numerusform") if key[3] == "yes" else [translation]
            if not forms:
                errors.append(f"{path.name}: empty plural forms {key[:2]!r}")
            for form in forms:
                value = "".join(form.itertext())
                if not value.strip():
                    errors.append(f"{path.name}: empty translation {key[:2]!r}")
                if Counter(PLACEHOLDER.findall(value)) != Counter(PLACEHOLDER.findall(source)):
                    errors.append(f"{path.name}: placeholder mismatch {key[:2]!r}")
                if value.count("\n") != source.count("\n"):
                    errors.append(f"{path.name}: multiline layout mismatch {key[:2]!r}")
                if locale == "en" and key[3] != "yes" and value != source:
                    errors.append(f"{path.name}: English catalog differs from source {key[:2]!r}")
    if lupdate:
        with tempfile.TemporaryDirectory(prefix="neverd-agent-i18n-") as temporary:
            catalog = Path(temporary) / "sources.ts"
            result = subprocess.run([lupdate, *sources(), "-no-obsolete", "-source-language", "en",
                                     "-ts", str(catalog)], capture_output=True, text=True, timeout=120)
            if result.returncode:
                errors.append(f"{name}: lupdate failed: {result.stderr}")
            else:
                _, found = messages(catalog)
                if set(found) != set(reference):
                    errors.append(f"{name}: English catalog is stale: missing "
                                  f"{sorted(set(found) - set(reference))!r}; obsolete "
                                  f"{sorted(set(reference) - set(found))!r}")
    return len(reference)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-sources", action="store_true",
                        help="Use lupdate to check the catalogs against the sources")
    parser.add_argument("--lupdate", help="Explicit lupdate executable; implies --check-sources")
    args = parser.parse_args()
    lupdate = None
    errors = []
    if args.check_sources or args.lupdate:
        lupdate = args.lupdate or shutil.which("lupdate") or shutil.which("lupdate6")
        if not lupdate:
            errors.append("Source validation requires lupdate; supply --lupdate PATH")
    counts = [check(*catalog, lupdate, errors) for catalog in CATALOGS]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Agent translations: " + ", ".join(
        f"{name} {count} source keys" for (name, *_), count in zip(CATALOGS, counts)) +
        f"; {len(LOCALES) + 1} locales with complete coverage and matching placeholders.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
