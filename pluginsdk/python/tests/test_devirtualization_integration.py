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
            (3, abi.NeverDDevirtualizeOptionsV3()),
            (4, abi.NeverDDevirtualizeOptionsV4()),
            (5, abi.NeverDDevirtualizeOptionsV5()),
            (6, abi.NeverDDevirtualizeOptionsV6()),
            (7, abi.NeverDDevirtualizeOptionsV7()),
            (8, abi.NeverDDevirtualizeOptionsV8()),
        ):
            if version == 1:
                options.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize flags"
            elif version == 2:
                options.base.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize v2 flags"
            elif version == 3:
                options.base.base.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize v3 flags"
            elif version == 4:
                options.base.base.base.struct_size = ctypes.sizeof(options)
                options.flags = 4
                expected_error = "invalid devirtualize v4 flags"
            elif version == 5:
                options.base.base.base.base.struct_size = ctypes.sizeof(options)
                options.entry_frame_residue = 1
                expected_error = "entry residue requires an alignment"
            elif version == 6:
                options.base.base.base.base.base.struct_size = ctypes.sizeof(options)
                options.flags = 2
                expected_error = "invalid devirtualize v6 flags"
            elif version == 7:
                options.base.base.base.base.base.base.struct_size = ctypes.sizeof(options)
                options.flags = 2
                expected_error = "invalid devirtualize v7 flags"
            else:
                options.base.base.base.base.base.base.base.struct_size = ctypes.sizeof(options)
                options.reserved = 1
                expected_error = "invalid devirtualize v8 reserved field"
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
