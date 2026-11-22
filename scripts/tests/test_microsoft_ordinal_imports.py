"""Exercise Microsoft ordinal import identity and malformed archive rejection."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import microsoft_ordinal_imports as ordinal


def member(data):
    header = (b"member/".ljust(16) + b"0".ljust(12) + b"0".ljust(6) +
              b"0".ljust(6) + b"100644".ljust(8) + str(len(data)).encode().ljust(10) + b"`\n")
    return header + data + (b"\n" if len(data) & 1 else b"")


def short_import(symbol, module, number, *, machine=0x14c, flags=0):
    strings = symbol + b"\0" + module + b"\0"
    return struct.pack("<HHHHIIHH", 0, 0xffff, 0, machine, 0, len(strings), number, flags) + strings


class MicrosoftOrdinalImportsTests(unittest.TestCase):
    def test_only_code_at_the_witnessed_module_machine_and_ordinal_contributes(self):
        correct = short_import(b"_SourceApi@4", b"EXAMPLE.dll", 8)
        inputs = [b"regular archive member", correct,
                  short_import(b"_OtherOrdinal", b"example.dll", 9),
                  short_import(b"_OtherDll", b"another.dll", 8),
                  short_import(b"_Data", b"example.dll", 8, flags=1),
                  short_import(b"_NameLookup", b"example.dll", 8, flags=4),
                  short_import(b"OtherTarget", b"example.dll", 8, machine=0xaa64)]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.lib"
            path.write_bytes(b"!<arch>\n" + b"".join(map(member, inputs)))
            self.assertEqual(ordinal.symbols(path, "x86", "example.dll", {"PublicApi": 8}),
                             {"PublicApi": {"_SourceApi@4"}})
            with self.assertRaisesRegex(ValueError, "lacks witnessed SDK code ordinals"):
                ordinal.symbols(path, "x86", "example.dll", {"Absent": 7})
            with self.assertRaisesRegex(ValueError, "ambiguous witnessed export ordinal"):
                ordinal.symbols(path, "x86", "example.dll", {"One": 8, "Two": 8})

    def test_truncated_payload_and_invalid_import_strings_are_rejected(self):
        valid = short_import(b"Api", b"example.dll", 1)
        archive = b"!<arch>\n" + member(valid)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.lib"
            for raw, error in ((archive[1:], "archive signature"),
                               (archive[:-2], "truncated COFF archive member"),
                               (b"!<arch>\n" + member(valid[:-1]), "string table size"),
                               (b"!<arch>\n" + member(valid[:-1] + b"X"), "symbol or DLL name")):
                with self.subTest(error=error):
                    path.write_bytes(raw)
                    with self.assertRaisesRegex(ValueError, error):
                        ordinal.symbols(path, "x86", "example.dll", {"Api": 1})


if __name__ == "__main__":
    unittest.main()
