"""Synthetic Mach-O metadata plus iOS package/native process contracts."""

from __future__ import annotations

import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "neverd"))

from mobile.common import Limits, MobileError
from mobile.ios import decompile_ios
from mobile.macho import MachO, objc_header, objc_metadata, select_slice, swift_metadata


def source_report(source, *, count=1, limitations=()):
    return json.dumps({"schema_version": 1, "status": "success",
                       "native_source": source, "native_function_count": count,
                       "methods": [], "limitations": list(limitations), "objc_metadata": {"classes": [],
                           "status": "section-absent", "limitations": []}})


def native_fixture(*, is64=True, relative=False, indirect=False, encrypted=False, chained=False):
    """Build original fixture bytes with a class, metaclass, methods and Swift type."""
    cpu = 0x0100000C if is64 else 12
    base = 0x100000000 if is64 else 0x1000
    pointer_size = 8 if is64 else 4
    data = bytearray(0x1000)

    def ptr(offset, value):
        struct.pack_into("<Q" if is64 else "<I", data, offset, value)

    def string(offset, value):
        raw = value.encode() + b"\0"
        data[offset:offset + len(raw)] = raw

    def command_segment():
        command = 0x19 if is64 else 1
        size = (72 + 80 * 2) if is64 else (56 + 68 * 2)
        fmt = "<II16s4Q4I" if is64 else "<II16s8I"
        header = struct.pack(fmt, command, size, b"__DATA", base, len(data), 0, len(data), 7, 3, 2, 0)
        sections = b""
        for name, offset, length in ((b"__objc_classlist", 0x380, pointer_size), (b"__swift5_types", 0x390, 4)):
            fmt = "<16s16s2Q8I" if is64 else "<16s16s9I"
            values = [name, b"__DATA", base + offset, length, offset, 2, 0, 0, 0, 0, 0]
            if is64:
                values.append(0)
            sections += struct.pack(fmt, *values)
        return header + sections

    commands = [command_segment(), struct.pack("<6I", 2, 24, 0x820, 1, 0x850, 64)]
    if encrypted:
        commands.append(struct.pack("<6I", 0x2C, 24, 0x900, 16, 1, 0))
    if chained:
        commands.append(struct.pack("<4I", 0x80000034, 16, 0x940, 28))
    header = struct.pack("<7I", 0xFEEDFACF if is64 else 0xFEEDFACE, cpu, 0, 2, len(commands), sum(map(len, commands)), 0)
    if is64:
        header += bytes(4)
    data[:len(header)] = header
    cursor = len(header)
    for command in commands:
        data[cursor:cursor + len(command)] = command
        cursor += len(command)
    ptr(0x380, base + 0x400)
    ptr(0x400, base + 0x450)
    ptr(0x400 + pointer_size * 4, base + 0x500)
    ptr(0x450, base + 0x450)
    ptr(0x450 + pointer_size * 4, base + 0x550)
    struct.pack_into("<I", data, 0x500, 2)
    name_offset = 24 if is64 else 16
    ptr(0x500 + name_offset, base + 0x700)
    ptr(0x500 + name_offset + pointer_size, base + 0x600)
    ptr(0x550 + name_offset, base + 0x700)
    ptr(0x550 + name_offset + pointer_size, base + 0x650)
    string(0x700, "Calculator")
    string(0x710, "add:to:")
    string(0x730, "q32@0:8q16q24")
    string(0x750, "answer")
    string(0x760, "q16@0:8")
    if relative:
        flags = 0x80000000 | 12 | (0 if indirect else 0x40000000)
        struct.pack_into("<II", data, 0x600, flags, 1)
        name = 0x6C0 if indirect else 0x710
        if indirect:
            ptr(0x6C0, base + 0x710)
        struct.pack_into("<iii", data, 0x608, name - 0x608, 0x730 - 0x60C, 0x900 - 0x610)
    else:
        struct.pack_into("<II", data, 0x600, pointer_size * 3, 1)
        for index, target in enumerate((0x710, 0x730, 0x900)):
            ptr(0x608 + index * pointer_size, base + target)
    struct.pack_into("<II", data, 0x650, pointer_size * 3, 1)
    for index, target in enumerate((0x750, 0x760, 0x910)):
        ptr(0x658 + index * pointer_size, base + target)
    struct.pack_into("<i", data, 0x390, 0x780 - 0x390)
    struct.pack_into("<III", data, 0x780, 17, 0, 0x7A0 - 0x788)
    string(0x7A0, "Record")
    struct.pack_into("<IBBHQ" if is64 else "<IBBHI", data, 0x820, 1, 0xF, 1, 0, base + 0x900)
    string(0x851, "_$s4Demo6RecordVMn")
    return bytes(data)


def universal_fixture(*, fat64=False, overlap=False):
    slices = [native_fixture(is64=False), native_fixture()]
    fmt = ">IIQQII" if fat64 else ">5I"
    header = struct.pack(">II", 0xCAFEBABF if fat64 else 0xCAFEBABE, 2)
    offsets = [0x1000, 0x1000 if overlap else 0x2000]
    for cpu, offset, data in zip((12, 0x0100000C), offsets, slices):
        values = [cpu, 0, offset, len(data), 12]
        if fat64:
            values.append(0)
        header += struct.pack(fmt, *values)
    output = bytearray(0x3000)
    output[:len(header)] = header
    for offset, data in zip(offsets, slices):
        output[offset:offset + len(data)] = data
    return bytes(output)


class MachOTests(unittest.TestCase):
    def test_class_instance_and_class_method_signatures(self):
        for is64 in (True, False):
            with self.subTest(is64=is64):
                image, available = select_slice(native_fixture(is64=is64))
                metadata = objc_metadata(image)
                self.assertEqual(metadata["status"], "recovered")
                self.assertEqual(metadata["classes"][0]["name"], "Calculator")
                methods = metadata["classes"][0]["methods"]
                self.assertEqual([item["selector"] for item in methods], ["add:to:", "answer"])
                self.assertTrue(methods[1]["class_method"])
                header = objc_header(metadata)
                self.assertIn("- (long long)add:(long long)arg0 to:(long long)arg1;", header)
                self.assertIn("+ (long long)answer;", header)
                self.assertEqual(available, ["arm64" if is64 else "arm"])

    def test_relative_direct_and_indirect_selector_methods(self):
        for indirect in (False, True):
            image = MachO(native_fixture(relative=True, indirect=indirect))
            metadata = objc_metadata(image)
            self.assertEqual(metadata["classes"][0]["methods"][0]["selector"], "add:to:")

    def test_legacy_long_encoding_keeps_32_bit_declarations_on_lp64(self):
        metadata = objc_metadata(MachO(native_fixture()))
        metadata["classes"][0]["methods"][0]["type_encoding"] = "l32@0:8l16L24"
        self.assertIn("- (int)add:(int)arg0 to:(unsigned int)arg1;", objc_header(metadata))

    def test_swift_nominal_type_and_mangled_symbols(self):
        metadata = swift_metadata(MachO(native_fixture()))
        self.assertEqual(metadata["types"][0]["name"], "Record")
        self.assertEqual(metadata["types"][0]["kind"], "struct")
        self.assertEqual(metadata["symbols"][0]["name"], "_$s4Demo6RecordVMn")

    def test_swift_indirect_type_descriptor(self):
        data = bytearray(native_fixture())
        struct.pack_into("<i", data, 0x390, (0x7D0 - 0x390) | 1)
        struct.pack_into("<Q", data, 0x7D0, 0x100000780)
        metadata = swift_metadata(MachO(bytes(data)))
        self.assertEqual(metadata["status"], "recovered")
        self.assertEqual(metadata["types"][0]["name"], "Record")

    def test_swift_indirect_chained_reference_is_explicitly_partial(self):
        data = bytearray(native_fixture(chained=True))
        struct.pack_into("<i", data, 0x390, (0x7D0 - 0x390) | 1)
        metadata = swift_metadata(MachO(bytes(data)))
        self.assertEqual(metadata["status"], "partial")
        self.assertIn("resolved absolute pointers", " ".join(metadata["limitations"]))

    def test_swift_objc_reference_kind_is_explicitly_partial(self):
        data = bytearray(native_fixture())
        struct.pack_into("<i", data, 0x390, (0x7D0 - 0x390) | 2)
        metadata = swift_metadata(MachO(bytes(data)))
        self.assertEqual(metadata["status"], "partial")
        self.assertIn("interoperability", " ".join(metadata["limitations"]))

    def test_chained_metadata_is_explicitly_unavailable(self):
        metadata = objc_metadata(MachO(native_fixture(chained=True)))
        self.assertEqual(metadata["status"], "unsupported-pointer-layout")
        self.assertEqual(metadata["classes"], [])
        self.assertIn("chained fixups", " ".join(metadata["limitations"]))

    def test_duplicate_chained_fixup_command_cannot_hide_pointer_layout(self):
        data = bytearray(native_fixture(chained=True))
        command_end = 32 + struct.unpack_from("<I", data, 20)[0]
        command_count = struct.unpack_from("<I", data, 16)[0]
        struct.pack_into("<II", data, 16, command_count + 1, command_end - 32 + 16)
        struct.pack_into("<4I", data, command_end, 0x80000034, 16, 0, 0)
        with self.assertRaisesRegex(MobileError, "duplicate.*chained-fixup"):
            MachO(bytes(data))

    def test_universal_architecture_selection_and_fat64(self):
        for fat64 in (False, True):
            image, available = select_slice(universal_fixture(fat64=fat64))
            self.assertEqual(image.architecture, "arm64")
            self.assertEqual(available, ["arm", "arm64"])
            self.assertEqual(select_slice(universal_fixture(fat64=fat64), "arm")[0].architecture, "arm")

    def test_rejects_requested_architecture_mismatch(self):
        for data in (native_fixture(), universal_fixture()):
            with self.assertRaises(MobileError):
                select_slice(data, "x86_64")

    def test_rejects_truncated_load_commands_and_bad_count(self):
        for offset, value in ((16, 0xFFFFFFFF), (20, 0xFFFFFFFF), (36, 4), (36, 0xFFFFFFFC)):
            data = bytearray(native_fixture())
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(MobileError):
                MachO(bytes(data))
        with self.assertRaises(MobileError):
            MachO(native_fixture()[:24])

    def test_rejects_overlapping_fat_slices_and_mismatched_cpu(self):
        with self.assertRaisesRegex(MobileError, "overlapping"):
            select_slice(universal_fixture(overlap=True))
        data = bytearray(universal_fixture())
        struct.pack_into("<I", data, 0x2000 + 8, 2)
        with self.assertRaisesRegex(MobileError, "disagrees"):
            select_slice(bytes(data))

    def test_metadata_count_and_bad_pointer_do_not_allocate_unboundedly(self):
        for offset, value in ((0x604, 0xFFFFFFFF), (0x608, 0)):
            data = bytearray(native_fixture())
            struct.pack_into("<I", data, offset, value)
            metadata = objc_metadata(MachO(bytes(data)))
            self.assertEqual(metadata["status"], "partial")
            self.assertEqual(metadata["classes"], [])

    def test_bad_symbol_index_is_rejected(self):
        data = bytearray(native_fixture())
        struct.pack_into("<I", data, 0x820, 64)
        with self.assertRaisesRegex(MobileError, "string index"):
            swift_metadata(MachO(bytes(data)))

    def test_rejects_section_file_and_virtual_mapping_mismatch(self):
        data = bytearray(native_fixture())
        struct.pack_into("<I", data, 32 + 72 + 48, 0x388)
        with self.assertRaisesRegex(MobileError, "mappings disagree"):
            MachO(bytes(data))

    def test_rejects_zero_fill_language_metadata(self):
        data = bytearray(native_fixture())
        struct.pack_into("<I", data, 32 + 72 + 64, 1)
        with self.assertRaisesRegex(MobileError, "not file-backed"):
            objc_metadata(MachO(bytes(data)))

    def test_unsupported_complex_encoding_is_retained_without_fake_signature(self):
        metadata = objc_metadata(MachO(native_fixture()))
        metadata["classes"][0]["methods"][0]["type_encoding"] = "{Pair=ii}32@0:8i16i24"
        self.assertNotIn("add:", objc_header(metadata))
        self.assertIn("Method declaration omitted", objc_header(metadata))

    def test_malformed_hidden_method_arguments_do_not_create_declaration(self):
        metadata = objc_metadata(MachO(native_fixture()))
        metadata["classes"][0]["methods"][1]["type_encoding"] = "q16q0q8"
        self.assertNotIn("answer;", objc_header(metadata))

    def test_unknown_superclass_is_not_reported_as_a_root_class(self):
        data = bytearray(native_fixture())
        struct.pack_into("<I", data, 0x500, 0)
        metadata = objc_metadata(MachO(bytes(data)))
        self.assertFalse(metadata["classes"][0]["root_class"])
        self.assertEqual(metadata["classes"][0]["inheritance_status"], "unresolved")
        self.assertIn("Superclass could not be resolved", objc_header(metadata))

    def test_header_orders_superclasses_and_handles_cycles(self):
        child = {"name": "Child", "superclass": "Parent", "methods": [], "root_class": False}
        parent = {"name": "Parent", "superclass": None, "methods": [], "root_class": True}
        header = objc_header({"classes": [child, parent]})
        self.assertLess(header.index("@interface Parent"), header.index("@interface Child"))
        self.assertIn("@interface Child : Parent", header)
        parent["superclass"] = "Child"
        header = objc_header({"classes": [child, parent]})
        self.assertNotIn(" : ", header)


class IOSPackageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="neverd ios tests ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / "output"
        self.output.mkdir()

    def run_ios(self, source, **kwargs):
        options = dict(neverd="fake-neverd", arch="auto", artifact=None, metadata_only=True,
                       max_func=0, limits=Limits())
        options.update(kwargs)
        return decompile_ios(source, self.output, **options)

    def bundle(self, name="Demo.app", executable="Demo", data=None):
        app = self.root / name
        app.mkdir()
        (app / "Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": executable, "CFBundleIdentifier": "test.demo"}, fmt=plistlib.FMT_BINARY))
        if data is None:
            data = native_fixture()
        (app / "Demo").write_bytes(data)
        return app

    def ipa(self, apps=1):
        ipa = self.root / "Demo.ipa"
        with zipfile.ZipFile(ipa, "w") as archive:
            for index in range(apps):
                prefix = f"Payload/Demo{index}.app/"
                archive.writestr(prefix + "Info.plist", plistlib.dumps({"CFBundleExecutable": "Demo"}))
                archive.writestr(prefix + "Demo", native_fixture())
                archive.writestr(prefix + "Frameworks/Helper.framework/Helper", native_fixture(is64=False))
        return ipa

    def test_app_binary_plist_metadata_only(self):
        with patch("mobile.ios.run_tool") as run, \
                patch("mobile.swift_source.recover_swift_sources") as swift:
            report = self.run_ios(self.bundle(), swift_demangle="missing-explicit-tool")
            run.assert_not_called()
            swift.assert_not_called()
        self.assertEqual(report["bundle"]["CFBundleIdentifier"], "test.demo")
        self.assertEqual(report["selected_artifact"], "Demo")
        self.assertEqual(report["objc_class_count"], 1)
        self.assertFalse((self.output / "input").exists())
        self.assertIsNone(report["swift_method_recovery"])
        metadata = json.loads((self.output / "metadata" / "objc.json").read_text())
        self.assertEqual(metadata["classes"][0]["name"], "Calculator")

    def test_ipa_selects_main_executable(self):
        report = self.run_ios(self.ipa())
        self.assertEqual(report["input_kind"], "ipa")
        self.assertEqual(report["selected_artifact"], "Payload/Demo0.app/Demo")
        self.assertFalse((self.output / "input").exists())

    def test_rejects_ambiguous_main_apps(self):
        with self.assertRaisesRegex(MobileError, "exactly one"):
            self.run_ios(self.ipa(apps=2))

    def test_explicit_ipa_artifact_is_relative_to_main_app(self):
        report = self.run_ios(self.ipa(), artifact="Frameworks/Helper.framework/Helper")
        self.assertEqual(report["selected_artifact"], "Payload/Demo0.app/Frameworks/Helper.framework/Helper")
        self.assertEqual(report["architecture"], "arm")

    def test_explicit_app_artifact_uses_same_relative_path(self):
        app = self.bundle()
        helper = app / "Frameworks/Helper.framework/Helper"
        helper.parent.mkdir(parents=True)
        helper.write_bytes(native_fixture(is64=False))
        report = self.run_ios(app, artifact="Frameworks/Helper.framework/Helper")
        self.assertEqual(report["architecture"], "arm")

    def test_encrypted_selected_slice_never_reaches_native_decompiler(self):
        source = self.root / "Demo"
        source.write_bytes(native_fixture(encrypted=True))
        with patch("mobile.ios.run_tool") as run, self.assertRaisesRegex(MobileError, "encrypted"):
            self.run_ios(source, metadata_only=False)
        run.assert_not_called()

    def test_rejects_escaping_bundle_executable(self):
        with self.assertRaisesRegex(MobileError, "CFBundleExecutable"):
            self.run_ios(self.bundle(executable="../Demo"))

    def test_rejects_artifact_path_traversal(self):
        with self.assertRaisesRegex(MobileError, "relative path"):
            self.run_ios(self.bundle(), artifact="../outside")

    def test_rejects_invalid_plist(self):
        app = self.bundle()
        (app / "Info.plist").write_bytes(b"not a plist")
        with self.assertRaisesRegex(MobileError, "Info.plist"):
            self.run_ios(app)

    def test_rejects_malformed_xml_plist_with_domain_error(self):
        app = self.bundle()
        (app / "Info.plist").write_bytes(b'<?xml version="1.0"?><plist><dict>')
        with self.assertRaisesRegex(MobileError, "Info.plist"):
            self.run_ios(app)

    def test_native_invocation_uses_selected_slice_and_output_contract(self):
        source = self.root / "universal"
        source.write_bytes(universal_fixture())

        def backend(argv, log, timeout):
            self.assertEqual(argv[:2], ["fake-neverd", "export"])
            self.assertIn("--format=objc-methods", argv)
            self.assertEqual(MachO(Path(argv[2]).read_bytes()).architecture, "arm")
            self.assertIn("--max-func=7", argv)
            self.assertEqual(timeout, 300)
            Path(argv[argv.index("-o") + 1]).write_text(source_report(
                "int example(void) { return 42; }\n", limitations=["Variadic tails are not described by runtime encodings."]))
            log.write_text("complete\n")

        with patch("mobile.ios.run_tool", side_effect=backend):
            report = self.run_ios(source, arch="arm", metadata_only=False, max_func=7)
        self.assertEqual(report["outputs"]["native_source"], "sources/native.c")
        self.assertEqual(report["objc_method_recovery"]["method_count"], 0)
        self.assertIn("Variadic tails are not described by runtime encodings.", report["limitations"])
        self.assertFalse((self.output / "artifacts/native-recovery.json").exists())

    def test_native_success_without_source_is_failure(self):
        source = self.root / "Demo"
        source.write_bytes(native_fixture())
        with patch("mobile.ios.run_tool"), self.assertRaisesRegex(MobileError, "did not produce"):
            self.run_ios(source, metadata_only=False)

    def test_native_include_only_output_is_failure(self):
        source = self.root / "Demo"
        source.write_bytes(native_fixture())

        def backend(argv, log, timeout):
            Path(argv[argv.index("-o") + 1]).write_text(source_report(
                '#include <stdint.h>\n/* int fake(void) { return 42; } */\n'
                'const char *example = "int fake(void) { return 42; }";\n'
                'static inline int helper(void) { return 1; }\n', count=0))

        with patch("mobile.ios.run_tool", side_effect=backend), self.assertRaisesRegex(MobileError, "no function bodies"):
            self.run_ios(source, metadata_only=False)

    def test_native_report_rejects_invalid_schema_and_function_counts(self):
        source = self.root / "Demo"
        source.write_bytes(native_fixture())
        valid = json.loads(source_report("int example(void) { return 42; }\n"))
        for report, diagnostic in (
            ([], "schema"),
            ({**valid, "schema_version": 99}, "schema"),
            ({**valid, "native_source": 1}, "schema"),
            ({**valid, "native_function_count": 2}, "function count"),
            ({**valid, "native_function_count": True}, "function count"),
            ({**valid, "limitations": "unsupported"}, "schema"),
            ({**valid, "limitations": [None]}, "schema"),
        ):
            with self.subTest(report=report):
                shutil.rmtree(self.output, ignore_errors=True)

                def backend(argv, log, timeout):
                    Path(argv[argv.index("-o") + 1]).write_text(json.dumps(report))

                with patch("mobile.ios.run_tool", side_effect=backend), self.assertRaisesRegex(MobileError, diagnostic):
                    self.run_ios(source, metadata_only=False)

    def test_real_native_data_only_image_is_not_source_recovery(self):
        build = Path(os.environ.get("NEVERD_BUILD_DIR", ROOT / "build"))
        neverd = build / "bin" / ("neverd.exe" if os.name == "nt" else "neverd")
        if not neverd.is_file():
            if "NEVERD_BUILD_DIR" in os.environ:
                self.fail(f"configured NeverD CLI is missing: {neverd}")
            self.skipTest("requires a built NeverD CLI")
        source = self.root / "data-only"
        data = bytearray(native_fixture())
        struct.pack_into("<I", data, 32 + 232 + 12, 0)
        source.write_bytes(data)
        with self.assertRaisesRegex(MobileError, "no function bodies"):
            self.run_ios(source, neverd=str(neverd.resolve()), metadata_only=False)

    def test_raw_input_byte_limit(self):
        source = self.root / "Demo"
        source.write_bytes(native_fixture())
        with self.assertRaisesRegex(MobileError, "byte limit"):
            self.run_ios(source, limits=Limits(max_bytes=100))

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires Apple Clang and SDK")
    def test_compiled_arm64_objc_bundle_recovery(self):
        build = Path(os.environ.get("NEVERD_BUILD_DIR", ROOT / "build"))
        neverd = build / "bin" / "neverd"
        if not neverd.is_file():
            if "NEVERD_BUILD_DIR" in os.environ:
                self.fail(f"configured NeverD CLI is missing: {neverd}")
            self.skipTest("requires a built NeverD CLI")
        source = self.root / "calculator.m"
        source.write_text("""__attribute__((objc_root_class))
@interface Calculator
- (long long)add:(long long)x to:(long long)y;
+ (long long)answer;
@end
@implementation Calculator
- (long long)add:(long long)x to:(long long)y { return x + y; }
+ (long long)answer { return 42; }
@end
int main(void) { return 0; }
""")
        binary = self.root / "calculator"
        subprocess.run(["clang", "-arch", "arm64", "-g", "-O0", "-Wl,-no_fixup_chains",
                        str(source), "-lobjc", "-o", str(binary)], check=True, capture_output=True, timeout=30)
        report = self.run_ios(self.bundle(data=binary.read_bytes()), neverd=str(neverd.resolve()),
                              arch="arm64", metadata_only=False)
        self.assertEqual(report["architecture"], "arm64")
        self.assertEqual(report["objc_class_count"], 1)
        self.assertGreater(report["native_function_count"], 0)
        metadata = json.loads((self.output / "metadata/objc.json").read_text())
        self.assertEqual(metadata["status"], "recovered")
        methods = metadata["classes"][0]["methods"]
        self.assertEqual({method["selector"] for method in methods}, {"add:to:", "answer"})
        self.assertIn("Calculator", (self.output / "sources/native.c").read_text())
        header = self.output / "metadata/objc.h"
        self.assertIn("- (long long)add:(long long)arg0 to:(long long)arg1;", header.read_text())
        self.assertIn("+ (long long)answer;", header.read_text())
        subprocess.run(["clang", "-arch", "arm64", "-x", "objective-c", "-fsyntax-only",
                        str(header)], check=True, capture_output=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
