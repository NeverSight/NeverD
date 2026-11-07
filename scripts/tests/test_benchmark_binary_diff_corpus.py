"""Independent compiled-source truth and stripped-input separation checks."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace

from scripts import benchmark_binary_diff as bench
from scripts import benchmark_binary_diff_corpus as corpus


class DwarfIntervalTests(unittest.TestCase):
    def test_nested_and_fragmented_ranges_quarantine_both_sides(self):
        self.assertEqual(corpus.overlapping_entries(
            [(10, 50), (20, 25), (30, 35), (60, 65), (80, 100)],
            [10, 20, 30, 50, 60, 80, 90, 100]), {10, 20, 30, 80, 90})
        self.assertEqual(corpus.overlapping_entries([(10, 20), (10, 20), (20, 30)],
                                                   [10, 20]), set())

    def test_converging_origin_is_not_a_cycle(self):
        def die(offset, edges):
            return SimpleNamespace(offset=offset, attributes=dict.fromkeys(edges),
                                   get_DIE_from_attribute=edges.__getitem__)
        end = die(3, {})
        left = die(1, {"DW_AT_specification": end})
        right = die(2, {"DW_AT_abstract_origin": end})
        root = die(0, {"DW_AT_specification": left, "DW_AT_abstract_origin": right})
        self.assertEqual(corpus.inherited_attribute(root, "DW_AT_decl_file"), (None, None))
        edges = {"DW_AT_specification": root}
        end.attributes = dict.fromkeys(edges)
        end.get_DIE_from_attribute = edges.__getitem__
        with self.assertRaisesRegex(ValueError, "cyclic"):
            corpus.inherited_attribute(root, "DW_AT_decl_file")

    def test_definition_unit_follows_origin_but_not_a_declaration_specification(self):
        root = Path("/source-corpus").resolve()

        def unit(name):
            top = SimpleNamespace(attributes={
                "DW_AT_name": SimpleNamespace(value=name.encode()),
                "DW_AT_comp_dir": SimpleNamespace(value=str(root).encode())})
            return SimpleNamespace(get_top_DIE=lambda: top, header=SimpleNamespace(version=4))

        def die(offset, cu, attributes=None, references=None):
            edges = references or {}
            values = {key: SimpleNamespace(value=value) for key, value in (attributes or {}).items()}
            values.update(dict.fromkeys(edges))
            return SimpleNamespace(offset=offset, cu=cu, attributes=values,
                                   get_DIE_from_attribute=edges.__getitem__)

        declaration = die(4, unit("other.c"), {"DW_AT_decl_file": 1, "DW_AT_name": b"helper"})
        definition = die(3, unit("definition.c"), references={"DW_AT_specification": declaration})
        intermediate = die(2, unit("<artificial>"), references={"DW_AT_abstract_origin": definition})
        concrete = die(1, unit("<artificial>"), references={"DW_AT_abstract_origin": intermediate})
        dwarf = SimpleNamespace(line_program_for_CU=lambda cu: {
            "file_entry": [SimpleNamespace(name=b"declarations.h", dir_index=0)]})
        expected = ("definition.c", "declarations.h", "helper")
        self.assertEqual(corpus.source_identity(concrete, dwarf, root), expected)
        self.assertEqual(corpus.source_identity(definition, dwarf, root), expected)
        intermediate.get_DIE_from_attribute = lambda key: concrete
        with self.assertRaisesRegex(ValueError, "cyclic DWARF abstract origin"):
            corpus.source_identity(concrete, dwarf, root)


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("gcc") and
                     shutil.which("strip") and importlib.util.find_spec("elftools"),
                     "Linux GCC/binutils and optional pyelftools are required")
class SourceCorpusTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sources = []
        self.binaries = []
        for side, bound in (("before", 17), ("after", 19)):
            source = self.root / side
            source.mkdir()
            for directory, value in (("one", 3), ("two", 5)):
                unit = source / directory / "worker.c"
                unit.parent.mkdir()
                unit.write_text(f"static __attribute__((noinline)) int duplicate(int x) {{ return x * {value}; }}\n"
                                f"int {directory}(int x) {{ return duplicate(x) + {bound}; }}\n")
            (source / "main.c").write_text("int one(int); int two(int);\n"
                "int original(int x) { return x + 41; }\n"
                "int alias(int) __attribute__((alias(\"original\")));\n"
                "int main(int argc, char **argv) { return one(argc) + two(argc) + alias(argc); }\n")
            binary = self.root / (side + ".elf")
            subprocess.run([shutil.which("gcc"), "-O1", "-g", "-gdwarf-4", "-fno-inline", "-fPIE", "-pie",
                            str(source / "one/worker.c"), str(source / "two/worker.c"),
                            str(source / "main.c"), "-o", str(binary)], check=True, capture_output=True)
            self.sources.append(source)
            self.binaries.append(binary)

    def test_source_directories_disambiguate_duplicate_names_and_aliases_abstain(self):
        functions, inventory, _ = corpus.inspect_elf(self.binaries[0], self.sources[0])
        self.assertIn(("one/worker.c", "one/worker.c", "duplicate"), functions)
        self.assertIn(("two/worker.c", "two/worker.c", "duplicate"), functions)
        self.assertNotIn(("main.c", "main.c", "original"), functions)
        self.assertGreater(inventory["excluded_reasons"]["aliased-native-entry"], 0)
        self.assertEqual(inventory["image_base"], "0x0")

    def test_lto_uses_original_source_units_and_keeps_duplicate_names_distinct(self):
        source = self.sources[0]
        linked = self.root / "lto.elf"
        subprocess.run([shutil.which("gcc"), "-O1", "-g", "-gdwarf-4", "-fno-inline",
                        "-flto=2", "-fPIE", "-pie", str(source / "one/worker.c"),
                        str(source / "two/worker.c"), str(source / "main.c"),
                        "-o", str(linked)], check=True, capture_output=True)
        expected, _, _ = corpus.inspect_elf(self.binaries[0], source)
        actual, inventory, _ = corpus.inspect_elf(linked, source)
        self.assertEqual(set(actual), set(expected))
        self.assertIn(("one/worker.c", "one/worker.c", "duplicate"), actual)
        self.assertIn(("two/worker.c", "two/worker.c", "duplicate"), actual)
        self.assertGreater(inventory["excluded_reasons"]["aliased-native-entry"], 0)

    def test_real_stripping_preserves_runtime_data_and_excludes_truth(self):
        functions, _, sections = corpus.inspect_elf(self.binaries[0], self.sources[0])
        stripped = self.root / "stripped.elf"
        subprocess.run([shutil.which("strip"), "--strip-all", "-o", str(stripped), str(self.binaries[0])], check=True)
        corpus.verify_stripped(sections, stripped)
        with self.assertRaisesRegex(ValueError, "symbols or debug"):
            corpus.verify_stripped(sections, self.binaries[0])
        # An actual text byte edit must fail the strip invariant, not just an
        # altered ELF header or missing section name.
        from elftools.elf.elffile import ELFFile
        with stripped.open("rb") as stream:
            offset = ELFFile(stream).get_section_by_name(".text")["sh_offset"]
        data = bytearray(stripped.read_bytes())
        data[offset] ^= 1
        stripped.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "allocated code/data"):
            corpus.verify_stripped(sections, stripped)

    def test_load_map_changes_and_nonzero_first_offset_are_not_admitted(self):
        from elftools.elf.elffile import ELFFile
        _, _, layout = corpus.inspect_elf(self.binaries[0], self.sources[0])
        stripped = self.root / "stripped.elf"
        subprocess.run([shutil.which("strip"), "--strip-all", "-o", str(stripped), str(self.binaries[0])], check=True)
        with stripped.open("rb") as stream:
            elf = ELFFile(stream)
            segments = list(elf.iter_segments())
            first = next(i for i, s in enumerate(segments) if s["p_type"] == "PT_LOAD")
            offset = elf["e_phoff"] + first * elf["e_phentsize"]
            flags = segments[first]["p_flags"]
        data = bytearray(stripped.read_bytes())
        struct.pack_into("<I", data, offset + 4, flags ^ 1)
        stripped.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "load mappings"):
            corpus.verify_stripped(layout, stripped)
        data = bytearray(self.binaries[0].read_bytes())
        struct.pack_into("<Q", data, offset + 8, 0x1000)
        self.binaries[0].write_bytes(data)
        with self.assertRaisesRegex(ValueError, "offset-zero"):
            corpus.inspect_elf(self.binaries[0], self.sources[0])

    def test_interior_dwarf_body_and_nonexecutable_body_are_excluded(self):
        from elftools.elf.elffile import ELFFile
        with self.binaries[0].open("rb") as stream:
            elf = ELFFile(stream)
            dwarf = elf.get_dwarf_info()
            entries = sorted((die.attributes["DW_AT_low_pc"].value, die)
                for unit in dwarf.iter_CUs() for die in unit.iter_DIEs()
                if die.tag == "DW_TAG_subprogram" and "DW_AT_low_pc" in die.attributes)
            (start, first), (second, interior) = entries[:2]
            high = first.attributes["DW_AT_high_pc"]
            self.assertEqual(high.form, "DW_FORM_data8")
            patch = elf.get_section_by_name(".debug_info")["sh_offset"] + high.offset
            names = [corpus.source_identity(die, dwarf, self.sources[0]) for die in (first, interior)]
            text_index = next(i for i, s in enumerate(elf.iter_sections()) if s.name == ".text")
            header_offset = elf["e_shoff"] + text_index * elf["e_shentsize"]
            flags = elf.get_section(text_index)["sh_flags"]
        original = self.binaries[0].read_bytes()
        data = bytearray(original)
        struct.pack_into("<Q", data, patch, second - start + 1)
        self.binaries[0].write_bytes(data)
        functions, inventory, _ = corpus.inspect_elf(self.binaries[0], self.sources[0])
        for name in names:
            self.assertNotIn(name, functions)
        self.assertGreaterEqual(inventory["excluded_reasons"]["overlapping-native-body"], 2)
        data = bytearray(original)
        struct.pack_into("<Q", data, header_offset + 8, flags & ~corpus.SHF_EXECINSTR)
        self.binaries[0].write_bytes(data)
        functions, inventory, _ = corpus.inspect_elf(self.binaries[0], self.sources[0])
        self.assertFalse(functions)
        self.assertGreater(inventory["excluded_reasons"]["nonexecutable-body"], 0)

    def test_prepare_binds_sources_builds_and_both_input_directions(self):
        evidence = self.root / "license-build.txt"
        evidence.write_text("Owned fixture, built with GCC -O1 -gdwarf-4 -fPIE -pie")
        case = {"id": "owned-pie", "category": "regression"}
        for side, binary, source in zip(("primary", "secondary"), self.binaries, self.sources):
            case[side] = {"project": "owned", "revision": side, "source_root": str(source),
                          "binary": bench.artifact(binary, self.root), "license": "owned",
                          "source_url": "owned fixture", "provenance": [bench.artifact(evidence, self.root)]}
        spec = self.root / "spec.json"
        spec.write_text(json.dumps({"schema": corpus.SPEC_SCHEMA, "cases": [case]}))
        output = self.root / "prepared"
        args = argparse.Namespace(spec=spec, strip=Path(shutil.which("strip")), output_dir=output)
        self.assertEqual(corpus.prepare(args), 0)
        manifest = bench.load(output / "manifest.json")
        prepared = bench.corpus_cases(manifest)[0]
        self.assertFalse(prepared["truth_complete"])
        self.assertEqual(len(prepared["truth"]), 5)
        for row in prepared["truth"]:
            self.assertIsNotNone(row["primary"])
            self.assertIsNotNone(row["secondary"])
        for item in prepared["truth_artifacts"]:
            bench.checked_artifact(item, output)
        self.assertEqual({path.name for path in (output / "owned-pie/inputs").iterdir()},
                         {"primary.elf", "secondary.elf"})
        with self.assertRaises(FileExistsError):
            corpus.prepare(args)
        evidence.write_text("stale")
        args.output_dir = self.root / "stale-output"
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            corpus.prepare(args)
        self.assertFalse(args.output_dir.exists())


if __name__ == "__main__":
    unittest.main()
