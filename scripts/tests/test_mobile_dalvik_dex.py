"""Independent binary fixtures for the standard DEX reader."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "neverd"))
from mobile.common import Limits, MobileError
from mobile.dalvik_dex import _Cursor, _Dex, parse_dex
from mobile.dalvik_model import Budget, FieldRef, MethodRef


def uleb(value: int) -> bytes:
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        result.append(byte | (128 if value else 0))
        if not value: return bytes(result)


def mutf8(value: str) -> bytes:
    units = value.encode("utf-16-le", errors="surrogatepass")
    result = bytearray(uleb(len(units) // 2))
    for at in range(0, len(units), 2):
        unit = int.from_bytes(units[at:at + 2], "little")
        if 0 < unit < 128: result.append(unit)
        elif unit < 2048: result.extend((0xc0 | unit >> 6, 0x80 | unit & 63))
        else: result.extend((0xe0 | unit >> 12, 0x80 | unit >> 6 & 63, 0x80 | unit & 63))
    return bytes(result) + b"\0"


def seal(data: bytes | bytearray) -> bytes:
    data = bytearray(data)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return bytes(data)


def fixture(words=(0x000f,), *, params=("I",), returns="I", registers=1,
            version="035", flags=9, tries=(), handlers=b"", extras=(), static_value=None):
    """Write one documented class/table layout; no production writer is used."""
    owner, parent = "Lfixture/Sample;", "Ljava/lang/Object;"
    shorty = "".join("L" if typ.startswith(("L", "[")) else typ for typ in (returns, *params))
    strings = sorted({owner, parent, "value", returns, *params, shorty, *extras,
                      *(('VALUE', 'I') if static_value is not None else ())},
                     key=lambda text: text.encode("utf-16-be", errors="surrogatepass"))
    types = sorted({owner, parent, returns, *params, *(("I",) if static_value is not None else ())}, key=strings.index)
    incoming = sum(2 if typ in ("J", "D") else 1 for typ in params) + int(not flags & 8)
    out = bytearray(b"\0" * 112)
    sections = [(0, 0, 1)]

    def align():
        while len(out) % 4: out.append(0)

    def section(kind, count, raw):
        at = len(out)
        out.extend(raw)
        sections.append((kind, at, count))
        return at

    string_ids = section(1, len(strings), b"\0" * (4 * len(strings)))
    type_ids = section(2, len(types), b"".join(struct.pack("<I", strings.index(typ)) for typ in types))
    proto_ids = section(3, 1, b"\0" * 12)
    field_ids = section(4, 1, struct.pack("<HHI", types.index(owner), types.index("I"), strings.index("VALUE"))) if static_value is not None else 0
    method_ids = section(5, 1, struct.pack("<HHI", types.index(owner), 0, strings.index("value")))
    class_defs = section(6, 1, b"\0" * 32)
    data_off = len(out)
    string_data = len(out)
    for index, text in enumerate(strings):
        struct.pack_into("<I", out, string_ids + index * 4, len(out))
        out.extend(mutf8(text))
    sections.append((0x2002, string_data, len(strings)))
    align()
    parameters = 0
    if params:
        parameters = section(0x1001, 1, struct.pack("<I", len(params)) + b"".join(struct.pack("<H", types.index(typ)) for typ in params))
        align()
    no_code = bool(flags & (0x100 | 0x400))
    code = 0
    if not no_code:
        raw = struct.pack("<HHHHII", registers, incoming, 255, len(tries), 0, len(words))
        raw += struct.pack("<" + str(len(words)) + "H", *words)
        if tries and len(words) % 2: raw += b"\0\0"
        raw += b"".join(struct.pack("<IHH", *item) for item in tries)
        raw += handlers
        code = section(0x2001, 1, raw)
    direct = bool(flags & (8 | 2 | 0x10000))
    counts = (int(static_value is not None), 0, int(direct), int(not direct))
    encoded_class = b"".join(uleb(count) for count in counts)
    if static_value is not None: encoded_class += uleb(0) + uleb(0x19)
    encoded_class += uleb(0) + uleb(flags) + uleb(code)
    class_data = section(0x2000, 1, encoded_class)
    values = section(0x2005, 1, b"\1" + static_value) if static_value is not None else 0
    align()
    map_off = len(out)
    sections.append((0x1000, map_off, 1))
    out += struct.pack("<I", len(sections))
    out += b"".join(struct.pack("<HHII", kind, 0, count, at) for kind, at, count in sections)
    out[:8] = b"dex\n" + version.encode() + b"\0"
    struct.pack_into("<IIIIII", out, 32, len(out), 112, 0x12345678, 0, 0, map_off)
    for kind, count, at in ((1, len(strings), string_ids), (2, len(types), type_ids), (3, 1, proto_ids),
                             (4, int(static_value is not None), field_ids), (5, 1, method_ids), (6, 1, class_defs)):
        struct.pack_into("<II", out, 56 + (kind - 1) * 8, count, at)
    struct.pack_into("<II", out, 104, len(out) - data_off, data_off)
    struct.pack_into("<III", out, proto_ids, strings.index(shorty), types.index(returns), parameters)
    struct.pack_into("<IIIIIIII", out, class_defs, types.index(owner), 1, types.index(parent), 0, 0xffffffff, 0, class_data, values)
    return seal(out), {"code": code, "map": map_off, "strings": string_ids, "string_data": string_data,
                       "types": type_ids, "protos": proto_ids, "methods": method_ids, "class": class_defs,
                       "class_data": class_data, "values": values}


class DexReaderTests(unittest.TestCase):
    def parse(self, data): return parse_dex(data, input_id="classes2.dex", budget=Budget(Limits()))

    def test_standard_versions_recover_exact_method_inventory(self):
        for version in ("035", "037", "038", "039", "040"):
            with self.subTest(version=version):
                data, _ = fixture(version=version)
                classes = self.parse(data)
                self.assertEqual(len(classes), 1)
                self.assertEqual(classes[0].source_id, "classes2.dex")
                method = classes[0].methods[0]
                self.assertEqual(method.reference, MethodRef("Lfixture/Sample;", "value", ("I",), "I"))
                self.assertEqual([(i.pc, i.opcode, i.registers) for i in method.instructions], [(0, "return", (0,))])

    def test_full_header_integrity_and_unsupported_versions(self):
        data, _ = fixture()
        mutations = [data[:111], b"cdex001\0" + data[8:], data[:40] + b"y" + data[41:]]
        for offset, value in ((32, len(data) - 1), (36, 120), (40, 0x78563412), (44, 8), (52, 0xffffffff)):
            broken = bytearray(data); struct.pack_into("<I", broken, offset, value); mutations.append(seal(broken))
        for version in ("036", "041", "999"):
            broken = bytearray(data); broken[4:7] = version.encode(); mutations.append(seal(broken))
        for broken in mutations:
            with self.subTest(header=broken[:8]):
                with self.assertRaises(MobileError): self.parse(broken)

    def test_table_indices_shorty_and_map_inventory_are_checked(self):
        data, at = fixture()
        for offset, value in ((at["strings"], 0), (at["types"], 0xffffffff), (at["protos"], 0xffffffff),
                              (at["protos"] + 4, 0xffffffff), (at["methods"] + 4, 0xffffffff),
                              (at["class"], 0xffffffff), (at["map"] + 8, 0), (at["class"] + 24, at["string_data"])):
            broken = bytearray(data); struct.pack_into("<I", broken, offset, value)
            with self.subTest(offset=offset):
                with self.assertRaises(MobileError): self.parse(seal(broken))

    def test_mutf8_preserves_nul_supplementary_and_isolated_surrogates(self):
        text = "A\0λ😀\ud800"
        data, _ = fixture(extras=(text,))
        reader = _Dex(data, "fixture", Budget(Limits()))
        reader.parse()
        self.assertIn(text, reader.strings)
        for malformed in (b"\1\xc1\x81\0", b"\1\xe0\x80\x81\0", b"\1\xf0\x90\x80\x80\0", b"\2a\0", b"\1\xc0A\0"):
            with self.assertRaises(MobileError): reader.mutf8(_Cursor(malformed, 0, len(malformed), Budget(Limits())))

    def test_leb128_bounds_and_sign_extension(self):
        for raw, signed, expected in ((b"\xff\xff\xff\xff\x0f", False, 0xffffffff),
                                      (b"\x80\x80\x80\x80\x78", True, -(1 << 31)), (b"\x7f", True, -1)):
            self.assertEqual(_Cursor(raw, 0, len(raw), Budget(Limits())).leb(signed), expected)
        for raw, signed in ((b"\x80" * 6, False), (b"\xff\xff\xff\xff\x10", False),
                             (b"\xff\xff\xff\xff\x0f", True), (b"\x80", False)):
            with self.assertRaises(MobileError): _Cursor(raw, 0, len(raw), Budget(Limits())).leb(signed)

    def test_instruction_layouts_preserve_word_registers_and_literals(self):
        words = (0x2112, 0x0113, 0xffff, 0x0115, 0x8000, 0x0290, 0x0100, 0x00d8, 0x8001, 0x000f)
        data, _ = fixture(words, registers=3)
        instructions = self.parse(data)[0].methods[0].instructions
        self.assertEqual([(i.pc, i.opcode, i.registers, i.literal) for i in instructions],
                         [(0, "const/4", (1,), 2), (1, "const/16", (1,), -1),
                          (3, "const/high16", (1,), -(1 << 31)), (5, "add-int", (2, 0, 1), None),
                          (7, "add-int/lit8", (0, 1), -128), (9, "return", (0,), None)])

    def test_wide_and_instance_parameters_keep_last_word_registers(self):
        data, _ = fixture((0x0010,), params=("J",), returns="J", registers=2)
        self.assertEqual(self.parse(data)[0].methods[0].incoming_words, 2)
        data, _ = fixture((0x010f,), params=("I",), registers=2, flags=1)
        self.assertEqual(self.parse(data)[0].methods[0].instructions[0].registers, (1,))
        for words, registers in (((0x0110,), 2), ((0x0312, 0x000f), 2), ((0x0004, 0x000f), 1)):
            with self.assertRaises(MobileError): self.parse(fixture(words, registers=registers)[0])

    def test_references_and_invoke_word_count(self):
        data, _ = fixture((0x1071, 0, 0, 0x000a, 0x000f))
        instructions = self.parse(data)[0].methods[0].instructions
        self.assertEqual(instructions[0].reference, MethodRef("Lfixture/Sample;", "value", ("I",), "I"))
        self.assertEqual(instructions[0].registers, (0,))
        for words in ((0x2071, 0, 0, 0x000f), (0x1071, 65535, 0, 0x000f), (0x00fa, 0, 0, 0, 0x000f)):
            with self.assertRaises(MobileError): self.parse(fixture(words)[0])

    def test_switch_payloads_use_switch_relative_targets(self):
        for payload in ((0x100, 2, 0xffff, 0xffff, 3, 0, 3, 0),
                        (0x200, 2, 0xfffe, 0xffff, 100, 0, 3, 0, 3, 0)):
            data, _ = fixture((0x002b if payload[0] == 0x100 else 0x002c, 4, 0, 0x000f, *payload))
            method = self.parse(data)[0].methods[0]
            self.assertEqual(len(method.instructions), 2)
            self.assertEqual(method.instructions[0].targets, (3, 3))
            self.assertEqual(method.instructions[0].keys, (-1, 0) if payload[0] == 0x100 else (-2, 100))

    def test_array_payload_preserves_unsigned_storage_bits(self):
        words = (0x0026, 4, 0, 0x0011, 0x300, 4, 3, 0, 1, 0, 0xfffe, 0xffff, 0xffff, 0x7fff)
        method = self.parse(fixture(words, params=("[I",), returns="[I")[0])[0].methods[0]
        self.assertEqual(method.instructions[0].data, (1, 0xfffffffe, 0x7fffffff))
        self.assertEqual(method.instructions[0].element_width, 4)

    def test_malformed_branch_payload_and_truncation_never_skip_code(self):
        for words in ((0x0014, 1), (0x0238, 1, 0x000f), (0x0029, 1, 0x000f),
                      (0x0000,), (0x00e3, 0x000f), (0x002b, 4, 0, 0x000f, 0x200, 0),
                      (0x002b, 4, 0, 0x000f, 0x100, 1, 0, 0, 1, 0),
                      (0x0000, 0x300, 1, 1, 0, 0)):
            with self.subTest(words=words):
                with self.assertRaises(MobileError): self.parse(fixture(words)[0])

    def test_try_ranges_and_catch_all_are_exact(self):
        words = (0x0093, 0x0100, 0x000f, 0x000d, 0xf012, 0x000f)
        data, _ = fixture(words, params=("I", "I"), registers=2,
                          tries=((0, 2, 1),), handlers=b"\1\0\3")
        region = self.parse(data)[0].methods[0].tries[0]
        self.assertEqual((region.start, region.end, region.handlers), (0, 2, ((None, 3),)))
        for tries, handlers in ((((1, 1, 1),), b"\1\0\3"), (((0, 2, 2),), b"\1\0\3"),
                                (((0, 2, 1),), b"\1\0\1"), (((0, 2, 1), (0, 2, 1)), b"\1\0\3")):
            with self.assertRaises(MobileError):
                self.parse(fixture(words, params=("I", "I"), registers=2, tries=tries, handlers=handlers)[0])

    def test_handler_and_move_result_entry_semantics(self):
        for words, tries, handlers in (
                ((0x000d, 0x000f), (), b""),
                ((0x000a, 0x000f), (), b""),
                ((0x1071, 0, 0, 0x000b, 0x000f), (), b""),
                ((0x0028, 0x000f), (), b""),
                ((0x000f, 0x000d), ((0, 1, 1),), b"\1\0\1"),
                ((0x0000, 0x000d, 0x000f), ((0, 1, 1),), b"\1\0\1")):
            with self.subTest(words=words):
                with self.assertRaises(MobileError):
                    self.parse(fixture(words, tries=tries, handlers=handlers)[0])
        words = (0x0038, 5, 0x1071, 0, 0, 0x000a, 0x000f)
        with self.assertRaisesRegex(MobileError, "move-result"):
            self.parse(fixture(words)[0])

    def test_static_encoded_values_and_abstract_declarations(self):
        cls = self.parse(fixture(static_value=b"\x64\xff\xff\xff\x7f")[0])[0]
        self.assertEqual(cls.fields[0].reference, FieldRef("Lfixture/Sample;", "VALUE", "I"))
        self.assertEqual(cls.fields[0].value, 0x7fffffff)
        cls = self.parse(fixture(flags=0x401)[0])[0]
        self.assertEqual(cls.methods[0].instructions, [])
        self.assertIn("abstract", cls.methods[0].access)
        with self.assertRaises(MobileError): self.parse(fixture(static_value=b"\x1f")[0])

    def test_encoded_float_bits_and_recursive_values_are_bounded(self):
        reader = _Dex(b"", "fixture", Budget(Limits()))
        for raw, expected in ((b"\x70\x00\x00\x80\xff", {"kind": "float-bits", "bits": 0xff800000}),
                              (b"\x11\x80", {"kind": "double-bits", "bits": 0x8000000000000000}),
                              (b"\x70\x01\x00\x80\x7f", {"kind": "float-bits", "bits": 0x7f800001})):
            self.assertEqual(reader.encoded(_Cursor(raw, 0, len(raw), Budget(Limits()))).value, expected)
        for raw in (b"\x3e", b"\xff", b"\x20\0\0", b"\x1c\1" * 70 + b"\x1e"):
            with self.assertRaises(MobileError): reader.encoded(_Cursor(raw, 0, len(raw), Budget(Limits())))

    def test_resource_budget_applies_before_large_tables_and_loops(self):
        data, _ = fixture()
        budget = Budget(Limits()); budget.remaining = 1
        with self.assertRaisesRegex(MobileError, "budget"):
            parse_dex(data, input_id="fixture", budget=budget)
        budget = Budget(Limits()); budget.deadline = 0
        with self.assertRaisesRegex(MobileError, "budget"):
            parse_dex(data, input_id="fixture", budget=budget)
        with self.assertRaises(MobileError):
            parse_dex(data, input_id="fixture", budget=Budget(Limits(max_bytes=112)))

    @unittest.skipUnless(os.environ.get("NEVERD_D8") and os.environ.get("JAVA_HOME"),
                         "independent DEX compiler test requires NEVERD_D8 and JAVA_HOME")
    def test_independent_d8_nested_metadata_payloads_and_static_bits(self):
        java = Path(os.environ["JAVA_HOME"]) / "bin" / ("javac.exe" if os.name == "nt" else "javac")
        source = r'''package fixture;
public class ReaderFixture {
  public static final String WORD="neverd\0λ😀";
  public static final long BIG=-9223372036854775807L;
  public static final float NEGATIVE_ZERO=-0.0f;
  public static class Nested { public static int bump(int n) { return n+3; } }
  private int value;
  public ReaderFixture(int n) { value=n; }
  public int add(int n) { return value+n; }
  public static int sum(int[] a) { int s=0; for(int v:a) s+=v; return s; }
  public static int divide(int x, int y) { try { return x/y; } catch(ArithmeticException e) { return -1; } }
  public static int choose(int n) { switch(n) { case 1:return 4; case 2:return 8; case 3:return 9; default:return -1;} }
  public static int sparse(int n) { switch(n) { case 1:return 4; case 100:return 8; case 333:return 9; default:return -1;} }
  public static int[] array() { return new int[]{1,-2,2147483647,4,5,6,7,8,9,10,11,12}; }
  public static long wide(long x, double y) { return x+(long)y; }
}'''
        with tempfile.TemporaryDirectory(prefix="neverd-dex-reader-") as temporary:
            work = Path(temporary)
            original = work / "ReaderFixture.java"
            original.write_text(source, encoding="utf-8")
            classes_dir, dex_dir = work / "classes", work / "dex"
            classes_dir.mkdir(); dex_dir.mkdir()
            def run(argv):
                result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run([java, "-encoding", "UTF-8", "-g", "-d", classes_dir, original])
            run([os.environ["NEVERD_D8"], "--output", dex_dir, *sorted(classes_dir.rglob("*.class"))])
            classes = self.parse((dex_dir / "classes.dex").read_bytes())
            self.assertEqual({cls.name for cls in classes}, {"Lfixture/ReaderFixture;", "Lfixture/ReaderFixture$Nested;"})
            self.assertEqual(sum(len(cls.methods) for cls in classes), 10)
            outer = next(cls for cls in classes if cls.name == "Lfixture/ReaderFixture;")
            nested = next(cls for cls in classes if cls.name.endswith("$Nested;"))
            self.assertEqual((nested.enclosing, nested.inner_name), (outer.name, "Nested"))
            values = {field.reference.name: field.value for field in outer.fields}
            self.assertEqual(values["WORD"], "neverd\0λ😀")
            self.assertEqual(values["BIG"], -9223372036854775807)
            self.assertEqual(values["NEGATIVE_ZERO"], {"kind": "float-bits", "bits": 0x80000000})
            methods = {method.reference.name: method for method in outer.methods}
            self.assertEqual(methods["divide"].tries[0].handlers[0][0], "Ljava/lang/ArithmeticException;")
            self.assertTrue(any(instruction.opcode == "packed-switch" for instruction in methods["choose"].instructions))
            self.assertTrue(any(instruction.opcode == "sparse-switch" for instruction in methods["sparse"].instructions))
            payload = next(instruction for instruction in methods["array"].instructions if instruction.opcode == "fill-array-data")
            self.assertEqual(payload.data, (1, 0xfffffffe, 0x7fffffff, 4, 5, 6, 7, 8, 9, 10, 11, 12))
            self.assertEqual(methods["wide"].reference.parameters, ("J", "D"))


if __name__ == "__main__": unittest.main()
