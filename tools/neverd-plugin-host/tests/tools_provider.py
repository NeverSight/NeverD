"""Actual child-process command fixture; the C++ host controls reply timing."""
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(root / "pluginsdk/gui"), str(root / "pluginsdk/common")]
from neverd_tools_plugin import run


def handler(task):
    task.command("rename", address="0x1000", name="first")
    if task.action == "twice":
        task.command("rename", address="0x1000", name="second")
    return dict(views=[], data=1)


sys.exit(run(handler))
