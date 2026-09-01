"""End-to-end Python concolic coverage against the built libneverd."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import unittest


class _AddressBridge:
    """Expose a test-owned C session through the plugin lifetime protocol."""

    def session_address(self, native_handle: object) -> int:
        if isinstance(native_handle, ctypes.c_void_p):
            return int(native_handle.value or 0)
        if isinstance(native_handle, int) and not isinstance(native_handle, bool):
            return native_handle
        return 0


class ConcolicIntegrationTests(unittest.TestCase):
    def test_real_library_runs_verified_concolic_report(self) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)
        fixture = (
            Path(__file__).resolve().parents[3]
            / "unittests"
            / "concolic"
            / "fixtures"
            / "binaries"
            / "lowir_concolic_elf_x64"
        ).resolve(strict=True)

        from neverd_plugin import ConcolicFlipStatus
        from neverd_plugin import ConcolicRegisterSeed
        from neverd_plugin import ConcolicReplayStatus
        from neverd_plugin import ConcolicTraceReason
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        host = HostAPI(ctypes.CDLL(str(resolved_library)))
        native_handle = host.call("neverd_session_create")
        address = int(native_handle or 0)
        self.assertGreater(address, 0)
        self.addCleanup(
            host.call,
            "neverd_session_destroy",
            ctypes.c_void_p(address),
        )

        session = Session(address, _native=_AddressBridge(), _host=host)
        session.load(fixture)
        function_index = int(
            host.call(
                "neverd_func_find_by_name",
                ctypes.c_void_p(address),
                b"concolic_branch",
            )
        )
        self.assertGreaterEqual(function_index, 0)
        entry = int(
            host.call(
                "neverd_func_entry",
                ctypes.c_void_p(address),
                function_index,
            )
        )
        self.assertGreater(entry, 0)

        seed = ConcolicRegisterSeed(offset=56, value=0, bytes=4)
        first = session.lowir_concolic(entry, [seed])
        second = session.lowir_concolic(entry, [seed])

        self.assertEqual(first, second)
        self.assertTrue(first.ok)
        self.assertFalse(first.exhaustive)
        self.assertTrue(first.trace_complete)
        self.assertTrue(first.trace_exact)
        self.assertEqual(first.function.name, "concolic_branch")
        self.assertEqual(first.initial_seed, (seed,))
        self.assertEqual(len(first.flips), 1)
        self.assertIs(first.flips[0].status, ConcolicFlipStatus.VERIFIED)
        self.assertIs(first.flips[0].replay_status, ConcolicReplayStatus.VERIFIED)
        self.assertEqual(len(first.candidates), 1)
        self.assertEqual(first.candidates[0].seed[0].value, 7)

        inexact = session.lowir_concolic(entry)
        self.assertTrue(inexact.ok)
        self.assertFalse(inexact.trace_complete)
        self.assertFalse(inexact.trace_exact)
        self.assertIs(
            inexact.trace_reason,
            ConcolicTraceReason.INCOMPLETE_CONCRETE_TRACE,
        )
        self.assertEqual(inexact.initial_seed, ())
        self.assertGreater(inexact.executed_steps, 0)
        self.assertEqual(inexact.flips, ())
        self.assertEqual(inexact.candidates, ())


if __name__ == "__main__":
    unittest.main()
