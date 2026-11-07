"""RDBSS compatibility keeps the official enum values and compiler ABI."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import microsoft_rdbss_types as rdbss


CREATE = "typedef enum { Create = 1 } RX_CONTEXT_CREATE_FLAGS;\n"
LOWIO = "typedef enum { LowIo = 2 } RX_CONTEXT_LOWIO_FLAGS;\n"
BROKEN = CREATE + LOWIO.replace("RX_CONTEXT_LOWIO_FLAGS", "RX_CONTEXT_CREATE_FLAGS")


class MicrosoftRdbssTypes(unittest.TestCase):
    def test_only_the_matching_second_typedef_is_repaired(self):
        self.assertEqual(rdbss.bridge_enum_typedefs(BROKEN, CREATE + LOWIO), CREATE + LOWIO)
        self.assertEqual(rdbss.bridge_enum_typedefs(CREATE + LOWIO, CREATE + LOWIO),
                         CREATE + LOWIO)
        self.assertEqual(rdbss.bridge_enum_typedefs(BROKEN + "int other;\n", LOWIO + CREATE),
                         CREATE + LOWIO + "int other;\n")

    def test_changed_values_missing_and_ambiguous_enums_are_refused(self):
        for c_source, cpp_source in (
                (BROKEN.replace("LowIo = 2", "LowIo = 3"), CREATE + LOWIO),
                (BROKEN, CREATE), (BROKEN + CREATE, CREATE + LOWIO),
                (BROKEN, CREATE + LOWIO + LOWIO),
                (CREATE + CREATE, CREATE + LOWIO),
                (CREATE.replace("Create = 1", "LowIo = 2") + LOWIO,
                 CREATE + LOWIO)):
            with self.subTest(c_source=c_source, cpp_source=cpp_source):
                with self.assertRaises(ValueError):
                    rdbss.bridge_enum_typedefs(c_source, cpp_source)

    def test_parameter_or_calling_convention_changes_refuse_publication(self):
        names = list(rdbss.NO_ARGUMENTS) + ["DuplicateTransportAddress", "_RxLog"]
        aliases = [f"neverd_rdbss_contract_{index}" for index in range(len(names))]
        ir = "".join(f"@neverd_ucrt_abi_{i} = global ptr @{alias}, align 8\n"
                     f"declare i32 @{alias}()\n" for i, alias in enumerate(aliases))
        for changed in (ir, ir.replace("declare i32", "declare x86_stdcallcc i32", 1),
                        ir.replace(f"@{aliases[0]}()", f"@{aliases[0]}(i64)", 1)):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                original = directory / "original.cpp"
                original.write_text("/* original fixture */\n")
                source = directory / "provider.h"
                source.write_text("/* C fixture */\n")
                proof = {"source": str(original), "arguments": ["clang", "-x", "c++"],
                         "commands": []}

                def run(command, log, output):
                    text = ir if "c++" in command else changed
                    output.write_text(text)
                    return text

                if changed == ir:
                    rdbss.verify_abis(proof, source, ["clang"], directory, names, run)
                    self.assertEqual(proof["verified_functions"], sorted(names))
                else:
                    with self.assertRaisesRegex(ValueError, "compiler ABI differs"):
                        rdbss.verify_abis(proof, source, ["clang"], directory, names, run)
                    self.assertNotIn("verified_functions", proof)
                self.assertEqual(original.read_text(), "/* original fixture */\n")


if __name__ == "__main__":
    unittest.main()
