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


if __name__ == "__main__":
    unittest.main()
