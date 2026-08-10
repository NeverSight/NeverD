from __future__ import annotations

import sys
import types
import unittest
from dataclasses import FrozenInstanceError
from unittest import mock


class PluginDecoratorTests(unittest.TestCase):
    def test_plugin_decorator_exports_validated_spec(self) -> None:
        module_name = "_neverd_plugin_test_valid_spec"
        module = types.ModuleType(module_name)
        sys.modules[module_name] = module
        try:
            exec(
                """
from neverd_plugin import Plugin

@Plugin(name="Example", version="1.2.3", author="Team")
class Example:
    pass
""",
                module.__dict__,
            )
            spec = module.__neverd_plugin__
            self.assertIs(spec.plugin_class, module.Example)
            self.assertEqual(spec.name, "Example")
            self.assertEqual(spec.version, "1.2.3")
            self.assertEqual(spec.version_info, (1, 2, 3))
            self.assertEqual(spec.author, "Team")
        finally:
            sys.modules.pop(module_name, None)

    def test_plugin_decorator_rejects_numeric_prerelease_with_leading_zero(
        self,
    ) -> None:
        from neverd_plugin import Plugin

        with self.assertRaisesRegex(ValueError, "strict SemVer"):
            Plugin(name="Example", version="1.2.3-01")

    def test_plugin_decorator_rejects_metadata_that_is_not_valid_utf8(self) -> None:
        from neverd_plugin import Plugin

        with self.assertRaisesRegex(ValueError, "UTF-8"):
            Plugin(name="bad\ud800name", version="1.0.0")

    def test_plugin_decorator_rejects_invalid_contract_values(self) -> None:
        from neverd_plugin import Plugin

        for version in ("1", "1.2", "01.2.3", "1.2.3-", "1.2.3+"):
            with self.subTest(version=version):
                with self.assertRaisesRegex(ValueError, "strict SemVer"):
                    Plugin(name="Example", version=version)
        with self.assertRaisesRegex(ValueError, "non-empty"):
            Plugin(name="", version="1.0.0")
        with self.assertRaisesRegex(ValueError, "NUL"):
            Plugin(name="bad\0name", version="1.0.0")
        with self.assertRaisesRegex(TypeError, "boolean"):
            Plugin(name="Example", version="1.0.0", type=True)
        with self.assertRaisesRegex(ValueError, "unknown plugin type"):
            Plugin(name="Example", version="1.0.0", type="unknown")

    def test_plugin_spec_is_immutable_and_module_allows_only_one(self) -> None:
        module_name = "_neverd_plugin_test_duplicate"
        module = types.ModuleType(module_name)
        sys.modules[module_name] = module
        try:
            exec(
                """
from neverd_plugin import Plugin

@Plugin(name="First", version="1.0.0")
class First:
    pass
""",
                module.__dict__,
            )
            with self.assertRaises(FrozenInstanceError):
                module.__neverd_plugin__.name = "Changed"
            with self.assertRaisesRegex(RuntimeError, "only one"):
                exec(
                    """
@Plugin(name="Second", version="1.0.0")
class Second:
    pass
""",
                    module.__dict__,
                )
        finally:
            sys.modules.pop(module_name, None)


class _FakeNativeBridge:
    def __init__(self) -> None:
        self.active = True

    def session_address(self, _handle: object) -> int:
        if not self.active:
            raise RuntimeError("NeverD session context is no longer active")
        return 0x1234


class _FailIfCalledHost:
    def owned_string(self, *_arguments: object) -> str:
        raise AssertionError("FFI must not run for a stale session")


class _RecordingHost:
    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def call(self, name: str, *arguments: object) -> object:
        self.calls.append((name, arguments))
        values = {
            "neverd_session_is_loaded": 1,
            "neverd_session_bitness": 64,
            "neverd_func_count": 3,
        }
        return values[name]

    def owned_string(self, name: str, *arguments: object) -> str:
        self.calls.append((name, arguments))
        values = {
            "neverd_session_file_path": "/tmp/input.bin",
            "neverd_session_arch_name": "x86_64",
            "neverd_project_name": "NeverD",
            "neverd_version_number": "3389.0.1",
        }
        return values[name]


class _StatusHost:
    def __init__(self) -> None:
        self.results = {
            "neverd_session_analyze": 1,
            "neverd_annotations_save": 0,
            "neverd_annotations_load": 0,
            "neverd_rename_func": 0,
        }

    def call(self, name: str, *_arguments: object) -> object:
        return self.results[name]

    def owned_string(self, name: str, *_arguments: object) -> str:
        if name == "neverd_last_error":
            return "host failure"
        raise AssertionError(name)


class _BorrowingHost:
    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def borrowed_bytes(self, name: str, *arguments: object) -> bytes:
        self.calls.append((name, arguments))
        return b"\x01\x02"


class SessionTests(unittest.TestCase):
    def test_default_host_is_lazy_and_cached(self) -> None:
        import neverd_plugin.api as api_module
        from neverd_plugin import Session

        host = _RecordingHost()
        with mock.patch.object(api_module, "HostAPI", return_value=host) as factory:
            session = Session(object(), _native=_FakeNativeBridge())

            factory.assert_not_called()
            self.assertTrue(session.loaded)
            self.assertEqual(session.project_name, "NeverD")
            factory.assert_called_once_with()

    def test_stale_default_session_is_rejected_before_host_loading(self) -> None:
        import neverd_plugin.api as api_module
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        with mock.patch.object(api_module, "HostAPI") as factory:
            session = Session(object(), _native=native)
            native.active = False

            with self.assertRaisesRegex(RuntimeError, "no longer active"):
                _ = session.file_path
            factory.assert_not_called()

    def test_raw_borrowed_buffers_offer_global_and_lifetime_checked_calls(
        self,
    ) -> None:
        from neverd_plugin import Session

        host = _BorrowingHost()
        session = Session(object(), _native=_FakeNativeBridge(), _host=host)

        self.assertEqual(session.raw.borrowed_bytes("global", 7), b"\x01\x02")
        self.assertEqual(
            session.raw.session_borrowed_bytes("neverd_roundtrip_obj"),
            b"\x01\x02",
        )
        self.assertEqual(host.calls[0], ("global", (7,)))
        self.assertEqual(host.calls[1][0], "neverd_roundtrip_obj")
        self.assertEqual(host.calls[1][1][0].value, 0x1234)

    def test_stale_session_is_rejected_before_ffi_access(self) -> None:
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        session = Session(object(), _native=native, _host=_FailIfCalledHost())
        native.active = False

        with self.assertRaisesRegex(RuntimeError, "no longer active"):
            _ = session.file_path

    def test_typed_session_queries_revalidate_the_native_handle(self) -> None:
        from neverd_plugin import Session

        native = _FakeNativeBridge()
        host = _RecordingHost()
        session = Session(object(), _native=native, _host=host)

        self.assertTrue(session.loaded)
        self.assertEqual(session.file_path, "/tmp/input.bin")
        self.assertEqual(session.architecture, "x86_64")
        self.assertEqual(session.bitness, 64)
        self.assertEqual(session.function_count, 3)
        self.assertEqual(session.project_name, "NeverD")
        self.assertEqual(session.version, "3389.0.1")
        pointer_arguments = [
            arguments[0]
            for name, arguments in host.calls
            if name not in {"neverd_project_name", "neverd_version_number"}
        ]
        self.assertEqual(len(pointer_arguments), 5)
        self.assertTrue(all(argument.value == 0x1234 for argument in pointer_arguments))

    def test_session_respects_boolean_and_zero_success_conventions(self) -> None:
        from neverd_plugin import NeverDError, Session

        host = _StatusHost()
        session = Session(object(), _native=_FakeNativeBridge(), _host=host)
        session.analyze()
        session.save_annotations()
        session.load_annotations()
        session.rename_function("old", "new")

        host.results["neverd_annotations_save"] = 1
        with self.assertRaisesRegex(NeverDError, "host failure"):
            session.save_annotations()
        host.results["neverd_session_analyze"] = 0
        with self.assertRaisesRegex(NeverDError, "host failure"):
            session.analyze()


class EventTests(unittest.TestCase):
    def test_all_event_variants_copy_only_their_relevant_payload(self) -> None:
        from neverd_plugin import Event, EventType, Session

        session = Session(
            object(), _native=_FakeNativeBridge(), _host=_FailIfCalledHost()
        )
        loaded = Event.from_host(session, EventType.BINARY_LOADED, path="/tmp/a.bin")
        selected = Event.from_host(
            session,
            EventType.FUNCTION_SELECTED,
            address=0x401000,
            name="main",
        )
        changed = Event.from_host(session, EventType.ADDRESS_CHANGED, address=0x402000)
        closing = Event.from_host(session, EventType.BINARY_CLOSING)
        analyzed = Event.from_host(session, EventType.ANALYSIS_DONE)
        patched = Event.from_host(
            session,
            EventType.PATCH_APPLIED,
            output_path="/tmp/patched.bin",
            code_size=17,
        )

        self.assertEqual(loaded.path, "/tmp/a.bin")
        self.assertEqual((selected.address, selected.name), (0x401000, "main"))
        self.assertEqual(changed.address, 0x402000)
        self.assertIsNone(closing.path)
        self.assertIsNone(analyzed.address)
        self.assertEqual(
            (patched.output_path, patched.code_size), ("/tmp/patched.bin", 17)
        )
        with self.assertRaisesRegex(ValueError, "requires path"):
            Event.from_host(session, EventType.BINARY_LOADED)


if __name__ == "__main__":
    unittest.main()
