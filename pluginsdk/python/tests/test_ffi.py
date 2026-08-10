from __future__ import annotations

import ctypes
import sys
import unittest
from unittest import mock


class _FakeFunction:
    def __init__(self, implementation):
        self.implementation = implementation
        self.argtypes = None
        self.restype = None

    def __call__(self, *arguments):
        return self.implementation(*arguments)


class _FakeLibrary:
    def __init__(self) -> None:
        self.buffer = ctypes.create_string_buffer("NeverD ✓".encode())
        self.binary = (ctypes.c_ubyte * 3)(0x00, 0x7F, 0xFF)
        self.freed: list[int] = []
        self.neverd_version = _FakeFunction(lambda: ctypes.addressof(self.buffer))
        self.neverd_free_string = _FakeFunction(self._free)
        self.neverd_roundtrip_obj = _FakeFunction(self._roundtrip_obj)

    def _free(self, pointer) -> None:
        address = ctypes.cast(pointer, ctypes.c_void_p).value
        self.freed.append(int(address or 0))

    def _roundtrip_obj(self, _session, out_length):
        ctypes.cast(out_length, ctypes.POINTER(ctypes.c_ulonglong))[0] = 3
        return ctypes.cast(self.binary, ctypes.POINTER(ctypes.c_ubyte))


class HostAPITests(unittest.TestCase):
    def test_owned_string_is_decoded_and_freed_exactly_once(self) -> None:
        from neverd_plugin.ffi import HostAPI

        library = _FakeLibrary()
        api = HostAPI(library)

        self.assertEqual(api.owned_string("neverd_version"), "NeverD ✓")
        self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])
        self.assertIs(library.neverd_version.restype, ctypes.c_void_p)
        self.assertEqual(library.neverd_version.argtypes, [])

    def test_owned_string_is_freed_when_strict_utf8_decode_fails(self) -> None:
        from neverd_plugin.ffi import HostAPI

        library = _FakeLibrary()
        library.buffer = ctypes.create_string_buffer(b"\xff")
        api = HostAPI(library)

        with self.assertRaises(UnicodeDecodeError):
            api.owned_string("neverd_version")
        self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])

    def test_null_owned_string_is_not_freed(self) -> None:
        from neverd_plugin.ffi import HostAPI

        library = _FakeLibrary()
        library.neverd_version = _FakeFunction(lambda: 0)
        api = HostAPI(library)

        self.assertIsNone(api.owned_string("neverd_version"))
        self.assertEqual(library.freed, [])

    def test_borrowed_buffer_is_copied_before_returning(self) -> None:
        from neverd_plugin.ffi import HostAPI

        library = _FakeLibrary()
        api = HostAPI(library)

        result = api.borrowed_bytes("neverd_roundtrip_obj", ctypes.c_void_p(1))
        library.binary[1] = 0

        self.assertEqual(result, b"\x00\x7f\xff")

    def test_borrowed_buffer_rejects_null_with_nonzero_length(self) -> None:
        from neverd_plugin.ffi import HostAPI

        def invalid_buffer(_session, out_length):
            ctypes.cast(out_length, ctypes.POINTER(ctypes.c_ulonglong))[0] = 4
            return None

        library = _FakeLibrary()
        library.neverd_roundtrip_obj = _FakeFunction(invalid_buffer)
        api = HostAPI(library)

        with self.assertRaisesRegex(RuntimeError, "null buffer"):
            api.borrowed_bytes("neverd_roundtrip_obj", ctypes.c_void_p(1))

    def test_default_host_reports_actionable_error_outside_neverd(self) -> None:
        from neverd_plugin.ffi import HostAPI, HostUnavailableError

        with mock.patch.dict(sys.modules, {"_neverd_plugin": None}):
            with self.assertRaisesRegex(
                HostUnavailableError, "Python-enabled NeverD plugin"
            ):
                HostAPI()


if __name__ == "__main__":
    unittest.main()
