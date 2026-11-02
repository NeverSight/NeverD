"""Native graph inventory, independent of IDA's earlier function list."""
from pathlib import Path
import tempfile
import unittest

from scripts import benchmark_binary_diff as bench
from scripts import benchmark_binary_diff_binexport as reader


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def scalar(field, value):
    return varint(field << 3) + varint(value)


def message(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value


def graph(identity, entries):
    return message(1, message(2, identity.encode())) + message(8, b"".join(
        message(1, scalar(1, address) + scalar(2, kind)) for address, kind in entries))


class InventoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / "native.BinExport"

    def read(self, data):
        self.path.write_bytes(data)
        return reader.inventory(self.path)

    def test_native_vertices_include_imports_and_graph_only_entries(self):
        value = self.read(graph("a" * 64, [(0, 0), (4096, 1), (8192, 2), (1 << 63, 3)]))
        self.assertEqual(value["entries"], [0, 4096, 8192, 1 << 63])
        self.assertEqual(value["vertex_types"]["IMPORTED"], 1)
        self.assertEqual(value["executable_id"], "a" * 64)

    def test_absent_type_defaults_to_normal_and_other_payloads_are_skipped(self):
        data = message(1, message(2, b"a" * 40)) + message(4, b"\xff" * 100000)
        data += message(8, message(1, scalar(1, 123)))
        self.assertEqual(self.read(data)["vertex_types"]["NORMAL"], 1)

    def test_malformed_boundaries_overflows_duplicates_and_types_are_refused(self):
        good = graph("a" * 64, [(4096, 0)])
        bad = [good[:-1], b"\x80", b"\x80" * 10, b"\xff" * 9 + b"\x02",
               b"\x00", scalar(1 << 29, 0), b"\x0b", b"\x0a\x7fshort",
               message(1, message(2, b"x")) + scalar(8, 0),
               good + message(8, b""), good + message(1, b""),
               graph("a" * 64, [(4096, 0), (4096, 0)]),
               graph("a" * 64, [(4096, 9)]),
               message(1, message(2, b"x")) + message(8, message(1, scalar(2, 0))),
               message(1, message(2, b"x")) + message(8, message(1, scalar(1, 1) + scalar(1, 2))),
               message(1, message(2, b"x") + message(2, b"y")) + message(8, b"")]
        for data in bad:
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                self.read(data)

    def test_binexport_inventory_admits_native_extra_nodes_without_deleting_errors(self):
        inputs, analyses = [], []
        for side in ("primary", "secondary"):
            source = self.root / (side + ".elf")
            source.write_bytes(side.encode())
            inputs.append(source)
            export = self.root / (side + ".BinExport")
            export.write_bytes(graph(bench.digest(source, "sha1"), [(4096, 0), (8192, 2)]))
            receipt = self.root / (side + ".json")
            bench.save(receipt, {"schema": "neverd.benchmark.binexport.v1", "complete": True,
                                "input_sha256": bench.digest(source), "export_sha256": bench.digest(export),
                                "image_base": "0x0", "function_entries": ["0x1000"]})
            analyses.append({"receipt": bench.artifact(receipt, self.root),
                             "export": bench.artifact(export, self.root)})
        spaces = bench.analysis_spaces({"format": "bindiff", "analyses": analyses}, self.path, self.root, inputs)
        case = {"primary": {"image_base": "0x0"}, "secondary": {"image_base": "0x0"},
                "truth": [{"primary": "0x1000", "secondary": "0x1000"}]}
        pairs, coverage = bench.normalize_pairs([(4096, 8192)], spaces, case)
        self.assertEqual(bench.score_partial({(4096, 4096)}, pairs)["wrong"], 1)
        self.assertEqual(coverage[0]["discovered_functions"], 2)
        self.assertEqual(coverage[0]["ida_function_entries"], 1)
        with self.assertRaisesRegex(ValueError, "binding"):
            bench.analysis_spaces({"format": "bindiff", "analyses": analyses}, self.path, self.root, inputs[::-1])


if __name__ == "__main__":
    unittest.main()
