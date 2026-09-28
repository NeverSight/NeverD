"""Check interpreter-recovery struct and owned-report ABI against libneverd."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import unittest

from neverd_plugin import abi
from neverd_plugin.ffi import HostAPI


class DevirtualizationIntegrationTests(unittest.TestCase):
    def test_native_failure_report_preserves_owned_pointer(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(path)))
        session = host.call("neverd_session_create")
        self.assertTrue(session)
        self.addCleanup(host.call, "neverd_session_destroy", session)
        for version, options in (
            (1, abi.NeverDDevirtualizeOptionsV1()),
            (2, abi.NeverDDevirtualizeOptionsV2()),
        ):
            if version == 1:
                options.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize flags"
            else:
                options.base.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize v2 flags"
            for prefix, source_abi in (
                ("source", "ordinary-source"),
                ("machine_source", "x64-machine-state-v1"),
            ):
                entry_point = f"neverd_devirtualize_{prefix}_v{version}"
                with self.subTest(entry_point=entry_point):
                    report = ctypes.c_void_p()
                    source = host.owned_string(
                        entry_point, session, 0,
                        ctypes.byref(options), ctypes.byref(report),
                    )
                    self.assertIsNone(source)
                    self.assertTrue(report.value)
                    try:
                        result = json.loads(ctypes.string_at(report.value))
                        self.assertFalse(result["complete"])
                        self.assertEqual(result["error"], expected_error)
                        self.assertEqual(result["sourceABI"], source_abi)
                    finally:
                        host.call("neverd_free_string", ctypes.cast(report, ctypes.c_char_p))



if __name__ == "__main__":
    unittest.main()
