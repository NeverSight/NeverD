"""Copy a complete standalone plugin into a chosen external directory."""
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
    shutil.copy2(source / "explorer.py", destination / "explorer.py")
    shutil.copytree(source / "icons", destination / "icons", dirs_exist_ok=True)
    shutil.copy2(source.parents[1] / "neverd_gui_plugin.py", destination / "neverd_gui_plugin.py")
    readme = (source / "README.md").read_text(encoding="utf-8")
    readme = readme.replace("[full protocol, limits and lifecycle](../../../../docs/gui-plugins.md)",
                            "full protocol, limits and lifecycle in `docs/gui-plugins.md` in the NeverD source tree")
    (destination / "README.md").write_text(
        "This is a prepared copy. Import the adjacent `plugin.json` in NeverD GUI.\n\n" + readme,
        encoding="utf-8")
    for name in ("plugin.json", "plugin-developer.json"):
        manifest = json.loads((source / name).read_text(encoding="utf-8"))
        manifest["program"] = sys.executable
        path = destination / name
        path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(destination / "plugin.json")


if __name__ == "__main__":
    main()
