"""Execute genuine WDK API-raised C SEH through the existing owned JSON API.

Optional artifacts use NEVERD_TEST_LIBNEVERD, NEVERD_TEST_WDM_SEH_FIXTURE and
NEVERD_TEST_WDM_SEH_CFG_FIXTURE. No SDK download or repository-local binary path
is inferred when those variables are absent.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import unittest


class DriverSEHIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_WDM_SEH_FIXTURE")
        if not library or not normal:
            self.skipTest("built libneverd and genuine WDM SEH fixture are not configured")

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
        cfg = os.environ.get("NEVERD_TEST_WDM_SEH_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(("cfg", Path(cfg).resolve(strict=True)))

    def _run(self, fixture: Path, mode: str, address: int = 0x190000000) -> dict:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000, memory_limit=64 * 1024 * 1024,
            event_limit=100_000, timeout_milliseconds=10_000,
            service_name=("NeverDSEH" + mode).encode("ascii"),
        )
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, os.fsencode(fixture),
            json.dumps({"unload": True, "load_address": hex(address)}).encode("utf-8"),
            ctypes.byref(options),
        )
        self.assertIsNotNone(raw, self.host.owned_string("neverd_last_error", self.session))
        return json.loads(raw)

    def _success(self, result: dict, mode: str, expected_raises: list[str]) -> None:
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertTrue(result["scenario_success"])
        self.assertTrue(result["unload_completed"])
        self.assertIsNone(result["fault"])
        self.assertEqual(result["devices"], [])
        self.assertEqual(result["requests"], [])
        messages = result["messages"]
        self.assertFalse(any("WDM SEH: failure" in m for m in messages))
        complete = next(i for i, m in enumerate(messages)
                        if "WDM SEH: complete mode=" + mode in m)
        unload = next(i for i, m in enumerate(messages) if "WDM SEH: unload" in m)
        self.assertLess(complete, unload)
        raises = [call for call in result["calls"] if call["name"].startswith("ExRaise")]
        self.assertEqual([call["name"] for call in raises], expected_raises)
        for call in raises:
            self.assertIsNone(call["result"])

    def test_three_exports_reach_actual_handlers_with_normal_cfg_and_rebasing(self) -> None:
        exports = {
            "S": ("ExRaiseStatus", "c000009a"),
            "A": ("ExRaiseAccessViolation", "c0000005"),
            "D": ("ExRaiseDatatypeMisalignment", "80000002"),
        }
        for variant, fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                for mode, (name, code) in exports.items():
                    with self.subTest(variant=variant, address=address, mode=mode):
                        result = self._run(fixture, mode, address)
                        self._success(result, mode, [name])
                        self.assertEqual(int(result["image_base"], 16), address)
                        self.assertTrue(any("code=" + code in m for m in result["messages"]))
                        self.assertTrue(any("local=12cb5687" in m for m in result["messages"]))

    def test_helper_registers_nested_scopes_and_reraised_handlers(self) -> None:
        cases = {
            "H": (["ExRaiseStatus"], "nonvolatile restored"),
            "N": (["ExRaiseAccessViolation"], "nested inner code=c0000005"),
            "R": (["ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"],
                  "reraising outer code=80000002"),
            "G": (["ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"],
                  "helper reraising outer code=80000002"),
        }
        for variant, fixture in self.fixtures:
            for mode, (names, marker) in cases.items():
                with self.subTest(variant=variant, mode=mode):
                    result = self._run(fixture, mode)
                    self._success(result, mode, names)
                    self.assertTrue(any(marker in m for m in result["messages"]))

    def test_dynamic_filter_and_finally_are_rejected_before_execution(self) -> None:
        for variant, fixture in self.fixtures:
            for mode, kind in (("F", "filter"), ("T", "finally")):
                with self.subTest(variant=variant, mode=mode):
                    result = self._run(fixture, mode)
                    self.assertEqual(result["stop_reason"], "model_error")
                    self.assertFalse(result["scenario_success"])
                    self.assertFalse(result["unload_completed"])
                    self.assertIn(kind, result["diagnostic"])
                    self.assertFalse(any("unsupported " + kind in m for m in result["messages"]))

    def test_unhandled_api_raise_and_cpu_fault_remain_distinct_stops(self) -> None:
        for variant, fixture in self.fixtures:
            for mode in ("U", "C"):
                with self.subTest(variant=variant, mode=mode):
                    result = self._run(fixture, mode)
                    self.assertFalse(result["scenario_success"])
                    self.assertFalse(result["unload_completed"])
                    self.assertFalse(any("WDM SEH: complete" in m for m in result["messages"]))
                    if mode == "U":
                        self.assertEqual(result["stop_reason"], "model_error")
                        self.assertIn("unhandled", result["diagnostic"])
                    else:
                        self.assertEqual(result["stop_reason"], "memory_fault")
                        self.assertEqual(int(result["fault"]["address"], 16), 0x11100000)
                        self.assertEqual(result["fault"]["access"], "read")
                        self.assertFalse(any("unsupported CPU handler" in m
                                             for m in result["messages"]))


if __name__ == "__main__":
    unittest.main()
