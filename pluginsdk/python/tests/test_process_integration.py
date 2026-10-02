"""Run actual ELF processes through Python and the built shared engine.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_PROCESS_FIXTURES explicitly. These
tests use the generated freestanding ELF corpus, without invoking host programs.
"""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
from types import SimpleNamespace
import unittest


class ProcessIntegrationTests(unittest.TestCase):
    def test_both_architectures_execute_and_retain_binary_output(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_PROCESS_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and ELF process fixtures are not configured")
        from neverd_plugin import NeverDError, Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        # Standalone test owns the actual native session. Only capsule/address
        # adaptation is supplied here; every operation calls the built library.
        bridge = SimpleNamespace(session_address=lambda _: address)
        session = Session(handle, _native=bridge, _host=host)
        options = json.dumps({"backend": "unicorn", "arguments": ["fixture", "normal"],
                              "environment": ["NEVERD_GUEST=explicit"]})
        for architecture in ("X64", "AArch64"):
            with self.subTest(architecture=architecture):
                path = str((Path(fixtures) / (architecture + ".elf")).resolve(strict=True))
                result = session.emulate_process(path, "linux-elf64-v1", options)
                self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                self.assertEqual(result["exit_status"], 37)
                self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"linux process ok\n")
                self.assertEqual(bytes.fromhex(result["stderr_hex"]), bytes([0, 255, 127]))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                with self.assertRaises(NeverDError):
                    session.emulate_process(path, "linux-elf64-v1", '{"instruction_limit":0}')

    def test_android_native_call_and_trace(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android native fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "add_arguments", "arguments": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
            "trace_limit": 1024,
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 402)
        self.assertEqual(len(result["android"]["trace"]), result["instructions"])
        self.assertFalse(result["android"]["trace_truncated"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "dynamic_lookup",
            "libraries": {"libfixture.so": ["strlen"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 4)
        lookup = next(e for e in result["android"]["native_calls"] if e["name"] == "dlsym")
        call = next(e for e in result["android"]["native_calls"] if e["name"] == "strlen")
        self.assertEqual(lookup["symbol"], "strlen")
        self.assertEqual(lookup["library"], "libfixture.so")
        self.assertEqual(call["library"], "libfixture.so")
        self.assertEqual(call["pc"], lookup["result"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)


if __name__ == "__main__":
    unittest.main()
