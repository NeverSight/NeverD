"""Prepare an independent GUI/CLI plugin with the current Python executable."""
import argparse
import json
from pathlib import Path
import shutil
import sys

# Prepared paths have a stable UTF-8 representation even on an English Windows pipe.
sys.stdout.reconfigure(encoding="utf-8")

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parent
destination = args.destination.resolve()
destination.mkdir(parents=True, exist_ok=True)
for name in ("observer.py", "README.md"):
    shutil.copy2(source / name, destination / name)
shutil.copy2(source.parents[1] / "neverd_host_plugin.py", destination / "neverd_host_plugin.py")
shutil.copy2(source.parents[2] / "gui/neverd_gui_plugin.py", destination / "neverd_gui_plugin.py")
manifest = json.loads((source / "plugin.json").read_text(encoding="utf-8"))
manifest["program"] = sys.executable
(destination / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(destination / "plugin.json")
