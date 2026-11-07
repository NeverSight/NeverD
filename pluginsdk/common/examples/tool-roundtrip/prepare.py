"""Prepare the two-tool external plugin; no NeverD build integration is required."""
import argparse
import json
from pathlib import Path
import shutil
import sys

# Prepared paths have a stable UTF-8 representation even on an English Windows pipe.
sys.stdout.reconfigure(encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("plugin.py", "tools.json", "caller.py", "README.md"):
        shutil.copy2(source / name, destination / name)
    shutil.copy2(source.parents[1] / "neverd_tools_plugin.py", destination / "neverd_tools_plugin.py")
    shutil.copy2(source.parents[2] / "gui/neverd_gui_plugin.py", destination / "neverd_gui_plugin.py")
    manifest = json.loads((source / "plugin.json").read_text(encoding="utf-8"))
    manifest["program"] = sys.executable
    (destination / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(destination / "plugin.json")


if __name__ == "__main__":
    main()
