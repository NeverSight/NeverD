"""Fixture format and result-integrity tests for the inventory benchmark."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

from scripts import benchmark_mobile_inventory as bench


def uint(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def leb(data, offset):
    value, shift = 0, 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
        shift += 7


def dex_strings(data):
    count, table = struct.unpack_from("<II", data, 56)
    result = []
    for index in range(count):
        length, start = leb(data, uint(data, table + index * 4))
        end = data.index(0, start)
        value = data[start:end].decode("ascii")
        if len(value) != length:
            raise AssertionError("invalid string length")
        result.append(value)
    return result


class FixtureTests(unittest.TestCase):
    def test_dex_checksums_tables_and_map(self):
        data = bench.make_dex(17, dex_index=2, extra_string_bytes=8193)
        self.assertEqual(data[:8], b"dex\n035\0")
        self.assertEqual(uint(data, 32), len(data))
        self.assertEqual(uint(data, 36), 112)
        self.assertEqual(uint(data, 40), 0x12345678)
        self.assertEqual(uint(data, 8), zlib.adler32(data[12:]) & 0xFFFFFFFF)
        self.assertEqual(data[12:32], hashlib.sha1(data[32:]).digest())
        self.assertEqual(uint(data, 104) + uint(data, 108), len(data))
        strings = dex_strings(data)
        self.assertEqual(strings, sorted(set(strings)))
        type_count, type_off = struct.unpack_from("<II", data, 64)
        types = [strings[uint(data, type_off + i * 4)] for i in range(type_count)]
        self.assertEqual(types, sorted(set(types)))
        class_count, class_off = struct.unpack_from("<II", data, 96)
        descriptors = []
        for index in range(class_count):
            fields = struct.unpack_from("<8I", data, class_off + index * 32)
            descriptors.append(types[fields[0]])
            self.assertEqual(types[fields[2]], "Ljava/lang/Object;")
            self.assertEqual(fields[3:], (0, 0xFFFFFFFF, 0, 0, 0))
        self.assertEqual(descriptors, bench.class_descriptors(17, 2))
        map_off = uint(data, 52)
        entries = [struct.unpack_from("<HHII", data, map_off + 4 + i * 12)
                   for i in range(uint(data, map_off))]
        self.assertEqual([entry[3] for entry in entries], sorted(entry[3] for entry in entries))
        self.assertEqual({entry[0] for entry in entries}, {0, 1, 2, 6, 0x1000, 0x2002})
        self.assertEqual(entries[-1], (0x1000, 0, 1, map_off))
        self.assertEqual(map_off + 4 + 12 * len(entries), len(data))
        self.assertEqual(data, bench.make_dex(17, dex_index=2, extra_string_bytes=8193))

    def test_optional_code_is_mapped_and_referenced(self):
        data = bench.make_dex(3, code_units=5)
        self.assertEqual(uint(data, 88), 3)
        class_off, map_off = uint(data, 100), uint(data, 52)
        entries = [struct.unpack_from("<HHII", data, map_off + 4 + i * 12)
                   for i in range(uint(data, map_off))]
        self.assertIn((0x2001, 0, 3), [entry[:3] for entry in entries])
        self.assertIn((0x2000, 0, 3), [entry[:3] for entry in entries])
        for index in range(3):
            offset = uint(data, class_off + index * 32 + 24)
            self.assertEqual(data[offset:offset + 4], b"\0\0\x01\0")
            method_index, offset = leb(data, offset + 4)
            access, offset = leb(data, offset)
            code_off, _ = leb(data, offset)
            self.assertEqual(method_index, index)
            self.assertEqual(access, 9)
            self.assertEqual(code_off % 4, 0)
            self.assertEqual(struct.unpack_from("<HHHHII", data, code_off), (0, 0, 0, 0, 0, 5))
            self.assertEqual(data[code_off + 16:code_off + 26], bytes(8) + b"\x0e\0")

    def test_workloads_hashes_multidex_and_compression(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "first"
            manifest = bench.generate_workloads(root, class_count=5, dex_count=2,
                                                resource_bytes=129, class_prefix="Lbench/")
            self.assertEqual(len(manifest["workloads"]), 5)
            for item in manifest["workloads"]:
                path = root / item["input"]
                self.assertEqual(bench.sha256_file(path), item["sha256"])
                expected = (root / item["expected"]).read_text().splitlines()
                self.assertEqual(len(expected), 6 if item["name"].startswith("multidex") else 3)
                self.assertEqual(len(expected), len(set(expected)))
                self.assertTrue(all(value.startswith("Lbench/") for value in expected))
                if path.suffix == ".apk":
                    with zipfile.ZipFile(path) as archive:
                        self.assertIsNone(archive.testzip())
                        self.assertEqual(len(archive.read("assets/unrelated.bin")), 129)
                        compression = zipfile.ZIP_STORED if "stored" in item["name"] else zipfile.ZIP_DEFLATED
                        self.assertTrue(all(info.compress_type == compression for info in archive.infolist()))
                        self.assertEqual("classes2.dex" in archive.namelist(), item["name"].startswith("multidex"))
            first_hashes = [item["sha256"] for item in manifest["workloads"]]
            repeated = bench.generate_workloads(Path(temporary) / "second", class_count=5, dex_count=2,
                                                resource_bytes=129, class_prefix="Lbench/")
            self.assertEqual(first_hashes, [item["sha256"] for item in repeated["workloads"]])

    def test_generate_only_needs_no_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "fixtures"
            self.assertEqual(bench.main(["--output-dir", str(root), "--class-count", "2",
                                         "--generate-only", "--neverd", "/missing/neverd"]), 0)
            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["parameters"]["class_count_per_dex"], 2)
            self.assertFalse((root / "results.json").exists())

    def test_explicit_workload_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "fixtures"
            self.assertEqual(bench.main(["--output-dir", str(root), "--class-count", "2", "--generate-only",
                                         "--workload", "single-stored", "--workload", "multidex-deflated"]), 0)
            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["selected_workloads"], ["single-stored", "multidex-deflated"])
            self.assertEqual([item["name"] for item in manifest["workloads"]], manifest["selected_workloads"])
            self.assertEqual({path.name for path in root.iterdir()}, {
                "manifest.json", "single-stored.apk", "single-stored.expected.txt",
                "multidex-deflated.apk", "multidex-deflated.expected.txt"})

    def test_existing_output_directory_preserves_previous_evidence(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(io.StringIO()):
            root = Path(temporary) / "fixtures"
            bench.generate_workloads(root, class_count=1, dex_count=2)
            (root / "results.json").write_text('{"previous_run": true}\n')
            previous = {path.name: path.read_bytes() for path in root.iterdir()}
            for options in (["--generate-only"], ["--neverd", sys.executable]):
                status = bench.main(["--output-dir", str(root), "--class-count", "3", *options])
                self.assertEqual(status, 1)
                self.assertEqual(previous, {path.name: path.read_bytes() for path in root.iterdir()})
            empty = Path(temporary) / "empty"
            empty.mkdir()
            with self.assertRaises(FileExistsError):
                bench.generate_workloads(empty, class_count=1, dex_count=2)

    def test_invalid_sizes_are_rejected(self):
        for count in (0, -1, 65534):
            with self.assertRaises(ValueError):
                bench.make_dex(count)
        for options in ({"extra_string_bytes": -1}, {"code_units": -1}):
            with self.assertRaises(ValueError):
                bench.make_dex(1, **options)


class RunnerTests(unittest.TestCase):
    def test_exact_inventory_with_order_independence(self):
        expected = ["Lbench/A;", "Lbench/B;"]
        bench.validate_output(b"Lbench/B;\nLbench/A;\n", expected)
        for output in (b"Lbench/A;\n", b"Lbench/A;\nLbench/A;\n", b"banner\nLbench/A;\nLbench/B;\n",
                       b"Lbench/A;\nLbench/B;\n\n", b"\xff"):
            with self.subTest(output=output), self.assertRaises(RuntimeError):
                bench.validate_output(output, expected)

    def test_fresh_process_success_and_failure(self):
        command = [sys.executable, "-c", "print('Lbench/A;')"]
        result = bench.run_once(command, ["Lbench/A;"], timeout=5)
        self.assertGreater(result["wall_seconds"], 0)
        self.assertEqual(result["stdout_sha256"], hashlib.sha256(b"Lbench/A;\n").hexdigest())
        with self.assertRaisesRegex(RuntimeError, "exited 7"):
            bench.run_once([sys.executable, "-c", "raise SystemExit(7)"], [], timeout=5)
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            bench.run_once(command, [], timeout=5)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            bench.run_once([sys.executable, "-c", "import time; time.sleep(10)"], [], timeout=0.1)

    def test_benchmark_rejects_changed_fixture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "fixtures"
            manifest = bench.generate_workloads(root, class_count=1, dex_count=2)
            (root / "classes.dex").write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "fixture changed"):
                bench.benchmark(root, manifest, {"test": [sys.executable, "-c", "pass"]},
                                repetitions=1, warmups=0, timeout=5, measure_rss=False)

    def test_apk_selection_applies_identically_to_both_commands(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "fixtures"
            manifest = bench.generate_workloads(root, class_count=1, dex_count=2, class_prefix="Lmissing/",
                                                selected_workloads=["single-stored", "multidex-deflated"])
            command = [sys.executable, "-c", "import sys; assert sys.argv[1].endswith('.apk')", "{input}"]
            report = bench.benchmark(root, manifest, {"primary": command, "peer": command},
                                     repetitions=2, warmups=0, timeout=5, measure_rss=False)
            self.assertEqual(report["selected_workloads"], ["single-stored", "multidex-deflated"])
            self.assertEqual({(row["workload"], row["command"]) for row in report["results"]}, {
                (workload, label) for workload in report["selected_workloads"] for label in ("primary", "peer")})
            self.assertTrue(all(len(row["samples"]) == 2 for row in report["results"]))

    def test_failed_cli_benchmark_does_not_publish_timings(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(io.StringIO()):
            root = Path(temporary) / "fixtures"
            # Python cannot interpret the mobile command as a source file.
            status = bench.main(["--output-dir", str(root), "--class-count", "1", "--dex-count", "2",
                                 "--neverd", sys.executable, "--repetitions", "1", "--warmups", "0"])
            self.assertEqual(status, 1)
            self.assertTrue((root / "manifest.json").exists())
            self.assertFalse((root / "results.json").exists())


if __name__ == "__main__":
    unittest.main()
