from __future__ import annotations

import os
import ctypes
import builtins

from neverd_plugin import Event, Plugin, Session
from neverd_plugin import abi
import _neverd_plugin


_layout = _neverd_plugin.abi_layout()
assert _layout["session_handle_size"] == ctypes.sizeof(abi.SessionHandle)
assert _layout["virtual_address_size"] == ctypes.sizeof(abi.VirtualAddress)
assert _layout["event_size"] == ctypes.sizeof(abi.NeverDEvent)
assert _layout["event_type_offset"] == abi.NeverDEvent.Type.offset
assert _layout["event_session_offset"] == abi.NeverDEvent.Session.offset
assert _layout["event_data_offset"] == abi.NeverDEvent.Data.offset
assert _layout["plugin_size"] == ctypes.sizeof(abi.NeverDPlugin)
assert _layout["plugin_name_offset"] == abi.NeverDPlugin.Name.offset
assert _layout["plugin_version_offset"] == abi.NeverDPlugin.Version.offset
assert _layout["plugin_type_offset"] == abi.NeverDPlugin.Type.offset
assert _layout["plugin_init_offset"] == abi.NeverDPlugin.Init.offset
assert _layout["plugin_event_offset"] == abi.NeverDPlugin.Event.offset


_session: Session | None = None


def _record(line: str) -> None:
    with open(
        os.environ["NEVERD_PYTHON_PLUGIN_TRACE"], "a", encoding="utf-8"
    ) as stream:
        stream.write(line + "\n")


@Plugin(name="Python Runtime Fixture", version="1.0.0")
class RuntimeLifecyclePlugin:
    def on_init(self, session: Session) -> None:
        global _session
        _session = session
        builtins._neverd_stale_session = session
        _record("init")

    def on_run(self, session: Session, arg: int) -> int:
        _record(f"run:{arg}")
        return arg + 9

    def on_event(self, event: Event) -> None:
        _record(f"event:{event.type.name}:{event.path or ''}")

    def on_term(self) -> None:
        assert _session is not None
        try:
            _ = _session.file_path
        except RuntimeError as error:
            if "no longer active" not in str(error):
                raise
            _record("stale")
        else:
            raise AssertionError("terminated Session remained active")
        _record("term")
