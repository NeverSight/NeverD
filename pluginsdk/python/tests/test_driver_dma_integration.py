"""Real driver scenarios through the Python SDK's owned C-ABI JSON binding.

The genuine WDK fixtures are optional build outputs, supplied explicitly via
NEVERD_TEST_WDM_DMA_FIXTURE and NEVERD_TEST_WDM_DMA_CFG_FIXTURE. No SDK download
or machine-specific artifact location is required by test discovery.
"""

from __future__ import annotations

import copy
import ctypes
import json
import os
from pathlib import Path
import struct
import unittest


LOGICAL = 0x40000000
PAYLOAD = bytes(range(0xA0, 0xB0))


def _scenario(mode: str) -> dict:
    def pnp(minor: str, delay: int = 0) -> dict:
        return {
            "kind": "pnp",
            "device_id": "dma0",
            "minor": minor,
            "bus_completion": {"status": 0, "delay_100ns": delay},
        }

    transfer = {
        "kind": "ioctl",
        "file": 1,
        "code": "0x222000",
        "output_size": 32,
    }
    if mode != "I":
        transfer["interrupt_events"] = [{
            "after_100ns": 7,
            "device_id": "dma0",
            "interrupt_id": "line0",
        }]
        transfer["dma_events"] = [{
            "after_100ns": 7,
            "device_id": "dma0",
            "logical_address": hex(LOGICAL + (4096 if mode == "Q" else 0)),
            "direction": "write_memory",
            "length": len(PAYLOAD),
            "data_hex": PAYLOAD.hex(),
        }]
        if mode == "C":
            transfer["dma_events"].insert(0, {
                "after_100ns": 5,
                "device_id": "dma0",
                "logical_address": hex(LOGICAL),
                "direction": "read_memory",
                "length": len(PAYLOAD),
            })
    return {
        "load_address": "0x190000000",
        "unload": True,
        "pnp_devices": [{
            "id": "dma0",
            "bus": "register_bank",
            "initial_device_power": "D0",
            "initial_system_power": "working",
            "interrupts": [{
                "id": "line0",
                "raw_vector": 17,
                "raw_level": 7,
                "raw_affinity": 1,
                "translated_vector": 145,
                "translated_level": 5,
                "translated_affinity": 1,
                "mode": "latched",
                "share": "device_exclusive",
            }],
            "dma": {
                "address_bits": 64,
                "maximum_length": 4096,
                "map_registers": 1 if mode == "Q" else 4,
                "alignment": 1,
                "logical_base": hex(LOGICAL),
                "logical_length": 65536,
                "scatter_gather": True,
            },
        }],
        "requests": [
            pnp("start", 11),
            {"kind": "create", "device_id": "dma0", "file": 1},
            transfer,
            {"kind": "cleanup", "file": 1},
            {"kind": "close", "file": 1},
            pnp("query_remove"),
            pnp("remove", 3),
        ],
    }


class DriverDMAIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_WDM_DMA_FIXTURE")
        if not library or not normal:
            self.skipTest("built libneverd and genuine WDM DMA fixture are not configured")

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
        cfg = os.environ.get("NEVERD_TEST_WDM_DMA_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(("cfg", Path(cfg).resolve(strict=True)))

    def _invoke(self, fixture: Path, mode: str, scenario: dict) -> str | None:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000,
            memory_limit=64 * 1024 * 1024,
            event_limit=100_000,
            timeout_milliseconds=10_000,
            service_name=("NeverDDma" + mode).encode("ascii"),
        )
        return self.host.owned_string(
            "neverd_emulate_driver_scenario_json",
            self.session,
            os.fsencode(fixture),
            json.dumps(scenario).encode("utf-8"),
            ctypes.byref(options),
        )

    def _run(self, fixture: Path, mode: str, scenario: dict) -> dict:
        raw = self._invoke(fixture, mode, scenario)
        self.assertIsNotNone(
            raw, self.host.owned_string("neverd_last_error", self.session)
        )
        result = json.loads(raw)
        self.assertFalse(any("WDM DMA: failure" in m for m in result["messages"]))
        return result

    def _success(self, result: dict, callbacks: int, isrs: int, data: bytes) -> None:
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertTrue(result["scenario_success"])
        self.assertTrue(result["unload_completed"])
        request = next(r for r in result["requests"] if r["kind"] == "ioctl")
        self.assertTrue(request["completed"])
        self.assertEqual(request["information"], 32)
        output = bytes.fromhex(request["output_hex"])
        self.assertEqual(struct.unpack("<4I", output[:16]), (1, 1, callbacks, isrs))
        self.assertEqual(output[16:], data)
        self.assertEqual(int(result["image_base"], 16), 0x190000000)

    def test_common_buffer_bidirectional_bytes_precede_the_real_interrupt(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "C", _scenario("C"))
                self._success(result, 0, 1, PAYLOAD)
                read, write = result["dma_transfers"]
                self.assertEqual(bytes.fromhex(read["data_hex"]), bytes(range(0x11, 0x21)))
                self.assertEqual(bytes.fromhex(write["data_hex"]), PAYLOAD)
                self.assertEqual(read["completed_at_100ns"], 16)
                self.assertEqual(write["completed_at_100ns"], 18)
                self.assertEqual([read["source_request_index"], write["source_request_index"]], [2, 2])
                irq, = result["interrupts"]
                self.assertTrue(irq["claimed"])
                self.assertEqual(irq["delivered_at_100ns"], write["completed_at_100ns"])
                self.assertEqual(irq["return_value"], 1)

    def test_sg_resource_fifo_delivers_two_real_void_callbacks(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "Q", _scenario("Q"))
                self._success(result, 2, 1, PAYLOAD)
                messages = result["messages"]
                order = [
                    "callback unit=1 packet=0",
                    "get returned unit=1 packet=0",
                    "get returned unit=1 packet=1",
                    "second packet queued unit=1",
                    "put unit=1 packet=0",
                    "callback unit=1 packet=1",
                ]
                positions = [next(i for i, text in enumerate(messages) if part in text) for part in order]
                self.assertEqual(positions, sorted(positions))
                transfer, = result["dma_transfers"]
                self.assertEqual(int(transfer["logical_address"], 16), LOGICAL + 4096)
                self.assertEqual(bytes.fromhex(transfer["data_hex"]), PAYLOAD)

    def test_callback_can_put_retire_adapter_and_complete_before_get_returns(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "I", _scenario("I"))
                self._success(result, 1, 0, bytes(range(0x21, 0x31)))
                self.assertEqual(result["dma_transfers"], [])
                self.assertEqual(result["interrupts"], [])
                messages = result["messages"]
                completed = next(i for i, m in enumerate(messages) if "callback completed before return" in m)
                returned = next(i for i, m in enumerate(messages) if "get returned" in m)
                self.assertLess(completed, returned)

    def test_late_dma_failure_remains_visible_after_source_irp_completion(self) -> None:
        scenario = _scenario("I")
        scenario["requests"][2]["dma_events"] = [{
            "after_100ns": 7,
            "device_id": "dma0",
            "logical_address": hex(LOGICAL),
            "direction": "read_memory",
            "length": 16,
        }]
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "I", scenario)
                self.assertEqual(result["stop_reason"], "model_error")
                self.assertFalse(result["scenario_success"])
                request = next(r for r in result["requests"] if r["kind"] == "ioctl")
                self.assertTrue(request["completed"])
                self.assertEqual(request["information"], 32)
                transfer, = result["dma_transfers"]
                self.assertEqual(transfer["source_request_index"], 2)
                self.assertEqual(transfer["occurred_at_100ns"], 18)
                self.assertIsNone(transfer["completed_at_100ns"])
                self.assertTrue(transfer["failure_reason"])
                self.assertEqual(transfer["data_hex"], "")
                self.assertEqual(result["interrupts"], [])

    def test_invalid_dma_json_is_rejected_before_driver_execution(self) -> None:
        scenario = _scenario("C")
        invalid = []
        wrong_size = copy.deepcopy(scenario)
        wrong_size["requests"][2]["dma_events"][1]["data_hex"] = "aa"
        invalid.append((wrong_size, "exactly length"))
        unknown = copy.deepcopy(scenario)
        unknown["requests"][2]["dma_events"][0]["surprise_dma"] = True
        invalid.append((unknown, "unknown"))
        boolean_count = copy.deepcopy(scenario)
        boolean_count["pnp_devices"][0]["dma"]["map_registers"] = True
        invalid.append((boolean_count, "integer"))
        for variant, fixture in self.fixtures:
            for input_json, diagnostic in invalid:
                with self.subTest(variant=variant, diagnostic=diagnostic):
                    self.assertIsNone(self._invoke(fixture, "C", input_json))
                    message = self.host.owned_string("neverd_last_error", self.session)
                    self.assertIn(diagnostic, message)


if __name__ == "__main__":
    unittest.main()
