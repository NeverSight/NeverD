import copy
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_cxx_rewrite as runner
from scripts import windows_registration_cleanup_relift as cleanup
from scripts.tests import test_check_windows_registration_cxx_rewrite as first_fixture


class CleanupReliftEvidenceTests(unittest.TestCase):
    def images(self):
        first, contract = first_fixture.WindowsRegistrationCxxRuntimeAdmissionTests.compiled_pe()
        contract.update(image_sha256=hashlib.sha256(first).hexdigest(),
                        generated_owner_end_rva=0x2100)
        second = bytearray(first)
        second.extend(bytes(0x1200 - len(second)))
        struct.pack_into("<H", second, 0x86, 4)
        section = 0x98 + 224 + 120
        second[section:section + 8] = b".ndtext\0"
        struct.pack_into("<4I", second, section + 8, 0x400, 0x4000, 0x400, 0xe00)
        struct.pack_into("<I", second, section + 36, 0x60000020)
        second[0x600] = 0xe9
        struct.pack_into("<i", second, 0x601, 0x4000 - 0x2005)
        struct.pack_into("<I", second, 0xc44, 3)
        struct.pack_into("<I", second, 0xd08, 0x4100)
        second[0xf00] = 0xb8
        struct.pack_into("<I", second, 0xf01, 0x404200)
        second[0xf05] = 0xe9
        struct.pack_into("<i", second, 0xf06, 0x1010 - 0x410a)
        struct.pack_into("<I", second, 0x1000, 0x19930522)
        size_field = 0x98 + 96 + 44
        size = struct.unpack_from("<I", second, size_field)[0]
        struct.pack_into("<IIHH", second, 0xd80 + size, 0x4000, 12, 0x3101, 0)
        struct.pack_into("<I", second, size_field, size + 12)
        receipt = {"schema": 1, "evidence": "checked-realigned-source-reconstruction",
                   "source_frame": "realigned", "base": 0x400000,
                   "source_image_sha256": contract["image_sha256"],
                   "image_sha256": hashlib.sha256(second).hexdigest(),
                   "entry_registers": 1, "entry_pop": 0,
                   "cleanup_actions": 2, "cleanup_calls": 2,
                   "incoming_reads": 0, "incoming_writes": 0,
                   "generated_handler": 0x4100, "generated_func_info": 0x4200,
                   "source_begin": 0x2000, "source_end": 0x2100,
                   "generated_begin": 0x4000, "generated_end": 0x4080}
        return runner.PE32(first), runner.PE32(second), receipt, contract

    def test_binds_both_generations_and_cleanup_context(self):
        first, second, receipt, contract = self.images()
        cleanup.validate_cleanup_installation(first, second, receipt, contract)
        for key, value in (("source_frame", "direct"), ("entry_registers", 0),
                           ("entry_pop", 4), ("cleanup_actions", 1),
                           ("cleanup_calls", 1), ("incoming_reads", 1),
                           ("incoming_writes", 1), ("source_begin", 0x2001),
                           ("source_end", 0x2080), ("generated_end", 0x4401),
                           ("source_image_sha256", "stale"), ("image_sha256", "stale")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                cleanup.validate_cleanup_installation(first, second, receipt | {key: value}, contract)
        changed = copy.deepcopy(contract)
        changed["generated_owner_end_rva"] -= 1
        with self.assertRaises(ValueError):
            cleanup.validate_cleanup_installation(first, second, receipt, changed)

    def test_rehashing_cannot_hide_wrong_trampolines_or_safeseh(self):
        first, second, receipt, contract = self.images()
        for offset in (0x400, 0x401, 0x600, 0x601, 0xc44, 0xd00, 0xd08,
                       0xf00, 0xf01, 0xf05, 0x1000):
            data = bytearray(second.data)
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                cleanup.validate_cleanup_installation(
                    first, runner.PE32(data),
                    receipt | {"image_sha256": hashlib.sha256(data).hexdigest()}, contract)

    def test_second_generation_routes_are_mandatory_at_both_bases(self):
        records = [{"image": label + suffix + ".exe", "generated": label != "original",
                    "runtime_base": base}
                   for label in runner.IMAGE_LABELS + cleanup.RELIFT_LABELS
                   for suffix, base in (("", runner.BASES[0]), ("-rebased", runner.BASES[1]))]
        runner.require_image_matrix(records, 3)
        for i in range(len(records)):
            with self.subTest(i=i), self.assertRaises(ValueError):
                runner.require_image_matrix(records[:i] + records[i + 1:], 3)
        for wrong in (records[:12], records[:-1] + records[:1]):
            with self.assertRaises(ValueError):
                runner.require_image_matrix(wrong, 3)

    def test_runtime_caller_must_reach_the_second_owner(self):
        _, second, receipt, contract = self.images()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "relifted.exe"
            path.write_bytes(second.data)
            for caller in (0x1030, 0x2030, 0x4030, 0x4080):
                result = {"exit_code": 0, "stdout":
                          f"neverd-registration-cxx: value=7 caller={0x400000 + caller:08x} "
                          "entry=00401000 chain=1 iterations=4 trace=213 caught=18\n"}
                with self.subTest(caller=caller), patch.object(runner, "run_image", return_value=result):
                    args = (path, True, True, 0x1080, contract, [], {}, 1, receipt)
                    if caller == 0x4030:
                        runner.observe(*args)
                    else:
                        with self.assertRaises(ValueError):
                            runner.observe(*args)


if __name__ == "__main__":
    unittest.main()
