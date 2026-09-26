"""Exercise process-owned MDL views through the public JSON C API binding.

Optional artifacts use NEVERD_TEST_LIBNEVERD, NEVERD_TEST_WDM_USER_MAPPING_FIXTURE
and NEVERD_TEST_WDM_USER_MAPPING_CFG_FIXTURE. Test discovery never downloads SDKs.
"""

from __future__ import annotations

import copy
import ctypes
import json
import os
from pathlib import Path
import unittest


PROCESS_EXIT = 0x22210B
WRONG_PROCESS_UNMAP = 0x22210F
MAPPING_SHORTAGE = 0x222113

SCENARIO = (Path(__file__).resolve().parents[3]
            / "docs/examples/driver-user-mapping-scenario.json")


class DriverUserMappingIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_WDM_USER_MAPPING_FIXTURE")
        if not library or not normal:
            self.skipTest("built library and genuine WDM user mapping fixture are not configured")
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
        self.fixtures = [("normal", Path(normal).resolve(strict=True))]
        cfg = os.environ.get("NEVERD_TEST_WDM_USER_MAPPING_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(("cfg", Path(cfg).resolve(strict=True)))

    def _run(self, fixture: Path, scenario: dict, memory_limit: int = 64 * 1024 * 1024) -> dict:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000, memory_limit=memory_limit,
            event_limit=100_000, timeout_milliseconds=10_000,
        )
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, os.fsencode(fixture),
            json.dumps(scenario).encode("utf-8"), ctypes.byref(options),
        )
        self.assertIsNotNone(raw, self.host.owned_string("neverd_last_error", self.session))
        return json.loads(raw)

    def _success(self, result: dict, reports: list[str], revoked: bool = False) -> None:
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertTrue(result["scenario_success"], result["diagnostic"])
        self.assertTrue(result["unload_completed"])
        self.assertIsNone(result["fault"])
        requests = [request for request in result["requests"] if request["kind"] == "ioctl"]
        self.assertEqual(len(requests), len(reports))
        for request, expected in zip(requests, reports):
            self.assertTrue(request["completed"])
            self.assertEqual(request["io_status"], 0)
            report, = request["user_buffers"]
            self.assertEqual(report["id"], "report")
            self.assertEqual(report["backing_hex"], expected)
            self.assertEqual(report["revoked"], revoked)

    def test_example_exercises_alias_protection_and_attached_worker(self) -> None:
        scenario = json.loads(SCENARIO.read_text(encoding="utf-8"))
        for variant, fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                with self.subTest(variant=variant, address=address):
                    scenario["load_address"] = hex(address)
                    result = self._run(fixture, scenario)
                    self._success(result, ["43750107", "a7010000"])
                    self.assertEqual(int(result["image_base"], 16), address)
                    calls = [call["name"] for call in result["calls"]]
                    mapping_modes = [
                        int(call["arguments"][1], 16) & 0xFF
                        for call in result["calls"]
                        if call["name"] == "MmMapLockedPagesSpecifyCache"
                    ]
                    self.assertCountEqual(mapping_modes, [1, 1, 0, 0, 1])
                    self.assertIn("KeStackAttachProcess", calls)
                    self.assertIn("KeUnstackDetachProcess", calls)
                    self.assertEqual(
                        [entry["source_request_index"]
                         for entry in result["configuration"]["user_memory"]], [1, 2])

    def _single_request(self, action: int) -> dict:
        scenario = json.loads(SCENARIO.read_text(encoding="utf-8"))
        request = copy.deepcopy(scenario["requests"][1])
        request["code"] = hex(action)
        scenario["requests"] = [scenario["requests"][0], request,
                                {"kind": "cleanup"}, {"kind": "close"}]
        return scenario

    def test_exit_keeps_the_locked_report_observable(self) -> None:
        scenario = self._single_request(PROCESS_EXIT)
        scenario["requests"][1]["requestor_exit_after_dispatch"] = True
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                self._success(self._run(fixture, scenario), ["a7010000"], revoked=True)

    def test_wrong_process_unmap_remains_an_explicit_model_error(self) -> None:
        scenario = self._single_request(WRONG_PROCESS_UNMAP)
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, scenario)
                self.assertEqual(result["stop_reason"], "model_error")
                self.assertFalse(result["scenario_success"])
                self.assertFalse(result["requests"][1]["completed"])
                self.assertEqual(result["calls"][-1]["name"], "MmUnmapLockedPages")

    def test_resource_shortage_is_caught_inside_the_genuine_driver(self) -> None:
        scenario = self._single_request(MAPPING_SHORTAGE)
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                self._success(self._run(fixture, scenario, 4 * 1024 * 1024), ["01000000"])


if __name__ == "__main__":
    unittest.main()
