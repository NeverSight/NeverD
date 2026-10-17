"""Run ELF and Mach-O processes through Python and the built shared engine.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_PROCESS_FIXTURES explicitly. These
tests use the generated freestanding ELF corpus, without invoking host programs.
NEVERD_TEST_DARWIN_FIXTURES enables the macOS/iOS Mach-O corpus.
"""
from __future__ import annotations

import base64
import ctypes
import json
import os
import struct
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest


class ProcessIntegrationTests(unittest.TestCase):
    def test_android_jni_values_and_thread_identity(self) -> None:
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
        for entry in ("jni_vm_lifecycle", "jni_string_values", "jni_arrays",
                      "jni_references", "jni_threads", "jni_primitive_bits",
                      "jni_art_string_edges", "jni_method_values", "jni_method_exceptions"):
            with self.subTest(entry=entry):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": entry, "initialize": False,
                    "thread_limit": 2, "jni": True}}
                result = session.emulate_process(str(Path(fixtures) / "jni-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertEqual(result["return_value"], "0")

    def test_android_binder_control_and_thread_identity(self) -> None:
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
        for entry in ("binder_control", "binder_descriptions", "binder_threads",
                      "binder_service_transactions", "binder_missing_targets"):
            with self.subTest(entry=entry):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": entry, "initialize": False,
                    "thread_limit": 5}, "linux_files": {"files": [], "devices": [
                        {"path": "/dev/binder", "access_mode": 2, "open_errno": 0,
                         "ioctls": [], "binder": True}]}}
                result = session.emulate_process(str(Path(fixtures) / "binder-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertEqual(result["return_value"], "0")

    def test_android_live_proc_and_maintained_stat(self) -> None:
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
        metadata = {"device": 0, "inode": 42, "mode": 0o100644,
                    "link_count": 1, "uid": 0, "gid": 0, "size": 3,
                    "block_size": 4096, "blocks": 8}
        metadata.update({key: {"seconds": 0, "nanoseconds": 0}
                         for key in ("access_time", "modification_time", "change_time")})
        options = {"backend": "unicorn", "android": {
            "initialize": False, "thread_limit": 2},
            "linux_time": {"clocks": [{"id": 0, "seconds": 100, "nanoseconds": 7}]},
            "linux_files": {"dynamic_proc": {"name": "fixture", "parent_pid": 1},
                "files": [{"path": "/fixture/value", "bytes_hex": "616263",
                           "write_limit": 16384, "track_write_metadata": True,
                           "metadata": metadata}]}}
        for entry in ("proc_write_stat", "proc_live_maps", "proc_thread_lifecycle"):
            with self.subTest(entry=entry):
                options["android"]["entry_symbol"] = entry
                result = session.emulate_process(str(Path(fixtures) / "proc-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertEqual(result["return_value"], "0")

    def test_android_stdio_uses_guest_files_and_provider_lifetime(self) -> None:
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
        metadata = {"device": 1, "inode": 2, "mode": 0o100444,
                    "link_count": 1, "uid": 0, "gid": 0, "size": 15,
                    "block_size": 8, "blocks": 1}
        metadata.update({key: {"seconds": 0, "nanoseconds": 0}
                         for key in ("access_time", "modification_time", "change_time")})
        options = {"backend": "unicorn", "android": {
            "entry_symbol": "stdio_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 24}],
            "libraries": {"libstreams.so": ["fopen", "fgetc", "fclose"]},
        }, "linux_files": {"files": [{"path": "/fixture/input",
            "bytes_hex": b"alpha\nbeta\nlast".hex(), "metadata": metadata}]}}
        for close in (False, True):
            with self.subTest(close_provider=close):
                options["android"]["arguments"][1] = int(close)
                result = session.emulate_process(str(Path(fixtures) / "stdio-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"],
                                 "unsupported_service" if close else "returned", result["diagnostic"])
                if close:
                    call = result["android"]["native_calls"][-1]
                    self.assertEqual(call["name"], "fgetc")
                    self.assertEqual(call["library"], "libstreams.so")
                    self.assertIsNone(call["result"])
                else:
                    self.assertEqual(result["return_value"], "0")
                    memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                    self.assertEqual(struct.unpack("<3Q", memory), (97, 1, 0))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_mutable_files_and_live_proc_namespace(self) -> None:
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
        options = {"backend": "unicorn", "android": {
            "entry_symbol": "filesystem_shared", "initialize": False,
            "arguments": [0x20000000],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 1024}],
        }, "linux_files": {"files": [{"path": "/fixture/input",
            "bytes_hex": b"alpha\nbeta\nlast".hex(), "write_limit": 64}],
            "directories": ["/fixture/sub"], "dynamic_proc_fds": True}}
        for entry in ("filesystem_shared", "filesystem_proc"):
            for replay in range(2):
                with self.subTest(entry=entry, replay=replay):
                    options["android"]["entry_symbol"] = entry
                    result = session.emulate_process(str(Path(fixtures) / "stdio-O2-relr.so"),
                                                     "android-aarch64-api28-v1", json.dumps(options))
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertEqual(result["return_value"], "0")
                    memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                    if entry == "filesystem_shared":
                        expected = bytearray(1024)
                        struct.pack_into("<11Q", expected, 0, 2,2,4,18,1,12,7,0,19,0,0)
                        expected[512:516] = b"XYph"
                        expected[528:535] = b"ast\0\0\0Z"
                        self.assertEqual(memory, expected)
                    else:
                        self.assertEqual(struct.unpack_from("<8Q",memory),
                                         (14,2**64-1,2,4,12,7,0,0))
                        self.assertEqual(memory[512:526],b"/fixture/input")
                        self.assertEqual(memory[544:556],b"/fixture/sub")
                        self.assertEqual([memory[n:n+16].rstrip(b"\0") for n in range(576,688,16)],
                                         [b".",b"..",b"0",b"1",b"2",b"3",b"4"])
                    self.assertTrue(all(c["result"] is not None for c in result["android"]["native_calls"]))
                    self.assertEqual(host.call("neverd_session_is_loaded", handle),0)

    def test_android_property_callbacks_share_identity_and_provider_lifetime(self) -> None:
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
        names = ["__system_property_find", "__system_property_read",
                 "__system_property_read_callback", "__system_property_foreach"]
        options = {"backend": "unicorn", "android": {
            "entry_symbol": "property_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "properties": {"test.alpha": "one"},
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 80}],
            "libraries": {"libproperties.so": names},
        }}
        for close in (False, True):
            with self.subTest(close_provider=close):
                options["android"]["arguments"][1] = int(close)
                result = session.emulate_process(str(Path(fixtures) / "properties-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"],
                                 "unsupported_service" if close else "returned", result["diagnostic"])
                calls = result["android"]["native_calls"]
                if close:
                    self.assertEqual(calls[-1]["name"], "__system_property_read")
                    self.assertIsNone(calls[-1]["result"])
                else:
                    self.assertEqual(result["return_value"], "0")
                    memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                    self.assertEqual(struct.unpack("<10Q", memory),
                                     (1, 0xece42d7a4bc366ee, 0x1d4996, 0x3000000, 0x3000000,
                                      3, 0x1d4996, 1, 1, 0x3000000))
                    for name in names:
                        lookup = next(c for c in calls if c["name"] == "dlsym" and c["symbol"] == name)
                        named = next(c for c in calls if c["name"] == name and
                                     c["library"] == "libproperties.so")
                        self.assertEqual(lookup["library"], "libproperties.so")
                        self.assertEqual(named["pc"], lookup["result"])
                        self.assertIsNotNone(named["result"])
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_explicit_device_transactions_share_native_and_raw_calls(self) -> None:
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
        device = {"path": "/fixture/control", "access_mode": 2, "open_errno": 0,
                  "ioctls": [
                      {"request": 0xa1000001, "result": 37, "memory": [
                          {"offset": 0, "expected_hex": "3132", "output_hex": "5253"},
                          {"offset": 1, "pointer_offset": 8,
                           "expected_hex": "5556", "output_hex": "9091"}]},
                      {"request": 0xa1000002, "result": -13,
                       "argument": str(0x8877665544332211), "memory": []}]}
        for route in range(4):
            options = {"backend": "unicorn", "linux_files": {"files": [], "devices": [device]},
                       "android": {"entry_symbol": "devices_sequence", "initialize": False,
                                   "arguments": [route], "libraries": {"libdevice.so": ["ioctl"]}}}
            options["linux_files"]["ioctl_trace"] = {
                "event_limit": 16, "byte_limit": 4096,
                "requests": [{"request": 0xa1000001, "memory": [
                    {"offset": 0, "size": 24},
                    {"offset": 0, "size": 8, "pointer_offset": 8}]}]}
            result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                             "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")
            self.assertFalse(result["ioctl_trace_truncated"])
            self.assertEqual(len(result["ioctl_trace"]), 5)
            for index in (0, 2, 4):
                event = result["ioctl_trace"][index]
                self.assertEqual(event["memory"][1]["before_hex"], "c75556c7c7c7c7c7")
                self.assertEqual(event["memory"][1]["after_hex"], "c79091c7c7c7c7c7")
                self.assertEqual("return_address" in event, route != 0)
            if route in (1, 3):
                calls = [c for c in result["android"]["native_calls"] if c["name"] == "ioctl"]
                self.assertTrue(any(c["result"] == "25" for c in calls))
                self.assertTrue(any(c["result"] == "ffffffffffffffff" for c in calls))
                if route == 3:
                    self.assertTrue(all(c["library"] == "libdevice.so" for c in calls))
        device["read_only_mappings"] = [{"offset": 4096,
                                         "bytes_hex": "31" * 4096 + "32" * 4096 + "33" * 4096}]
        for route in range(4):
            options["android"].update(entry_symbol="devices_mapping", arguments=[route],
                                       libraries={"libdevice.so": ["ioctl", "mmap"]})
            result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                             "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")

        pages = b"1" * 4096 + b"2" * 4096 + b"3" * 4096 + bytes(range(256)) * 80
        device["read_only_mappings"] = [{"offset": 4096,
                                         "bytes_base64": base64.b64encode(pages).decode("ascii")}]
        for route in range(4):
            options["android"]["arguments"] = [route]
            encoded = json.dumps(options)
            self.assertLess(len(encoded), 64 * 1024)
            result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                             "android-aarch64-api28-v1", encoded)
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")

        options["linux_files"]["files"] = [{"path": "/fixture/bytes", "bytes_hex": "616263"}]
        options["android"].update(entry_symbol="devices_duplicates",
                                   libraries={"libdevice.so": ["ioctl", "mmap", "dup", "dup3"]})
        for route in range(4):
            options["android"]["arguments"] = [route]
            result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                             "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")
            self.assertEqual(result["stdout_hex"], b"out".hex())
            self.assertEqual(result["stderr_hex"], b"err".hex())

        device["ioctls"] = []
        device["kgsl_contexts"] = {"memstore_offset": 4096, "context_limit": 3}
        device["read_only_mappings"] = [{"offset": 4096, "bytes_hex": "a5" * 4096,
                                         "updates": [
                                             {"after_nanoseconds": 1000000, "offset": 40, "bytes_hex": "07000000"},
                                             {"after_nanoseconds": 1000000, "offset": 48, "bytes_hex": "05000000"},
                                             {"after_nanoseconds": 2000000, "offset": 48, "bytes_hex": "07000000"}]}]
        options["linux_time"] = {"advance_on_idle": True}
        options["android"]["entry_symbol"] = "devices_contexts"
        for route in range(4):
            options["android"]["arguments"] = [route]
            result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                             "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")

        device["kgsl_contexts"]["synchronous_a3xx"] = True
        device["read_only_mappings"][0]["updates"] = []
        options["android"].update(entry_symbol="devices_gpu_commands", arguments=[0])
        result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        device["kgsl_contexts"]["memory_objects"] = True
        options["android"].update(entry_symbol="devices_gpu_objects", arguments=[0, 1])
        result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        options["android"].update(entry_symbol="devices_gpu_legacy", arguments=[0, 1])
        result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        del device["kgsl_contexts"]["memory_objects"]
        del device["kgsl_contexts"]["synchronous_a3xx"]

        device["read_only_mappings"][0]["updates"] = [
            {"after_nanoseconds": 1000000, "offset": 0, "bytes_hex": "9a"}]
        options["instruction_quantum"] = 1024
        options["linux_time"]["nanoseconds_per_instruction"] = "100"
        options["android"].update(entry_symbol="devices_thread_updates", arguments=[],
                                   thread_limit=2, drain_threads=True)
        result = session.emulate_process(str(Path(fixtures) / "devices-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        self.assertEqual(len(result["android"]["threads"]), 2)
        self.assertTrue(all(t["finished"] for t in result["android"]["threads"]))

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
                          "files_filesystem_status", "files_filesystem_status_bionic",
                          "files_links", "files_links_bionic"):
                with self.subTest(optimization=optimization, entry=entry):
                    options = {
                        "backend": "unicorn", "instruction_quantum": 31,
                        "linux_files": {"files": [{"path": "/fixture/data", "bytes_hex": "00ff410a805a"}]},
                        "android": {"entry_symbol": entry, "initialize": False, "thread_limit": 2},
                    }
                    if entry in ("files_access", "files_directory_errors", "files_trailing_paths"):
                        options["linux_files"]["descriptor_limit"] = 4
                    if entry.startswith("files_links"):
                        # O0 fills a page in guest code; keep semantic budgets
                        # while allowing wall-clock contention on shared hosts.
                        options["timeout_microseconds"] = 30_000_000
                        options["linux_files"]["symbolic_links"] = [
                            {"path": "/links/raw", "target_hex": "2f78ff79"}]
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

    def test_darwin_immediate_select_preserves_bitmaps_errors_and_provider_premises(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        values = (0, 3, 6, 3, 6, 9, 9, 12, 22, 14, 22, 22, 14, 14, 9)
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x02000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "immediate-select", "/data"],
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566"}]}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "73")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 115)
                    for api, number in enumerate((93, 407)):
                        for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                            for i, value in enumerate(values):
                                call = calls[9 + (api * 3 + h) * 15 + i]
                                self.assertEqual(call["number"], format(high | cls | number, "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], i >= 8)
                    for index, value in ((99, 6), (100, 4), (106, 3), (107, 0), (108, 6)):
                        self.assertEqual(calls[index]["result"], format(value, "x"))
                        self.assertFalse(calls[index]["error"])
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for mode, number, diagnostic in (
                ("immediate-select-wait", 93, "Darwin select waiting is not implemented"),
                ("immediate-select-capacity", 407, "Darwin select descriptor table capacity is not modeled"),
            ):
                request = json.loads(original)
                request["arguments"][1] = mode
                result = session.emulate_process(path, profile, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(result["diagnostic"], diagnostic)
                self.assertEqual(len(result["services"]), 2)
                self.assertEqual(result["services"][-1]["number"], format(cls | number, "x"))
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)


    def test_darwin_ordinary_modes_keep_strict_object_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 1 if size else 2, "uid": 501, "gid": 20,
                    "size": size, "block_size": 512, "blocks": 1 if size else 0,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x2000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-mode-mutations", "/data"],
                       "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                         "real_gid": 31, "effective_gid": 20}},
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                                   "metadata": metadata(0o100600, 2, 6)}],
                                        "directories": [{"path": "/", "metadata": metadata(0o40700, 1, 0)}],
                                        "working_directory": "/",
                                        "mode_mutations": {"objects": [{"path": "/data",
                                                                      "time": {"seconds": 71, "nanoseconds": 72}}]}}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "4d")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 54)
                    for api, number in enumerate((15, 124, 467)):
                        for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                            call = calls[4 + (api * 3 + h) * 4]
                            self.assertEqual(call["number"], format(high | cls | number, "x"))
                            self.assertEqual(call["result"], "0")
                            self.assertFalse(call["error"])
                    for i, value in enumerate((14, 9, 22, 9)):
                        self.assertEqual(calls[40 + i]["result"], format(value, "x"))
                        self.assertTrue(calls[40 + i]["error"])
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = json.loads(original)
            request["darwin_files"]["mode_mutations"]["objects"] = []
            result = session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["diagnostic"], "Darwin object mode provider is not configured")
            self.assertEqual(len(result["services"]), 5)
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
            request["darwin_files"]["mode_mutations"]["extra"] = True
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_symbolic_ownership_keeps_vnode_and_target_authority(self) -> None:
        self._check_darwin_symbolic_ownership(False)

    def test_darwin_libsystem_imports_preserve_errno_and_named_evidence(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        names = ["___error", "___error", "_getpid", "_getpid", "_write", "_write", "_read",
                 "_write", "_read", "_close", "_close", "_read", "_write", "_close"]
        arities = [0, 0, 0, 0, 3, 3, 3, 3, 3, 1, 1, 3, 3, 1]
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            for kind in ("classic", "chained"):
                path = str((Path(fixtures) / f"{platform}-{architecture}-libsystem-{kind}").resolve(strict=True))
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000, "arguments": ["libsystem"],
                           "darwin_files": {"files": [], "stdin_hex": "6162006364"}}
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, kind=kind, repeat=repeat):
                        result = session.emulate_process(path, profile, json.dumps(options))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 58)
                        self.assertEqual(result["schema_version"], 1)
                        self.assertEqual(result["stdout_hex"], "4100ff4221")
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["services"], [])
                        self.assertEqual(result["events"], 15)
                        calls = result["darwin"]["native_calls"]
                        self.assertEqual([call["name"] for call in calls], names)
                        self.assertEqual([len(call["arguments"]) for call in calls], arities)
                        self.assertTrue(all(call["module"] == "/usr/lib/libSystem.B.dylib" for call in calls))
                        self.assertEqual(calls[0]["result"], calls[1]["result"])
                        self.assertEqual([call["result"] for call in calls[2:]],
                                         ["3e8", "3e8", "4", "ffffffffffffffff", "ffffffffffffffff",
                                          "ffffffffffffffff", "3", "ffffffffffffffff", "0",
                                          "ffffffffffffffff", "1", "ffffffffffffffff"])
                        self.assertEqual(calls[12]["arguments"][0], "1234567800000001")
                        self.assertEqual(calls[13]["arguments"][0], "12345678ffffffff")
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                options["arguments"] = ["libsystem", "exit"]
                result = session.emulate_process(path, profile, json.dumps(options))
                self.assertEqual((result["stop_reason"], result["exit_status"], result["stdout_hex"]),
                                 ("exited", 35, "58"))
                calls = result["darwin"]["native_calls"]
                self.assertEqual([call["name"] for call in calls], ["_write", "__exit"])
                self.assertEqual(calls[1]["arguments"], ["123"])
                self.assertIsNone(calls[1]["result"])
                self.assertEqual(result["services"], [])
                options["arguments"] = ["libsystem"]
                options["event_limit"] = 1
                result = session.emulate_process(path, profile, json.dumps(options))
                self.assertEqual(result["stop_reason"], "event_limit")
                self.assertEqual(result["events"], 1)
                self.assertEqual(result["stdout_hex"], "")
                self.assertEqual(len(result["darwin"]["native_calls"]), 1)
                self.assertEqual(result["darwin"]["native_calls"][0]["name"], "___error")

    def test_darwin_classic_lazy_imports_keep_eager_slots_and_errno(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        names = ["___error", "___error", "_getpid", "_getpid", "_write", "_write",
                 "_read", "_close", "_read", "_close", "_getpid", "_write", "__exit"]
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            path = str((Path(fixtures) / f"{platform}-{architecture}-libsystem-lazy").resolve(strict=True))
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000, "arguments": ["lazy"],
                       "darwin_files": {"files": [], "stdin_hex": ""}}
            for repeat in range(2):
                with self.subTest(profile=profile, architecture=architecture, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 83)
                    self.assertEqual(result["schema_version"], 1)
                    self.assertEqual(result["stdout_hex"], "41005a53")
                    self.assertEqual(result["stderr_hex"], "")
                    self.assertEqual(result["services"], [])
                    self.assertEqual(result["events"], 13)
                    calls = result["darwin"]["native_calls"]
                    self.assertEqual([call["name"] for call in calls], names)
                    self.assertTrue(all(call["module"] == "/usr/lib/libSystem.B.dylib" for call in calls))
                    self.assertEqual(calls[0]["result"], calls[1]["result"])
                    self.assertEqual([call["result"] for call in calls[2:]],
                                     ["3e8", "3e8", "3", "ffffffffffffffff", "0", "0",
                                      "ffffffffffffffff", "ffffffffffffffff", "3e8", "1", None])
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            options["arguments"] = ["lazy", "exit"]
            result = session.emulate_process(path, profile, json.dumps(options))
            self.assertEqual((result["stop_reason"], result["exit_status"], result["stdout_hex"]),
                             ("exited", 35, "58"))
            self.assertEqual([call["name"] for call in result["darwin"]["native_calls"]], ["_write", "__exit"])
            self.assertEqual(result["darwin"]["native_calls"][-1]["arguments"], ["123"])
            options["arguments"] = ["lazy"]
            options["event_limit"] = 1
            result = session.emulate_process(path, profile, json.dumps(options))
            self.assertEqual(result["stop_reason"], "event_limit")
            self.assertEqual(result["events"], 1)
            self.assertEqual(result["stdout_hex"], "")
            self.assertEqual(len(result["darwin"]["native_calls"]), 1)

    def test_darwin_libsystem_file_calls_keep_optional_mode_and_offsets(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        names = ["___error", "_open", "_open", "_open", "_read", "_dup", "_read", "_lseek",
                 "_pread", "_lseek", "_pwrite", "_lseek", "_lseek", "_read", "_dup2", "_read",
                 "_lseek", "_dup2", "_dup2", "_lseek", "_lseek", "_lseek", "_pread", "_lseek",
                 "_lseek", "_lseek", "_pwrite", "_pread", "_close", "_close", "_close", "_open",
                 "_write", "_close", "_open", "_read", "_read", "_close", "_open", "_write"]
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            for kind in ("classic", "chained"):
                path = str((Path(fixtures) / f"{platform}-{architecture}-libsystem-files-{kind}").resolve(strict=True))
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000,
                           "arguments": ["files", "/data", "/created", "/missing"],
                           "darwin_files": {"files": [{"path": "/data", "bytes_hex": "30313233343536373839", "writable": True}],
                                            "directories": [{"path": "/", "mutable": True}], "umask": 0o27}}
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, kind=kind, repeat=repeat):
                        result = session.emulate_process(path, profile, json.dumps(options))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 73)
                        self.assertEqual(result["schema_version"], 1)
                        self.assertEqual(result["stdout_hex"], "460044")
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["services"], [])
                        self.assertEqual(result["events"], 41)
                        calls = result["darwin"]["native_calls"]
                        self.assertEqual([call["name"] for call in calls], names)
                        self.assertTrue(all(call["module"] == "/usr/lib/libSystem.B.dylib" for call in calls))
                        self.assertTrue(all(call["result"] is not None for call in calls))
                        self.assertEqual(calls[20]["result"], "100000007")
                        self.assertEqual(calls[21]["result"], "100000007")
                        self.assertEqual(calls[24]["result"], "ffffffffffffffff")
                        self.assertEqual(calls[26]["arguments"][3], "ffffffffffffffff")
                        self.assertEqual([len(calls[i]["arguments"]) for i in (1, 2, 3, 31, 34, 38)], [2, 2, 2, 3, 2, 3])
                        self.assertEqual(int(calls[31]["arguments"][2], 16) & 0xffffffff, 0x123401a1)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_libsystem_memory_calls_keep_c_checks_and_private_bytes(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        names = ["___error", "_mmap", "_mmap", "_mmap", "_mprotect", "_mprotect",
                 "_mprotect", "_write", "_munmap", "_mmap", "_mmap", "_open", "_mmap",
                 "_close", "_open", "_pread", "_close", "_munmap"]
        counts = [0, 6, 6, 6, 3, 3, 3, 3, 2, 6, 6, 2, 6, 1, 2, 4, 1, 2]
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            page = 16384 if architecture == "arm64" else 4096
            contents = b"x" * page + b"f\0l"
            for kind in ("classic", "chained"):
                path = str((Path(fixtures) / f"{platform}-{architecture}-libsystem-memory-{kind}").resolve(strict=True))
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000,
                           "arguments": ["memory", str(page), "/data"],
                           "darwin_files": {"files": [{"path": "/data", "bytes_hex": contents.hex()}]}}
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, kind=kind, repeat=repeat):
                        result = session.emulate_process(path, profile, json.dumps(options))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 79)
                        self.assertEqual(result["schema_version"], 1)
                        self.assertEqual(result["stdout_hex"], "4d0050")
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["services"], [])
                        self.assertEqual(result["events"], 19)
                        calls = result["darwin"]["native_calls"]
                        self.assertEqual([call["name"] for call in calls], names)
                        self.assertTrue(all(call["module"] == "/usr/lib/libSystem.B.dylib" for call in calls))
                        self.assertTrue(all(call["result"] is not None for call in calls))
                        self.assertEqual([len(c["arguments"]) for c in calls], counts)
                        for i in (1, 2, 5, 9, 10):
                            self.assertEqual(calls[i]["result"], "ffffffffffffffff")
                        self.assertEqual(calls[3]["arguments"][3], "1002")
                        self.assertEqual(int(calls[12]["arguments"][5], 16), page)
                        self.assertEqual(int(calls[15]["arguments"][3], 16), page)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_constructors_and_tls_callbacks_keep_process_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory_handle = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory_handle.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        modes = {"return": (37, "LPEabFM2N1", 8), "exit": (37, "LPEabFM2N1", 7),
                 "raw": (38, "LPEabFM", 4), "ctor-exit": (37, "LPEab2N1", 5),
                 "ctor-raw": (38, "LPEab", 2), "stack-exit": (37, "LPEabFMS2N1", 8),
                 "dtor-raw": (39, "LPEabFM2", 5)}
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            for kind in ("classic", "chained"):
                directory = (Path(fixtures) / f"{platform}-{architecture}-callbacks-{kind}").resolve(strict=True)
                modules = [{"name": f"@loader_path/{name}", "path": str(directory / name)}
                           for name in ("libleaf.dylib", "libmiddle.dylib")]
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000, "darwin": {"modules": modules},
                           "environment": ["NEVERD_CALLBACK_SCOPE=one"]}
                for mode, (status, output, returns) in modes.items():
                    options["arguments"] = ["different/argv-zero", mode]
                    for repeat in range(2):
                        with self.subTest(platform=platform, architecture=architecture,
                                          kind=kind, mode=mode, repeat=repeat):
                            result = session.emulate_process(str(directory / "main"),
                                                             f"{platform}-macho64-v1", json.dumps(options))
                            self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                            self.assertEqual(result["exit_status"], status)
                            self.assertEqual(result["stdout_hex"], output.encode().hex())
                            self.assertEqual(result["stderr_hex"], "")
                            self.assertEqual(result["services"], [])
                            self.assertEqual(result["schema_version"], 1)
                            calls = result["darwin"]["native_calls"]
                            registrations = [c for c in calls if c["name"] == "__tlv_atexit"]
                            self.assertEqual(len(registrations), 4 if mode == "stack-exit" else
                                             2 if "raw" in mode else 3)
                            self.assertTrue(all(c["result"] is None and len(c["arguments"]) == 2
                                                for c in registrations))
                            self.assertEqual(result["events"], len(calls) + returns)
                            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_global_and_static_destructors_keep_process_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory_handle = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory_handle.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        modes = {"return": (37, "LPBTGMStvUYLns4Ng30p2l1", 10, 4),
                 "exit": (37, "LPBTGMStvUYLns4Ng30p2l1", 10, 4),
                 "raw": (38, "LPBTGMS", 7, 4),
                 "global-raw": (38, "LPBTGMStvUYR", 10, 4),
                 "deep-exit": (37, "LPBTGMStvUYKLns4Ng30p2l1", 10, 5),
                 "ctor-exit": (37, "LPBTtvUY0p2l1", 5, 2),
                 "ctor-raw": (38, "LPBT", 3, 2),
                 "opaque": (37, "LPBTGMStvUYnnZILns4Ng30p2l1", 12, 6)}
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            for kind in ("classic", "chained"):
                directory = (Path(fixtures) / f"{platform}-{architecture}-exit-{kind}").resolve(strict=True)
                modules = [{"name": f"@loader_path/{name}", "path": str(directory / name)}
                           for name in ("libleaf.dylib", "libparent.dylib")]
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000, "darwin": {"modules": modules}}
                for mode, (status, output, count_c, count_cxx) in modes.items():
                    options["arguments"] = ["different/argv-zero", mode]
                    for repeat in range(2):
                        with self.subTest(platform=platform, architecture=architecture,
                                          kind=kind, mode=mode, repeat=repeat):
                            result = session.emulate_process(str(directory / "main"),
                                                             f"{platform}-macho64-v1", json.dumps(options))
                            self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                            self.assertEqual(result["exit_status"], status)
                            self.assertEqual(result["stdout_hex"], output.encode().hex())
                            self.assertEqual(result["stderr_hex"], "")
                            self.assertEqual(result["services"], [])
                            self.assertEqual(result["schema_version"], 1)
                            calls = result["darwin"]["native_calls"]
                            ordinary = [c for c in calls if c["name"] == "_atexit"]
                            objects = [c for c in calls if c["name"] == "___cxa_atexit"]
                            self.assertEqual(len(ordinary), count_c)
                            self.assertEqual(len(objects), count_cxx)
                            self.assertTrue(all(c["result"] == "0" for c in ordinary + objects))
                            self.assertTrue(all(len(c["arguments"]) == 1 for c in ordinary))
                            self.assertTrue(all(len(c["arguments"]) == 3 for c in objects))
                            self.assertTrue(all(c["module"] == "/usr/lib/libSystem.B.dylib" for c in calls))
                            if mode == "opaque":
                                self.assertEqual(sum(c["arguments"][1:] ==
                                                     ["fedcba9876543210", "ffffffffffffffff"]
                                                     for c in objects), 1)
                                self.assertEqual(sum(c["arguments"][1:] ==
                                                     ["0", "fedcba9876543210"]
                                                     for c in objects), 1)
                            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_default_static_guards_keep_process_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory_handle = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory_handle.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        modes = {"return": (37, "LAPBCSMTHRtUYuXVvhspbla"),
                 "exit": (37, "LAPBCSMTHRtUYuXVvhspbla"),
                 "raw": (38, "LAPBCSMTHR"),
                 "global-raw": (38, "LAPBCSMTHRtUYuQ"),
                 "ctor-exit": (37, "LAPBCSspbla"),
                 "ctor-raw": (38, "LAPBCS")}
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            for kind in ("classic", "chained"):
                directory = (Path(fixtures) / f"{platform}-{architecture}-guard-{kind}").resolve(strict=True)
                modules = [{"name": f"@loader_path/{name}", "path": str(directory / name)}
                           for name in ("libleaf.dylib", "libparent.dylib")]
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000, "darwin": {"modules": modules},
                           "darwin_system": {"thread_self_port": 0xf1234567}}
                for mode, (status, output) in modes.items():
                    options["arguments"] = ["different/argv-zero", mode]
                    for repeat in range(2):
                        with self.subTest(platform=platform, architecture=architecture,
                                          kind=kind, mode=mode, repeat=repeat):
                            result = session.emulate_process(str(directory / "main"),
                                                             f"{platform}-macho64-v1", json.dumps(options))
                            self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                            self.assertEqual(result["exit_status"], status)
                            self.assertEqual(result["stdout_hex"], output.encode().hex())
                            self.assertEqual(result["stderr_hex"], "")
                            self.assertEqual(result["services"], [])
                            self.assertEqual(result["schema_version"], 1)
                            calls = result["darwin"]["native_calls"]
                            guards = [c for c in calls if c["name"].startswith("___cxa_guard_")]
                            self.assertGreaterEqual(len(guards), 6)
                            self.assertTrue(all(c["module"] == "/usr/lib/libc++.1.dylib" and
                                                len(c["arguments"]) == 1 for c in guards))
                            self.assertTrue(all(c["module"] == "/usr/lib/libSystem.B.dylib"
                                                for c in calls if c not in guards))
                            acquire = [c for c in guards if c["name"] == "___cxa_guard_acquire"]
                            self.assertTrue(all(c["result"] in ("0", "1") for c in acquire))
                            self.assertTrue(all(c["result"] is None for c in guards if c not in acquire))
                            constructor = mode.startswith("ctor-")
                            self.assertEqual(sum(c["name"] == "___cxa_guard_abort" for c in guards),
                                             0 if constructor else 1)
                            self.assertEqual(sum(c["result"] == "0" for c in acquire),
                                             0 if constructor else 1)
                            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                for mode, output, diagnostic in (
                        ("recursive", "LAPBCSMTH", "recursive Darwin static initialization is unsupported"),
                        ("foreign", "LAPBCSMTH", "unsupported Darwin C++ guard state"),
                        ("global-reentrant", "LAPBCSMTHRtUYuE",
                         "reentrant Darwin exit during termination is unsupported")):
                    options["arguments"] = ["different/argv-zero", mode]
                    result = session.emulate_process(str(directory / "main"),
                                                     f"{platform}-macho64-v1", json.dumps(options))
                    self.assertEqual(result["stop_reason"], "runtime_failure")
                    self.assertEqual(result["diagnostic"], diagnostic)
                    self.assertEqual(result["stdout_hex"], output.encode().hex())
                    self.assertIsNone(result["exit_status"])
                options["arguments"] = ["different/argv-zero", "return"]
                result = session.emulate_process(str(directory / "main"),
                                                 f"{platform}-macho64-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                self.assertEqual(result["stdout_hex"], modes["return"][1].encode().hex())

    def test_darwin_startup_dylibs_share_data_and_isolate_tls(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            for kind in ("classic", "chained"):
                directory = (Path(fixtures) / f"{platform}-{architecture}-dylib-{kind}").resolve(strict=True)
                modules = [{"name": f"@loader_path/{name}", "path": str(directory / name)}
                           for name in ("libneverd-base.dylib", "libneverd-leaf.dylib")]
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000, "darwin": {"modules": modules}}
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, kind=kind, repeat=repeat):
                        result = session.emulate_process(str(directory / "main"), profile, json.dumps(options))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 83)
                        self.assertEqual(result["schema_version"], 1)
                        self.assertEqual(result["stdout_hex"], "440054")
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["services"], [])
                        calls = result["darwin"]["native_calls"]
                        self.assertGreater(len(calls), 10)
                        self.assertEqual(result["events"], len(calls) + 1)
                        names = [c["name"] for c in calls]
                        self.assertEqual(names[-1], "_write")
                        self.assertEqual(names.count("_getpid"), 2)
                        self.assertEqual(names.count("__tlv_get_addr"), len(calls) - 3)
                        self.assertTrue(all(c["module"] == "/usr/lib/libSystem.B.dylib" for c in calls))
                        self.assertTrue(all(c["result"] is not None for c in calls))
                        self.assertTrue(all(len(c["arguments"]) ==
                                            {"__tlv_get_addr": 1, "_getpid": 0, "_write": 3}[c["name"]]
                                            for c in calls))
                        self.assertTrue(all(c["result"] == "3e8" for c in calls if c["name"] == "_getpid"))
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_dylib_addends_initialize_data_and_tls_pointers(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        for platform, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                       ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                       ("ios-simulator", "arm64")):
            profile = f"{platform}-macho64-v1"
            for kind in ("classic", "chained"):
                directory = (Path(fixtures) /
                             f"{platform}-{architecture}-dylib-addends-{kind}").resolve(strict=True)
                options = {"backend": "unicorn", "instruction_quantum": 1024,
                           "timeout_microseconds": 5000000,
                           "darwin": {"modules": [{"name": "@loader_path/libneverd-addends.dylib",
                                                    "path": str(directory / "libneverd-addends.dylib")}]}}
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, kind=kind, repeat=repeat):
                        result = session.emulate_process(str(directory / "main"), profile, json.dumps(options))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["schema_version"], 1)
                        self.assertEqual(result["exit_status"], 78)
                        self.assertEqual(result["stdout_hex"], "")
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["services"], [])
                        calls = result["darwin"]["native_calls"]
                        names = [call["name"] for call in calls]
                        self.assertEqual(names.count("_getpid"), 1)
                        self.assertGreater(names.count("__tlv_get_addr"), 0)
                        self.assertEqual(set(names), {"_getpid", "__tlv_get_addr"})
                        self.assertEqual(result["events"], len(calls) + 1)
                        for call in calls:
                            self.assertEqual(call["module"], "/usr/lib/libSystem.B.dylib")
                            self.assertEqual(len(call["arguments"]), 0 if call["name"] == "_getpid" else 1)
                            self.assertIsNotNone(call["result"])
                            if call["name"] == "_getpid":
                                self.assertEqual(call["result"], "3e8")
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_lchown_keeps_vnode_and_target_authority(self) -> None:
        self._check_darwin_symbolic_ownership(True)

    def _check_darwin_symbolic_ownership(self, lchown: bool) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        import struct
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode, inode, size=0, gid=20):
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 2 if mode == 0o40700 else 1,
                    "uid": 501, "gid": gid, "size": size,
                    "blocks": 1 if size else 0, "block_size": 512,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        def files():
            result = {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                 "metadata": metadata(0o100600, 2, 6, 31)}],
                      "directories": [], "symbolic_links": [], "working_directory": "/", "umask": 0o22,
                      "creation_policy": {"first_inode": 100, "block_size": 512, "generation": 9,
                                          "creation_time": {"seconds": 41, "nanoseconds": 42},
                                          "mutation_policy": {"allocation_unit": 512, "mutation_time": {"seconds": 101, "nanoseconds": 102}},
                                          "namespace_policy": {"symbolic_link_allocation_unit": 512, "directory_entry_size": 32, "directory_blocks": 7}},
                      "ownership_mutations": {"objects": [{"path": "/data", "time": {"seconds": 81, "nanoseconds": 82}}],
                                              "created_regular_objects": False, "created_directory_objects": False,
                                              "created_symbolic_link_objects": True}}
            for inode, path in enumerate(("/", "/dir"), 1):
                m = metadata(0o40700, 1 if path == "/" else 3, 0 if path == "/" else 32)
                m["blocks"] = 0 if path == "/" else 7
                entry = {"path": path, "metadata": m, "mutable": True,
                         "mutation_policy": {"directory_entry_size": 16,
                                             "mutation_time": {"seconds": 91 if path == "/" else 93, "nanoseconds": 92 if path == "/" else 94}}}
                if path == "/dir":
                    entry.update(movable=True, swap_rename=True)
                result["directories"].append(entry)
            for index, (name, target) in enumerate((("link", "../data"), ("dangling", "missing"), ("cycle", "cycle"), ("other", "../data"))):
                path = "/dir/" + name
                result["symbolic_links"].append({"path": path, "target_hex": target.encode().hex(),
                                                 "metadata": metadata(0o120755, 4 + index, len(target), 31),
                                                 "mutable": True, "mutation_policy": {"mutation_time": {"seconds": 101, "nanoseconds": 102}}})
                result["ownership_mutations"]["objects"].append({"path": path, "time": {"seconds": 181 + 2 * index, "nanoseconds": 182 + 2 * index}})
            return result

        def output(kind):
            if not kind & 1:
                return b"W"
            # Independent literal stat144 records and both eight-byte guards.
            link = [0xa5a5a5a5a5a5a5a5, 7 | (0o120755 << 32) | (1 << 48),
                    100 if kind >= 2 else 4, 501 | (20 << 32), 0,
                    41 if kind >= 2 else 1, 42 if kind >= 2 else 2,
                    41 if kind >= 2 else 3, 42 if kind >= 2 else 4,
                    101 if kind >= 2 else 181, 102 if kind >= 2 else 182,
                    41 if kind >= 2 else 7, 42 if kind >= 2 else 8,
                    7, 1, 512, 9, 0, 0, 0xa5a5a5a5a5a5a5a5]
            target = [0xa5a5a5a5a5a5a5a5, 7 | (0o100600 << 32) | (1 << 48),
                      2, 501 | (31 << 32), 0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 1, 512, 9, 0, 0, 0xa5a5a5a5a5a5a5a5]
            pack = lambda words: struct.pack("<20Q", *words)
            result = pack(link) + pack(target)
            target[3], target[9], target[10] = 501 | (20 << 32), 81, 82
            result += pack(target)
            link[1] = 7 | (0o120755 << 32) | (2 << 48)
            result += pack(link) * 2
            link[1] = 7 | (0o120755 << 32)
            result += pack(link)
            replacement = [0xa5a5a5a5a5a5a5a5, 7 | (0o120755 << 32) | (1 << 48),
                           101 if kind >= 2 else 100, 501 | (20 << 32), 0,
                           41, 42, 41, 42, 101, 102, 41, 42, 7, 1, 512, 9, 0, 0, 0xa5a5a5a5a5a5a5a5]
            return result + pack(replacement) + pack(link) + b"W"

        def trace(kind, direct=False):
            result = []
            uid, gid = 501, 20 if kind else 31
            high, none, bad = 0x13579bdf00000000, 0xffffffff, 0xffffffffffffffff
            def add(number, value=0, error=False, **args):
                result.append((number, value, error, {int(k[1:]): v for k, v in args.items()}))
            def opened(fd, flags=0x200000):
                add(5, fd, a1=flags, a2=0)
            def stat(fd):
                add(339, a0=fd)
            def own(fd, u=uid, g=gid, path=False):
                if direct and path:
                    add(364, a1=u, a2=g, a3=0, a4=0, a5=0)
                else:
                    add(123, a0=fd, a1=u, a2=g)
            def nofollow(value=0, error=False, u=uid, g=gid):
                if direct:
                    add(364, value, error, a1=u, a2=g, a3=0, a4=0, a5=0)
                else:
                    add(468, value, error, a0=bad, a2=u, a3=g, a4=0x20)
            def emit():
                if kind & 1:
                    add(4, 160, a0=1, a2=160)
            if kind >= 2:
                add(57)
            opened(3); add(41, 4, a0=3); opened(5); opened(6, 0)
            stat(3); stat(6); own(3, none, none, path=True); stat(3); own(4); stat(5); stat(6); emit(); emit()
            if direct:
                add(high | 364, a1=high | uid, a2=high | gid, a3=0, a4=0, a5=0)
            else:
                add(high | 468, a0=high | 0xfffffffe, a2=high | uid, a3=high | gid, a4=high | 0x20)
            stat(3); add(16, a1=none, a2=gid); stat(6); emit()
            add(471, a0=0xfffffffe, a2=0xfffffffe, a4=0); opened(7)
            if direct:
                add(high | 364, a1=high | uid, a2=high | gid, a3=0, a4=0, a5=0)
            else:
                add(high | 123, a0=high | 7, a1=high | uid, a2=high | gid)
            stat(3); emit(); add(128)
            nofollow(2, True)
            nofollow(); stat(7); emit()
            add(10); add(10); own(3); stat(4); emit()
            add(57); opened(8); stat(8); own(8, path=True); stat(8); emit(); own(5); stat(8); stat(3); emit()
            add(57); opened(9); own(9); add(16, 2, True, a1=uid, a2=gid)
            nofollow(u=none, g=none); add(57)
            add(16, 62, True, a1=uid, a2=gid); opened(10); own(10, none, none, path=True)
            add(123, 9, True, a0=bad, a1=uid, a2=gid)
            add(468, 22, True, a0=bad, a1=1, a2=uid, a3=gid, a4=8)
            add(6, a0=3); add(123, 9, True, a0=3, a1=uid, a2=gid)
            for fd in range(4, 11):
                add(6, a0=fd)
            add(10); add(10); add(10); add(4, 1, a0=1, a2=1)
            return result

        modes = (("lchown-ownership-mutations", "virtual-lchown-ownership") if lchown else
                 ("symbolic-ownership-mutations", "virtual-symbolic-ownership", "created-symbolic-ownership", "virtual-created-symbolic-ownership"))
        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for variant in range(4 if lchown else 7):
                kind = min(variant, 1) if lchown else (variant if variant < 4 else 2 if variant == 5 else 1)
                missing, late = (variant >= 2, variant == 3) if lchown else (variant >= 4, variant == 6)
                f = files()
                if variant == (2 if lchown else 4):
                    f["ownership_mutations"]["objects"] = f["ownership_mutations"]["objects"][:1]
                elif missing:
                    f["ownership_mutations"]["created_symbolic_link_objects"] = False
                request = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                           "arguments": ["guest", modes[kind], "/dir/new" if kind >= 2 else "/dir/link"],
                           "darwin_files": f, "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                                             "real_gid": 31, "effective_gid": 20,
                                                                             "groups": [20, 31], "group_membership_uid": 4294967195}}}
                wanted, expected = trace(kind, lchown), output(kind)
                last = next((i for i, c in enumerate(trace(kind)) if c[0] == 123 and c[3].get(0) == (8 if late else 3)), None) if missing else len(wanted)
                self.assertIsNotNone(last)
                if missing:
                    expected = expected[:960] if late else b""
                original = json.dumps(request, sort_keys=True)
                for repeat in range(1 if missing else 2):
                    with self.subTest(file=file, variant=variant, repeat=repeat):
                        report = session.emulate_process(str(Path(fixtures) / file), profile, original)
                        self.assertEqual(report["stop_reason"], "unsupported_service" if missing else "exited", report["diagnostic"])
                        self.assertEqual(report["stdout_hex"], expected.hex())
                        self.assertEqual(report["stderr_hex"], "")
                        self.assertEqual(report["backend"], "unicorn")
                        self.assertEqual(len(report["services"]), last + 1 if missing else len(wanted))
                        if missing:
                            self.assertEqual(report["diagnostic"], "Darwin object ownership provider is not configured")
                        else:
                            self.assertEqual(report["exit_status"], 58)
                        for i, event in enumerate(report["services"]):
                            number, value, error, args = wanted[i]
                            self.assertEqual(event["number"], format(cls | number, "x"))
                            self.assertEqual(len(event["arguments"]), 6)
                            for j, value_arg in args.items():
                                self.assertEqual(event["arguments"][j], format(value_arg, "x"))
                            if missing and i == last:
                                self.assertIsNone(event["result"])
                                self.assertNotIn("error", event)
                            else:
                                self.assertEqual(event["result"], format(value, "x"))
                                self.assertEqual(event["error"], error)
                        self.assertEqual(json.dumps(request, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                for bad_flag in (1, None, "true"):
                    request["darwin_files"]["ownership_mutations"]["created_symbolic_link_objects"] = bad_flag
                    with self.assertRaises(NeverDError):
                        session.emulate_process(str(Path(fixtures) / file), profile, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_directory_ownership_preserves_independent_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(inode: int, size: int = 0, blocks: int = 0) -> dict:
            return {"device": 7, "inode": inode, "mode": 0o40700, "link_count": 2,
                    "uid": 501, "gid": 20, "size": size, "blocks": blocks,
                    "block_size": 512, "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for variant in range(5):
                created, missing = variant in (2, 4), variant >= 3
                mode = "created-directory-ownership" if created else "directory-ownership-mutations" if variant == 0 else "virtual-directory-ownership"
                files = {"files": [], "working_directory": "/",
                         "directories": [{"path": "/", "metadata": metadata(1), "mutable": True,
                                          "mutation_policy": {"directory_entry_size": 16,
                                                              "mutation_time": {"seconds": 91, "nanoseconds": 92}}}],
                         "ownership_mutations": {"objects": [], "created_regular_objects": created and missing,
                                            "created_directory_objects": created and not missing}}
                if created:
                    files["umask"] = 0o22
                    files["creation_policy"] = {"first_inode": 100, "block_size": 512, "generation": 9,
                                                "creation_time": {"seconds": 41, "nanoseconds": 42},
                                                "mutation_policy": {"allocation_unit": 512,
                                                                    "mutation_time": {"seconds": 101, "nanoseconds": 102}},
                                                "namespace_policy": {"symbolic_link_allocation_unit": 512,
                                                                     "directory_entry_size": 32, "directory_blocks": 7}}
                else:
                    files["directories"].append({"path": "/dir", "metadata": metadata(3, 32, 7), "movable": True,
                                                  "mutation_policy": {"directory_entry_size": 16,
                                                                      "mutation_time": {"seconds": 81, "nanoseconds": 82}}})
                    files["directories"][-1]["metadata"]["gid"] = 31
                    if not missing:
                        files["ownership_mutations"]["objects"] = [{"path": "/dir", "time": {"seconds": 81, "nanoseconds": 82}}]
                options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                           "arguments": ["guest", mode, "/child" if created else "/dir"], "darwin_files": files,
                           "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                             "real_gid": 31, "effective_gid": 20}}}
                original = json.dumps(options, sort_keys=True)
                wanted = [(136, 0, False)] if created else []
                wanted += [(5, 3, False), (41, 4, False), (5, 5, False), (339, 0, False)]
                for number in (16,123,468):
                    for high in (0,0x1234567800000000,0xffffffff00000000):
                        for pattern in range(3 if high else 0, 4):
                            wanted += [(high | number,0,False),(339,0,False)]
                            if pattern == 2:
                                wanted += [(338,0,False),(339,0,False)]
                wanted += [(123,9,True),(468,22,True),(16,2,True),(339,0,False),(128,0,False),
                           (339,0,False),(123,0,False),(338,0,False),(6,0,False),(123,9,True),
                           (123,0,False),(339,0,False),(6,0,False),(6,0,False),(4,1,False)]
                for repeat in range(2):
                    with self.subTest(file=file, variant=variant, repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures) / file), profile, original)
                        self.assertEqual(result["stop_reason"], "unsupported_service" if missing else "exited", result["diagnostic"])
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["stdout_hex"], "" if missing else "4f")
                        if missing:
                            self.assertEqual(result["diagnostic"], "Darwin object ownership provider is not configured")
                            self.assertEqual(len(result["services"]), 6 if created else 5)
                            self.assertEqual(result["services"][-1]["number"], format(cls | 16, "x"))
                            self.assertIsNone(result["services"][-1]["result"])
                            self.assertNotIn("error", result["services"][-1])
                        else:
                            self.assertEqual(result["exit_status"], 55)
                            actual = [(c["number"], c["result"], c["error"]) for c in result["services"]]
                            self.assertEqual(actual, [(format(cls | n, "x"), format(v, "x"), e) for n, v, e in wanted])
                        self.assertEqual(json.dumps(options, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                malformed = json.loads(original)
                malformed["darwin_files"]["ownership_mutations"]["created_directory_objects"] = 1
                with self.assertRaises(NeverDError):
                    session.emulate_process(str(Path(fixtures) / file), profile, json.dumps(malformed))

    def test_darwin_directory_timestamps_preserve_independent_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(inode: int, size: int = 0, blocks: int = 0) -> dict:
            return {"device": 7, "inode": inode, "mode": 0o40700, "link_count": 2,
                    "uid": 501, "gid": 20, "size": size, "blocks": blocks,
                    "block_size": 512, "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for variant in range(5):
                created, missing = variant in (2, 4), variant >= 3
                mode = "created-directory-timestamps" if created else "directory-timestamp-mutations" if variant == 0 else "virtual-directory-timestamps"
                files = {"files": [], "working_directory": "/",
                         "directories": [{"path": "/", "metadata": metadata(1), "mutable": True,
                                          "mutation_policy": {"directory_entry_size": 16,
                                                              "mutation_time": {"seconds": 91, "nanoseconds": 92}}}],
                         "timestamp_mutations": {"objects": [], "created_regular_objects": created and missing,
                                            "created_directory_objects": created and not missing}}
                if created:
                    files["umask"] = 0o22
                    files["creation_policy"] = {"first_inode": 100, "block_size": 512, "generation": 9,
                                                "creation_time": {"seconds": 41, "nanoseconds": 42},
                                                "mutation_policy": {"allocation_unit": 512,
                                                                    "mutation_time": {"seconds": 101, "nanoseconds": 102}},
                                                "namespace_policy": {"symbolic_link_allocation_unit": 512,
                                                                     "directory_entry_size": 32, "directory_blocks": 7}}
                else:
                    files["directories"].append({"path": "/dir", "metadata": metadata(3, 32, 7), "movable": True,
                                                  "mutation_policy": {"directory_entry_size": 16,
                                                                      "mutation_time": {"seconds": 81, "nanoseconds": 82}}})
                    if not missing:
                        files["timestamp_mutations"]["objects"] = [{"path": "/dir", "time": {"seconds": 111, "nanoseconds": 112}}]
                options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                           "arguments": ["guest", mode, "/child" if created else "/dir"], "darwin_files": files,
                           "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                             "real_gid": 31, "effective_gid": 20}}}
                options["darwin_time"] = {"time_of_day": {"seconds":21,"microseconds":22}}
                original = json.dumps(options, sort_keys=True)
                wanted = [(136, 0, False)] if created else []
                wanted += [(5, 3, False), (41, 4, False), (5, 5, False), (339, 0, False)]
                for number in (138,139):
                    for high in (0,0x1234567800000000,0xffffffff00000000):
                        wanted += [(high | number,0,False),(339,0,False),(338,0,False),(339,0,False)]
                wanted += [(139,0,False),(339,0,False),(138,2,True),(139,14,True),(139,9,True),(139,14,True),
                           (339,0,False),(128,0,False),(339,0,False),(139,0,False),(338,0,False),(6,0,False),
                           (139,9,True),(139,0,False),(339,0,False),(6,0,False),(6,0,False),(4,1,False)]
                for repeat in range(2):
                    with self.subTest(file=file, variant=variant, repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures) / file), profile, original)
                        self.assertEqual(result["stop_reason"], "unsupported_service" if missing else "exited", result["diagnostic"])
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["stdout_hex"], "" if missing else "54")
                        if missing:
                            self.assertEqual(result["diagnostic"], "Darwin object timestamp provider is not configured")
                            self.assertEqual(len(result["services"]), 6 if created else 5)
                            self.assertEqual(result["services"][-1]["number"], format(cls | 138, "x"))
                            self.assertIsNone(result["services"][-1]["result"])
                            self.assertNotIn("error", result["services"][-1])
                        else:
                            self.assertEqual(result["exit_status"], 54)
                            actual = [(c["number"], c["result"], c["error"]) for c in result["services"]]
                            self.assertEqual(actual, [(format(cls | n, "x"), format(v, "x"), e) for n, v, e in wanted])
                        self.assertEqual(json.dumps(options, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                malformed = json.loads(original)
                malformed["darwin_files"]["timestamp_mutations"]["created_directory_objects"] = 1
                with self.assertRaises(NeverDError):
                    session.emulate_process(str(Path(fixtures) / file), profile, json.dumps(malformed))

    def test_darwin_directory_modes_preserve_object_and_namespace_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(inode: int, size: int = 0, blocks: int = 0) -> dict:
            return {"device": 7, "inode": inode, "mode": 0o40700, "link_count": 2,
                    "uid": 501, "gid": 20, "size": size, "blocks": blocks,
                    "block_size": 512, "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for variant in range(5):
                created, missing = variant in (2, 4), variant >= 3
                mode = "created-directory-mode" if created else "directory-mode-mutations" if variant == 0 else "virtual-directory-mode"
                files = {"files": [], "working_directory": "/",
                         "directories": [{"path": "/", "metadata": metadata(1), "mutable": True,
                                          "mutation_policy": {"directory_entry_size": 16,
                                                              "mutation_time": {"seconds": 91, "nanoseconds": 92}}}],
                         "mode_mutations": {"objects": [], "created_regular_objects": created and missing,
                                            "created_directory_objects": created and not missing}}
                if created:
                    files["umask"] = 0o22
                    files["creation_policy"] = {"first_inode": 100, "block_size": 512, "generation": 9,
                                                "creation_time": {"seconds": 41, "nanoseconds": 42},
                                                "mutation_policy": {"allocation_unit": 512,
                                                                    "mutation_time": {"seconds": 101, "nanoseconds": 102}},
                                                "namespace_policy": {"symbolic_link_allocation_unit": 512,
                                                                     "directory_entry_size": 32, "directory_blocks": 7}}
                else:
                    files["directories"].append({"path": "/dir", "metadata": metadata(3, 32, 7), "movable": True,
                                                  "mutation_policy": {"directory_entry_size": 16,
                                                                      "mutation_time": {"seconds": 81, "nanoseconds": 82}}})
                    if not missing:
                        files["mode_mutations"]["objects"] = [{"path": "/dir", "time": {"seconds": 71, "nanoseconds": 72}}]
                options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                           "arguments": ["guest", mode, "/child" if created else "/dir"], "darwin_files": files,
                           "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                             "real_gid": 31, "effective_gid": 20}}}
                original = json.dumps(options, sort_keys=True)
                wanted = [(136, 0, False)] if created else []
                wanted += [(5, 3, False), (41, 4, False), (5, 5, False), (339, 0, False)]
                for number in (15, 124, 467):
                    for high in (0, 0x1234567800000000, 0xffffffff00000000):
                        wanted += [(high | number, 0, False), (339, 0, False), (338, 0, False), (339, 0, False)]
                wanted += [(13, 0, False), (15, 0, False), (467, 0, False), (339, 0, False),
                           (128, 0, False), (339, 0, False), (124, 0, False), (338, 0, False),
                           (124, 9, True), (467, 22, True), (339, 0, False), (12, 0, False), (6, 0, False),
                           (124, 9, True), (124, 0, False), (339, 0, False), (6, 0, False), (6, 0, False), (4, 1, False)]
                for repeat in range(2):
                    with self.subTest(file=file, variant=variant, repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures) / file), profile, original)
                        self.assertEqual(result["stop_reason"], "unsupported_service" if missing else "exited", result["diagnostic"])
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["stdout_hex"], "" if missing else "51")
                        if missing:
                            self.assertEqual(result["diagnostic"], "Darwin object mode provider is not configured")
                            self.assertEqual(len(result["services"]), 6 if created else 5)
                            self.assertEqual(result["services"][-1]["number"], format(cls | 15, "x"))
                            self.assertIsNone(result["services"][-1]["result"])
                            self.assertNotIn("error", result["services"][-1])
                        else:
                            self.assertEqual(result["exit_status"], 53)
                            actual = [(c["number"], c["result"], c["error"]) for c in result["services"]]
                            self.assertEqual(actual, [(format(cls | n, "x"), format(v, "x"), e) for n, v, e in wanted])
                        self.assertEqual(json.dumps(options, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                malformed = json.loads(original)
                malformed["darwin_files"]["mode_mutations"]["created_directory_objects"] = 1
                with self.assertRaises(NeverDError):
                    session.emulate_process(str(Path(fixtures) / file), profile, json.dumps(malformed))

    def test_darwin_ordinary_ownership_keep_strict_object_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 1 if size else 2, "uid": 501, "gid": 31 if size else 20,
                    "size": size, "block_size": 512, "blocks": 1 if size else 0,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x2000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-ownership-mutations", "/data"],
                       "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                         "real_gid": 31, "effective_gid": 20}},
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                                   "metadata": metadata(0o100600, 2, 6)}],
                                        "directories": [{"path": "/", "metadata": metadata(0o40700, 1, 0)}],
                                        "working_directory": "/",
                                        "ownership_mutations": {"objects": [{"path": "/data",
                                                                      "time": {"seconds": 81, "nanoseconds": 82}}]}}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 38)
                    self.assertEqual(result["stdout_hex"], "4f")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 77)
                    for api, number in enumerate((16, 123, 468)):
                        for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                            call = calls[6 + (api * 3 + h) * 6]
                            self.assertEqual(call["number"], format(high | cls | number, "x"))
                            self.assertEqual(call["result"], "0")
                            self.assertFalse(call["error"])
                    for i, value in enumerate((14, 9, 22, 22, 9)):
                        self.assertEqual(calls[60 + i]["result"], format(value, "x"))
                        self.assertTrue(calls[60 + i]["error"])
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = json.loads(original)
            request["darwin_files"]["ownership_mutations"]["objects"] = []
            result = session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["diagnostic"], "Darwin object ownership provider is not configured")
            self.assertEqual(len(result["services"]), 7)
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
            request["darwin_files"]["ownership_mutations"]["extra"] = True
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_ordinary_advisory_locks_keep_description_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 1 if size else 2, "uid": 501, "gid": 20,
                    "size": size, "block_size": 512, "blocks": 1 if size else 0,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x2000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-advisory-locks", "/data"],
                       "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                         "real_gid": 31, "effective_gid": 20}},
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                                   "metadata": metadata(0o100600, 2, 6)}],
                                        "directories": [{"path": "/", "metadata": metadata(0o40700, 1, 0)}],
                                        "working_directory": "/",
                                        "advisory_locks": {"objects": ["/data"]}}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 39)
                    self.assertEqual(result["stdout_hex"], "4b")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 77)
                    values = (0, 0, 35, 0, 0, 0, 35, 0, 35, 0, 0, 0)
                    fds = (3, 5, 4, 5, 4, 3, 3, 4, 4, 3, 4, 4)
                    actions = (2, 6, 5, 8, 5, 5, 6, 8, 6, 8, 6, 8)
                    for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                        for i, value in enumerate(values):
                            call = calls[5 + h * 17 + i]
                            self.assertEqual(call["number"], format(high | cls | 131, "x"))
                            self.assertEqual(call["result"], format(value, "x"))
                            self.assertEqual(call["error"], bool(value))
                            self.assertEqual(call["arguments"][:2], [format(high | fds[i], "x"), format(high | actions[i], "x")])
                    for i, value in enumerate((9, 9, 9, 0, 0, 0, 35, 0, 9)):
                        self.assertEqual(calls[56 + i]["result"], format(value, "x"))
                        self.assertEqual(calls[56 + i]["error"], bool(value))
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = json.loads(original)
            request["darwin_files"]["advisory_locks"]["objects"] = []
            result = session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["diagnostic"], "Darwin regular-object advisory lock provider is not configured")
            self.assertEqual(len(result["services"]), 6)
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
            request["darwin_files"]["advisory_locks"]["extra"] = True
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_filesystem_snapshots_keep_layout_and_independent_object_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device":7,"inode":inode,"mode":mode,"link_count":2 if mode==0o40700 else 1,"uid":501,"gid":20,"size":size,"block_size":512,"blocks":1 if size else 0,"flags":0,"generation":9,"access_time":{"seconds":1,"nanoseconds":2},"modification_time":{"seconds":3,"nanoseconds":4},"change_time":{"seconds":5,"nanoseconds":6},"birth_time":{"seconds":7,"nanoseconds":8}}
        snapshot = {"block_size":270544960,"io_size":-4096,"blocks":"18364758544493064720","free_blocks":"72623859790382856","available_blocks":0,"files":"18446744073709551615","free_files":9,"filesystem_id":[-1,-2147483648],"owner":4294967294,"type":26,"flags":16909060,"subtype":7,"type_name":"virtfs","mount_point":"/虚拟","mounted_from":"neverd:declared","extended_flags":2271560481}
        base = {"files":[{"path":"/data","bytes_hex":"616200ff6566","writable":True,"metadata":metadata(0o100600,2,6)}],"directories":[{"path":"/","metadata":metadata(0o40700,1,0)}],"working_directory":"/","filesystem_observations":{"objects":[{"path":"/data","snapshot":snapshot},{"path":"/","snapshot":snapshot}]}}
        # Independent literal prefix and guarded record. No native count oracle
        # and no production serializer supplies these expected bytes.
        prefix = bytes.fromhex("4030201000f0ffff1032547698badcfe08070605040302010000000000000000ffffffffffffffff0900000000000000ffffffff00000080feffffff1a0000000403020107000000")
        record = bytearray(b"\xa5"*16 + b"\0"*2168 + b"\xa5"*16)
        record[16:88] = prefix
        record[88:95] = b"virtfs\0"
        mount = "/虚拟".encode()+b"\0"
        record[104:104+len(mount)] = mount
        record[1128:1144] = b"neverd:declared\0"
        record[2152:2156] = bytes.fromhex("21436587")
        literal = bytes(record)*2
        self.assertEqual(len(record),2200)
        mask=(1<<64)-1
        common=[(5,3,False,None,None),(5,4,False,None,None),(41,5,False,3,None),(5,6,False,None,None),(199,2,False,5,None),(92,0,False,3,None),(339,0,False,3,None)]
        for high in (0,0x1234567800000000,0xffffffff00000000):
            common.append((high|345,0,False,None,None))
            for fd in (3,4,5,6):common.append((high|346,0,False,high|fd,None))
            common.extend([(339,0,False,3,None),(199,2,False,5,None),(199,0,False,4,None),(92,0,False,3,None),(92,1,False,4,None)])
        common.extend([(345,2,True,None,1),(346,9,True,mask,1),(346,14,True,3,1),(346,14,True,3,0),(345,14,True,None,1),(339,0,False,4,None),(6,0,False,3,None),(346,9,True,3,1),(6,0,False,5,None),(6,0,False,4,None),(6,0,False,6,None),(4,1,False,1,None)])
        virtual=[(5,3,False,None,None),(346,0,False,3,None),(41,4,False,3,None),(346,0,False,4,None),(6,0,False,3,None),(346,9,True,3,1),(6,0,False,4,None),(4,2200,False,1,None),(4,2200,False,1,None)]
        self.assertEqual(len(common),49)
        for file,profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls=0x2000000 if file.endswith("x86_64") else 0
            for created in (False,True):
                files=json.loads(json.dumps(base))
                if created:
                    parent=files["directories"][0]
                    parent["mutable"]=True
                    parent["mutation_policy"]={"directory_entry_size":16,"mutation_time":{"seconds":91,"nanoseconds":92}}
                    files["umask"]=18
                    files["creation_policy"]={"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}}
                    files["filesystem_observations"]["created_regular_objects"]=True
                request={"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","virtual-filesystem-creation" if created else "filesystem-observations","/data"],"darwin_system":{"credentials":{"real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20}},"darwin_files":files}
                original=json.dumps(request,sort_keys=True)
                for repeat in range(2):
                    with self.subTest(file=file,created=created,repeat=repeat):
                        result=session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(request))
                        self.assertEqual(result["stop_reason"],"exited",result["diagnostic"])
                        self.assertEqual(result["exit_status"],51 if created else 50)
                        self.assertEqual(result["stdout_hex"],literal.hex() if created else "57")
                        self.assertEqual(result["stderr_hex"],"")
                        wanted=virtual if created else common
                        self.assertEqual(len(result["services"]),len(wanted))
                        for call,(number,value,error,obj,pointer) in zip(result["services"],wanted):
                            self.assertEqual(call["number"],format(cls|number,"x"))
                            self.assertEqual(call["result"],format(value,"x"))
                            self.assertEqual(call["error"],error)
                            for index,known in enumerate((obj,pointer)):
                                if known is not None:self.assertEqual(call["arguments"][index],format(known,"x"))
                            if number&0xffffff in (345,346):self.assertEqual(call["arguments"][2:],[format(mask,"x")]*4)
                        self.assertEqual(json.dumps(request,sort_keys=True),original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
                for missing in (False,True):
                    refused=json.loads(original)
                    if missing:del refused["darwin_files"]["filesystem_observations"]
                    else:refused["darwin_files"]["filesystem_observations"]={"objects":[]}
                    result=session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                    self.assertEqual(result["stop_reason"],"unsupported_service")
                    self.assertEqual(len(result["services"]),2 if created else 8)
                    self.assertIsNone(result["services"][-1]["result"])
                    self.assertNotIn("error",result["services"][-1])
                    self.assertEqual(result["diagnostic"],"Darwin filesystem observation environment is not configured" if missing else "Darwin object filesystem snapshot is not configured")
                refused=json.loads(original)
                refused["darwin_files"]["filesystem_observations"]["created_regular_objects"]=1
                with self.assertRaises(NeverDError):
                    session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                self.assertEqual(host.call("neverd_session_is_loaded",handle),0)


    def test_darwin_created_directory_filesystems_keep_parent_and_held_lifetime(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        root = {"device": 7, "inode": 1, "mode": 0o40700, "link_count": 2,
                "uid": 501, "gid": 20, "size": 0, "block_size": 512, "blocks": 0,
                "flags": 0, "generation": 9,
                "access_time": {"seconds": 1, "nanoseconds": 2},
                "modification_time": {"seconds": 3, "nanoseconds": 4},
                "change_time": {"seconds": 5, "nanoseconds": 6},
                "birth_time": {"seconds": 7, "nanoseconds": 8}}
        snapshot = {"block_size": 270544960, "io_size": -4096,
                    "blocks": "18364758544493064720", "free_blocks": "72623859790382856",
                    "available_blocks": 0, "files": "18446744073709551615", "free_files": 9,
                    "filesystem_id": [-1, -2147483648], "owner": 4294967294, "type": 26,
                    "flags": 16909060, "subtype": 7, "type_name": "virtfs",
                    "mount_point": "/虚拟", "mounted_from": "neverd:declared",
                    "extended_flags": 2271560481}
        base = {"files": [], "directories": [{"path": "/", "metadata": root, "mutable": True,
                "mutation_policy": {"directory_entry_size": 16,
                                    "mutation_time": {"seconds": 91, "nanoseconds": 92}}}],
                "working_directory": "/", "umask": 18,
                "creation_policy": {"first_inode": 100, "block_size": 512, "generation": 9,
                    "creation_time": {"seconds": 41, "nanoseconds": 42},
                    "mutation_policy": {"allocation_unit": 512,
                                        "mutation_time": {"seconds": 101, "nanoseconds": 102}},
                    "namespace_policy": {"symbolic_link_allocation_unit": 512,
                                         "directory_entry_size": 32, "directory_blocks": 7}},
                "filesystem_observations": {"objects": [{"path": "/", "snapshot": snapshot}],
                                           "created_directory_objects": True,
                                           "created_regular_objects": False}}
        # Independent literal2168-byte record, with16-byte guards on both sides.
        prefix = bytes.fromhex("4030201000f0ffff1032547698badcfe08070605040302010000000000000000ffffffffffffffff0900000000000000ffffffff00000080feffffff1a0000000403020107000000")
        record = bytearray(b"\xa5" * 16 + b"\0" * 2168 + b"\xa5" * 16)
        record[16:88] = prefix
        record[88:95] = b"virtfs\0"
        mount = "/虚拟".encode() + b"\0"
        record[104:104 + len(mount)] = mount
        record[1128:1144] = b"neverd:declared\0"
        record[2152:2156] = bytes.fromhex("21436587")
        self.assertEqual(len(record), 2200)
        mask = (1 << 64) - 1
        common = [(136,0,False,None,None),(5,3,False,None,None),
                  (41,4,False,3,None),(346,0,False,3,None),
                  (345,0,False,None,None),(339,0,False,3,None)]
        for high in (0x1234567800000000, 0xffffffff00000000):
            common.extend([(high|346,0,False,high|4,None),(high|345,0,False,None,None)])
        common.extend([(339,0,False,3,None),(92,0,False,3,None)])
        middle = [(346,9,True,mask,1),(346,14,True,3,1),(345,2,True,None,1),
                  (128,0,False,None,None),(345,2,True,None,1),(346,0,False,4,None),
                  (345,0,False,None,None),(137,0,False,None,None),
                  (345,2,True,None,1),(346,0,False,4,None),(136,0,False,None,None),
                  (5,5,False,None,None),(346,0,False,5,None),(339,0,False,5,None)]
        tail = [(346,0,False,4,None),(6,0,False,3,None),(346,9,True,3,1),
                (6,0,False,4,None),(6,0,False,5,None),(137,0,False,None,None),
                (4,1,False,1,None)]
        for file, profile in (("macos-arm64","macos-macho64-v1"),
                              ("macos-x86_64","macos-macho64-v1"),
                              ("ios-arm64","ios-macho64-v1"),
                              ("ios-simulator-arm64","ios-simulator-macho64-v1"),
                              ("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls = 0x2000000 if file.endswith("x86_64") else 0
            path = str(Path(fixtures) / file)
            for variant in range(6):
                literal, missing = variant == 1, variant >= 2
                files = json.loads(json.dumps(base))
                if variant == 2:
                    del files["filesystem_observations"]
                elif variant == 3:
                    files["filesystem_observations"]["objects"] = []
                elif variant >= 4:
                    files["filesystem_observations"]["created_directory_objects"] = False
                    files["filesystem_observations"]["created_regular_objects"] = variant == 4
                request = {"backend":"unicorn","instruction_quantum":1024,
                           "timeout_microseconds":5000000,
                           "arguments":["guest","virtual-created-directory-filesystem" if literal
                                        else "created-directory-filesystem","/child"],
                           "darwin_system":{"credentials":{"real_uid":700,"effective_uid":501,
                                                         "real_gid":31,"effective_gid":20}},
                           "darwin_files":files}
                original = json.dumps(request, sort_keys=True)
                for repeat in range(1 if missing else 2):
                    with self.subTest(file=file, variant=variant, repeat=repeat):
                        result = session.emulate_process(path, profile, json.dumps(request))
                        self.assertEqual(result["stop_reason"], "unsupported_service" if missing else "exited", result["diagnostic"])
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(result["stdout_hex"], "" if missing else
                                         (bytes(record)*2+b"D").hex() if literal else "44")
                        calls = result["services"]
                        if missing:
                            self.assertEqual(len(calls), 4)
                            self.assertEqual(calls[-1]["number"], format(cls|346, "x"))
                            self.assertIsNone(calls[-1]["result"])
                            self.assertNotIn("error", calls[-1])
                            self.assertEqual(result["diagnostic"],
                                "Darwin filesystem observation environment is not configured" if variant == 2
                                else "Darwin object filesystem snapshot is not configured")
                        else:
                            self.assertEqual(result["exit_status"], 56)
                            emit = [(4,2200,False,1,None)] if literal else []
                            wanted = common + emit + middle + emit + tail
                            self.assertEqual(len(calls), 35 if literal else 33)
                            for call, (number,value,error,obj,out) in zip(calls, wanted):
                                self.assertEqual(call["number"], format(cls|number, "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], error)
                                for index, known in enumerate((obj, out)):
                                    if known is not None:
                                        self.assertEqual(call["arguments"][index], format(known, "x"))
                                if number&0xffffff in (345,346):
                                    self.assertEqual(call["arguments"][2:], [format(mask, "x")]*4)
                        self.assertEqual(json.dumps(request, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle), 0)
            malformed = json.loads(original)
            malformed["darwin_files"]["filesystem_observations"]["created_directory_objects"] = 1
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(malformed))
            self.assertEqual(host.call("neverd_session_is_loaded",handle), 0)



    def test_darwin_symbolic_filesystems_keep_own_and_followed_snapshots(self) -> None:
        library, fixtures = os.environ.get("NEVERD_TEST_LIBNEVERD"), os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        def metadata(mode, inode, size):
            return {"device":7,"inode":inode,"mode":mode,"link_count":2 if mode==0o40700 else 1,
                    "uid":501,"gid":20,"size":size,"block_size":512,"blocks":1 if size else 0,
                    "flags":0,"generation":9,
                    "access_time":{"seconds":1,"nanoseconds":2},
                    "modification_time":{"seconds":3,"nanoseconds":4},
                    "change_time":{"seconds":5,"nanoseconds":6},
                    "birth_time":{"seconds":7,"nanoseconds":8}}
        snapshot = {"block_size":270544960,"io_size":-4096,"blocks":"18364758544493064720",
                    "free_blocks":"72623859790382856","available_blocks":0,
                    "files":"18446744073709551615","free_files":9,
                    "filesystem_id":[-1,-2147483648],"owner":4294967294,"type":26,
                    "flags":16909060,"subtype":7,"type_name":"virtfs","mount_point":"/虚拟",
                    "mounted_from":"neverd:declared","extended_flags":2271560481}
        base = {"files":[{"path":"/data","bytes_hex":"616200ff6566","metadata":metadata(0o100600,2,6)}],
                "directories":[{"path":"/","metadata":metadata(0o40700,1,0),"mutable":True,
                "mutation_policy":{"directory_entry_size":16,"mutation_time":{"seconds":91,"nanoseconds":92}}}],
                "working_directory":"/","umask":18,
                "creation_policy":{"first_inode":100,"block_size":512,"generation":9,
                    "creation_time":{"seconds":41,"nanoseconds":42},
                    "mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}},
                    "namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}},
                "filesystem_observations":{"objects":[{"path":"/","snapshot":snapshot},{"path":"/data","snapshot":snapshot}],
                    "created_directory_objects":False,"created_regular_objects":False,"created_symbolic_link_objects":True}}
        # Independently authored full2168-byte payload and both16-byte guards.
        prefix = bytes.fromhex("4030201000f0ffff1032547698badcfe08070605040302010000000000000000ffffffffffffffff0900000000000000ffffffff00000080feffffff1a0000000403020107000000")
        record = bytearray(b"\xa5"*16+b"\0"*2168+b"\xa5"*16)
        record[16:88]=prefix;record[88:95]=b"virtfs\0"
        mount="/虚拟".encode()+b"\0";record[104:104+len(mount)]=mount
        record[1128:1144]=b"neverd:declared\0";record[2152:2156]=bytes.fromhex("21436587")
        self.assertEqual(len(record),2200)
        own=bytearray(record);own[72:76]=bytes.fromhex("11000000")
        mask=(1<<64)-1
        common=[(5,3,False,None,None),(41,4,False,3,None),(346,0,False,3,None),
                (345,0,False,None,None),(339,0,False,3,None)]
        for high in (0x1234567800000000,0xffffffff00000000):
            common.extend([(high|346,0,False,high|4,None),(high|345,0,False,None,None)])
        common.append((339,0,False,3,None))
        middle=[(346,9,True,mask,1),(346,14,True,3,1),(345,2,True,None,1),
                (128,0,False,None,None),(345,2,True,None,1),(346,0,False,4,None),
                (345,0,False,None,None),(10,0,False,None,None),(345,2,True,None,1),
                (346,0,False,4,None),(57,0,False,None,None),(5,5,False,None,None),
                (346,0,False,5,None),(339,0,False,5,None)]
        tail=[(346,0,False,4,None),(345,2,True,None,1),(6,0,False,3,None),
              (346,9,True,3,1),(6,0,False,4,None),(6,0,False,5,None),
              (10,0,False,None,None),(4,1,False,1,None)]
        modes=("symbolic-filesystem-observations","created-symbolic-filesystem","virtual-symbolic-filesystem")
        for file,profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),
                             ("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),
                             ("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls=0x2000000 if file.endswith("x86_64") else 0;path=str(Path(fixtures)/file)
            for variant in range(6):
                missing=variant>=3;kind=1 if missing else variant
                files=json.loads(json.dumps(base))
                if kind!=1:
                    files["symbolic_links"]=[{"path":"/link","target_hex":"64617461","metadata":metadata(0o120777,10,4),
                        "mutable":True,"mutation_policy":{"mutation_time":{"seconds":81,"nanoseconds":82}}}]
                    link_snapshot=json.loads(json.dumps(snapshot))
                    if kind==2:link_snapshot["owner"]=17
                    files["filesystem_observations"]["objects"].append({"path":"/link","snapshot":link_snapshot})
                if variant==3:del files["filesystem_observations"]
                elif variant==4:files["filesystem_observations"]["objects"]=[]
                elif variant==5:
                    files["filesystem_observations"]["created_symbolic_link_objects"]=False
                    files["filesystem_observations"]["created_regular_objects"]=True
                    files["filesystem_observations"]["created_directory_objects"]=True
                request={"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,
                    "arguments":["guest",modes[kind],"/link"],"darwin_files":files,
                    "darwin_system":{"credentials":{"real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20}}}
                original=json.dumps(request,sort_keys=True)
                for repeat in range(1 if missing else 2):
                    with self.subTest(file=file,variant=variant,repeat=repeat):
                        result=session.emulate_process(path,profile,json.dumps(request))
                        self.assertEqual(result["stop_reason"],"unsupported_service" if missing else "exited",result["diagnostic"])
                        self.assertEqual(result["stderr_hex"],"")
                        self.assertEqual(result["stdout_hex"],"" if missing else
                                         (bytes(own)+bytes(record)*2+b"Y").hex() if kind==2 else "59")
                        calls=result["services"]
                        if missing:
                            self.assertEqual(len(calls),4);self.assertEqual(calls[-1]["number"],format(cls|346,"x"))
                            self.assertIsNone(calls[-1]["result"]);self.assertNotIn("error",calls[-1])
                            self.assertEqual(result["diagnostic"],"Darwin filesystem observation environment is not configured" if variant==3
                                             else "Darwin object filesystem snapshot is not configured")
                        else:
                            self.assertEqual(result["exit_status"],57)
                            emit=[(4,2200,False,1,None)] if kind==2 else []
                            wanted=([(57,0,False,None,None)] if kind==1 else [])+common+emit+emit+middle+emit+tail
                            self.assertEqual(len(calls),33 if kind==1 else 35 if kind==2 else 32)
                            for call,(number,value,error,obj,out) in zip(calls,wanted):
                                self.assertEqual(call["number"],format(cls|number,"x"))
                                self.assertEqual(call["result"],format(value,"x"));self.assertEqual(call["error"],error)
                                for index,known in enumerate((obj,out)):
                                    if known is not None:self.assertEqual(call["arguments"][index],format(known,"x"))
                                if number&0xffffff in (345,346):self.assertEqual(call["arguments"][2:],[format(mask,"x")]*4)
                        self.assertEqual(json.dumps(request,sort_keys=True),original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
            malformed=json.loads(original)
            malformed["darwin_files"]["filesystem_observations"]["created_symbolic_link_objects"]=1
            with self.assertRaises(NeverDError):session.emulate_process(path,profile,json.dumps(malformed))
            self.assertEqual(host.call("neverd_session_is_loaded",handle),0)



    def test_darwin_mount_enumeration_keeps_order_and_refresh_cache(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        snapshot = {"block_size":270544960,"io_size":-4096,"blocks":"18364758544493064720","free_blocks":"72623859790382856","available_blocks":0,"files":"18446744073709551615","free_files":9,"filesystem_id":[-1,-2147483648],"owner":4294967294,"type":26,"flags":16909060,"subtype":7,"type_name":"virtfs","mount_point":"/虚拟","mounted_from":"neverd:declared","extended_flags":2271560481}
        zero = {key: "" if key in ("type_name", "mount_point", "mounted_from") else [0, 0] if key == "filesystem_id" else 0 for key in snapshot}
        files = {"files": [], "filesystem_enumeration": {"mounts": [
            {"cached":snapshot,"refreshed":zero}, {"cached":zero,"refreshed":snapshot},
            {"cached":snapshot,"refreshed":zero}]}}
        # Literal bytes independent of the runtime serializer.
        record = bytearray(2168)
        record[:72] = bytes.fromhex("4030201000f0ffff1032547698badcfe08070605040302010000000000000000ffffffffffffffff0900000000000000ffffffff00000080feffffff1a0000000403020107000000")
        record[72:79] = b"virtfs\0"
        mount = "/虚拟".encode() + b"\0"
        record[88:88+len(mount)] = mount
        record[1112:1128] = b"neverd:declared\0"
        record[2136:2140] = bytes.fromhex("21436587")
        self.assertEqual(len(record), 2168)
        initial = b"\xa5"*16 + bytes(record) + bytes(2168) + bytes(record) + b"\xa5"*16
        refreshed = b"\xa5"*16 + bytes(2168) + bytes(record) + bytes(2168) + b"\xa5"*16
        literal = initial + refreshed*2 + b"M"
        mask = (1 << 64)-1
        def services(virtual: bool) -> list:
            rows = []
            def call(pointer, size, flags, value, error=False, number=347):
                rows.append((number,value,error,[pointer,size,flags,mask,mask,mask]))
            def emit():
                if virtual: rows.append((4,6536,False,[1,None,6536,0,0,0]))
            call(0,0,2,3)
            call(0,mask,0,3)
            call(1,0,0,0)
            call(1,2167,4,0)
            call(None,4335,2,1)
            call(None,4336,2,2)
            call(None,6504,2,3)
            emit()
            call(1,0xffffffff,2,14,True)
            call(None,6504,1,3)
            emit()
            call(None,6504,2,3)
            emit()
            for high in (0,0x1234567800000000,0xffffffff00000000):
                call(None,high|4336,high|2,2,number=high|347)
            call(1,2168,1,14,True)
            call(0,2168,2,3)
            rows.append((4,1,False,[1,None,1,0,0,0]))
            return rows
        for file, profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for virtual in (False, True):
                request = {"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","virtual-mount-enumeration" if virtual else "mount-enumeration"],"darwin_files":files}
                original = json.dumps(request,sort_keys=True)
                for repeat in range(2):
                    with self.subTest(file=file,virtual=virtual,repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(request))
                        self.assertEqual(result["stop_reason"],"exited",result["diagnostic"])
                        self.assertEqual(result["exit_status"],53 if virtual else 52)
                        self.assertEqual(result["stdout_hex"],literal.hex() if virtual else "4d")
                        self.assertEqual(result["stderr_hex"],"")
                        wanted = services(virtual)
                        self.assertEqual(len(wanted),19 if virtual else 16)
                        self.assertEqual(len(result["services"]),len(wanted))
                        for event,(number,value,error,args) in zip(result["services"],wanted):
                            self.assertEqual(event["number"],format(cls|number,"x"))
                            self.assertEqual(event["result"],format(value,"x"))
                            self.assertEqual(event["error"],error)
                            self.assertEqual(len(event["arguments"]),6)
                            for actual, known in zip(event["arguments"],args):
                                if known is not None: self.assertEqual(actual,format(known,"x"))
                        self.assertEqual(json.dumps(request,sort_keys=True),original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
            request["darwin_files"] = {"files": []}
            result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(request))
            self.assertEqual(result["stop_reason"],"unsupported_service")
            self.assertEqual(result["diagnostic"],"Darwin mount enumeration environment is not configured")
            self.assertEqual(result["stdout_hex"],"")
            self.assertEqual(len(result["services"]),1)
            self.assertIsNone(result["services"][0]["result"])
            self.assertNotIn("error",result["services"][0])

    def test_darwin_timestamp_mutations_keep_explicit_clock_and_object_authority(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device":7,"inode":inode,"mode":mode,"link_count":2 if mode==0o40700 else 1,"uid":501,"gid":20,"size":size,"block_size":512,"blocks":1 if size else 0,"flags":0,"generation":9,"access_time":{"seconds":1,"nanoseconds":2},"modification_time":{"seconds":3,"nanoseconds":4},"change_time":{"seconds":5,"nanoseconds":6},"birth_time":{"seconds":7,"nanoseconds":8}}
        base = {"files":[{"path":"/data","bytes_hex":"616200ff6566","metadata":metadata(0o100600,2,6)}],"directories":[{"path":"/","metadata":metadata(0o40700,1,0)}],"working_directory":"/","timestamp_mutations":{"objects":[{"path":"/data","time":{"seconds":111,"nanoseconds":112}}]}}
        common = [(5,3,False,None,None),(5,4,False,None,None),(41,5,False,3,None),(199,0,False,5,None),(199,2,False,5,None),(92,0,False,3,None)]
        mask = 0xffffffffffffffff
        for n in range(6):
            high = (0,0x1234567800000000,0xffffffff00000000)[n%3]
            for fd in (False,True):
                common += [(339,0,False,3,None),(high|(139 if fd else 138),0,False,high|5 if fd else None,None)]
                common += [(339,0,False,f,None) for f in (3,4,5)]
                common += [(199,2,False,5,None),(199,0,False,4,None),(92,0,False,3,None),(92,0,False,4,None)]
        common += [(339,0,False,4,None),(138,2,True,None,1),(139,14,True,mask,1),(139,9,True,mask,None),(339,0,False,4,None),(6,0,False,3,None),(139,9,True,3,None),(6,0,False,5,None),(6,0,False,4,None),(4,1,False,1,None)]
        virtual = [(5,3,False,None,None),(339,0,False,3,None),(139,0,False,3,None),(339,0,False,3,None),(139,0,False,3,0),(339,0,False,3,None),(6,0,False,3,None),(139,9,True,3,None),(4,160,False,1,None),(4,160,False,1,None)]
        self.assertEqual(len(common),124)
        record = bytearray(160)
        record[:8] = record[152:] = b"\xa5"*8
        def store(offset: int, width: int, value: int) -> None:
            record[8+offset:8+offset+width] = value.to_bytes(width,"little")
        for offset,width,value in ((0,4,7),(4,2,0o100600),(6,2,1),(8,8,100),(16,4,501),(20,4,20),(112,4,512),(120,4,9)):
            store(offset,width,value)
        for offset in (32,48,64,80):
            store(offset,8,41);store(offset+8,8,42)
        before = bytes(record)
        for offset in (32,48,80):
            store(offset,8,0);store(offset+8,8,0)
        store(64,8,101);store(72,8,102)
        literal = before + record
        for file,profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for created in (False,True):
                files = json.loads(json.dumps(base))
                if created:
                    files["umask"] = 18
                    files["directories"][0].update(mutable=True,mutation_policy={"directory_entry_size":16,"mutation_time":{"seconds":91,"nanoseconds":92}})
                    files["creation_policy"] = {"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}}
                    files["timestamp_mutations"] = {"objects":[],"created_regular_objects":True}
                request = {"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","virtual-timestamp-creation" if created else "ordinary-timestamp-mutations","/data"],"darwin_system":{"credentials":{"real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20}},"darwin_time":{"time_of_day":{"seconds":0,"microseconds":0}},"darwin_files":files}
                original = json.dumps(request,sort_keys=True)
                for repeat in range(2):
                    with self.subTest(file=file,created=created,repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(request))
                        self.assertEqual(result["stop_reason"],"exited",result["diagnostic"])
                        self.assertEqual(result["exit_status"],49 if created else 48)
                        self.assertEqual(result["stdout_hex"],literal.hex() if created else "54")
                        self.assertEqual(result["stderr_hex"],"")
                        wanted = virtual if created else common
                        self.assertEqual(len(result["services"]),len(wanted))
                        for call,(number,value,error,obj,pointer) in zip(result["services"],wanted):
                            self.assertEqual(call["number"],format(cls|number,"x"))
                            self.assertEqual(call["result"],format(value,"x"))
                            self.assertEqual(call["error"],error)
                            for index,known in enumerate((obj,pointer)):
                                if known is not None:self.assertEqual(call["arguments"][index],format(known,"x"))
                            if number&0xffffff in (138,139):
                                self.assertEqual(call["arguments"][2:], [format(mask,"x")]*4)
                        self.assertEqual(json.dumps(request,sort_keys=True),original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
                for missing in (False,True):
                    refused = json.loads(original)
                    if missing:del refused["darwin_files"]["timestamp_mutations"]
                    else:refused["darwin_files"]["timestamp_mutations"] = {"objects":[]}
                    result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                    self.assertEqual(result["stop_reason"],"unsupported_service")
                    self.assertEqual(len(result["services"]),3 if created else 8)
                    self.assertIsNone(result["services"][-1]["result"])
                    self.assertNotIn("error",result["services"][-1])
                    self.assertEqual(result["diagnostic"],"Darwin ordinary timestamp environment is not configured" if missing else "Darwin object timestamp provider is not configured")
                if created:
                    refused = json.loads(original);del refused["darwin_time"]
                    result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                    self.assertEqual(result["stop_reason"],"unsupported_service")
                    self.assertEqual(result["diagnostic"],"Darwin time_of_day is not configured")
                    self.assertEqual(len(result["services"]),5)
                refused = json.loads(original)
                refused["darwin_files"]["timestamp_mutations"]["created_regular_objects"] = 1
                with self.assertRaises(NeverDError):
                    session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                self.assertEqual(host.call("neverd_session_is_loaded",handle),0)

    def test_darwin_full_barrier_synchronization_keep_independent_object_state(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device":7,"inode":inode,"mode":mode,"link_count":2 if mode==0o40700 else 1,"uid":501,"gid":20,"size":size,"block_size":512,"blocks":1 if size else 0,"flags":0,"generation":9,"access_time":{"seconds":1,"nanoseconds":2},"modification_time":{"seconds":3,"nanoseconds":4},"change_time":{"seconds":5,"nanoseconds":6},"birth_time":{"seconds":7,"nanoseconds":8}}
        base = {"files":[{"path":"/data","bytes_hex":"616200ff6566","metadata":metadata(0o100600,2,6)}],"directories":[{"path":"/","metadata":metadata(0o40700,1,0)}],"working_directory":"/","full_synchronization":{"objects":["/data"]},"barrier_synchronization":{"objects":["/data"]}}
        common = [(5,3,False,None,0,0),(5,4,False,None,0,0),(41,5,False,3,0,0),(199,2,False,5,2,0),(339,0,False,3,None,0),(92,0,False,3,3,0)]
        for h, high in enumerate((0,0x1234567800000000,0xffffffff00000000)):
            for number in (92,406):
                for command in (51,85):
                    argument = (0,1,0xffffffffffffffff)[h]
                    for fd in (3,5,4,0xffffffff):
                        common.append((high|number,9 if fd==0xffffffff else 0,fd==0xffffffff,high|fd,high|command,argument))
                    common += [(339,0,False,3,None,0),(339,0,False,4,None,0),(339,0,False,5,None,0),(199,2,False,5,0,1),(199,0,False,4,0,1),(92,0,False,3,3,0),(92,0,False,4,3,0)]
        common += [(6,0,False,3,0,0),(92,9,True,3,51,0xffffffffffffffff),(406,0,False,5,85,0xffffffffffffffff),(6,0,False,5,0,0),(92,9,True,5,51,0xffffffffffffffff),(406,0,False,4,85,0xffffffffffffffff),(339,0,False,4,None,0),(92,0,False,4,3,0),(6,0,False,4,0,0),(4,1,False,1,None,1)]
        virtual = [(5,3,False,None,0x202,0o600),(339,0,False,3,None,0),(92,0,False,3,51,0xffffffffffffffff),(406,0,False,3,85,0xffffffffffffffff),(339,0,False,3,None,0),(6,0,False,3,0,0),(92,9,True,3,51,0xffffffffffffffff),(406,9,True,3,85,0xffffffffffffffff),(4,160,False,1,None,160),(4,160,False,1,None,160)]
        self.assertEqual(len(common),148)
        record = bytearray(160)
        record[:8] = record[152:] = b"\xa5"*8
        def store(offset: int, width: int, value: int) -> None:
            record[8+offset:8+offset+width] = value.to_bytes(width,"little")
        for offset,width,value in ((0,4,7),(4,2,0o100600),(6,2,1),(8,8,100),(16,4,501),(20,4,20),(112,4,512),(120,4,9)):
            store(offset,width,value)
        for offset in (32,48,64,80):
            store(offset,8,41)
            store(offset+8,8,42)
        for file, profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            cls = 0x2000000 if file.endswith("x86_64") else 0
            for created in (False,True):
                files = json.loads(json.dumps(base))
                if created:
                    files["umask"] = 18
                    files["directories"][0].update(mutable=True,mutation_policy={"directory_entry_size":16,"mutation_time":{"seconds":91,"nanoseconds":92}})
                    files["creation_policy"] = {"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}}
                    for purpose in ("full_synchronization","barrier_synchronization"):
                        files[purpose] = {"objects":[],"created_regular_objects":True}
                request = {"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","virtual-full-barrier-creation" if created else "ordinary-full-barrier-synchronization","/data"],"darwin_system":{"credentials":{"real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20}},"darwin_files":files}
                original = json.dumps(request,sort_keys=True)
                for repeat in range(2):
                    with self.subTest(file=file,created=created,repeat=repeat):
                        result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(request))
                        self.assertEqual(result["stop_reason"],"exited",result["diagnostic"])
                        self.assertEqual(result["exit_status"],47 if created else 46)
                        self.assertEqual(result["stdout_hex"],(record*2).hex() if created else "46")
                        self.assertEqual(result["stderr_hex"],"")
                        calls = result["services"]
                        wanted = virtual if created else common
                        self.assertEqual(len(calls),len(wanted))
                        for call,(number,value,error,fd,command,argument) in zip(calls,wanted):
                            self.assertEqual(call["number"],format(cls|number,"x"))
                            self.assertEqual(call["result"],format(value,"x"))
                            self.assertEqual(call["error"],error)
                            for index,known in enumerate((fd,command,argument)):
                                if known is not None:
                                    self.assertEqual(call["arguments"][index],format(known,"x"))
                        self.assertEqual(json.dumps(request,sort_keys=True),original)
                        self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
                for full in (True,False):
                    for missing in (True,False):
                        refused = json.loads(original)
                        purpose = "full_synchronization" if full else "barrier_synchronization"
                        if missing:
                            del refused["darwin_files"][purpose]
                        else:
                            refused["darwin_files"][purpose] = {"objects":[],"created_regular_objects":False}
                        result = session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                        self.assertEqual(result["stop_reason"],"unsupported_service")
                        prefix = (2 if full else 3) if created else (6 if full else 17)
                        self.assertEqual(len(result["services"]),prefix+1)
                        self.assertIsNone(result["services"][-1]["result"])
                        self.assertNotIn("error",result["services"][-1])
                        selected = "full" if full else "barrier"
                        self.assertEqual(result["diagnostic"],f"Darwin {selected} synchronization environment is not configured" if missing else f"Darwin regular-object {selected} synchronization provider is not configured")
                for purpose in ("full_synchronization","barrier_synchronization"):
                    refused = json.loads(original)
                    refused["darwin_files"][purpose]["created_regular_objects"] = 1
                    with self.assertRaises(NeverDError):
                        session.emulate_process(str(Path(fixtures)/file),profile,json.dumps(refused))
                    self.assertEqual(host.call("neverd_session_is_loaded",handle),0)

    def test_darwin_ordinary_file_synchronization_keeps_provider_and_committed_state(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 1 if size else 2, "uid": 501, "gid": 20,
                    "size": size, "block_size": 512, "blocks": 1 if size else 0,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x2000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-file-synchronization", "/data"],
                       "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                         "real_gid": 31, "effective_gid": 20}},
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                                   "metadata": metadata(0o100600, 2, 6)}],
                                        "directories": [{"path": "/", "metadata": metadata(0o40700, 1, 0)}],
                                        "working_directory": "/",
                                        "synchronization": {"objects": ["/data"]}}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 40)
                    self.assertEqual(result["stdout_hex"], "53")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 68)
                    fds = (3, 5, 4, 0xffffffff)
                    values = (0, 0, 0, 9)
                    for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                        for n, number in enumerate((95, 408)):
                            for i, value in enumerate(values):
                                call = calls[5 + h * 18 + n * 9 + i]
                                self.assertEqual(call["number"], format(high | cls | number, "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], i == 3)
                                self.assertEqual(call["arguments"][0], format(high | fds[i], "x"))
                    for i, (number, value) in enumerate(zip((6, 95, 95, 6, 408, 408, 339, 6, 4), (0, 9, 0, 0, 9, 0, 0, 0, 1))):
                        self.assertEqual(calls[59 + i]["number"], format(cls | number, "x"))
                        self.assertEqual(calls[59 + i]["result"], format(value, "x"))
                        self.assertEqual(calls[59 + i]["error"], i in (1, 4))
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = json.loads(original)
            request["darwin_files"]["synchronization"]["objects"] = []
            result = session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["diagnostic"], "Darwin regular-object file integrity completion provider is not configured")
            self.assertEqual(len(result["services"]), 6)
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
            request["darwin_files"]["synchronization"]["extra"] = True
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_created_regular_attributes_keep_independent_providers_and_state(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        base = json.loads(r'''{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "616200ff6566",
      "metadata": {
        "device": 7,
        "inode": 2,
        "mode": 33152,
        "link_count": 1,
        "uid": 501,
        "gid": 20,
        "size": 6,
        "block_size": 512,
        "blocks": 1,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      }
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": 7,
        "inode": 1,
        "mode": 16832,
        "link_count": 2,
        "uid": 501,
        "gid": 20,
        "size": 0,
        "block_size": 512,
        "blocks": 0,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      },
      "mutable": true,
      "mutation_policy": {
        "directory_entry_size": 16,
        "mutation_time": {
          "seconds": 91,
          "nanoseconds": 92
        }
      }
    }
  ],
  "working_directory": "/",
  "umask": 18,
  "creation_policy": {
    "first_inode": 100,
    "block_size": 512,
    "generation": 9,
    "creation_time": {
      "seconds": 41,
      "nanoseconds": 42
    },
    "mutation_policy": {
      "allocation_unit": 512,
      "mutation_time": {
        "seconds": 101,
        "nanoseconds": 102
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "mode_mutations": {
    "objects": [],
    "created_regular_objects": true
  },
  "ownership_mutations": {
    "objects": [],
    "created_regular_objects": true
  }
}''')
        credentials = {'credentials': {'real_uid': 700, 'effective_uid': 501, 'real_gid': 31, 'effective_gid': 20, 'groups': [20, 31]}}
        # Authored raw outcomes and guarded bytes; no native trace/serializer oracle.
        expected = [
            (25, 501, None, False),
            (43, 20, None, False),
            (5, 3, None, False),
            (4, 10, 3, False),
            (5, 4, None, False),
            (41, 5, 3, False),
            (92, 10, 3, False),
            (90, 20, 3, False),
            (199, 3, 4, False),
            (92, 65538, 3, False),
            (339, 0, 3, False),
            (124, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (15, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721231, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584335, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (124, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721340, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584444, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (467, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721683, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584787, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (16, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (16, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (16, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (16, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721232, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (1311768464867721232, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721232, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721232, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584336, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (18446744069414584336, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584336, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584336, 0, None, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (123, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (123, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (123, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (123, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721339, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (1311768464867721339, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721339, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721339, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584443, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (18446744069414584443, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584443, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584443, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (468, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (468, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (468, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (468, 0, 4, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721684, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (1311768464867721684, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721684, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (1311768464867721684, 0, 1311768464867721220, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584788, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (18446744069414584788, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584788, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (18446744069414584788, 0, 18446744069414584324, False),
            (339, 0, 3, False),
            (339, 0, 4, False),
            (339, 0, 5, False),
            (15, 14, None, True),
            (124, 9, 18446744073709551615, True),
            (467, 22, 18446744073709551615, True),
            (467, 9, 18446744073709551615, True),
            (16, 14, None, True),
            (123, 9, 18446744073709551615, True),
            (468, 22, 18446744073709551615, True),
            (468, 9, 18446744073709551615, True),
            (339, 0, 20, False),
            (92, 65538, 3, False),
            (92, 0, 4, False),
            (199, 10, 3, False),
            (199, 10, 5, False),
            (199, 3, 4, False),
            (9, 0, None, False),
            (5, 6, None, False),
            (128, 0, None, False),
            (124, 0, 6, False),
            (339, 0, 3, False),
            (123, 0, 6, False),
            (339, 0, 6, False),
            (10, 0, None, False),
            (10, 0, None, False),
            (339, 0, 4, False),
            (5, 7, None, False),
            (124, 0, 7, False),
            (123, 0, 7, False),
            (339, 0, 7, False),
            (6, 0, 7, False),
            (124, 9, 7, True),
            (123, 9, 7, True),
            (6, 0, 3, False),
            (124, 9, 3, True),
            (123, 9, 3, True),
            (124, 0, 5, False),
            (123, 0, 5, False),
            (6, 0, 5, False),
            (6, 0, 10, False),
            (6, 0, 20, False),
            (124, 0, 4, False),
            (123, 0, 4, False),
            (339, 0, 4, False),
            (339, 0, 4, False),
            (199, 0, 4, False),
            (3, 10, 4, False),
            (6, 0, 6, False),
            (6, 0, 4, False),
            (10, 0, None, False),
            (4, 1, 1, False),
        ]
        virtual_expected = [(5, 3, None, False), (339, 0, 3, False), (124, 0, 3, False), (123, 0, 3, False), (339, 0, 3, False), (4, 160, 1, False), (4, 160, 1, False), (6, 0, 3, False), (10, 0, None, False)]
        virtual_hex = 'a5a5a5a5a5a5a5a507000000808101006400000000000000f501000014000000000000000000000029000000000000002a0000000000000029000000000000002a0000000000000029000000000000002a0000000000000029000000000000002a00000000000000000000000000000000000000000000000002000000000000090000000000000000000000000000000000000000000000a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a507000000a08101006400000000000000f50100001f000000000000000000000029000000000000002a0000000000000029000000000000002a000000000000006500000000000000660000000000000029000000000000002a00000000000000000000000000000000000000000000000002000000000000090000000000000000000000000000000000000000000000a5a5a5a5a5a5a5a5'
        self.assertEqual(len(expected), 235)
        self.assertEqual(len(virtual_expected), 9)
        for fixture, profile in (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"), ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"), ("ios-simulator-x86_64", "ios-simulator-macho64-v1")):
            path = str((Path(fixtures) / fixture).resolve(strict=True))
            cls = 0x2000000 if fixture.endswith("x86_64") else 0
            for virtual in (False, True):
                wanted = virtual_expected if virtual else expected
                mode = "virtual-created-regular-attributes" if virtual else "created-regular-attributes"
                for variant in range(9):
                    owner = variant > 4
                    change = variant - 4 if owner else variant
                    purpose = "ownership_mutations" if owner else "mode_mutations"
                    request = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000, "arguments": ["guest", mode, "/data"], "darwin_files": json.loads(json.dumps(base)), "darwin_system": credentials}
                    if variant:
                        provider = request["darwin_files"][purpose]
                        if change in (1, 4):
                            provider["created_regular_objects"] = False
                        elif change == 2:
                            del provider["created_regular_objects"]
                        elif change == 3:
                            del request["darwin_files"][purpose]
                        if change == 4:
                            provider["objects"] = [{"path": "/data", "time": {"seconds": 71, "nanoseconds": 72}}]
                    options = json.dumps(request, sort_keys=True)
                    for _ in range(2 if variant == 0 else 1):
                        result = session.emulate_process(path, profile, options)
                        self.assertEqual(result["stop_reason"], "unsupported_service" if variant else "exited")
                        self.assertEqual(result["stdout_hex"], "" if variant else virtual_hex if virtual else "41")
                        self.assertEqual(result["stderr_hex"], "")
                        count = ((4 if owner else 3) if virtual else (52 if owner else 12)) if variant else len(wanted)
                        calls = result["services"]
                        self.assertEqual(len(calls), count)
                        for i, call in enumerate(calls):
                            number, value, fd, error = wanted[i]
                            self.assertEqual(call["number"], format(cls | number, "x"))
                            if variant and i + 1 == count:
                                self.assertIsNone(call["result"])
                                self.assertNotIn("error", call)
                            else:
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], error)
                                if fd is not None:
                                    self.assertEqual(call["arguments"][0], format(fd, "x"))
                        if variant:
                            expected_reason = ("Darwin ordinary ownership environment is not configured" if change == 3 else "Darwin object ownership provider is not configured") if owner else ("Darwin ordinary mode mutation environment is not configured" if change == 3 else "Darwin object mode provider is not configured")
                            self.assertEqual(result["diagnostic"], expected_reason)
                        else:
                            self.assertEqual(result["exit_status"], 45 if virtual else 44)
                        self.assertEqual(json.dumps(request, sort_keys=True), options)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for purpose in ("mode_mutations", "ownership_mutations"):
                for bad in (None, 1, "true", [], {}):
                    request = {"darwin_files": json.loads(json.dumps(base))}
                    request["darwin_files"][purpose]["created_regular_objects"] = bad
                    with self.assertRaises(NeverDError):
                        session.emulate_process(path, profile, json.dumps(request))
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_created_regular_advisory_locks_keep_provider_and_description_status(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        base = json.loads(r'''{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "616200ff6566",
      "metadata": {
        "device": 7,
        "inode": 2,
        "mode": 33152,
        "link_count": 1,
        "uid": 501,
        "gid": 20,
        "size": 6,
        "block_size": 512,
        "blocks": 1,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      }
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": 7,
        "inode": 1,
        "mode": 16832,
        "link_count": 2,
        "uid": 501,
        "gid": 20,
        "size": 0,
        "block_size": 512,
        "blocks": 0,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      },
      "mutable": true,
      "mutation_policy": {
        "directory_entry_size": 16,
        "mutation_time": {
          "seconds": 91,
          "nanoseconds": 92
        }
      }
    }
  ],
  "working_directory": "/",
  "umask": 18,
  "creation_policy": {
    "first_inode": 100,
    "block_size": 512,
    "generation": 9,
    "creation_time": {
      "seconds": 41,
      "nanoseconds": 42
    },
    "mutation_policy": {
      "allocation_unit": 512,
      "mutation_time": {
        "seconds": 101,
        "nanoseconds": 102
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "advisory_locks": {
    "objects": [],
    "created_regular_objects": true
  }
}''')
        expected = [{'number': 5, 'result': 3, 'error': False}, {'number': 4, 'result': 10, 'error': False, 'fd': 3}, {'number': 5, 'result': 4, 'error': False}, {'number': 41, 'result': 5, 'error': False, 'fd': 3}, {'number': 92, 'result': 10, 'error': False, 'fd': 3}, {'number': 90, 'result': 20, 'error': False, 'fd': 3}, {'number': 199, 'result': 3, 'error': False, 'fd': 4}, {'number': 92, 'result': 0, 'error': False, 'fd': 3}, {'number': 92, 'result': 65538, 'error': False, 'fd': 3}, {'number': 339, 'result': 0, 'error': False, 'fd': 3}, {'number': 131, 'result': 0, 'error': False, 'fd': 3, 'how': 6}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 5}, {'number': 92, 'result': 0, 'error': False, 'fd': 4}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 5}, {'number': 92, 'result': 0, 'error': False, 'fd': 4}, {'number': 131, 'result': 0, 'error': False, 'fd': 5, 'how': 8}, {'number': 92, 'result': 0, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 131, 'result': 0, 'error': False, 'fd': 3, 'how': 1}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 1}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 131, 'result': 35, 'error': True, 'fd': 3, 'how': 6}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 8}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 131, 'result': 0, 'error': False, 'fd': 10, 'how': 6}, {'number': 131, 'result': 0, 'error': False, 'fd': 20, 'how': 8}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 6}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 8}, {'number': 131, 'result': 9, 'error': True, 'fd': 3, 'how': 0}, {'number': 131, 'result': 9, 'error': True, 'fd': 4294967295, 'how': 6}, {'number': 339, 'result': 0, 'error': False, 'fd': 3}, {'number': 339, 'result': 0, 'error': False, 'fd': 4}, {'number': 339, 'result': 0, 'error': False, 'fd': 5}, {'number': 199, 'result': 10, 'error': False, 'fd': 3}, {'number': 199, 'result': 3, 'error': False, 'fd': 4}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721219, 'how': 1311768464867721222}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 5}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 1311768464867721347, 'result': 35, 'error': True, 'fd': 1311768464867721220, 'how': 1311768464867721221}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721221, 'how': 1311768464867721224}, {'number': 92, 'result': 0, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721219, 'how': 1311768464867721217}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721220, 'how': 1311768464867721217}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 1311768464867721347, 'result': 35, 'error': True, 'fd': 1311768464867721219, 'how': 1311768464867721222}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721220, 'how': 1311768464867721224}, {'number': 1311768464867721347, 'result': 35, 'error': True, 'fd': 1311768464867721220, 'how': 1311768464867721222}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721226, 'how': 1311768464867721222}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721236, 'how': 1311768464867721224}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721220, 'how': 1311768464867721222}, {'number': 1311768464867721347, 'result': 0, 'error': False, 'fd': 1311768464867721220, 'how': 1311768464867721224}, {'number': 1311768464867721347, 'result': 9, 'error': True, 'fd': 1311768464867721219, 'how': 1311768464867721216}, {'number': 1311768464867721347, 'result': 9, 'error': True, 'fd': 1311768469162688511, 'how': 1311768464867721222}, {'number': 339, 'result': 0, 'error': False, 'fd': 3}, {'number': 339, 'result': 0, 'error': False, 'fd': 4}, {'number': 339, 'result': 0, 'error': False, 'fd': 5}, {'number': 199, 'result': 10, 'error': False, 'fd': 3}, {'number': 199, 'result': 3, 'error': False, 'fd': 4}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584323, 'how': 18446744069414584326}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 5}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 18446744069414584451, 'result': 35, 'error': True, 'fd': 18446744069414584324, 'how': 18446744069414584325}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584325, 'how': 18446744069414584328}, {'number': 92, 'result': 0, 'error': False, 'fd': 3}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584323, 'how': 18446744069414584321}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584324, 'how': 18446744069414584321}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 18446744069414584451, 'result': 35, 'error': True, 'fd': 18446744069414584323, 'how': 18446744069414584326}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584324, 'how': 18446744069414584328}, {'number': 18446744069414584451, 'result': 35, 'error': True, 'fd': 18446744069414584324, 'how': 18446744069414584326}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584330, 'how': 18446744069414584326}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584340, 'how': 18446744069414584328}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584324, 'how': 18446744069414584326}, {'number': 18446744069414584451, 'result': 0, 'error': False, 'fd': 18446744069414584324, 'how': 18446744069414584328}, {'number': 18446744069414584451, 'result': 9, 'error': True, 'fd': 18446744069414584323, 'how': 18446744069414584320}, {'number': 18446744069414584451, 'result': 9, 'error': True, 'fd': 18446744073709551615, 'how': 18446744069414584326}, {'number': 339, 'result': 0, 'error': False, 'fd': 3}, {'number': 339, 'result': 0, 'error': False, 'fd': 4}, {'number': 339, 'result': 0, 'error': False, 'fd': 5}, {'number': 199, 'result': 10, 'error': False, 'fd': 3}, {'number': 199, 'result': 3, 'error': False, 'fd': 4}, {'number': 92, 'result': 81922, 'error': False, 'fd': 3}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 9, 'result': 0, 'error': False}, {'number': 131, 'result': 0, 'error': False, 'fd': 3, 'how': 6}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 5, 'result': 6, 'error': False}, {'number': 131, 'result': 35, 'error': True, 'fd': 6, 'how': 6}, {'number': 128, 'result': 0, 'error': False}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 10, 'result': 0, 'error': False}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 10, 'result': 0, 'error': False}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 339, 'result': 0, 'error': False, 'fd': 4}, {'number': 5, 'result': 7, 'error': False}, {'number': 4, 'result': 10, 'error': False, 'fd': 7}, {'number': 131, 'result': 0, 'error': False, 'fd': 7, 'how': 6}, {'number': 92, 'result': 81922, 'error': False, 'fd': 7}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 131, 'result': 0, 'error': False, 'fd': 7, 'how': 8}, {'number': 6, 'result': 0, 'error': False, 'fd': 7}, {'number': 6, 'result': 0, 'error': False, 'fd': 3}, {'number': 131, 'result': 9, 'error': True, 'fd': 3, 'how': 6}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 6, 'result': 0, 'error': False, 'fd': 5}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 6, 'result': 0, 'error': False, 'fd': 10}, {'number': 131, 'result': 35, 'error': True, 'fd': 4, 'how': 6}, {'number': 6, 'result': 0, 'error': False, 'fd': 20}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 6}, {'number': 92, 'result': 16384, 'error': False, 'fd': 4}, {'number': 131, 'result': 0, 'error': False, 'fd': 4, 'how': 8}, {'number': 339, 'result': 0, 'error': False, 'fd': 4}, {'number': 199, 'result': 0, 'error': False, 'fd': 4}, {'number': 3, 'result': 10, 'error': False, 'fd': 4}, {'number': 6, 'result': 0, 'error': False, 'fd': 6}, {'number': 6, 'result': 0, 'error': False, 'fd': 4}, {'number': 10, 'result': 0, 'error': False}, {'number': 4, 'result': 1, 'error': False, 'fd': 1}]
        self.assertEqual(len(expected), 131)
        for fixture, profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            path = str((Path(fixtures) / fixture).resolve(strict=True))
            cls = 0x2000000 if fixture.endswith("x86_64") else 0
            for variant in range(5):
                request = {"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","created-regular-advisory-locks","/data"],"darwin_files":json.loads(json.dumps(base))}
                provider = request["darwin_files"]["advisory_locks"]
                if variant == 1:
                    provider["created_regular_objects"] = False
                elif variant == 2:
                    del provider["created_regular_objects"]
                elif variant == 3:
                    del request["darwin_files"]["advisory_locks"]
                elif variant == 4:
                    provider["created_regular_objects"] = False
                    provider["objects"] = ["/data"]
                options = json.dumps(request, sort_keys=True)
                for _ in range(2 if variant == 0 else 1):
                    result = session.emulate_process(path, profile, options)
                    self.assertEqual(result["stop_reason"], "exited" if variant == 0 else "unsupported_service")
                    self.assertEqual(result["stdout_hex"], "4c" if variant == 0 else "")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 131 if variant == 0 else 11)
                    if variant == 0:
                        self.assertEqual(result["exit_status"], 43)
                        for call, wanted in zip(calls, expected):
                            self.assertEqual(call["number"], format(cls | wanted["number"], "x"))
                            self.assertEqual(call["result"], format(wanted["result"], "x"))
                            self.assertEqual(call["error"], wanted["error"])
                            for index, key in ((0, "fd"), (1, "how")):
                                if key in wanted:
                                    self.assertEqual(call["arguments"][index], format(wanted[key], "x"))
                    else:
                        self.assertEqual(calls[-1]["number"], format(cls | 131, "x"))
                        self.assertIsNone(calls[-1]["result"])
                        self.assertNotIn("error", calls[-1])
                    self.assertEqual(json.dumps(request, sort_keys=True), options)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for bad in (None, 1, "true", [], {}):
                request = {"darwin_files":json.loads(json.dumps(base))}
                request["darwin_files"]["advisory_locks"]["created_regular_objects"] = bad
                with self.assertRaises(NeverDError):
                    session.emulate_process(path, profile, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = {"darwin_files":json.loads(json.dumps(base))}
            del request["darwin_files"]["creation_policy"]
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_created_regular_completion_keeps_independent_providers_and_state(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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
        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device":7,"inode":inode,"mode":mode,"link_count":2 if mode == 0o40700 else 1,"uid":501,"gid":20,"size":size,"block_size":512,"blocks":1 if size else 0,"flags":0,"generation":9,"access_time":{"seconds":1,"nanoseconds":2},"modification_time":{"seconds":3,"nanoseconds":4},"change_time":{"seconds":5,"nanoseconds":6},"birth_time":{"seconds":7,"nanoseconds":8}}
        base = {"files":[{"path":"/data","bytes_hex":"616200ff6566","metadata":metadata(0o100600,2,6)}],"directories":[{"path":"/","mutable":True,"metadata":metadata(0o40700,1,0),"mutation_policy":{"directory_entry_size":16,"mutation_time":{"seconds":91,"nanoseconds":92}}}],"working_directory":"/","umask":18,"creation_policy":{"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}},"synchronization":{"objects":[],"created_regular_objects":True},"data_synchronization":{"objects":[],"created_regular_objects":True}}
        expected = list(zip((5,4,5,41,92,90,199,92,339),(3,10,4,5,10,20,3,0x10002,0)))
        for high in (0,0x1234567800000000,0xffffffff00000000):
            for number in (95,408,187):
                expected += [(high | number, value) for value in (0,0,0,0,0,9)]
                expected += [(339,0)] * 3 + [(199,10),(199,3),(92,0x10002)]
        expected += list(zip((201,339,95,408,187,339,339,339,6,95,408,187,95,408,187,6,95,408,187,95,408,187,339,199,3,6,6,6,10,4),(0,0,0,0,0,0,0,0,0,9,9,9,0,0,0,0,9,9,9,0,0,0,0,0,6,0,0,0,0,1)))
        self.assertEqual(len(expected),147)
        for fixture, profile in (("macos-arm64","macos-macho64-v1"),("macos-x86_64","macos-macho64-v1"),("ios-arm64","ios-macho64-v1"),("ios-simulator-arm64","ios-simulator-macho64-v1"),("ios-simulator-x86_64","ios-simulator-macho64-v1")):
            path = str((Path(fixtures) / fixture).resolve(strict=True))
            cls = 0x2000000 if fixture.endswith("x86_64") else 0
            for variant in range(5):
                request = {"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest","created-regular-synchronization","/data"],"darwin_files":json.loads(json.dumps(base))}
                request["darwin_files"]["synchronization"]["created_regular_objects"] = variant in (0,1)
                request["darwin_files"]["data_synchronization"]["created_regular_objects"] = variant in (0,2)
                if variant == 4:
                    del request["darwin_files"]["synchronization"]
                    del request["darwin_files"]["data_synchronization"]
                options = json.dumps(request,sort_keys=True)
                for _ in range(2 if variant == 0 else 1):
                    result = session.emulate_process(path,profile,options)
                    self.assertEqual(result["stop_reason"],"exited" if variant == 0 else "unsupported_service")
                    self.assertEqual(result["stdout_hex"],"43" if variant == 0 else "")
                    self.assertEqual(result["stderr_hex"],"")
                    calls = result["services"]
                    self.assertEqual(len(calls),147 if variant == 0 else 34 if variant == 1 else 10)
                    if variant == 0:
                        self.assertEqual(result["exit_status"],42)
                        for call, (number,value) in zip(calls,expected):
                            self.assertEqual(call["number"],format(cls | number,"x"))
                            self.assertEqual(call["result"],format(value,"x"))
                            self.assertEqual(call["error"],value == 9)
                        for h, high in enumerate((0,0x1234567800000000,0xffffffff00000000)):
                            for n in range(3):
                                for i, fd in enumerate((3,5,10,20,4,0xffffffff)):
                                    self.assertEqual(calls[9+h*36+n*12+i]["arguments"][0],format(high | fd,"x"))
                    else:
                        self.assertEqual(calls[-1]["number"],format(cls | (187 if variant == 1 else 95),"x"))
                        self.assertIsNone(calls[-1]["result"])
                        self.assertNotIn("error",calls[-1])
                    self.assertEqual(json.dumps(request,sort_keys=True),options)
                    self.assertEqual(host.call("neverd_session_is_loaded",handle),0)
            for purpose in ("synchronization","data_synchronization"):
                for bad in (None,1,"true",[],{}):
                    request = {"darwin_files":json.loads(json.dumps(base))}
                    request["darwin_files"][purpose]["created_regular_objects"] = bad
                    with self.assertRaises(NeverDError):
                        session.emulate_process(path,profile,json.dumps(request))
                    self.assertEqual(host.call("neverd_session_is_loaded",handle),0)

    def test_darwin_ordinary_data_synchronization_keeps_provider_and_committed_state(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import Session, NeverDError
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

        def metadata(mode: int, inode: int, size: int) -> dict:
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 1 if size else 2, "uid": 501, "gid": 20,
                    "size": size, "block_size": 512, "blocks": 1 if size else 0,
                    "flags": 0, "generation": 9,
                    "access_time": {"seconds": 1, "nanoseconds": 2},
                    "modification_time": {"seconds": 3, "nanoseconds": 4},
                    "change_time": {"seconds": 5, "nanoseconds": 6},
                    "birth_time": {"seconds": 7, "nanoseconds": 8}}

        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x2000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-data-synchronization", "/data"],
                       "darwin_system": {"credentials": {"real_uid": 700, "effective_uid": 501,
                                                         "real_gid": 31, "effective_gid": 20}},
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566",
                                                   "metadata": metadata(0o100600, 2, 6)}],
                                        "directories": [{"path": "/", "metadata": metadata(0o40700, 1, 0)}],
                                        "working_directory": "/",
                                        "data_synchronization": {"objects": ["/data"]}}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 41)
                    self.assertEqual(result["stdout_hex"], "44")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 41)
                    fds = (3, 5, 4, 0xffffffff)
                    values = (0, 0, 0, 9)
                    for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                        for i, value in enumerate(values):
                            call = calls[5 + h * 9 + i]
                            self.assertEqual(call["number"], format(high | cls | 187, "x"))
                            self.assertEqual(call["result"], format(value, "x"))
                            self.assertEqual(call["error"], i == 3)
                            self.assertEqual(call["arguments"][0], format(high | fds[i], "x"))
                    for i, (number, value) in enumerate(zip((6, 187, 187, 6, 187, 187, 339, 6, 4), (0, 9, 0, 0, 9, 0, 0, 0, 1))):
                        self.assertEqual(calls[32 + i]["number"], format(cls | number, "x"))
                        self.assertEqual(calls[32 + i]["result"], format(value, "x"))
                        self.assertEqual(calls[32 + i]["error"], i in (1, 4))
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            request = json.loads(original)
            request["darwin_files"]["data_synchronization"]["objects"] = []
            result = session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["diagnostic"], "Darwin regular-object data integrity completion provider is not configured")
            self.assertEqual(len(result["services"]), 6)
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
            request["darwin_files"]["data_synchronization"]["extra"] = True
            with self.assertRaises(NeverDError):
                session.emulate_process(path, profile, json.dumps(request))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_immediate_pselect_preserves_timespec_masks_and_shared_bitmaps(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        values = (0, 3, 6, 3, 6, 9, 9, 12, 22, 14, 22, 22, 22, 22, 22, 14, 14, 14, 9)
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x02000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "immediate-pselect", "/data"],
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566"}]}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "71")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 139)
                    for api, number in enumerate((394, 395)):
                        for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                            for i, value in enumerate(values):
                                call = calls[9 + (api * 3 + h) * 19 + i]
                                self.assertEqual(call["number"], format(high | cls | number, "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], i >= 8)
                    for index, value in ((123, 6), (124, 4), (130, 3), (131, 0), (132, 6)):
                        self.assertEqual(calls[index]["result"], format(value, "x"))
                        self.assertFalse(calls[index]["error"])
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for mode, number, diagnostic in (
                ("immediate-pselect-wait", 394, "Darwin pselect waiting is not implemented"),
                ("immediate-pselect-capacity", 395, "Darwin select descriptor table capacity is not modeled"),
                ("immediate-pselect-mask", 394, "Darwin pselect signal-mask state is not implemented"),
            ):
                request = json.loads(original)
                request["arguments"][1] = mode
                result = session.emulate_process(path, profile, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(result["diagnostic"], diagnostic)
                self.assertEqual(len(result["services"]), 2)
                self.assertEqual(result["services"][-1]["number"], format(cls | number, "x"))
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)


    def test_darwin_immediate_poll_preserves_filters_errors_and_resource_knowledge(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
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
        profiles = (("macos-arm64", "macos-macho64-v1"), ("macos-x86_64", "macos-macho64-v1"),
                    ("ios-arm64", "ios-macho64-v1"), ("ios-simulator-arm64", "ios-simulator-macho64-v1"),
                    ("ios-simulator-x86_64", "ios-simulator-macho64-v1"))
        values = (0, 0, 1, 0, 1, 1, 2, 2, 0, 2, 22, 14)
        for file, profile in profiles:
            path = str(Path(fixtures) / file)
            cls = 0x02000000 if file.endswith("x86_64") else 0
            options = {"backend": "unicorn", "instruction_quantum": 1024, "timeout_microseconds": 5000000,
                       "arguments": ["guest", "immediate-poll", "/data"],
                       "darwin_files": {"files": [{"path": "/data", "bytes_hex": "616200ff6566"}]},
                       "darwin_system": {"resource_limits": [{"resource": 8, "current": 10240, "maximum": 10240}]}}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(file=file, repeat=repeat):
                    result = session.emulate_process(path, profile, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited")
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "70")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 86)
                    for api, number in enumerate((230, 417)):
                        for h, high in enumerate((0, 0x1234567800000000, 0xffffffff00000000)):
                            for i, value in enumerate(values):
                                call = calls[7 + (api * 3 + h) * 12 + i]
                                self.assertEqual(call["number"], format(high | cls | number, "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], i >= 10)
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            options["arguments"][1] = "immediate-poll-missing"
            options["darwin_system"] = {}
            result = session.emulate_process(path, profile, json.dumps(options))
            self.assertEqual(result["stop_reason"], "unsupported_service")
            self.assertEqual(result["stdout_hex"], "21")
            self.assertEqual(result["diagnostic"], "Darwin poll NOFILE resource observation is not configured")
            self.assertEqual(len(result["services"]), 2)
            self.assertEqual(result["services"][-1]["number"], format(cls | 230, "x"))
            self.assertIsNone(result["services"][-1]["result"])
            self.assertNotIn("error", result["services"][-1])
    def test_darwin_owner_queries_preserve_permissions_scope_and_admission(self) -> None:
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

        def metadata(mode, inode, size=0):
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 2 if mode in (16832, 16384) else 1,
                    "uid": 501, "gid": 20, "size": size,
                    "block_size": 4096, "blocks": 0, "flags": 0, "generation": 0,
                    **{key: {"seconds": 0, "nanoseconds": 0} for key in
                       ("access_time", "modification_time", "change_time", "birth_time")}}

        files = {"authorization": "static-owner-queries",
                 "files": [{"path": "/data", "bytes_hex": "616200ff6566",
                            "metadata": metadata(33024, 2, 6)},
                           {"path": "/directory/leaf", "bytes_hex": "",
                            "metadata": metadata(33216, 4)},
                           {"path": "/unknown", "bytes_hex": ""}],
                 "directories": [{"path": "/", "metadata": metadata(16832, 1)},
                                 {"path": "/directory", "metadata": metadata(16384, 3)}],
                 "symbolic_links": [{"path": "/alias", "target_hex": "64617461"},
                                    {"path": "/via", "target_hex": "6469726563746f7279"}],
                 "working_directory": "/"}
        system = {"credentials": {"real_uid": 501, "effective_uid": 501,
                                   "real_gid": 20, "effective_gid": 20}}
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            x64 = architecture == "x86_64"
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "owner-queries", "/data"],
                       "darwin_files": files, "darwin_system": system}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(profile=profile, architecture=architecture, repeat=repeat):
                    result = session.emulate_process(path, name, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "50")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 51)
                    self.assertEqual([c["result"] for c in calls[:24]],
                                     ["0", "d", "d", "d", "0", "d", "d", "d"] * 3)
                    self.assertEqual([c["error"] for c in calls[:24]],
                                     [False, True, True, True, False, True, True, True] * 3)
                    self.assertEqual(calls[47]["number"],
                                     "1234567802000021" if x64 else "1234567800000021")
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for mode, number in (("owner-query-open", 5), ("owner-query-map", 197)):
                request = json.loads(original)
                request["arguments"][1] = mode
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(result["diagnostic"],
                                 "Darwin static owner queries do not authorize other vnode operations")
                self.assertEqual(result["services"][-1]["number"],
                                 format(number | (0x02000000 if x64 else 0), "x"))
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
            for unknown in ("credentials", "metadata", "root", "nonowner"):
                request = json.loads(original)
                request["arguments"][1] = "owner-query-stop"
                if unknown == "credentials":
                    request.pop("darwin_system")
                elif unknown == "metadata":
                    request["darwin_files"]["files"][0].pop("metadata")
                else:
                    request["darwin_system"]["credentials"]["real_uid"] = 0 if unknown == "root" else 502
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
            for bad in (None, True, 0, [], {}, "owner", "", "StaticOwnerQueries"):
                request = {"darwin_files": {"files": [], "authorization": bad}}
                with self.assertRaisesRegex(NeverDError, "authorization"):
                    session.emulate_process("missing.macho", name, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_ordinary_queries_preserve_group_knowledge_scope_and_admission(self) -> None:
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

        def metadata(mode, inode, uid, gid, size=0):
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 2 if mode & 0o170000 == 0o40000 else 1,
                    "uid": uid, "gid": gid, "size": size,
                    "block_size": 4096, "blocks": 0, "flags": 0, "generation": 0,
                    **{key: {"seconds": 0, "nanoseconds": 0} for key in
                       ("access_time", "modification_time", "change_time", "birth_time")}}

        files = {"authorization": "static-ordinary-queries", "files": [
                    {"path": "/agreement", "bytes_hex": "616200ff6566",
                     "metadata": metadata(0o100644, 2, 700, 50, 6)},
                    {"path": "/group", "bytes_hex": "",
                     "metadata": metadata(0o100060, 3, 700, 20)},
                    {"path": "/supplement", "bytes_hex": "",
                     "metadata": metadata(0o100010, 4, 700, 40)},
                    {"path": "/world", "bytes_hex": "",
                     "metadata": metadata(0o100004, 5, 700, 20)},
                    {"path": "/unknown", "bytes_hex": "",
                     "metadata": metadata(0o100064, 6, 700, 50)},
                    {"path": "/directory/leaf", "bytes_hex": "",
                     "metadata": metadata(0o100644, 8, 700, 50)}],
                 "directories": [{"path": "/", "metadata": metadata(0o40777, 1, 0, 0)},
                                 {"path": "/directory", "metadata": metadata(0o40010, 7, 700, 20)}],
                 "symbolic_links": [{"path": "/alias", "target_hex": "61677265656d656e74"},
                                    {"path": "/via", "target_hex": "6469726563746f7279"}],
                 "working_directory": "/"}
        system = {"credentials": {"real_uid": 501, "effective_uid": 502,
                                   "real_gid": 30, "effective_gid": 20, "groups": [20, 40]}}
        read = [0, 13, 13, 13, 0, 13, 13, 13]
        none = [0, 13, 13, 13, 13, 13, 13, 13]
        rw = [0, 13, 0, 13, 0, 13, 0, 13]
        execute = [0, 0, 13, 13, 13, 13, 13, 13]
        expected = []
        for api in range(3):
            for request in range(8):
                expected.extend([read[request], (rw if api == 2 else none)[request],
                                 execute[request], (none if api == 2 else read)[request]])
        expected += read * 2 + [0, 13, 13, 0, 13, 13]
        expected += [0, 13, 13, 13, 13, 13, 0, 2] * 2 + [0, 2, 0, 0, 0, 0, 0, 2]
        self.assertEqual(len(expected), 142)
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            x64 = architecture == "x86_64"
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-queries", "/unknown"],
                       "darwin_files": files, "darwin_system": system}
            original = json.dumps(options, sort_keys=True)
            for repeat in range(2):
                with self.subTest(profile=profile, architecture=architecture, repeat=repeat):
                    result = session.emulate_process(path, name, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(result["stdout_hex"], "47")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = result["services"]
                    self.assertEqual(len(calls), 146)
                    self.assertEqual([c["result"] for c in calls[:142]], [format(v, "x") for v in expected])
                    self.assertEqual([c["error"] for c in calls[:142]], [v != 0 for v in expected])
                    self.assertEqual(calls[142]["number"],
                                     "1234567802000021" if x64 else "1234567800000021")
                    self.assertEqual(calls[142]["result"], "0")
                    self.assertEqual(json.dumps(options, sort_keys=True), original)
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for mode, number in (("ordinary-query-open", 5), ("ordinary-query-map", 197)):
                request = json.loads(original)
                request["arguments"][1] = mode
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(result["diagnostic"],
                                 "Darwin static ordinary queries do not authorize other vnode operations")
                self.assertEqual(len(result["services"]), 2)
                self.assertEqual(result["services"][-1]["number"],
                                 format(number | (0x02000000 if x64 else 0), "x"))
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
            for unknown in ("groups", "credentials", "metadata", "root"):
                request = json.loads(original)
                request["arguments"][1] = "ordinary-query-unknown"
                if unknown != "groups":
                    request["arguments"][2] = "/agreement"
                if unknown == "credentials":
                    request.pop("darwin_system")
                elif unknown == "metadata":
                    request["darwin_files"]["files"][0].pop("metadata")
                elif unknown == "root":
                    request["darwin_system"]["credentials"]["effective_uid"] = 0
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(len(result["services"]), 2)
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
                self.assertIn("Darwin ordinary authorization requires", result["diagnostic"])
            for bad in (None, True, 0, [], {}, "ordinary", "", "StaticOrdinaryQueries",
                        "static-ordinary-queries\0"):
                request = {"darwin_files": {"files": [], "authorization": bad}}
                with self.assertRaisesRegex(NeverDError, "authorization"):
                    session.emulate_process("missing.macho", name, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_ordinary_queries_use_explicit_membership_uid_without_resolver(self) -> None:
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

        def metadata(mode, inode, uid, gid, size=0):
            return {"device": 7, "inode": inode, "mode": mode,
                    "link_count": 2 if mode & 0o170000 == 0o40000 else 1,
                    "uid": uid, "gid": gid, "size": size,
                    "block_size": 4096, "blocks": 0, "flags": 0, "generation": 0,
                    **{key: {"seconds": 0, "nanoseconds": 0} for key in
                       ("access_time", "modification_time", "change_time", "birth_time")}}

        files = {"authorization": "static-ordinary-queries", "files": [
                    {"path": "/agreement", "bytes_hex": "616200ff6566",
                     "metadata": metadata(0o100644, 2, 700, 50, 6)},
                    {"path": "/group", "bytes_hex": "",
                     "metadata": metadata(0o100060, 3, 700, 20)},
                    {"path": "/supplement", "bytes_hex": "",
                     "metadata": metadata(0o100010, 4, 700, 40)},
                    {"path": "/world", "bytes_hex": "",
                     "metadata": metadata(0o100004, 5, 700, 20)},
                    {"path": "/unknown", "bytes_hex": "",
                     "metadata": metadata(0o100064, 6, 700, 50)},
                    {"path": "/directory/leaf", "bytes_hex": "",
                     "metadata": metadata(0o100644, 8, 700, 50)},
                    {"path": "/external/leaf", "bytes_hex": "",
                     "metadata": metadata(0o100644, 11, 700, 50)}],
                 "directories": [{"path": "/", "metadata": metadata(0o40777, 1, 0, 0)},
                                 {"path": "/directory", "metadata": metadata(0o40010, 7, 700, 20)},
                                 {"path": "/external", "metadata": metadata(0o40001, 9, 700, 50)},
                                 {"path": "/blocked", "metadata": metadata(0o40010, 10, 700, 50)}],
                 "symbolic_links": [{"path": "/alias", "target_hex": "61677265656d656e74"},
                                    {"path": "/via", "target_hex": "6469726563746f7279"},
                                    {"path": "/external-via", "target_hex": "65787465726e616c"}],
                 "working_directory": "/"}
        system = {"credentials": {"real_uid": 501, "effective_uid": 502,
                                   "real_gid": 30, "effective_gid": 20, "groups": [20, 40],
                                   "group_membership_uid": 4294967195}}
        read = [0, 13, 13, 13, 0, 13, 13, 13]
        none = [0, 13, 13, 13, 13, 13, 13, 13]
        rw = [0, 13, 0, 13, 0, 13, 0, 13]
        execute = [0, 0, 13, 13, 13, 13, 13, 13]
        expected = []
        for api in range(3):
            for request in range(8):
                expected.extend([read[request], (rw if api == 2 else none)[request],
                                 execute[request], (none if api == 2 else read)[request]])
        expected += read * 2 + [0, 13, 13, 0, 13, 13]
        expected += [0, 13, 13, 13, 13, 13, 0, 2] * 2 + [0, 2, 0, 0, 0, 0, 0, 2]
        self.assertEqual(len(expected), 142)
        search = [2, 0, 0, 0, 13, 13, 0, 0]
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            x64 = architecture == "x86_64"
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "ordinary-queries-closed-groups", "/unknown"],
                       "darwin_files": files, "darwin_system": system}
            original = json.dumps(options, sort_keys=True)
            for wire in (4294967195, "4294967195"):
                request = json.loads(original)
                request["darwin_system"]["credentials"]["group_membership_uid"] = wire
                for repeat in range(2):
                    with self.subTest(profile=profile, architecture=architecture, wire=wire, repeat=repeat):
                        result = session.emulate_process(path, name, json.dumps(request))
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 37)
                        self.assertEqual(result["stdout_hex"], "474e")
                        self.assertEqual(result["stderr_hex"], "")
                        calls = result["services"]
                        self.assertEqual(len(calls), 173)
                        self.assertEqual([c["result"] for c in calls[:142]], [format(v, "x") for v in expected])
                        self.assertEqual([c["error"] for c in calls[:142]], [v != 0 for v in expected])
                        self.assertEqual(calls[142]["number"],
                                         "1234567802000021" if x64 else "1234567800000021")
                        self.assertEqual(calls[142]["result"], "0")
                        self.assertEqual(calls[143]["error"], False)
                        self.assertNotEqual(calls[143]["result"], "0")
                        self.assertEqual(calls[144]["result"], "0")
                        self.assertEqual(calls[145]["result"], "1")
                        for i, mask in enumerate((2, 6), 146):
                            self.assertEqual(calls[i]["number"], "20001d2" if x64 else "1d2")
                            self.assertEqual(calls[i]["arguments"][2:4], [format(mask, "x"), "10"])
                            self.assertEqual(calls[i]["result"], "d")
                            self.assertEqual(calls[i]["error"], True)
                        for api, number in enumerate((33, 466, 466)):
                            for q, value in enumerate(search):
                                call = calls[148 + api * 8 + q]
                                self.assertEqual(call["number"], format(number | (0x02000000 if x64 else 0), "x"))
                                self.assertEqual(call["result"], format(value, "x"))
                                self.assertEqual(call["error"], value != 0)
                                self.assertEqual(call["arguments"][2 if api else 1], "4" if q in (1, 6) else "0")
                                if api:
                                    self.assertEqual(call["arguments"][3], "10" if api == 2 else "0")
                        self.assertEqual(calls[172]["result"], "1")
                        self.assertEqual(json.dumps(options, sort_keys=True), original)
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for wire in (None, 0, "0", 2147483647, "2147483647"):
                request = json.loads(original)
                if wire is None:
                    del request["darwin_system"]["credentials"]["group_membership_uid"]
                else:
                    request["darwin_system"]["credentials"]["group_membership_uid"] = wire
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "47")
                self.assertEqual(len(result["services"]), 147)
                self.assertEqual(result["diagnostic"], "Darwin ordinary authorization requires known group membership")
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
            for unknown in ("groups", "metadata", "root"):
                request = json.loads(original)
                request["arguments"][1] = "ordinary-query-unknown"
                if unknown == "groups":
                    del request["darwin_system"]["credentials"]["groups"]
                elif unknown == "metadata":
                    next(f for f in request["darwin_files"]["files"] if f["path"] == "/unknown").pop("metadata")
                else:
                    request["darwin_system"]["credentials"]["effective_uid"] = 0
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(len(result["services"]), 2)
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
                self.assertIn("Darwin ordinary authorization requires", result["diagnostic"])
            for mode, number in (("ordinary-query-open", 5), ("ordinary-query-map", 197)):
                request = json.loads(original)
                request["arguments"][1] = mode
                result = session.emulate_process(path, name, json.dumps(request))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["stdout_hex"], "21")
                self.assertEqual(result["diagnostic"],
                                 "Darwin static ordinary queries do not authorize other vnode operations")
                self.assertEqual(len(result["services"]), 2)
                self.assertEqual(result["services"][-1]["number"], format(number | (0x02000000 if x64 else 0), "x"))
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
            for bad in (None, True, False, {}, [], 0.5, -1, 2147483648,
                        4294967194, 4294967196, 4294967295, 4294967296,
                        "-1", "1.5", "0x10", "x", "", " 0", "0 ", "2147483648",
                        "4294967194", "4294967196", "4294967295", "4294967296", "1\0"):
                request = {"darwin_system": json.loads(json.dumps(system))}
                request["darwin_system"]["credentials"]["group_membership_uid"] = bad
                with self.assertRaisesRegex(NeverDError, "credentials|group membership UID"):
                    session.emulate_process("missing.macho", name, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for key in ("real_uid", "effective_uid", "real_gid", "effective_gid", "groups"):
                request = {"darwin_system": json.loads(json.dumps(system))}
                request["darwin_system"]["credentials"][key] = [20, 4294967195] if key == "groups" else 4294967195
                with self.assertRaisesRegex(NeverDError, "Darwin credentials"):
                    session.emulate_process("missing.macho", name, json.dumps(request))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for key in ("GroupMembershipUID", "group_membership_uid\0"):
                request = {"darwin_system": json.loads(json.dumps(system))}
                request["darwin_system"]["credentials"][key] = 4294967195
                with self.assertRaisesRegex(NeverDError, "credentials"):
                    session.emulate_process("missing.macho", name, json.dumps(request))
        for name in ("linux-elf64-v1", "windows-pe64-v1", "android-aarch64-api28-v1"):
            with self.assertRaisesRegex(NeverDError, "darwin_system requires"):
                session.emulate_process("missing.macho", name, json.dumps({"darwin_system": system}))
            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_mach_self_ports_preserve_raw_carriers_and_independent_inputs(self) -> None:
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
        fields = ("thread_self_port", "task_self_port", "host_self_port")
        mixed = dict(zip(fields, (2147483649, 0, "4294967295")))
        samples = ((0, "0000000000000000", "0"),
                   (1, "0100000000000000", "1"),
                   (2147483647, "ffffff7f00000000", "7fffffff"),
                   (2147483648, "00000080ffffffff", "ffffffff80000000"),
                   ("2147483649", "01000080ffffffff", "ffffffff80000001"),
                   (4294967295, "ffffffffffffffff", "ffffffffffffffff"),
                   ("4294967295", "ffffffffffffffff", "ffffffffffffffff"))
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            x64 = architecture == "x86_64"
            numbers = []
            for q in range(3):
                low = (0x1000000 | (27 + q)) if x64 else (-(27 + q) & 0xffffffff)
                prefixes = [0 if x64 else 0xffffffff00000000, 0, 0,
                            0x1234567800000000, 0x1234567800000000,
                            0xffffffff00000000, 0xffffffff00000000]
                numbers.extend(f"{prefix | low:x}" for prefix in prefixes)

            def check(result, carriers, expected):
                self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                self.assertEqual(result["exit_status"], 37)
                self.assertEqual(bytes.fromhex(result["stdout_hex"]), expected)
                self.assertEqual(result["stderr_hex"], "")
                calls = result["services"]
                self.assertEqual(len(calls), 22)
                self.assertEqual([c["number"] for c in calls[:21]], numbers)
                self.assertEqual([c["result"] for c in calls[:21]],
                                 [v for value in carriers for v in [value] * 7])
                for call in calls[:21]:
                    self.assertNotIn("error", call)
                    self.assertNotIn("thread_id", call)
                    self.assertEqual(call["arguments"][0], "ffffffffffffffff")
                    self.assertEqual(call["arguments"][2 if x64 else 1], "1122334455667788")
                    self.assertEqual(call["arguments"][1 if x64 else 2], "8877665544332211")
                self.assertEqual(calls[-1]["number"], "2000004" if x64 else "4")
                self.assertEqual(calls[-1]["arguments"][0], "1")
                self.assertEqual(calls[-1]["arguments"][2], f"{len(expected):x}")
                self.assertEqual(calls[-1]["result"], f"{len(expected):x}")
                self.assertFalse(calls[-1]["error"])
                self.assertNotIn("thread_id", calls[-1])
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "mach-self-port-values"]}
            for wire, hex_bytes, carrier in samples:
                options["darwin_system"] = dict.fromkeys(fields, wire)
                request = json.dumps(options)
                previous = None
                for repeat in range(2):
                    with self.subTest(profile=name, architecture=architecture, port=wire, repeat=repeat):
                        result = session.emulate_process(path, name, request)
                        check(result, [carrier] * 3, bytes.fromhex(hex_bytes * 3))
                        if previous is not None:
                            self.assertEqual(result, previous)
                        previous = result
            options["darwin_system"] = mixed.copy()
            result = session.emulate_process(path, name, json.dumps(options))
            check(result, ["ffffffff80000001", "0", "ffffffffffffffff"],
                  bytes.fromhex("01000080ffffffff0000000000000000ffffffffffffffff"))
            options["arguments"][1] = "mach-self-ports"
            result = session.emulate_process(path, name, json.dumps(options))
            check(result, ["ffffffff80000001", "0", "ffffffffffffffff"], b"J")
            for q, selection in enumerate(("thread", "task", "host")):
                options["arguments"] = ["guest", "mach-self-port-missing", selection]
                for state in range(3):
                    options.pop("darwin_system", None)
                    if state:
                        options["darwin_system"] = {}
                    if state == 2:
                        options["darwin_system"] = {**mixed, "thread_id": "18446744073709551615"}
                        del options["darwin_system"][fields[q]]
                    result = session.emulate_process(path, name, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "unsupported_service")
                    self.assertEqual(result["diagnostic"],
                                     f"Darwin current-{selection} Mach port observation is not configured")
                    self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"!")
                    self.assertEqual(len(result["services"]), 2)
                    last = result["services"][-1]
                    number = (0x1000000 | (27 + q)) if x64 else (-(27 + q) & 0xffffffffffffffff)
                    self.assertEqual(last["number"], f"{number:x}")
                    self.assertIsNone(last["result"])
                    self.assertNotIn("error", last)
                    self.assertNotIn("thread_id", last)
            for field in fields:
                for bad in (None, True, False, {}, [], -1, 1.5, 4294967296,
                            9007199254740992, 18446744073709551615,
                            "", "-1", "1.5", "0x10", "x", "4294967296",
                            "18446744073709551615", "1\x00"):
                    options["darwin_system"] = {field: bad}
                    with self.assertRaises(NeverDError) as caught:
                        session.emulate_process(path, name, json.dumps(options))
                    self.assertIn(field, str(caught.exception))
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_thread_identity_preserves_full_bits_and_independent_runs(self) -> None:
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
        reason = "Darwin current-thread identity observation is not configured"
        samples = ((0, b"\x00" * 8),
                   (4294967297, bytes.fromhex("0100000001000000")),
                   ("9223372036854775808", bytes.fromhex("0000000000000080")),
                   ("18364758544493064720", bytes.fromhex("1032547698badcfe")),
                   ("18446744073709551615", b"\xff" * 8))
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            x64 = architecture == "x86_64"
            numbers = ["2000174" if x64 else "174"] * 4 + [
                "1234567802000174" if x64 else "1234567800000174"] * 3 + [
                "ffffffff02000174" if x64 else "ffffffff00000174"] * 3
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "thread-identity-value"]}
            for wire, expected in samples:
                options["darwin_system"] = {"thread_id": wire}
                request = json.dumps(options)
                previous = None
                for repeat in range(2):
                    with self.subTest(profile=name, architecture=architecture, thread_id=wire, repeat=repeat):
                        result = session.emulate_process(path, name, request)
                        self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                        self.assertEqual(result["exit_status"], 37)
                        self.assertEqual(bytes.fromhex(result["stdout_hex"]), expected)
                        self.assertEqual(result["stderr_hex"], "")
                        self.assertEqual(len(result["services"]), 11)
                        write = result["services"][-1]
                        self.assertEqual(write["number"], "2000004" if x64 else "4")
                        self.assertEqual(write["arguments"][0], "1")
                        self.assertEqual(write["arguments"][2], "8")
                        self.assertEqual(write["result"], "8")
                        self.assertFalse(write["error"])
                        calls = result["services"][:10]
                        self.assertEqual([call["number"] for call in calls], numbers)
                        self.assertEqual([call["result"] for call in calls], [f"{int(wire):x}"] * 10)
                        self.assertEqual([call["error"] for call in calls], [False] * 10)
                        self.assertEqual(calls[0]["arguments"], ["ffffffffffffffff", "8000000000000000",
                                         "1122334455667788", "1", "ffffffffffffffff", "123456789abcdef0"])
                        for call in calls:
                            self.assertNotIn("thread_id", call)
                        if previous is not None:
                            self.assertEqual(result, previous)
                        previous = result
                        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            options["arguments"][1] = "thread-identity"
            options["darwin_system"] = {"thread_id": "18364758544493064720"}
            result = session.emulate_process(path, name, json.dumps(options))
            self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
            self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"T")
            options["arguments"][1] = "thread-identity-missing"
            for present in (False, True):
                options.pop("darwin_system", None)
                if present:
                    options["darwin_system"] = {}
                result = session.emulate_process(path, name, json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service")
                self.assertEqual(result["diagnostic"], reason)
                self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"!")
                self.assertEqual(len(result["services"]), 2)
                self.assertEqual(result["services"][-1]["number"], "2000174" if x64 else "174")
                self.assertIsNone(result["services"][-1]["result"])
                self.assertNotIn("error", result["services"][-1])
                self.assertNotIn("thread_id", result["services"][-1])
            for bad in (None, True, False, {}, [], -1, 1.5, 9007199254740992,
                        "", "-1", "1.5", "0x10", "x", "18446744073709551616", "1\x00"):
                options["darwin_system"] = {"thread_id": bad}
                with self.assertRaises(NeverDError) as caught:
                    session.emulate_process(path, name, json.dumps(options))
                self.assertIn("thread_id", str(caught.exception))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_darwin_entropy_replay_preserves_bytes_errors_and_fresh_runs(self) -> None:
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
        observations = ["deadbeef", "00ff80a5", "7f", bytes(range(256)).hex()]
        reasons = {
            "entropy-missing": "Darwin entropy observations are not configured",
            "entropy-exhausted": "Darwin entropy observations are exhausted",
            "entropy-mismatch": "Darwin entropy observation length does not match the request",
            "entropy-partial": "Darwin partial entropy output is unsupported",
        }
        for profile, architecture in (("macos", "x86_64"), ("macos", "arm64"),
                                      ("ios", "arm64"), ("ios-simulator", "x86_64"),
                                      ("ios-simulator", "arm64")):
            path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
            name = f"{profile}-macho64-v1"
            number = "20001f4" if architecture == "x86_64" else "1f4"
            options = {"backend": "unicorn", "instruction_quantum": 1024,
                       "timeout_microseconds": 5000000,
                       "arguments": ["guest", "entropy-replay"],
                       "darwin_system": {"entropy_reads": observations}}
            request = json.dumps(options)
            previous = None
            for repeat in range(2):
                with self.subTest(profile=name, architecture=architecture, repeat=repeat):
                    result = session.emulate_process(path, name, request)
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"R")
                    self.assertEqual(result["stderr_hex"], "")
                    calls = [e for e in result["services"] if e["number"] == number]
                    self.assertEqual(len(calls), 28)
                    self.assertEqual([e["result"] for e in calls],
                                     ["0" if i % 6 == 0 else "16" for i in range(24)]
                                     + ["e", "0", "0", "0"])
                    self.assertEqual([e["error"] for e in calls],
                                     [i % 6 != 0 for i in range(24)] + [True, False, False, False])
                    if previous is not None:
                        self.assertEqual(result, previous)
                    previous = result
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
            for mode, reason in reasons.items():
                with self.subTest(profile=name, architecture=architecture, mode=mode):
                    options["arguments"][1] = mode
                    options["darwin_system"] = {} if mode == "entropy-missing" else {
                        "entropy_reads": observations[:1] if mode == "entropy-exhausted" else observations}
                    result = session.emulate_process(path, name, json.dumps(options))
                    self.assertEqual(result["stop_reason"], "unsupported_service")
                    self.assertEqual(result["diagnostic"], reason)
                    self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"!")
                    self.assertEqual(result["services"][-1]["number"], number)
                    self.assertIsNone(result["services"][-1]["result"])
                    self.assertNotIn("error", result["services"][-1])
            options["arguments"][1] = "entropy-missing"
            options["darwin_system"] = {"entropy_reads": []}
            result = session.emulate_process(path, name, json.dumps(options))
            self.assertEqual(result["diagnostic"], reasons["entropy-exhausted"])
            for invalid in (None, True, {}, [None], [""], ["0"], ["gg"],
                            ["00\x00"], ["00" * 257], ["00"] * 257):
                options["darwin_system"] = {"entropy_reads": invalid}
                with self.assertRaises(NeverDError) as caught:
                    session.emulate_process(path, name, json.dumps(options))
                self.assertIn("entropy_reads", str(caught.exception))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

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
                                           ("extended-attributes", b"X"),
                                           ("extended-attributes-values", bytes.fromhex("00ff410080420a757365722e6e65766572642e6265746100757365722e6e65766572642e616c70686100757365722e6e65766572642e656d70747900")),
                                           ("extended-attributes-unsupported", b"X"),
                                           ("symbolic-descriptors", b"S"),
                                           ("nonblocking-descriptors", b"N"),
                                           ("nonblocking-flags-unsupported", b"N"),
                                           ("symbolic-descriptors-values", bytes.fromhex(
                                               "85ffffffe8a101001132547698badcfee803000098badcfe0000000000000000"
                                               "edffffffffffffffb168de3a00000000edffffffffffffffb168de3a00000000"
                                               "edffffffffffffffb168de3a00000000edffffffffffffffb168de3a00000000"
                                               "040000000000000001000000000000000020000000000000efcdab8900000000"
                                               "000000000000000000000000000000000800000005000000"
                                               )),
                                           ("symbolic-descriptors-name-unsupported", b"S"),
                                           ("hard-links", b"H"),
                                           ("hard-links-values", bytes.fromhex("51313233343536373839")),
                                           ("hard-links-name-unsupported", b"H"),
                                           ("hard-links-attributes-unsupported", b"H"),
                                           ("xattr-mutations", b"V"),
                                           ("xattr-mutations-values", bytes.fromhex("00ff410080420a757365722e6e65766572642e6265746100")),
                                           ("xattr-mutations-unsupported", b"V"),
                                           ("bulk-attributes", b"B"),
                                           ("bulk-attributes-values", bytes.fromhex("3000000009000080000000000000000000000000000000000c0000000600000005000000616c696173000000000000003000000009000080000000000000000000000000000000000c00000006000000050000006379636c65000000000000003000000009000080000000000000000000000000000000000c000000090000000500000064616e676c696e67000000003000000009000080000000000000000000000000000000000c00000005000000010000006461746100000000000000003000000009000080000000000000000000000000000000000c0000000600000002000000656d70747900000000000000")),
                                           ("bulk-attributes-unsupported", b"B"),
                                           ("attribute-names", b"N"),
                                           ("attribute-names-values", bytes.fromhex(
                                               "2c0000000900008000000000000000000000000000000000"
                                               "0c00000005000000010000006461746100000000")),
                                           ("attribute-names-unsupported", b"N"),
                                           ("common-attributes", b"A"),
                                           ("common-attributes-values", bytes.fromhex(
                                               "780000000a9e07820000000000000000000000000000000085ffffff01000000"
                                               "fbffffffffffffff0600000000000000ffffffffffffff7fffc99a3b00000000"
                                               "fdffffffffffffff040000000000000001000000000000800100000000000000"
                                               "efcdab8998badcfea4810000341200001032547698badcfe")),
                                           ("common-attributes-unsupported", b"A"),
                                           ("kernel-pathconf", b"C"),
                                           ("kernel-pathconf-values", bytes.fromhex(
                                               "0100000000000000010000000000000001000000000000000000000000000000"
                                               "0010000000000000000001000000000000100000000000000010000000000000"
                                               "ff000000000000000000000000000000")),
                                           ("kernel-pathconf-unsupported", b"C"),
                                           ("writable-files", b"00006e"),
                                           ("writable-files-nocancel", b"00006e"),
                                           ("sparse-file-seek", b"s"),
                                           ("unlinked-file", b"u"),
                                           ("created-file", b"c"),
                                           ("created-file-metadata", b"q"),
                                           ("mutable-initial-links", b"M"),
                                           ("directory-link-roots", b"R"),
                                           ("virtual-directory-link-roots", bytes.fromhex(
                                               "85ffffffffa101003900000000000000efcdab8998badcfe0000000000000000"
                                               "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
                                               "f3ffffffffffffffc801000000000000fbffffffffffffff0600000000000000"
                                               "060000000000000008000000000000000010000000000000efcdab8900000000"
                                               "00000000000000000000000000000000"
                                               )),
                                           ("virtual-mutable-initial-links", bytes.fromhex(
                                               "85ffffffffa101003900000000000000efcdab8998badcfe0000000000000000"
                                               "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
                                               "f3ffffffffffffffc801000000000000fbffffffffffffff0600000000000000"
                                               "040000000000000008000000000000000010000000000000efcdab8900000000"
                                               "00000000000000000000000000000000"
                                               )),
                                           ("initial-directory-metadata", b"I"),
                                           ("virtual-initial-directory-metadata", bytes.fromhex(
                                               "85ffffffed4105002900000000000000efcdab8998badcfe0000000000000000"
                                               "01000000000000800100000000000000f5ffffffffffffff4101000000000000"
                                               "f5ffffffffffffff4101000000000000fbffffffffffffff0600000000000000"
                                               "550000000000000000000000000000000010000000000000efcdab8900000000"
                                               "00000000000000000000000000000000"
                                               )),
                                           ("directory-enumeration-mutations", b"E"),
                                           ("virtual-directory-enumeration", bytes.fromhex(
                                               "1132547698badcfe000000000000000020000100042e00000000000000000000"
                                               "2900000000000000000000000000000020000200042e2e000000000000000000"
                                               "1332547698badcfe000000000000000020000100046400000000000000000000"
                                               "1432547698badcfe000000000000000020000100086600000000000000000000"
                                               "1532547698badcfe0000000000000000200001000a6c00000000000000000000"
                                               )),
                                           ("created-namespace-metadata", b"N"),
                                           ("virtual-created-namespace-metadata", bytes.fromhex(
                                               "85ffffffe84102001132547698badcfee803000098badcfe0000000000000000"
                                               "edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000"
                                               "f9ffffffffffffff15cd5b0700000000edffffffffffffffb168de3a00000000"
                                               "400000000000000007000000000000000020000000000000efcdab8900000000"
                                               "0000000000000000000000000000000085ffffffe8a101001232547698badcfe"
                                               "e803000098badcfe0000000000000000edffffffffffffffb168de3a00000000"
                                               "edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000"
                                               "edffffffffffffffb168de3a0000000004000000000000000100000000000000"
                                               "0020000000000000efcdab890000000000000000000000000000000000000000"
                                               )),
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
                                           ("symbolic-link-unlink-protected", b"U"),
                                           ("symbolic-link-rename", b"R"),
                                           ("symbolic-link-rename-protected", b"R")):
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
                        if mode in ("created-namespace-metadata", "virtual-created-namespace-metadata",
                                    "mutable-initial-links", "virtual-mutable-initial-links",
                                    "directory-link-roots", "virtual-directory-link-roots",
                                    "directory-enumeration-mutations", "virtual-directory-enumeration",
                                    "initial-directory-metadata", "virtual-initial-directory-metadata"):
                            namespace_options = json.loads(file_options)
                            namespace_options["instruction_quantum"] = 1024
                            namespace_options["timeout_microseconds"] = 5_000_000
                            files = namespace_options["darwin_files"]
                            files["umask"] = 0o27
                            files["creation_policy"] = {
                                "first_inode": "18364758544493064721",
                                "block_size": 8192, "generation": 2309737967,
                                "creation_time": {"seconds": -19, "nanoseconds": 987654321},
                                "mutation_policy": {"allocation_unit": 4096,
                                                    "mutation_time": {"seconds": -7, "nanoseconds": 123456789}},
                                "namespace_policy": {"symbolic_link_allocation_unit": 512,
                                                     "directory_entry_size": 32, "directory_blocks": 7}}
                            original = files["files"][0]["metadata"]
                            original["flags"] = 0
                            original["link_count"] = 1
                            parent = dict(original, inode=41, mode=0o40755, size=0, blocks=0)
                            root = next(d for d in files["directories"] if d["path"] == "/")
                            root.update(mutable=True, swap_rename=True, metadata=parent)
                            if mode in ("mutable-initial-links", "virtual-mutable-initial-links"):
                                root.pop("contents", None)
                                link_metadata = dict(original, inode=57, mode=0o120777, size=4)
                                files["symbolic_links"] = [
                                    {"path": "/initial", "target_hex": "64617461", "mutable": True,
                                     "metadata": link_metadata,
                                     "mutation_policy": {"mutation_time": {"seconds": -13, "nanoseconds": 456}}},
                                    {"path": "/initial-dir", "target_hex": "656d707479", "mutable": True}]
                                next(d for d in files["directories"] if d["path"] == "/empty")["metadata"] = dict(parent, inode=42)
                            if mode in ("directory-link-roots", "virtual-directory-link-roots"):
                                files["working_directory"] = "/"
                                files["directories"] = []
                                for inode, name in enumerate(("/", "/a", "/a/d", "/a/other", "/b", "/b/other"), 41):
                                    directory = {"path": name, "metadata": dict(parent, inode=inode)}
                                    if name in ("/", "/a", "/b", "/a/d"):
                                        directory["mutable"] = True
                                    if name in ("/a", "/b", "/a/d"):
                                        directory.update(movable=True, swap_rename=True)
                                    if name == "/a/d":
                                        directory.update(exchangeable=True, mutation_policy={
                                            "directory_entry_size": 17,
                                            "mutation_time": {"seconds": -11, "nanoseconds": 321}})
                                    files["directories"].append(directory)
                                files["files"] = [
                                    {"path": name, "bytes_hex": bytes([value]).hex(),
                                     "metadata": dict(original, inode=inode, size=1, blocks=8)}
                                    for inode, (name, value) in enumerate((
                                        ("/a/d/c", 31), ("/a/other/mark", 41), ("/a/target", 11),
                                        ("/b/other/mark", 42), ("/b/target", 22)), 101)]
                                files["symbolic_links"] = [
                                    {"path": name, "target_hex": target.encode().hex(), "mutable": True,
                                     "metadata": dict(original, inode=inode, mode=0o120777,
                                                      size=len(target), blocks=8),
                                     "mutation_policy": {"mutation_time": {"seconds": -13, "nanoseconds": 456}}}
                                    for inode, (name, target) in enumerate((
                                        ("/b/l", "target"), ("/b/dang", "missing"),
                                        ("/b/dirlink", "other"), ("/b/self", "../a/d"),
                                        ("/a/d/inside", "../target")), 57)]
                            if mode in ("initial-directory-metadata", "virtual-initial-directory-metadata"):
                                root["mutation_policy"] = {
                                    "directory_entry_size": 17,
                                    "mutation_time": {"seconds": -11, "nanoseconds": 321}}
                            if mode in ("directory-enumeration-mutations", "virtual-directory-enumeration"):
                                root.pop("contents", None)
                                root["enumeration_policy"] = {
                                    "minimum_buffer_size": 1,
                                    "initial_minimum_buffer_size": 64, "seek_offset": 0}
                            file_options = json.dumps(namespace_options)
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
                        rename_links = mode.startswith("symbolic-link-rename")
                        protected_link = (unlink_links or rename_links) and mode.endswith("-protected")
                        if (mode in ("symbolic-link-mutations", "symbolic-link-creation")
                                or unlink_links or rename_links):
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
                            if rename_links:
                                mixed_options["darwin_files"]["directories"][1]["swap_rename"] = True
                            mixed_options["arguments"][2] = "/work/data"
                            if protected_link:
                                mixed_options["arguments"][1] = (
                                    "symbolic-link-rename" if rename_links else "symbolic-link-unlink")
                                mixed_options["arguments"].append("protected")
                            file_options = json.dumps(mixed_options)
                        unknown_attributes = mode == "common-attributes-unsupported"
                        if mode.startswith("common-attributes"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
    "files": [
        {
            "path": "/data",
            "bytes_hex": "30313233343536373839",
            "metadata": {
                "device": -123,
                "inode": "18364758544493064720",
                "mode": 33188,
                "link_count": 3,
                "uid": 2309737967,
                "gid": 4275878552,
                "size": 10,
                "block_size": 4096,
                "blocks": 8,
                "flags": 4660,
                "generation": 2309737967,
                "access_time": {
                    "seconds": "-9223372036854775807",
                    "nanoseconds": 1
                },
                "modification_time": {
                    "seconds": "9223372036854775807",
                    "nanoseconds": 999999999
                },
                "change_time": {
                    "seconds": -3,
                    "nanoseconds": 4
                },
                "birth_time": {
                    "seconds": -5,
                    "nanoseconds": 6
                }
            }
        }
    ],
    "directories": [
        {
            "path": "/",
            "metadata": {
                "device": -123,
                "inode": 41,
                "mode": 16877,
                "link_count": 3,
                "uid": 2309737967,
                "gid": 4275878552,
                "size": 0,
                "block_size": 4096,
                "blocks": 8,
                "flags": 4660,
                "generation": 2309737967,
                "access_time": {
                    "seconds": "-9223372036854775807",
                    "nanoseconds": 1
                },
                "modification_time": {
                    "seconds": "9223372036854775807",
                    "nanoseconds": 999999999
                },
                "change_time": {
                    "seconds": -3,
                    "nanoseconds": 4
                },
                "birth_time": {
                    "seconds": -5,
                    "nanoseconds": 6
                }
            }
        },
        {
            "path": "/empty",
            "metadata": {
                "device": -123,
                "inode": 42,
                "mode": 16877,
                "link_count": 3,
                "uid": 2309737967,
                "gid": 4275878552,
                "size": 0,
                "block_size": 4096,
                "blocks": 8,
                "flags": 4660,
                "generation": 2309737967,
                "access_time": {
                    "seconds": "-9223372036854775807",
                    "nanoseconds": 1
                },
                "modification_time": {
                    "seconds": "9223372036854775807",
                    "nanoseconds": 999999999
                },
                "change_time": {
                    "seconds": -3,
                    "nanoseconds": 4
                },
                "birth_time": {
                    "seconds": -5,
                    "nanoseconds": 6
                }
            }
        }
    ],
    "working_directory": "/empty",
    "symbolic_links": [
        {
            "path": "/alias",
            "target_hex": "64617461",
            "metadata": {
                "device": -123,
                "inode": 123,
                "mode": 41471,
                "link_count": 3,
                "uid": 2309737967,
                "gid": 4275878552,
                "size": 4,
                "block_size": 4096,
                "blocks": 8,
                "flags": 4660,
                "generation": 2309737967,
                "access_time": {
                    "seconds": "-9223372036854775807",
                    "nanoseconds": 1
                },
                "modification_time": {
                    "seconds": "9223372036854775807",
                    "nanoseconds": 999999999
                },
                "change_time": {
                    "seconds": -3,
                    "nanoseconds": 4
                },
                "birth_time": {
                    "seconds": -5,
                    "nanoseconds": 6
                }
            }
        },
        {
            "path": "/dangling",
            "target_hex": "6d697373696e67"
        },
        {
            "path": "/cycle",
            "target_hex": "6379636c65"
        }
    ]
}
''')
                            file_options = json.dumps(query_options)
                        unknown_pathconf = mode == "kernel-pathconf-unsupported"
                        if mode.startswith("kernel-pathconf"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = {
                                "files": [{"path": "/data", "bytes_hex": "30313233343536373839"}],
                                "directories": [{"path": "/"}, {"path": "/empty"}],
                                "working_directory": "/empty",
                                "symbolic_links": [
                                    {"path": "/" + name, "target_hex": target.encode().hex()}
                                    for name, target in (("alias", "data"), ("dangling", "missing"),
                                                         ("cycle", "cycle"))]}
                            file_options = json.dumps(query_options)
                        unknown_bulk = mode == "bulk-attributes-unsupported"
                        if mode.startswith("bulk-attributes"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839"
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": 0,
        "inode": 41,
        "mode": 16877,
        "link_count": 2,
        "uid": 0,
        "gid": 0,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 0,
        "access_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "modification_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "change_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "birth_time": {
          "seconds": 0,
          "nanoseconds": 0
        }
      },
      "enumeration_policy": {
        "minimum_buffer_size": 1,
        "initial_minimum_buffer_size": 64,
        "seek_offset": 0,
        "bulk_attributes": true
      }
    },
    {
      "path": "/empty",
      "metadata": {
        "device": 0,
        "inode": 42,
        "mode": 16877,
        "link_count": 2,
        "uid": 0,
        "gid": 0,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 0,
        "access_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "modification_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "change_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "birth_time": {
          "seconds": 0,
          "nanoseconds": 0
        }
      },
      "enumeration_policy": {
        "minimum_buffer_size": 1,
        "initial_minimum_buffer_size": 64,
        "seek_offset": 0,
        "bulk_attributes": true
      }
    }
  ],
  "working_directory": "/",
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461"
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67"
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65"
    }
  ]
}
''')
                            file_options = json.dumps(query_options)
                        unknown_names = mode == "attribute-names-unsupported"
                        if mode.startswith("attribute-names"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = {
                                "files": [{"path": "/data", "bytes_hex": "30313233343536373839"}],
                                "directories": [{"path": "/", "mutable": True},
                                                {"path": "/empty", "movable": True, "removable": True}],
                                "symbolic_links": [{"path": "/alias", "target_hex": "64617461", "mutable": True},
                                                   {"path": "/dangling", "target_hex": "6d697373696e67", "mutable": True},
                                                   {"path": "/cycle", "target_hex": "6379636c65", "mutable": True}],
                                "working_directory": "/"}
                            file_options = json.dumps(query_options)
                        unknown_nonblocking = mode == "nonblocking-flags-unsupported"
                        if mode.startswith("nonblocking-"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = {
                                "files": [{"path": "/data", "bytes_hex": "30313233343536373839", "writable": True}],
                                "directories": [{"path": "/"}],
                                "symbolic_links": [{"path": "/fd-nonblock", "target_hex": "64617461"}],
                                "working_directory": "/"}
                            file_options = json.dumps(query_options)
                        if mode.startswith("symbolic-descriptors"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839"
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 41,
        "mode": 16877,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    }
  ],
  "creation_policy": {
    "first_inode": "18364758544493064721",
    "block_size": 8192,
    "generation": 2309737967,
    "creation_time": {
      "seconds": -19,
      "nanoseconds": 987654321
    },
    "mutation_policy": {
      "allocation_unit": 4096,
      "mutation_time": {
        "seconds": -7,
        "nanoseconds": 123456789
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "umask": 23,
  "working_directory": "/",
  "symbolic_links": [
    {
      "path": "/fd-attrs",
      "target_hex": "64617461",
      "mutable": true,
      "extended_attributes": [],
      "mutable_extended_attributes": true
    }
  ]
}
''')
                            file_options = json.dumps(query_options)
                        unknown_hard_link_name = mode in ("symbolic-descriptors-name-unsupported", "hard-links-name-unsupported", "hard-links-attributes-unsupported")
                        if mode.startswith("hard-links"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839",
      "writable": true,
      "metadata": {
        "device": -123,
        "inode": "18364758544493064720",
        "mode": 33188,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 10,
        "block_size": 4096,
        "blocks": 8,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      },
      "mutation_policy": {
        "allocation_unit": 4096,
        "mutation_time": {
          "seconds": -7,
          "nanoseconds": 123456789
        }
      },
      "extended_attributes": [],
      "mutable_extended_attributes": true
    },
    {
      "path": "/attributes",
      "bytes_hex": "78",
      "extended_attributes": [],
      "mutable_extended_attributes": true
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 41,
        "mode": 16877,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    },
    {
      "path": "/empty"
    }
  ],
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 57,
        "mode": 41471,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 4,
        "block_size": 4096,
        "blocks": 8,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      },
      "mutation_policy": {
        "mutation_time": {
          "seconds": -13,
          "nanoseconds": 456
        }
      },
      "extended_attributes": [],
      "mutable_extended_attributes": true
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67",
      "mutable": true
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65",
      "mutable": true
    }
  ],
  "creation_policy": {
    "first_inode": "18364758544493064721",
    "block_size": 8192,
    "generation": 2309737967,
    "creation_time": {
      "seconds": -19,
      "nanoseconds": 987654321
    },
    "mutation_policy": {
      "allocation_unit": 4096,
      "mutation_time": {
        "seconds": -7,
        "nanoseconds": 123456789
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "umask": 23,
  "working_directory": "/"
}
''')
                            file_options = json.dumps(query_options)
                        unknown_xattr_mutation = mode == "xattr-mutations-unsupported"
                        if mode.startswith("xattr-mutations"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
  "files":[
    {"path":"/data","bytes_hex":"30313233343536373839",
     "extended_attributes":[],"mutable_extended_attributes":true},
    {"path":"/readonly","bytes_hex":"","extended_attributes":[]}],
  "directories":[{"path":"/","mutable":true},
    {"path":"/empty","extended_attributes":[],
     "mutable_extended_attributes":true}],
  "symbolic_links":[
    {"path":"/alias","target_hex":"64617461","mutable":true,
     "extended_attributes":[],"mutable_extended_attributes":true},
    {"path":"/dangling","target_hex":"6d697373696e67","mutable":true},
    {"path":"/cycle","target_hex":"6379636c65","mutable":true}],
  "working_directory":"/"}
''')
                            file_options = json.dumps(query_options)
                        unknown_xattrs = mode == "extended-attributes-unsupported"
                        if mode.startswith("extended-attributes"):
                            query_options = json.loads(file_options)
                            query_options["instruction_quantum"] = 1024
                            query_options["timeout_microseconds"] = 5_000_000
                            query_options["darwin_files"] = json.loads(r'''
{
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839",
      "extended_attributes": [
        {
          "name": "user.neverd.beta",
          "bytes_hex": "00ff410080420a"
        },
        {
          "name": "user.neverd.alpha",
          "bytes_hex": "616c706861"
        },
        {
          "name": "user.neverd.empty",
          "bytes_hex": ""
        }
      ]
    },
    {
      "path": "/unknown",
      "bytes_hex": ""
    },
    {
      "path": "/known-empty",
      "bytes_hex": "",
      "extended_attributes": []
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true
    }
  ],
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461",
      "mutable": true,
      "extended_attributes": []
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67",
      "mutable": true,
      "extended_attributes": []
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65",
      "mutable": true,
      "extended_attributes": []
    }
  ],
  "working_directory": "/"
}
''')
                            file_options = json.dumps(query_options)
                        incomplete = protected_link or unknown_pathconf or unknown_attributes or unknown_xattrs or unknown_names or unknown_bulk or unknown_xattr_mutation or unknown_hard_link_name or unknown_nonblocking
                        result = session.emulate_process(path, f"{profile}-macho64-v1", file_options)
                        self.assertEqual(result["stop_reason"],
                                         "unsupported_service" if incomplete else "exited",
                                         f"{mode}: {result['diagnostic']}")
                        self.assertEqual(result["exit_status"], None if incomplete else 37, mode)
                        self.assertEqual(bytes.fromhex(result["stdout_hex"]), expected)
                        self.assertEqual(result["stderr_hex"], "")
                        if unknown_nonblocking:
                            self.assertEqual(result["diagnostic"], "unsupported Darwin fcntl command")
                            self.assertEqual(result["services"][-1]["number"],
                                             "200005c" if architecture == "x86_64" else "5c")
                            self.assertIsNone(result["services"][-1]["result"])
                            self.assertNotIn("error", result["services"][-1])
                        if unknown_hard_link_name:
                            self.assertEqual(result["diagnostic"], "Darwin multiple-name vnode observations are unsupported")
                            last = result["services"][-1]
                            number = "5c" if mode in ("hard-links-name-unsupported", "symbolic-descriptors-name-unsupported") else "e4"
                            self.assertEqual(last["number"], "20000" + number if architecture == "x86_64" else number)
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                        if unknown_bulk:
                            self.assertEqual(result["diagnostic"],
                                             "Darwin selected file attributes are not modeled")
                            last = result["services"][-1]
                            self.assertEqual(last["number"], "20001cd" if architecture == "x86_64" else "1cd")
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                        if unknown_names:
                            self.assertEqual(result["diagnostic"],
                                             "Darwin object name is outside the bounded UTF-8 catalogue contract")
                            self.assertEqual(result["services"][-1]["number"],
                                             "20000dc" if architecture == "x86_64" else "dc")
                            self.assertIsNone(result["services"][-1]["result"])
                            self.assertNotIn("error", result["services"][-1])
                        if unknown_xattr_mutation:
                            last = result["services"][-1]
                            self.assertEqual(last["number"], "20000ec" if architecture == "x86_64" else "ec")
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                            self.assertEqual(result["diagnostic"], "Darwin extended-attribute mutation is not authorized")
                        if unknown_xattrs:
                            last = result["services"][-1]
                            self.assertEqual(last["number"], "20000ea" if architecture == "x86_64" else "ea")
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                            self.assertEqual(result["diagnostic"], "Darwin extended-attribute observations are unknown")
                        if unknown_attributes:
                            last = result["services"][-1]
                            self.assertEqual(last["number"], "20000dc" if architecture == "x86_64" else "dc")
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                            self.assertEqual(result["diagnostic"],
                                             "Darwin selected file attributes are not modeled")
                        if unknown_pathconf:
                            last = result["services"][-1]
                            self.assertEqual(last["number"], "20000bf" if architecture == "x86_64" else "bf")
                            self.assertIsNone(last["result"])
                            self.assertNotIn("error", last)
                            self.assertEqual(result["diagnostic"],
                                             "Darwin filesystem-dependent pathconf selector is not modeled")
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
                        if rename_links:
                            services = result["services"]
                            rename_numbers = (("2000080", "20001d1", "20001e8")
                                              if architecture == "x86_64" else ("80", "1d1", "1e8"))
                            renames = [event for event in services if event["number"] in rename_numbers]
                            self.assertTrue(any(event.get("error") is False and event["result"] == "0"
                                                for event in renames), mode)
                            self.assertTrue(any(event["number"] == rename_numbers[1]
                                                and event.get("error") is True and event["result"] == "42"
                                                for event in renames), mode)
                            flagged = [event for event in renames if event["number"] == rename_numbers[2]]
                            for code in ("11", "3e", "2", "16"):
                                self.assertTrue(any(event.get("error") is True and event["result"] == code
                                                    for event in flagged), mode)
                            for flag in ("2", "12"):
                                self.assertTrue(any(event.get("error") is False and event["result"] == "0"
                                                    and event["arguments"][4] == flag
                                                    for event in flagged), mode)
                            self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                            if protected_link:
                                self.assertEqual(services[-1]["number"], rename_numbers[0])
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
        for properties, expected in (({}, -1), ({"ro.build.version.sdk": "33"}, 33)):
            with self.subTest(properties=properties):
                options = json.dumps({"backend": "unicorn", "android": {
                    "entry_symbol": "device_api_level", "arguments": [0x20000000, 1],
                    "properties": properties,
                    "libraries": {"libplatform.so": ["android_get_device_api_level"]},
                    "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 16}],
                }})
                result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                                 "android-aarch64-api28-v1", options)
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertEqual(int(result["return_value"], 16), expected % (1 << 64))
                snapshot = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                self.assertEqual(snapshot, struct.pack("<IIII", expected % (1 << 32), 77,
                                                      expected % (1 << 32), 77))
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
