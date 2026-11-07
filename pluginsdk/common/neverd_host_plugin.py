"""Frontend-independent entry to the shared process provider helper.

The historical neverd_gui_plugin module owns framing and compatibility.
Services and events work in both GUI and headless NPH hosts.
"""
from neverd_gui_plugin import QueryError, Task, run

__all__ = ["QueryError", "Task", "run"]
