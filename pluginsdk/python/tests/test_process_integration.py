"""Run ELF and Mach-O processes through Python and the built shared engine.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_PROCESS_FIXTURES explicitly. These
tests use the generated freestanding ELF corpus, without invoking host programs.
NEVERD_TEST_DARWIN_FIXTURES enables the macOS/iOS Mach-O corpus.
"""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest


class ProcessIntegrationTests(unittest.TestCase):
    def test_android_integer_scanning_preserves_provider_and_output(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for mode, name in enumerate(("sscanf", "vsscanf")):
            for closed in (0, 1):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "scan_dynamic", "initialize": False,
                    "arguments": [0x180000000, closed, mode],
                    "memory": [{"address": 0x180000000, "size": 4096}],
                    "read_memory": [{"address": 0x180000000, "size": 16}],
                    "libraries": {"libscan-model.so": [name]},
                }}
                result = session.emulate_process(str(Path(fixtures) / "scan-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(c for c in calls if c["name"] == "dlsym")
                call = next(c for c in calls if c["name"] == name)
                self.assertEqual(lookup["symbol"], name)
                self.assertEqual(call["library"], "libscan-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "1")
                memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                self.assertEqual(memory, bytes(16) if closed else bytes([15]) + bytes(15))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_fortified_search_preserves_names_bounds_and_errno(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        path = str(Path(fixtures) / "search-O2-relr.so")
        for reverse, name in enumerate(("__strchr_chk", "__strrchr_chk")):
            options = {
                "backend": "unicorn", "android": {
                    "entry_symbol": "search_dynamic", "initialize": False,
                    "arguments": [0x20000000, 0, reverse],
                    "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 24}],
                    "libraries": {"libsearch.so": [name]},
                },
            }
            result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")
            calls = result["android"]["native_calls"]
            lookup = next(c for c in calls if c["name"] == "dlsym")
            self.assertEqual(lookup["symbol"], name)
            searches = [c for c in calls if c["name"] == name]
            self.assertEqual(len(searches), 2)
            for call in searches:
                self.assertEqual(call["library"], "libsearch.so")
                self.assertEqual(call["pc"], lookup["result"])
            memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
            self.assertEqual([int.from_bytes(memory[i:i + 8], "little")
                              for i in range(0, len(memory), 8)],
                             [10 if reverse else 4, 15, 733])
            options["android"]["arguments"][1] = 1
            closed = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(closed["stop_reason"], "unsupported_service")
            self.assertIsNone(closed["android"]["native_calls"][-1]["result"])
            options = {"backend": "unicorn", "android": {
                "entry_symbol": "search_supplied", "initialize": False,
                "arguments": [1, 58, 0, reverse + 2],
            }}
            failed = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(failed["stop_reason"], "runtime_failure")
            self.assertIn("FORTIFY", failed["diagnostic"])
            self.assertEqual(failed["android"]["native_calls"][-1]["name"], name)
            self.assertIsNone(failed["android"]["native_calls"][-1]["result"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_memory_files_share_descriptors_with_guest_threads(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for entry in ("files_sequence", "files_faults", "files_bionic",
                          "files_status", "files_status_bionic", "files_status_at",
                          "files_status_at_bionic", "files_access",
                          "files_access_faults", "files_access_bionic",
                          "files_directory_errors", "files_directory_bionic",
                          "files_trailing_paths", "files_trailing_paths_bionic",
                          "files_filesystem_status", "files_filesystem_status_bionic"):
                with self.subTest(optimization=optimization, entry=entry):
                    options = {
                        "backend": "unicorn", "instruction_quantum": 31,
                        "linux_files": {"files": [{"path": "/fixture/data", "bytes_hex": "00ff410a805a"}]},
                        "android": {"entry_symbol": entry, "initialize": False, "thread_limit": 2},
                    }
                    if entry in ("files_access", "files_directory_errors", "files_trailing_paths"):
                        options["linux_files"]["descriptor_limit"] = 4
                    if entry.startswith("files_status"):
                        options["linux_files"]["files"][0]["metadata"] = {
                            "device": 0xfe12cd34, "inode": str(0xfedcba9876543210),
                            "mode": 0o100644, "link_count": 0x89abcdef,
                            "uid": 0x87654321, "gid": 0xfedcba98, "size": 0,
                            "block_size": 16384, "blocks": 0x1234567890,
                            "access_time": {"seconds": str(-0x7fffffffffffffff),
                                            "nanoseconds": 123456789},
                            "modification_time": {"seconds": 4294967297,
                                                  "nanoseconds": 987654321},
                            "change_time": {"seconds": str(0x7fffffffffffffff),
                                            "nanoseconds": 999999999},
                        }
                    result = session.emulate_process(
                        str(Path(fixtures) / f"files-{optimization}-relr.so"),
                        "android-aarch64-api28-v1", json.dumps(options))
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertEqual(result["return_value"], "0")
                    if entry == "files_bionic":
                        reads = [e for e in result["android"]["native_calls"]
                                 if e["name"] == "read" and e["thread_id"] == 1001]
                        self.assertEqual(len(reads), 1)
                        self.assertEqual(reads[0]["arguments"][0], "3")
                        self.assertEqual(reads[0]["result"], "2")
                    if entry == "files_status_bionic":
                        calls = [e for e in result["android"]["native_calls"]
                                 if e["name"] == "fstat64" and e["thread_id"] == 1001]
                        self.assertEqual(len(calls), 1)
                        self.assertEqual(calls[0]["arguments"][0], "3")
                        self.assertEqual(calls[0]["result"], "0")
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_guest_threads_keep_identity_and_named_imports(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "instruction_quantum": 7, "android": {
                    "entry_symbol": "threads_dynamic", "thread_limit": 4, "trace_limit": 1000,
                    "arguments": [0x20000000, int(closed)], "initialize": False,
                    "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 64}],
                    "libraries": {"libthread-model.so": ["pthread_create", "pthread_join"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"threads-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                android = result["android"]
                self.assertEqual(len(android["threads"]), 1 if closed else 2)
                names = ("pthread_create",) if closed else ("pthread_create", "pthread_join")
                for name in names:
                    lookup = next(e for e in android["native_calls"] if e["name"] == "dlsym" and e["symbol"] == name)
                    call = next(e for e in android["native_calls"] if e["name"] == name)
                    self.assertEqual(call["thread_id"], 1000)
                    self.assertEqual(call["pc"], lookup["result"])
                    self.assertEqual(call["library"], "libthread-model.so")
                    self.assertEqual(call["result"], None if closed else "0")
                if not closed:
                    child = android["threads"][1]
                    self.assertEqual(child["thread_id"], 1001)
                    self.assertTrue(child["finished"] and child["retired"])
                    self.assertEqual({span["thread_id"] for span in android["trace_threads"]}, {1000, 1001})
                    memory = bytes.fromhex(android["memory"][0]["bytes_hex"])
                    self.assertEqual(int.from_bytes(memory[32:40], "little"), 0x1234567800000042)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_thread_attributes_keep_bytes_and_dynamic_names(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "thread_attribute_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 80}],
                    "libraries": {"libthread-model.so": ["pthread_attr_init", "pthread_attr_getstacksize"]},
                }}
                result = session.emulate_process(
                    str(Path(fixtures) / f"thread-attributes-{optimization}-relr.so"),
                    "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                events = result["android"]["native_calls"]
                names = ("pthread_attr_init",) if closed else ("pthread_attr_init", "pthread_attr_getstacksize")
                for name in names:
                    lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == name)
                    call = next(e for e in events if e["name"] == name)
                    self.assertEqual(call["library"], "libthread-model.so")
                    self.assertEqual(call["pc"], lookup["result"])
                    self.assertEqual(call["result"], None if closed else "0")
                memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                expected = bytearray([0xa5] * 56)
                expected[:4] = bytes(4)
                expected[8:40] = bytes(32)
                expected[16:24] = (0xfc000).to_bytes(8, "little")
                expected[24:32] = (4096).to_bytes(8, "little")
                self.assertEqual(memory[16:], bytes(64) if closed else (0xfc000).to_bytes(8, "little") + expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_finalizers_execute_guest_callbacks(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "cxa_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 256}],
                    "libraries": {"libfinalize-model.so": ["__cxa_atexit", "__cxa_finalize"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"finalizers-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(e for e in calls if e["name"] == "dlsym" and e["symbol"] == "__cxa_finalize")
                call = next(e for e in calls if e["name"] == "__cxa_finalize")
                self.assertEqual(call["library"], "libfinalize-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "0")
                if not closed:
                    self.assertEqual(int(result["return_value"], 16), 73)
                expected = bytearray(256)
                if not closed:
                    for offset, value in ((0, 1), (48, 1000), (64, 0x100000009)):
                        expected[offset:offset + 8] = value.to_bytes(8, "little")
                self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_formatting_uses_guest_variadic_calls(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "format_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 64}],
                    "libraries": {"libformat-model.so": ["snprintf"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"format-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(e for e in calls if e["name"] == "dlsym")
                call = next(e for e in calls if e["name"] == "snprintf")
                self.assertEqual(call["library"], "libformat-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "12")
                if not closed:
                    self.assertEqual(int(result["return_value"], 16), 18)
                expected = bytes(64) if closed else b"symbol=0x10000000a" + bytes(46)
                self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_local_memory_input_exceeds_json_limit(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android native fixtures are not configured")
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
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        image = str(Path(fixtures) / "relr.so")
        with TemporaryDirectory(prefix="neverd-memory-") as directory:
            path = Path(directory) / "input-数据.bin"
            source = bytes((i * 37 + (i >> 8)) % 256 for i in range(65539))
            path.write_bytes(source)
            expected = 14695981039346656037
            whole = len(source) // 8 * 8
            for i in range(0, whole, 8):
                word = int.from_bytes(source[i:i + 8], "little")
                expected = ((expected ^ word) * 1099511628211) % (1 << 64)
            for b in source[whole:]:
                expected = ((expected ^ b) * 1099511628211) % (1 << 64)
            region = {"address": 0x20000000, "size": 18 * 4096, "path": str(path)}
            options = {"backend": "unicorn", "instruction_limit": 1000000,
                       "timeout_microseconds": 20000000, "android": {
                "entry_symbol": "inspect_memory_input",
                "arguments": [0x20000000, len(source)], "memory": [region],
                "read_memory": [{"address": 0x20000000, "size": 18 * 4096}],
            }}
            request = json.dumps(options)
            self.assertLess(len(request), 65536)
            result = session.emulate_process(image, "android-aarch64-api28-v1", request)
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], format(expected, "x"))
            actual = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
            mutated = bytes([source[0] ^ 255]) + source[1:] + bytes([165])
            self.assertEqual(actual, mutated.ljust(region["size"], b"\0"))
            self.assertEqual(path.read_bytes(), source)
            region["size"] = 4096
            with self.assertRaisesRegex(NeverDError, "file exceeds its region"):
                session.emulate_process(image, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(path.read_bytes(), source)

    def test_relative_sleep_retains_input_and_completes_original_event(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        options = {"backend": "unicorn", "linux_time": {
            "advance_on_idle": True,
            "clocks": [{"id": 0, "seconds": "4294967297", "nanoseconds": 999999998}]},
            "android": {"entry_symbol": "sleep_call", "initialize": False, "thread_limit": 2,
                "arguments": [2, 0x20000000, 0x20000000, 0x20000020],
                "memory": [{"address": 0x20000000, "size": 4096,
                            "bytes_hex": "00000000000000000500000000000000"}],
                "read_memory": [{"address": 0x20000000, "size": 64}]}}
        result = session.emulate_process(str(Path(fixtures) / "sleep-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        self.assertEqual(len(result["services"]), 1)
        self.assertEqual(result["services"][0]["result"], "0")
        expected = b"".join(v.to_bytes(8, "little") for v in
                            [0, 5, 0, 0, 73, 4294967298, 3, 4294967298])
        self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)

    def test_explicit_clocks_share_values_and_dynamic_api_names(self) -> None:
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
        options = {"backend": "unicorn", "linux_time": {"clocks": [
            {"id": 0, "seconds": "4294967297", "nanoseconds": 987654321},
            {"id": 1, "seconds": 123, "nanoseconds": 456789}]}, "android": {
                "entry_symbol": "time_dynamic", "initialize": False,
                "arguments": [0x20000000, 0],
                "memory": [{"address": 0x20000000, "size": 4096}],
                "read_memory": [{"address": 0x20000000, "size": 40}],
                "libraries": {"libclock-model.so": ["time", "clock_gettime", "gettimeofday"]}}}
        path = str(Path(fixtures) / "time-O2-relr.so")
        result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        calls = result["android"]["native_calls"]
        for name in ("time", "clock_gettime", "gettimeofday"):
            lookup = next(c for c in calls if c["name"] == "dlsym" and c["symbol"] == name)
            call = next(c for c in calls if c["name"] == name)
            self.assertEqual(call["pc"], lookup["result"])
            self.assertEqual(call["library"], "libclock-model.so")
        values = [4294967297, 123, 456789, 4294967297, 987654]
        expected = b"".join(v.to_bytes(8, "little") for v in values)
        self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
        del options["linux_time"]
        result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "unsupported_service")
        self.assertIsNone(result["android"]["native_calls"][-1]["result"])
        self.assertIn("no explicit linux_time input", result["diagnostic"])

    def test_darwin_profiles_preserve_bsd_errors_and_platform_identity(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        options = json.dumps({"backend": "unicorn", "arguments": ["guest", "normal", "argument"],
                              "environment": ["MODE=test"]})
        for profile, architectures in (("macos", ("arm64", "x86_64")),
                                       ("ios", ("arm64",)),
                                       ("ios-simulator", ("arm64", "x86_64"))):
            for architecture in architectures:
                with self.subTest(profile=profile, architecture=architecture):
                    path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
                    result = session.emulate_process(path, f"{profile}-macho64-v1", options)
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["profile"], f"{profile}-macho64-v1")
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"darwin\x00\xff\n")
                    self.assertEqual([e["error"] for e in result["services"]], [True, False, True, False])
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                    wrong = "macos-macho64-v1" if profile == "ios" else "ios-macho64-v1"
                    with self.assertRaises(NeverDError):
                        session.emulate_process(path, wrong, options)
                    for mode, expected in (("files", b"f"), ("files-nocancel", b"f"),
                                           ("writable-files", b"00006e"),
                                           ("writable-files-nocancel", b"00006e"),
                                           ("sparse-file-seek", b"s"),
                                           ("unlinked-file", b"u"),
                                           ("created-file", b"c"),
                                           ("created-file-metadata", b"q"),
                                           ("renamed-file", b"r"),
                                           ("renamed-directory", b"d"),
                                           ("swapped-directory", b"s"),
                                           ("vectored-io", b"v!"),
                                           ("file-access", b"a"),
                                           ("symbolic-links", b"y"),
                                           ("symbolic-link-mutations", b"z"),
                                           ("directory-mutations", b"m"),
                                           ("deleted-directories", b"h"),
                                           ("initial-directory-removal", b"j"),
                                           ("initial-directory-move", b"p"),
                                           ("initial-directory-swap", b"q"),
                                           ("credentials", b"k"),
                                           ("virtual-credentials", bytes.fromhex(
                                               "65000000ca0000002f0100009401000005000000"
                                               "9401000000000000ffffff7f0700000007000000")),
                                           ("resource-usage", b"g"),
                                           ("virtual-resource-usage", bytes.fromhex(
                                               "00000000000000803f420f0000000000efcdab896745230140e2010000000000"
                                               "ffffffffffffff7f0000000000000080ffffffffffffffff0000000000000000"
                                               "0100000000000000efcdab89674523011132547698badcfe0807060504030201"
                                               "0900000000000000f6ffffffffffffff0b00000000000000f4ffffffffffffff"
                                               "0d00000000000000f2ffffffffffffffffffffffffffff7f0000000000000000"
                                               "fdffffffffffffff01000000000000000000000000000000ffffffffffffffff"
                                               "0200000000000000fdffffffffffffff0400000000000000fbffffffffffffff"
                                               "0600000000000000f9ffffffffffffff0800000000000000f7ffffffffffffff"
                                               "0a00000000000000f5ffffffffffffff0c00000000000000ffffffffffffff7f")),
                                           ("resource-limits", b"l"),
                                           ("virtual-resource-limits", bytes.fromhex(
                                               "00000000000000000000000000000000ffffffffffffff7fffffffffffffff7f"
                                               "efcdab8967452301ffffffffffffff7f00000040000000000000008000000000"
                                               "0000000000000000ffffffffffffff7ffeffffffffffff7fffffffffffffff7f"
                                               "0020000000000000004000000000000020000000000000008000000000000000"
                                               "00010000000000000004000000000000")),
                                           ("system-info", b"i"),
                                           ("hostname", b"n"),
                                           ("virtual-hostname", b"abcd\0"),
                                           ("process-observations", b"P"),
                                           ("virtual-process-observations", bytes.fromhex(
                                               "070000000403020101000000")),
                                           ("process-priority", b"Q"),
                                           ("virtual-process-priority", bytes.fromhex(
                                               "f9ffffffffffffff")),
                                           ("login-buffer", b"L"),
                                           ("virtual-login-buffer", b"L\0\xff" +
                                            b"\xa5" * 251 + b"~"),
                                           ("virtual-system", bytes.fromhex(
                                               "44617277696e0032342e746573740000000080"
                                               "4e6576657244207669727475616c206b65726e656c0056343200"
                                               "7669727475616c3634005669727475616c4d6f64656c0007000000"
                                               "1032547698badcfe")),
                                           ("virtual-created-metadata", bytes.fromhex(
                                               "85ffffffe88100001132547698badcfee803000098badcfe0000000000000000edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000f9ffffffffffffff15cd5b0700000000edffffffffffffffb168de3a00000000082000000000000008000000000000000020000000000000efcdab890000000000000000000000000000000000000000")),
                                           ("virtual-file-metadata", bytes.fromhex(
                                               "85ffffffa48101001032547698badcfeefcdab8998badcfe0000000000000000"
                                               "01000000000000800100000000000000f9ffffffffffffff15cd5b0700000000"
                                               "f9ffffffffffffff15cd5b0700000000fbffffffffffffff0600000000000000"
                                               "012000000000000018000000000000000010000000000000efcdab8900000000"
                                               "00000000000000000000000000000000")),

                                           ("stdin", b"\x00\xffx"),
                                           ("output-descriptors", b"ok"),
                                           ("file-status", b"s"), ("file-mapping", b"m"),
                                           ("directories", b"d"), ("directory-entries", b"e"),
                                           ("time-values", bytes.fromhex(
                                               "674523f100000000f1fb090000000000"
                                               "20feffffffffffff1032547698badcfe")),
                                           ("mach-time", b"h"),
                                           ("mach-timebase-values", bytes.fromhex("674523f1795634e2")),
                                           ("mach-clock-values", bytes.fromhex(
                                               "1032547698badcfeffffffffffffffff")),
                                           ("symbolic-link-creation", b"b"),
                                           ("symbolic-link-unlink", b"U"),
                                           ("symbolic-link-unlink-protected", b"U")):
                        if architecture == "x86_64" and mode == "mach-clock-values":
                            continue
                        file_options = json.dumps({
                            "backend": "unicorn", "arguments": ["guest", mode, "/data"],
                            "darwin_system": {
                                "hostname": "abcd",
                                "process_group_id": 7, "session_id": 16909060,
                                "process_tainted": True, "nice": -7,
                                "login_name_hex": "4c00ff" + "a5" * 251 + "7e",
                                "os_type": "Darwin", "os_release": "24.test",
                                "os_revision": -2147483648, "os_version": "V42",
                                "kernel_version": "NeverD virtual kernel",
                                "machine": "virtual64", "model": "VirtualModel",
                                "cpu_count": 7, "memory_size": "18364758544493064720"},
                            "darwin_time": {
                                "time_of_day": {"seconds": 4045620583, "microseconds": 654321},
                                "timezone": {"minutes_west": -480, "dst_time": -1},
                                "mach_absolute_time": "18364758544493064720",
                                "mach_continuous_time": "18446744073709551615",
                                "timebase": {"numerator": 4045620583, "denominator": 3795080825}},
                            "darwin_files": {"files": [{"path": "/data", "bytes_hex":
                                                       b"0123456789".hex(), "metadata": {
                                "device": -123, "inode": "18364758544493064720",
                                "mode": 33188, "link_count": 3,
                                "uid": 2309737967, "gid": 4275878552, "size": 10,
                                "block_size": 4096, "blocks": 8, "flags": 4660,
                                "generation": 2309737967,
                                "access_time": {"seconds": "-9223372036854775807", "nanoseconds": 1},
                                "modification_time": {"seconds": "9223372036854775807", "nanoseconds": 999999999},
                                "change_time": {"seconds": -3, "nanoseconds": 4},
                                "birth_time": {"seconds": -5, "nanoseconds": 6}}}],
                                             "directories": [{"path": "/empty"}, {
                                                 "path": "/", "contents": {
                                                     "minimum_buffer_size": 1,
                                                     "entries": [
                                                         {"name": ".", "inode": 41, "type": 4,
                                                          "next_offset": 11, "seek_offset": 0,
                                                          "minimum_buffer_size": 64},
                                                         {"name": "..", "inode": 41, "type": 4,
                                                          "next_offset": 22, "seek_offset": 0},
                                                         {"name": "empty", "inode": 42, "type": 4,
                                                          "next_offset": 7, "seek_offset": 0},
                                                         {"name": "data", "inode": "18364758544493064720",
                                                          "type": 8, "next_offset": 99, "seek_offset": 0}]}}],
                                             "working_directory": "/empty",
                                             "stdin_hex": "00ff78", "descriptor_limit": 32}})
                        if mode in ("credentials", "virtual-credentials", "created-file-metadata"):
                            credential_options = json.loads(file_options)
                            credential_options["darwin_system"] = {"credentials": {
                                "real_uid": 101, "effective_uid": 202,
                                "real_gid": 303, "effective_gid": 404,
                                "groups": [404, 0, "2147483647", 7, 7]}}
                            file_options = json.dumps(credential_options)
                        if mode in ("resource-usage", "virtual-resource-usage"):
                            usage_options = json.loads(file_options)
                            usage_options["darwin_system"] = {
                                "resource_usage": {
                                    "self": {
                                        "user_seconds": "-9223372036854775808",
                                        "user_microseconds": 999999,
                                        "system_seconds": "81985529216486895",
                                        "system_microseconds": 123456,
                                        "counters": [
                                            "9223372036854775807",
                                            "-9223372036854775808",
                                            -1,
                                            0,
                                            1,
                                            "81985529216486895",
                                            "-81985529216486895",
                                            "72623859790382856",
                                            9,
                                            -10,
                                            11,
                                            -12,
                                            13,
                                            -14
                                        ]
                                    },
                                    "children": {
                                        "user_seconds": "9223372036854775807",
                                        "user_microseconds": 0,
                                        "system_seconds": -3,
                                        "system_microseconds": 1,
                                        "counters": [
                                            0,
                                            -1,
                                            2,
                                            -3,
                                            4,
                                            -5,
                                            6,
                                            -7,
                                            8,
                                            -9,
                                            10,
                                            -11,
                                            12,
                                            "9223372036854775807"
                                        ]
                                    }
                                }
                            }
                            file_options = json.dumps(usage_options)
                        if mode in ("resource-limits", "virtual-resource-limits"):
                            resource_options = json.loads(file_options)
                            resource_options["darwin_system"] = {"max_files_per_process": 64, "resource_limits": [
                                {"resource": 0, "current": 0, "maximum": 0},
                                {"resource": 1, "current": "9223372036854775807", "maximum": "9223372036854775807"},
                                {"resource": 2, "current": "81985529216486895", "maximum": "9223372036854775807"},
                                {"resource": 3, "current": 1073741824, "maximum": 2147483648},
                                {"resource": 4, "current": 0, "maximum": "9223372036854775807"},
                                {"resource": 5, "current": "9223372036854775806", "maximum": "9223372036854775807"},
                                {"resource": 6, "current": 8192, "maximum": 16384},
                                {"resource": 7, "current": 32, "maximum": 128},
                                {"resource": 8, "current": 256, "maximum": 1024}
                            ]}
                            file_options = json.dumps(resource_options)
                        if mode.startswith("writable-files") or mode in ("virtual-file-metadata", "sparse-file-seek", "unlinked-file", "created-file", "directory-mutations", "deleted-directories", "initial-directory-removal", "initial-directory-move", "initial-directory-swap", "created-file-metadata", "virtual-created-metadata", "renamed-file", "renamed-directory", "swapped-directory", "vectored-io"):
                            writable_options = json.loads(file_options)
                            writable_file = writable_options["darwin_files"]["files"][0]
                            writable_file["writable"] = True
                            writable_file["metadata"]["flags"] = 0
                            if mode in ("virtual-file-metadata", "sparse-file-seek", "unlinked-file", "created-file", "directory-mutations", "deleted-directories", "initial-directory-removal", "initial-directory-move", "initial-directory-swap", "created-file-metadata", "virtual-created-metadata", "renamed-file", "renamed-directory", "swapped-directory"):
                                writable_file["metadata"]["link_count"] = 1
                                writable_file["mutation_policy"] = {
                                    "allocation_unit": 4096,
                                    "mutation_time": {"seconds": -7, "nanoseconds": 123456789}}
                            if mode in ("unlinked-file", "created-file", "directory-mutations", "deleted-directories", "initial-directory-removal", "initial-directory-move", "initial-directory-swap", "created-file-metadata", "virtual-created-metadata", "renamed-file", "renamed-directory", "swapped-directory"):
                                next(d for d in writable_options["darwin_files"]["directories"]
                                     if d["path"] == "/")["mutable"] = True
                            if mode == "initial-directory-swap":
                                root = next(d for d in writable_options["darwin_files"]["directories"]
                                            if d["path"] == "/")
                                root["swap_rename"] = True
                                initial = next(d for d in writable_options["darwin_files"]["directories"]
                                               if d["path"] == "/empty")
                                initial["mutable"] = True
                                initial["exchangeable"] = True
                                initial["swap_rename"] = True
                            if mode == "initial-directory-move":
                                initial = next(d for d in writable_options["darwin_files"]["directories"]
                                               if d["path"] == "/empty")
                                initial["mutable"] = True
                                initial["movable"] = True
                            if mode == "initial-directory-removal":
                                next(d for d in writable_options["darwin_files"]["directories"]
                                     if d["path"] == "/empty")["removable"] = True
                            if mode in ("renamed-file", "swapped-directory"):
                                next(d for d in writable_options["darwin_files"]["directories"]
                                     if d["path"] == "/")["swap_rename"] = True
                            if mode in ("created-file-metadata", "virtual-created-metadata", "renamed-file", "renamed-directory", "swapped-directory"):
                                files = writable_options["darwin_files"]
                                files["umask"] = 0o27
                                files["creation_policy"] = {
                                    "first_inode": "18364758544493064721",
                                    "block_size": 8192, "generation": 2309737967,
                                    "creation_time": {"seconds": -19, "nanoseconds": 987654321},
                                    "mutation_policy": {"allocation_unit": 4096,
                                                        "mutation_time": {"seconds": -7, "nanoseconds": 123456789}}}
                                parent = dict(writable_file["metadata"], inode=41, mode=0o40755,
                                              flags=0, link_count=1, size=0, blocks=0)
                                next(d for d in files["directories"]
                                     if d["path"] == "/")["metadata"] = parent
                            file_options = json.dumps(writable_options)
                        if mode == "symbolic-links":
                            symbolic_options = json.loads(file_options)
                            files = symbolic_options["darwin_files"]
                            files["working_directory"] = "/"
                            files["symbolic_links"] = [
                                {"path": "/" + name, "target_hex": target.encode().hex()}
                                for name, target in (("link", "data"), ("chain", "link"),
                                                     ("dangling", "missing"), ("cycle", "cycle"),
                                                     ("dirlink", "empty"))]
                            files["symbolic_links"][0]["metadata"] = dict(
                                files["files"][0]["metadata"], mode=0o120777, inode=123, size=4)
                            for directory in files["directories"]:
                                directory.pop("contents", None)
                            file_options = json.dumps(symbolic_options)
                        unlink_links = mode.startswith("symbolic-link-unlink")
                        protected_link = mode == "symbolic-link-unlink-protected"
                        if mode in ("symbolic-link-mutations", "symbolic-link-creation") or unlink_links:
                            mixed_options = json.loads(file_options)
                            original = mixed_options["darwin_files"]["files"][0]
                            metadata = dict(original["metadata"], flags=0, link_count=1)
                            mutation = {"allocation_unit": 4096,
                                        "mutation_time": {"seconds": -7, "nanoseconds": 123456789}}
                            links = [{"path": "/static/" + name, "target_hex": target.encode().hex()}
                                     for name, target in (("alias", "../work"),
                                                          ("data-link", "../work/data"),
                                                          ("missing-link", "../work/new"))]
                            links[1]["metadata"] = dict(metadata, mode=0o120777, inode=123, size=12)
                            mixed_options["darwin_files"] = {
                                "files": [{"path": "/work/data", "bytes_hex": original["bytes_hex"],
                                           "metadata": metadata, "writable": True,
                                           "mutation_policy": mutation}],
                                "directories": [{"path": "/static"},
                                                {"path": "/work", "mutable": True,
                                                 "metadata": dict(metadata, mode=0o40755, inode=41,
                                                                  size=0, blocks=0)}],
                                "symbolic_links": links, "working_directory": "/", "umask": 0o27,
                                "creation_policy": {
                                    "first_inode": "18364758544493064721", "block_size": 8192,
                                    "generation": 2309737967,
                                    "creation_time": {"seconds": -19, "nanoseconds": 987654321},
                                    "mutation_policy": mutation}}
                            mixed_options["arguments"][2] = "/work/data"
                            if protected_link:
                                mixed_options["arguments"][1] = "symbolic-link-unlink"
                                mixed_options["arguments"].append("protected")
                            file_options = json.dumps(mixed_options)
                        result = session.emulate_process(path, f"{profile}-macho64-v1", file_options)
                        self.assertEqual(result["stop_reason"],
                                         "unsupported_service" if protected_link else "exited",
                                         f"{mode}: {result['diagnostic']}")
                        self.assertEqual(result["exit_status"], None if protected_link else 37, mode)
                        self.assertEqual(bytes.fromhex(result["stdout_hex"]), expected)
                        self.assertEqual(result["stderr_hex"], "")
                        if unlink_links:
                            # U follows the guest's target FD/map identity checks.
                            services = result["services"]
                            removal_numbers = (("200000a", "20001d8") if architecture == "x86_64"
                                               else ("a", "1d8"))
                            removals = [event for event in services
                                        if event["number"] in removal_numbers]
                            self.assertTrue(any(event.get("error") is False and event["result"] == "0"
                                                for event in removals), mode)
                            self.assertTrue(any(event.get("error") is True and event["result"] == "2"
                                                for event in removals), mode)
                            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                            if protected_link:
                                self.assertEqual(services[-1]["number"],
                                                 "200000a" if architecture == "x86_64" else "a")
                                self.assertIsNone(services[-1]["result"])
                                self.assertNotIn("error", services[-1])
                        if mode.startswith("mach-"):
                            services = result["services"]
                            clocks = mode == "mach-clock-values"
                            for service in services[:2 if clocks else 12]:
                                self.assertNotIn("error", service)
                            self.assertIs(services[-1]["error"], False)
                            if clocks:
                                self.assertEqual(services[0]["result"], "fedcba9876543210")
                                self.assertEqual(services[1]["result"], "ffffffffffffffff")
                            else:
                                self.assertEqual(services[5]["number"], "1234567801000059"
                                                 if architecture == "x86_64" else "12345678ffffffa7")

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
        names = ("getuid", "geteuid", "getgid", "getegid")
        for scope, stop, expected in ((["libfixture.so"], "returned", 6),
                                      ([], "returned", 100),
                                      (None, "unsupported_service", None)):
            native = {"entry_symbol": "default_call",
                      "libraries": {"libfixture.so": ["strlen"]}}
            if scope is not None:
                native["default_scope"] = scope
            result = session.emulate_process(
                str(Path(fixtures) / "relr.so"), "android-aarch64-api28-v1",
                json.dumps({"backend": "unicorn", "android": native}))
            self.assertEqual(result["stop_reason"], stop, result["diagnostic"])
            if expected is not None:
                self.assertEqual(int(result["return_value"], 16), expected)
            if scope:
                lookup, call = result["android"]["native_calls"]
                self.assertEqual(lookup["arguments"][0], "0")
                self.assertEqual(lookup["library"], "libfixture.so")
                self.assertEqual(call["name"], "strlen")
                self.assertEqual(call["pc"], lookup["result"])
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "dynamic_identities", "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 16}],
            "libraries": {"libidentity.so": list(names)},
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        for name in names:
            with self.subTest(symbol=name):
                events = result["android"]["native_calls"]
                lookup = next(e for e in events
                              if e["name"] == "dlsym" and e["symbol"] == name)
                call = next(e for e in events if e["name"] == name)
                self.assertEqual(lookup["library"], "libidentity.so")
                self.assertEqual(call["library"], "libidentity.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(int(call["result"], 16), 1000)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "once_values", "arguments": [0x20000000],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 44}],
        }})
        result = session.emulate_process(str(Path(fixtures) / "once-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 73)
        events = result["android"]["native_calls"]
        self.assertEqual([e["name"] for e in events],
                         ["pthread_once"] * 4 + ["getuid", "pthread_once"])
        self.assertEqual([int(e["result"], 16) for e in events],
                         [0, 0, 0, 0, 1000, 0])
        memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
        self.assertEqual([int.from_bytes(memory[i:i + 4], "little")
                          for i in range(0, len(memory), 4)],
                         [2, 2, 1, 1, 1, 1, 2, 1000, 1, 2, 1])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "token_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 40}],
            "libraries": {"libtokens.so": ["strtok_r"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "token-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        events = result["android"]["native_calls"]
        lookup = next(e for e in events if e["name"] == "dlsym")
        self.assertEqual(lookup["symbol"], "strtok_r")
        self.assertEqual(lookup["library"], "libtokens.so")
        calls = [e for e in events if e["name"] == "strtok_r"]
        self.assertEqual(len(calls), 2)
        for call in calls:
            self.assertEqual(call["library"], "libtokens.so")
            self.assertEqual(call["pc"], lookup["result"])
        memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
        self.assertEqual([int.from_bytes(memory[i:i + 8], "little")
                          for i in range(0, len(memory), 8)], [0, 6, 0, 6, 0])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "mutex_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 24}],
            "libraries": {"libpthread-model.so": ["pthread_mutex_lock", "pthread_mutex_unlock"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "mutex-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        for name in ("pthread_mutex_lock", "pthread_mutex_unlock"):
            events = result["android"]["native_calls"]
            lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == name)
            call = next(e for e in events if e["name"] == name)
            self.assertEqual(call["library"], "libpthread-model.so")
            self.assertEqual(call["pc"], lookup["result"])
            self.assertEqual(int(call["result"], 16), 0)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "syscall_dynamic", "initialize": False,
            "arguments": [0], "libraries": {"libservice.so": ["syscall"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "syscall-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        events = result["android"]["native_calls"]
        lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == "syscall")
        call = next(e for e in events if e["name"] == "syscall")
        self.assertEqual(call["library"], "libservice.so")
        self.assertEqual(call["pc"], lookup["result"])
        self.assertEqual(int(call["arguments"][0], 16), 178)
        self.assertEqual(int(call["result"], 16), 1000)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)


if __name__ == "__main__":
    unittest.main()
