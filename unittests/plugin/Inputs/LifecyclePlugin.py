from __future__ import annotations

import os

from neverd_plugin import Event, Plugin, PluginType, Session


def _record(line: str) -> None:
    path = os.environ["NEVERD_PYTHON_PLUGIN_TRACE"]
    with open(path, "a", encoding="utf-8") as stream:
        stream.write(line + "\n")


@Plugin(
    name="Python Lifecycle Fixture",
    version="2.3.4",
    author="NeverD tests",
    description="Exercises all Python plugin callbacks",
    type=PluginType.GENERIC,
)
class LifecyclePlugin:
    def on_init(self, session: Session) -> int:
        _record(f"init:{session.project_name}:{session.version}")
        return 0

    def on_run(self, session: Session, arg: int) -> int:
        _record(f"run:{arg}")
        return arg + 3

    def on_event(self, event: Event) -> int:
        _record(f"event:{event.type.name}:{event.path or ''}")
        return 0

    def on_term(self) -> None:
        _record("term")
