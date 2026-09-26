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

    def test_gs_cookie_success_and_corruption_in_fixed_and_aligned_frames(self) -> None:
        for variant, fixture in self.fixtures:
            for mode in ("g", "a", "b", "d", "s", "t", "u", "v"):
                with self.subTest(variant=variant, mode=mode):
                    result = self._run(fixture, mode)
                    self.assertNotEqual(int(result["security_cookie"], 16), 0)
                    if mode in ("g", "a", "s", "t"):
                        self._success(result, mode, ["ExRaiseAccessViolation"])
                        self.assertTrue(any("GS cookie checked" in m
                                            for m in result["messages"]))
                    else:
                        self.assertEqual(result["stop_reason"], "model_error")
                        self.assertFalse(result["scenario_success"])
                        self.assertFalse(result["unload_completed"])
                        self.assertIn("GS security cookie check failed", result["diagnostic"])
                        self.assertFalse(any("GS cookie checked" in m
                                             for m in result["messages"]))

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

    def test_dynamic_filters_and_finally_execute_in_search_and_unwind_order(self) -> None:
        cases = {
            "F": (["ExRaiseAccessViolation"], ["dynamic inner handled"]),
            "Q": (["ExRaiseAccessViolation"],
                  ["decision=0 stage=51", "decision=1 stage=52", "dynamic outer handled"]),
            "T": (["ExRaiseAccessViolation"],
                  ["filter decision=1 stage=61", "helper finally abnormal",
                   "parent finally abnormal=1", "finally handler"]),
            "L": ([], ["parent finally abnormal=0"]),
        }
        for variant, fixture in self.fixtures:
            for address in (0x180000000, 0x190000000):
                for mode, (raises, markers) in cases.items():
                    with self.subTest(variant=variant, address=address, mode=mode):
                        result = self._run(fixture, mode, address)
                        self._success(result, mode, raises)
                        indices = [next(i for i, message in enumerate(result["messages"])
                                        if marker in message) for marker in markers]
                        self.assertEqual(indices, sorted(indices))

    def test_nested_filters_and_collided_finally_preserve_search_order(self) -> None:
        for variant, fixture in self.fixtures:
            for mode in ("B", "J"):
                with self.subTest(variant=variant, mode=mode):
                    result = self._run(fixture, mode)
                    self._success(result, mode, ["ExRaiseAccessViolation",
                                                 "ExRaiseDatatypeMisalignment"])
                    messages = result["messages"]
                    nested = next(i for i, message in enumerate(messages)
                                  if "nested filter decision=" in message)
                    if mode == "B":
                        handled = next(i for i, message in enumerate(messages)
                                       if "dynamic inner handled" in message)
                        self.assertLess(nested, handled)
                    else:
                        cleanups = [i for i, message in enumerate(messages)
                                    if "collided finally entered" in message]
                        self.assertEqual(len(cleanups), 1)
                        self.assertLess(cleanups[0], nested)
                        outer = next(i for i, message in enumerate(messages)
                                     if "parent finally abnormal=1" in message)
                        self.assertLess(nested, outer)

    def test_unsupported_api_filter_continuation_stops(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "E")
                self.assertEqual(result["stop_reason"], "model_error")
                self.assertFalse(result["scenario_success"])
                self.assertFalse(result["unload_completed"])
                self.assertIn("continuing a modeled API", result["diagnostic"])
                self.assertFalse(any("dynamic inner handled" in m
                                     for m in result["messages"]))

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
