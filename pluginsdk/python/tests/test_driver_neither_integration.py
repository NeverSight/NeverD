"""Run genuine WDK user-buffer requests through the owned JSON C binding.

Optional inputs: NEVERD_TEST_LIBNEVERD, NEVERD_TEST_WDM_NEITHER_FIXTURE and
NEVERD_TEST_WDM_NEITHER_CFG_FIXTURE.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import unittest


class DriverNeitherIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_WDM_NEITHER_FIXTURE")
        if not library or not normal:
            self.skipTest("built libneverd and genuine WDM user-buffer fixture are not configured")

        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(self.host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        self.session = ctypes.c_void_p(address)
        self.addCleanup(self.host.call, "neverd_session_destroy", self.session)
        self.fixtures = [Path(normal).resolve(strict=True)]
        cfg = os.environ.get("NEVERD_TEST_WDM_NEITHER_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(Path(cfg).resolve(strict=True))

    def test_synchronization_primitives_restore_state(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        for fixture in self.fixtures:
            for code, output in (("0x222037", "5a"), ("0x22203b", "6b"),
                                 ("0x22203f", "7c")):
                with self.subTest(fixture=fixture, code=code):
                    scenario = {
                        "requests": [
                            {"kind": "create", "device": "\\Device\\NeverDNeither"},
                            {"kind": "ioctl", "code": code, "input": "01020304",
                             "output_size": 4},
                            {"kind": "cleanup"},
                            {"kind": "close"},
                        ],
                        "unload": True,
                    }
                    raw = self.host.owned_string(
                        "neverd_emulate_driver_scenario_json",
                        self.session,
                        os.fsencode(fixture),
                        json.dumps(scenario).encode("utf-8"),
                        ctypes.byref(options),
                    )
                    self.assertIsNotNone(raw)
                    result = json.loads(raw)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertTrue(result["scenario_success"])
                    self.assertTrue(result["unload_completed"])
                    self.assertEqual(result["requests"][1]["io_status"], 0)
                    self.assertEqual(result["requests"][1]["output_hex"], output)

    def test_public_scenario_preserves_user_and_locked_alias_bytes(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        scenario = Path(__file__).resolve().parents[3] / "docs/examples/driver-neither-scenario.json"
        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        for fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                with self.subTest(fixture=fixture, address=address):
                    data = json.loads(scenario.read_text())
                    data["load_address"] = hex(address)
                    raw = self.host.owned_string(
                        "neverd_emulate_driver_scenario_json",
                        self.session,
                        os.fsencode(fixture),
                        json.dumps(data).encode("utf-8"),
                        ctypes.byref(options),
                    )
                    self.assertIsNotNone(raw)
                    result = json.loads(raw)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertTrue(result["scenario_success"])
                    self.assertTrue(result["unload_completed"])
                    self.assertIsNone(result["fault"])
                    self.assertEqual(len(result["requests"]), 5)
                    self.assertEqual(result["requests"][1]["output_hex"], "5b58595e")
                    self.assertEqual(result["requests"][2]["output_hex"], "02030405")

    def test_read_only_user_output_fails_in_guest_probe(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        data = {
            "requests": [
                {"kind": "create", "device": "\\Device\\NeverDNeither"},
                {"kind": "ioctl", "code": "0x222003", "input": "01020304",
                 "output_size": 4, "user_output_access": "read_only"},
                {"kind": "cleanup"},
                {"kind": "close"},
            ],
            "unload": True,
        }
        for fixture in self.fixtures:
            with self.subTest(fixture=fixture):
                raw = self.host.owned_string(
                    "neverd_emulate_driver_scenario_json",
                    self.session,
                    os.fsencode(fixture),
                    json.dumps(data).encode("utf-8"),
                    ctypes.byref(options),
                )
                self.assertIsNotNone(raw)
                result = json.loads(raw)
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertFalse(result["scenario_success"])
                self.assertIsNone(result["fault"])
                self.assertEqual(result["requests"][1]["io_status"], 0xC0000005)
                self.assertTrue(result["requests"][1]["completed"])

    def test_neither_read_write_uses_caller_buffer(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        data = {
            "requests": [
                {"kind": "create", "device": "\\Device\\NeverDNeither"},
                {"kind": "write", "input": "01020304",
                 "user_input_access": "read_only"},
                {"kind": "read", "output_size": 4},
                {"kind": "cleanup"},
                {"kind": "close"},
            ],
            "unload": True,
        }
        for fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                with self.subTest(fixture=fixture, address=address):
                    data["load_address"] = hex(address)
                    raw = self.host.owned_string(
                        "neverd_emulate_driver_scenario_json",
                        self.session,
                        os.fsencode(fixture),
                        json.dumps(data).encode("utf-8"),
                        ctypes.byref(options),
                    )
                    self.assertIsNotNone(raw)
                    result = json.loads(raw)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertTrue(result["scenario_success"])
                    self.assertTrue(result["unload_completed"])
                    self.assertEqual(result["requests"][1]["information"], 4)
                    self.assertEqual(result["requests"][2]["output_hex"], "70717273")

    def test_locked_neither_buffers_survive_pending_worker(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        data = {
            "requests": [
                {"kind": "create", "device": "\\Device\\NeverDNeither"},
                {"kind": "ioctl", "code": "0x22201b", "input": "01020304",
                 "output_size": 4},
                {"kind": "cleanup"},
                {"kind": "close"},
            ],
            "unload": True,
        }
        for fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                with self.subTest(fixture=fixture, address=address):
                    data["load_address"] = hex(address)
                    raw = self.host.owned_string(
                        "neverd_emulate_driver_scenario_json",
                        self.session,
                        os.fsencode(fixture),
                        json.dumps(data).encode("utf-8"),
                        ctypes.byref(options),
                    )
                    self.assertIsNotNone(raw)
                    result = json.loads(raw)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertTrue(result["scenario_success"])
                    self.assertTrue(result["unload_completed"])
                    self.assertEqual(result["requests"][1]["dispatch_status"], 0x103)
                    self.assertEqual(result["requests"][1]["output_hex"], "11121314")

    def test_work_item_attaches_live_requestor_for_raw_user_bytes(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        for fixture in self.fixtures:
            for inaccessible in (False, True):
                with self.subTest(fixture=fixture, inaccessible=inaccessible):
                    request = {
                        "kind": "ioctl", "code": "0x222033", "input": "01020304",
                        "output_size": 4, "requestor_process_id": 12336,
                    }
                    if inaccessible:
                        request["user_input_access"] = "no_access"
                    scenario = {
                        "requests": [
                            {"kind": "create", "device": "\\Device\\NeverDNeither"},
                            request,
                            {"kind": "cleanup"},
                            {"kind": "close"},
                        ],
                        "unload": True,
                    }
                    raw = self.host.owned_string(
                        "neverd_emulate_driver_scenario_json",
                        self.session,
                        os.fsencode(fixture),
                        json.dumps(scenario).encode("utf-8"),
                        ctypes.byref(options),
                    )
                    self.assertIsNotNone(raw)
                    result = json.loads(raw)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertIsNone(result["fault"])
                    self.assertTrue(result["unload_completed"])
                    self.assertEqual(result["requests"][1]["dispatch_status"], 0x103)
                    self.assertEqual(result["requests"][1]["io_status"],
                                     0xC0000005 if inaccessible else 0)
                    self.assertEqual(result["requests"][1]["output_hex"],
                                     "" if inaccessible else "21222324")

    def test_pending_neither_request_completes_through_cancel_routine(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        data = {
            "requests": [
                {"kind": "create", "device": "\\Device\\NeverDNeither"},
                {"kind": "ioctl", "code": "0x222023", "input": "01020304",
                 "output_size": 4, "cancel_after_100ns": 0},
                {"kind": "cleanup"},
                {"kind": "close"},
            ],
            "unload": True,
        }
        for fixture in self.fixtures:
            with self.subTest(fixture=fixture):
                raw = self.host.owned_string(
                    "neverd_emulate_driver_scenario_json",
                    self.session,
                    os.fsencode(fixture),
                    json.dumps(data).encode("utf-8"),
                    ctypes.byref(options),
                )
                self.assertIsNotNone(raw)
                result = json.loads(raw)
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertFalse(result["scenario_success"])
                self.assertTrue(result["unload_completed"])
                request = result["requests"][1]
                self.assertEqual(request["dispatch_status"], 0x103)
                self.assertEqual(request["io_status"], 0xC0000120)
                self.assertEqual(request["cancel_requested_at_100ns"], 0)
                self.assertTrue(request["completed"])

    def test_revoked_user_addresses_keep_locked_worker_and_cancel_paths(self) -> None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=b"NeverDNeither",
        )
        for fixture in self.fixtures:
            for code, status in (("0x22201b", 0), ("0x222023", 0xC0000120)):
                for event in ("user_unmap_after_dispatch", "requestor_exit_after_dispatch"):
                    with self.subTest(fixture=fixture, code=code, event=event):
                        request = {
                            "kind": "ioctl", "code": code, "input": "01020304",
                            "output_size": 4, event: True,
                        }
                        if event == "requestor_exit_after_dispatch":
                            request["requestor_process_id"] = 4112
                        if status:
                            request["cancel_after_100ns"] = 0
                        data = {
                            "requests": [
                                {"kind": "create", "device": "\\Device\\NeverDNeither"},
                                request,
                                {"kind": "cleanup"}, {"kind": "close"},
                            ],
                            "unload": True,
                        }
                        raw = self.host.owned_string(
                            "neverd_emulate_driver_scenario_json", self.session,
                            os.fsencode(fixture), json.dumps(data).encode("utf-8"),
                            ctypes.byref(options),
                        )
                        self.assertIsNotNone(raw)
                        result = json.loads(raw)
                        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                        self.assertEqual(result["scenario_success"], not bool(status))
                        self.assertTrue(result["unload_completed"])
                        observed = result["requests"][1]
                        self.assertEqual(observed["io_status"], status)
                        self.assertEqual(observed["output_hex"], "")
                        self.assertEqual(observed["information"], 0 if status else 4)


if __name__ == "__main__":
    unittest.main()
