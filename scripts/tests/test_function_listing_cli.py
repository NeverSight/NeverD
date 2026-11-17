"""Check function-list JSON framing and identities using data-only ELF fixtures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def elf_functions(names: list[str]) -> bytes:
    """Build an ELF with explicit function symbols; never execute its contents."""
    text = b"\xc3" * max(len(names), 1)
    strings = bytearray(b"\0")
    symbols = bytearray(24)
    for index, name in enumerate(names):
        offset = len(strings)
        strings.extend(name.encode("utf-8") + b"\0")
        symbols.extend(struct.pack("<IBBHQQ", offset, 0x12, 0, 1,
                                   0x401000 + index, 1))
    sections = b"\0.text\0.strtab\0.symtab\0.shstrtab\0"
    image = bytearray(4096)
    image.extend(text)
    string_offset = len(image)
    image.extend(strings)
    symbol_offset = (len(image) + 7) & ~7
    image.extend(bytes(symbol_offset - len(image)))
    image.extend(symbols)
    section_names_offset = len(image)
    image.extend(sections)
    section_offset = (len(image) + 7) & ~7
    image.extend(bytes(section_offset - len(image)))
    image.extend(bytes(64))
    for row in [
        (1, 1, 6, 0x401000, 4096, len(text), 0, 0, 1, 0),
        (7, 3, 0, 0, string_offset, len(strings), 0, 0, 1, 0),
        (15, 2, 0, 0, symbol_offset, len(symbols), 2, 1, 8, 24),
        (23, 3, 0, 0, section_names_offset, len(sections), 0, 0, 1, 0),
    ]:
        image.extend(struct.pack("<IIQQQQIIQQ", *row))
    ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    image[:64] = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, 0,
                             64, section_offset, 0, 64, 56, 1, 64, 5, 4)
    image[64:120] = struct.pack("<IIQQQQQQ", 1, 5, 4096, 0x401000,
                                0x401000, len(text), len(text), 4096)
    return bytes(image)


class FunctionListingCLI(unittest.TestCase):
    executable: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        if cls.executable is None:
            raise unittest.SkipTest("a CLI path is required")

    def check_names(self, names: list[str]) -> None:
        with tempfile.TemporaryDirectory(prefix="neverd-funcs-json-") as directory:
            fixture = Path(directory) / "functions.elf"
            fixture.write_bytes(elf_functions(names))
            result = subprocess.run(
                [str(self.executable), "funcs", str(fixture), "--json"],
                capture_output=True, timeout=30,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.endswith(b"\n"))
        functions = json.loads(result.stdout)
        self.assertEqual(len(functions), len(names))
        for index, (function, name) in enumerate(zip(functions, names)):
            self.assertEqual(function["name"], name)
            self.assertEqual(function["linkage_name"], name)
            self.assertEqual(function["addr"], f"0x{0x401000 + index:X}")
            self.assertEqual(function["size"], 1)
            self.assertIn("display_name", function)
            self.assertIn("library_annotations", function)

    def test_empty_array(self) -> None:
        self.check_names([])

    def test_single_function(self) -> None:
        self.check_names(["single"])

    def test_escaping_and_order_across_many_rows(self) -> None:
        self.check_names([f'function_{index}_"\\\t名' for index in range(2048)])

    def test_missing_input_is_not_an_empty_success(self) -> None:
        with tempfile.TemporaryDirectory(prefix="neverd-funcs-missing-") as directory:
            result = subprocess.run(
                [str(self.executable), "funcs", str(Path(directory) / "missing"),
                 "--json"], capture_output=True, timeout=30,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    FunctionListingCLI.executable = options.neverd.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
