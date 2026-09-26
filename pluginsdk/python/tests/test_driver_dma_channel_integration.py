"""Execute genuine WDK DMA channels through the existing owned JSON C binding.

Optional artifacts are supplied through NEVERD_TEST_LIBNEVERD,
NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE and NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE.
Discovery does not download an SDK or assume a repository artifact location.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import struct
import unittest


LOGICAL = 0x40000000
PAYLOAD = bytes(range(0xA0, 0xC0))


def _scenario(mode: str) -> dict:
    def pnp(minor: str, delay: int = 0) -> dict:
        return {
            "kind": "pnp", "device_id": "dma0", "minor": minor,
            "bus_completion": {"status": 0, "delay_100ns": delay},
        }

    transfers = []
    interrupts = []
    for operation in range(2 if mode == "U" else 1):
        delay = 7 * (operation + 1)
        payload = bytes(range(0xA0 + operation * 16, 0xC0 + operation * 16))
        address = LOGICAL + 4088 + 8192 * (operation + (mode == "Q"))
        # One bus transaction deliberately crosses both SG page fragments.
        transfers.append({
            "after_100ns": delay, "device_id": "dma0",
            "logical_address": hex(address), "direction": "write_memory",
            "length": 32, "data_hex": payload.hex(),
        })
        interrupts.append({
            "after_100ns": delay, "device_id": "dma0", "interrupt_id": "line0",
        })
    if mode == "Q":
        transfers.insert(0, {
            "after_100ns": 5, "device_id": "dma0",
            "logical_address": hex(LOGICAL), "direction": "read_memory", "length": 16,
        })
    return {
        "load_address": "0x190000000", "unload": True,
        "pnp_devices": [{
            "id": "dma0", "bus": "register_bank",
            "initial_device_power": "D0", "initial_system_power": "working",
            "interrupts": [{
                "id": "line0", "raw_vector": 17, "raw_level": 7, "raw_affinity": 1,
                "translated_vector": 145, "translated_level": 5,
                "translated_affinity": 1, "mode": "latched", "share": "device_exclusive",
            }],
            "dma": {
                "address_bits": 64, "maximum_length": 8192,
                "map_registers": 3 if mode == "Q" else 4, "alignment": 1,
                "logical_base": hex(LOGICAL), "logical_length": 262144,
                "scatter_gather": True,
            },
        }],
        "requests": [
            pnp("start", 11),
            {"kind": "create", "device_id": "dma0", "file": 1},
            {"kind": "ioctl", "file": 1, "code": "0x222000", "output_size": 48,
             "dma_events": transfers, "interrupt_events": interrupts},
            {"kind": "cleanup", "file": 1}, {"kind": "close", "file": 1},
            pnp("query_remove"), pnp("remove", 3),
        ],
    }


class DriverDMAChannelIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE")
        if not library or not normal:
            self.skipTest("built libneverd and genuine WDM DMA channel fixture are not configured")

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
        cfg = os.environ.get("NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(("cfg", Path(cfg).resolve(strict=True)))

    def _run(self, fixture: Path, mode: str) -> dict:
        from neverd_plugin.abi import NeverDDriverOptionsV1

        options = NeverDDriverOptionsV1(
            struct_size=ctypes.sizeof(NeverDDriverOptionsV1),
            instruction_limit=1_000_000, memory_limit=64 * 1024 * 1024,
            event_limit=100_000, timeout_milliseconds=10_000,
            service_name=("NeverDChannel" + mode).encode("ascii"),
        )
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, os.fsencode(fixture),
            json.dumps(_scenario(mode)).encode("utf-8"), ctypes.byref(options),
        )
        self.assertIsNotNone(raw, self.host.owned_string("neverd_last_error", self.session))
        result = json.loads(raw)
        self.assertFalse(any("WDM channel: failure" in m for m in result["messages"]))
        return result

    def _success(self, result: dict, operations: int, data: bytes) -> None:
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertTrue(result["scenario_success"])
        self.assertTrue(result["unload_completed"])
        self.assertEqual(result["devices"], [])
        self.assertEqual(int(result["image_base"], 16), 0x190000000)
        request = result["requests"][2]
        self.assertEqual(request["kind"], "ioctl")
        self.assertTrue(request["completed"])
        self.assertEqual(request["information"], 48)
        output = bytes.fromhex(request["output_hex"])
        self.assertEqual(struct.unpack("<4I", output[:16]), (1, 1, operations, operations))
        self.assertEqual(output[16:], data)
        for transfer in result["dma_transfers"]:
            self.assertIsNotNone(transfer["completed_at_100ns"])
            self.assertIsNone(transfer["failure_reason"])

    def test_one_bus_transaction_crosses_two_fragments_before_aggregate_flush(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "S")
                self._success(result, 1, PAYLOAD)
                transfer, = result["dma_transfers"]
                self.assertEqual(transfer["length"], 32)
                self.assertEqual(bytes.fromhex(transfer["data_hex"]), PAYLOAD)
                self.assertEqual(int(transfer["logical_address"], 16), LOGICAL + 4088)
                self.assertEqual(transfer["completed_at_100ns"], 18)
                irq, = result["interrupts"]
                self.assertEqual(irq["delivered_at_100ns"], transfer["completed_at_100ns"])
                self.assertEqual(irq["return_value"], 1)
                names = [call["name"] for call in result["calls"]]
                self.assertEqual(names.count("MapTransfer"), 2)
                self.assertEqual(names.count("FlushAdapterBuffers"), 1)
                self.assertEqual(names.count("FreeMapRegisters"), 1)

    def test_retained_registers_support_a_second_operation_after_flush(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "U")
                self._success(result, 2, bytes(range(0xB0, 0xD0)))
                first, second = result["dma_transfers"]
                self.assertEqual(first["completed_at_100ns"], 18)
                self.assertEqual(second["completed_at_100ns"], 25)
                self.assertEqual(int(second["logical_address"], 16), LOGICAL + 12280)
                self.assertEqual(first["mapping"], second["mapping"])
                self.assertNotEqual(first["logical_address"], second["logical_address"])
                self.assertEqual(first["adapter"], second["adapter"])
                names = [call["name"] for call in result["calls"]]
                self.assertEqual(names.count("AllocateAdapterChannel"), 1)
                self.assertEqual(names.count("FlushAdapterBuffers"), 2)
                self.assertEqual(names.count("FreeMapRegisters"), 1)

    def test_shared_quota_preserves_the_registration_time_irp_snapshot(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "Q")
                self._success(result, 1, PAYLOAD)
                common, transfer = result["dma_transfers"]
                self.assertEqual(bytes.fromhex(common["data_hex"]), bytes(range(0x80, 0x90)))
                self.assertEqual(int(transfer["logical_address"], 16), LOGICAL + 12280)
                order = ["SG callback unit=1", "queued with original IRP; field cleared",
                         "allocate returned unit=1", "callback unit=1 snapshot=1"]
                positions = [next(i for i, m in enumerate(result["messages"]) if text in m)
                             for text in order]
                self.assertEqual(positions, sorted(positions))

    def test_partial_aggregate_flush_keeps_the_irp_incomplete(self) -> None:
        for variant, fixture in self.fixtures:
            with self.subTest(variant=variant):
                result = self._run(fixture, "P")
                self.assertEqual(result["stop_reason"], "model_error", result["diagnostic"])
                self.assertFalse(result["scenario_success"])
                self.assertFalse(result["unload_completed"])
                self.assertFalse(result["requests"][2]["completed"])
                transfer, = result["dma_transfers"]
                self.assertEqual(transfer["completed_at_100ns"], 18)
                self.assertIsNone(transfer["failure_reason"])
                self.assertEqual(bytes.fromhex(transfer["data_hex"]), PAYLOAD)
                self.assertFalse(any("freed registers" in m for m in result["messages"]))


if __name__ == "__main__":
    unittest.main()
