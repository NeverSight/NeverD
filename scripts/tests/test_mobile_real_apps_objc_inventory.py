"""Small independent oracle fixtures; no SDK or application execution.

The eight StickerBrowserViewController records below are a minimal excerpt of
Apple output from run 34326479452, wikipedia-release-arm64-simulator, consumer
bbe730e43a5a738f0cbc5ffe5002164c0ace60aa (commands 0359/0362/0363). Addresses,
selectors, encodings, and relative entries are retained for review. Mach-O bytes
and shortened tool envelopes are reconstructed here; this is a parser fixture,
not a substitute for the original binary or CI application acceptance.
"""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "mobile_real_apps_objc_inventory", Path(__file__).resolve().parents[1] / "mobile_real_apps_objc_inventory.py"
)
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)

BASE = 0x100000000
OWNER = "StickerBrowserViewController"
SELECTORS = [
    (0x1649, "viewDidLoad"), (0x14FB, "viewWillAppear:"), (0x1517, "bundlePath"),
    (0x1522, "dataSource"), (0x152D, "exceptionWithName:reason:userInfo:"),
    (0x1550, "fileURLWithPath:"), (0x1561, "infoDictionary"),
    (0x1570, "initWithStickerPackURL:"), (0x1588, "mainBundle"),
    (0x1593, "numberOfStickersInStickerBrowserView:"), (0x15B9, "objectForKey:"),
    (0x15C7, "reloadData"), (0x15D2, "respondsToSelector:"), (0x15E6, "setDataSource:"),
    (0x15F5, "stickerBrowserView"), (0x1608, "stickerBrowserView:stickerAtIndex:"),
    (0x162B, "stickerSize"), (0x1491, "stringByAppendingPathComponent:"),
    (0x1637, "stringWithFormat:"), (0x14ED, ".cxx_destruct"),
]
TYPES = {0x1674: "v16@0:8", 0x167C: "v20@0:8B16", 0x1687: "q24@0:8@16",
         0x1692: "@32@0:8@16q24", 0x16A0: "q16@0:8", 0x16A8: "@16@0:8", 0x16B0: "v24@0:8@16"}
# selref slot, types, IMP: all values are from the actual eight-record list.
METHODS = [(0x8118, 0x1674, 0xC88), (0x8120, 0x167C, 0xE1C),
           (0x8160, 0x1687, 0xE7C), (0x8190, 0x1692, 0xF18),
           (0x8198, 0x16A0, 0xF8C), (0x8130, 0x16A8, 0x102C),
           (0x8180, 0x16B0, 0x103C), (0x81B0, 0x1674, 0x107C)]


def fixture(*, architecture="arm64", shared_imp=False, category=False, nonlazy=False, chained=False,
            metaclass_method=False):
    data = bytearray(0xC000)
    sections = [
        ("__TEXT", "__text", 0xC88, 0x408, 0x80000400),
        ("__TEXT", "__objc_methlist", 0x13A0, 0x68, 0),
        ("__TEXT", "__objc_methname", 0x1491, 0x1C4, 2),
        ("__TEXT", "__objc_classname", 0x1655, 0x1F, 2),
        ("__TEXT", "__objc_methtype", 0x1674, 0x71, 2),
        ("__DATA_CONST", "__objc_classlist", 0x4058, 8, 0x10000000),
        ("__DATA", "__objc_const", 0x8048, 0xD0, 0),
        ("__DATA", "__objc_selrefs", 0x8118, 0xA0, 0),
        ("__DATA", "__objc_data", 0x81F0, 0x50, 0),
    ]
    if category:
        sections.append(("__DATA_CONST", "__objc_catlist", 0x4070, 8, 0))
    if category or metaclass_method:
        sections.append(("__DATA", "__objc_extra", 0x8300, 0x100, 0))
    if nonlazy:
        sections.append(("__DATA_CONST", "__objc_nlclslist", 0x4080, 8, 0))
    commands, load = [], []
    for segment, start in (("__TEXT", 0), ("__DATA_CONST", 0x4000), ("__DATA", 0x8000)):
        members = [s for s in sections if s[0] == segment]
        command = bytearray(struct.pack("<II16sQQQQIIII", 0x19, 72 + 80 * len(members), segment.encode(),
                                        BASE + start, 0x4000, start, 0x4000, 7, 5 if start == 0 else 3,
                                        len(members), 0))
        load.append(f"Load command {len(commands)}\n      cmd LC_SEGMENT_64\n")
        for owner, name, offset, size, flags in members:
            command.extend(struct.pack("<16s16sQQIIIIIIII", name.encode(), owner.encode(), BASE + offset, size,
                                       offset, 3, 0, 0, flags, 0, 0, 0))
            load.append(f"Section\n  sectname {name}\n   segname {owner}\n      addr {hex(BASE + offset)}\n"
                        f"      size {hex(size)}\n    offset {offset}\n     flags {hex(flags)}\n")
        commands.append(bytes(command))
    commands.append(struct.pack("<II16s", 0x1B, 24, bytes(range(16))))
    load.append(f"Load command {len(commands) - 1}\n      cmd LC_UUID\n")
    if chained:
        commands.append(struct.pack("<IIII", 0x80000034, 16, 0, 0))
        load.append(f"Load command {len(commands) - 1}\n      cmd LC_DYLD_CHAINED_FIXUPS\n")
    command_data = b"".join(commands)
    struct.pack_into("<8I", data, 0, 0xFEEDFACF, 0x100000C if architecture == "arm64" else 0x1000007,
                     0, 2, len(commands), len(command_data), 0, 0)
    data[32:32 + len(command_data)] = command_data

    def pointer(offset, target):
        struct.pack_into("<Q", data, offset, BASE + target if target else 0)

    for address, text in [*SELECTORS, *TYPES.items(), (0x1655, OWNER)]:
        encoded = text.encode() + b"\0"
        data[address:address + len(encoded)] = encoded
    pointer(0x4058, 0x81F0)
    pointer(0x81F0, 0x8218)
    pointer(0x81F0 + 32, 0x80D0)
    pointer(0x8218 + 32, 0x8048)
    struct.pack_into("<I", data, 0x80D0, 0x184)
    struct.pack_into("<I", data, 0x8048, 0x185)
    pointer(0x80D0 + 24, 0x1655)
    pointer(0x8048 + 24, 0x1655)
    pointer(0x80D0 + 32, 0x13A0)
    for index, (address, _) in enumerate(SELECTORS):
        pointer(0x8118 + index * 8, address)
    struct.pack_into("<II", data, 0x13A0, 0x8000000C, 8)
    method_lines, dyld_lines = [], []
    for ordinal, (name, types, imp) in enumerate(METHODS):
        if shared_imp and ordinal == 1:
            imp = METHODS[0][2]
        entry = 0x13A8 + ordinal * 12
        relative = (name - entry, types - (entry + 4), imp - (entry + 8))
        struct.pack_into("<iii", data, entry, *relative)
        selector = SELECTORS[(name - 0x8118) // 8][1]
        method_lines.extend([
            f"            name    {hex(relative[0] & 0xFFFFFFFF)} ({hex(BASE + name)}) {selector}",
            f"            types   {hex(relative[1] & 0xFFFFFFFF)} ({hex(BASE + types)}) {TYPES[types]}",
            f"            imp     {hex(relative[2] & 0xFFFFFFFF)} ({hex(BASE + imp)})",
        ])
        dyld_lines.append(f"          {hex(BASE + imp)}  -[{OWNER} {selector}]")
    otool = ("fixture:\nContents of (__DATA_CONST,__objc_classlist) section\n"
             "0000000100004058 0x1000081f0 _OBJC_CLASS_$_StickerBrowserViewController\n"
             "    isa        0x100008218 _OBJC_METACLASS_$_StickerBrowserViewController\n"
             "    data       0x1000080d0\n"
             "        name           0x100001655 StickerBrowserViewController\n"
             "        baseMethods    0x1000013a0\n"
             "            entsize 12 (relative)\n            count   8\n"
             + "\n".join(method_lines) + "\nMeta Class\n"
             "    data       0x100008048\n"
             "        name           0x100001655 StickerBrowserViewController\n"
             "        baseMethods    0x0\n")
    dyld = ("fixture [arm64]:\n    -objc:\n"
            "        @interface StickerBrowserViewController : MSStickerBrowserViewController\n"
            + "\n".join(dyld_lines) + "\n        @end\n")
    if metaclass_method:
        pointer(0x8048 + 32, 0x8360)
        struct.pack_into("<II", data, 0x8360, 24, 1)
        for offset, target in ((0x8368, 0x1649), (0x8370, 0x1674), (0x8378, 0xC88)):
            pointer(offset, target)
        otool = otool.replace("        baseMethods    0x0\n",
                              "        baseMethods    0x100008360\n            entsize 24\n            count 1\n"
                              "            name 0x100001649 viewDidLoad\n            types 0x100001674 v16@0:8\n"
                              "            imp 0x100000c88\n")
        dyld = dyld.replace("        @end\n", "          0x100000c88 +[StickerBrowserViewController viewDidLoad]\n        @end\n")
    if category:
        # A small authored absolute-list supplement exercises category identity.
        pointer(0x4070, 0x8300)
        data[0x16D8:0x16DE] = b"Extra\0"
        for offset, target in ((0x8300, 0x16D8), (0x8308, 0x81F0), (0x8310, 0x8328)):
            pointer(offset, target)
        struct.pack_into("<II", data, 0x8328, 24, 1)
        for offset, target in ((0x8330, 0x1649), (0x8338, 0x1674), (0x8340, 0xC88)):
            pointer(offset, target)
        otool += ("Contents of (__DATA_CONST,__objc_catlist) section\n"
                  "0000000100004070 0x100008300\n"
                  "    name 0x1000016d8 Extra\n    cls 0x1000081f0\n"
                  "    instanceMethods 0x100008328\n        entsize 24\n        count 1\n"
                  "        name 0x100001649 viewDidLoad\n        types 0x100001674 v16@0:8\n"
                  "        imp 0x100000c88\n    classMethods 0x0\n")
        dyld += ("        @interface StickerBrowserViewController(Extra)\n"
                 "          0x100000c88 -[StickerBrowserViewController viewDidLoad]\n        @end\n")
    otool += "Contents of (__DATA,__objc_selrefs) section\n" + "".join(
        f"    {hex(BASE + address)} {text}\n" for address, text in SELECTORS)
    outputs = {"load-commands": "fixture:\n" + "".join(load), "objc-otool": otool, "objc": dyld,
               "fixups": "fixture [arm64]:\n    -fixups:\n        segment section address type target\n"}
    if nonlazy:
        pointer(0x4080, 0x81F0)
        words = "f0 81 00 00 01 00 00 00" if architecture == "x86_64" else "000081f0 00000001"
        outputs["objc-raw:__DATA_CONST:__objc_nlclslist"] = (
            "fixture:\nContents of (__DATA_CONST,__objc_nlclslist) section\n0000000100004080 " + words + "\n")
    return data, outputs


class ObjCDiskInventoryTests(unittest.TestCase):
    def analyze(self, data, outputs, *, architecture="arm64", expected_sha=None, check_budget=lambda: None):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "fixture"
            binary.write_bytes(data)
            artifact = {"path": "PlugIns/Fixture.appex/Fixture", "sha256": expected_sha or hashlib.sha256(data).hexdigest()}
            return oracle.objc_inventory(artifact, architecture, outputs, binary, check_budget)

    def assert_unknown(self, result, reason):
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(result["denominator_known"])
        self.assertIsNone(result["method_count"])
        self.assertTrue(any(reason in issue for issue in result["issues"]), result["issues"])

    def test_real_stickers_eight_record_excerpt_reconciles(self):
        result = self.analyze(*fixture())
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["method_count"], 8)
        self.assertTrue(result["denominator_known"])
        self.assertEqual(result["scope"], "on-disk-objc-method-records")
        first = result["methods"][0]
        self.assertEqual((first["owner_address"], first["metaclass_address"], first["method_list"], first["ordinal"]),
                         ("0x1000081f0", "0x100008218", "0x1000013a0", 0))
        self.assertEqual((first["selector"], first["type_encoding"], first["implementation"]),
                         ("viewDidLoad", "v16@0:8", "0x100000c88"))
        self.assertIn("dynamic runtime registration", result["exclusions"])

    def test_shared_imp_and_category_identities_are_not_collapsed(self):
        result = self.analyze(*fixture(shared_imp=True, category=True))
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["method_count"], 9)
        same_imp = [row for row in result["methods"] if row["implementation"] == "0x100000c88"]
        self.assertEqual(len(same_imp), 3)
        self.assertEqual({row["category"] for row in same_imp}, {"", "Extra"})

    def test_nonlazy_alias_retains_slot_without_multiplying_methods(self):
        for architecture in ("arm64", "x86_64"):
            with self.subTest(architecture=architecture):
                result = self.analyze(*fixture(nonlazy=True, architecture=architecture), architecture=architecture)
                self.assertEqual(result["issues"], [])
                self.assertEqual(result["method_count"], 8)
                self.assertEqual(len(result["root_slots"]), 2)
                self.assertTrue(result["root_slots"][1]["alias_of_declaration"])

    def test_metaclass_method_keeps_same_selector_and_imp_as_instance_method(self):
        result = self.analyze(*fixture(metaclass_method=True))
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["method_count"], 9)
        methods = [row for row in result["methods"] if row["selector"] == "viewDidLoad"]
        self.assertEqual({row["class_method"] for row in methods}, {False, True})
        self.assertEqual({row["method_list"] for row in methods}, {"0x1000013a0", "0x100008360"})

    def test_real_main_and_wmf_nonlazy_missing_evidence_stays_unknown(self):
        # Real main/WMF each declare an 8-byte nonlazy category section that
        # their recorded -ov output does not print. Reproduce the missing-slot
        # relationship on the small authored image, without app-specific counts.
        data, outputs = fixture(nonlazy=True)
        del outputs["objc-raw:__DATA_CONST:__objc_nlclslist"]
        self.assert_unknown(self.analyze(data, outputs), "nonlazy raw section evidence is missing")
        for address, offset in ((0x100663C68, 6700136), (0xBD2D08, 12397832)):
            # These two actual load-command declarations motivated additional
            # raw-section collection; preferred VM bases differ per image.
            excerpt = ("Section\n  sectname __objc_nlcatlist\n   segname __DATA_CONST\n"
                       f"      addr {hex(address)}\n      size 0x8\n    offset {offset}\n     flags 0x10000000\n")
            self.assertEqual(oracle.nonlazy_sections(excerpt), [("__DATA_CONST", "__objc_nlcatlist")])

    def test_raw_nonlazy_alias_must_match_disk_and_known_declaration(self):
        data, outputs = fixture(nonlazy=True)
        key = "objc-raw:__DATA_CONST:__objc_nlclslist"
        outputs[key] = outputs[key].replace("000081f0", "000081f8")
        self.assert_unknown(self.analyze(data, outputs), "disagrees with disk")
        struct.pack_into("<Q", data, 0x4080, BASE + 0x81F8)
        self.assert_unknown(self.analyze(data, outputs), "absent from the reconciled roots")

    def test_chained_zero_rendered_as_image_base_is_unknown(self):
        data, outputs = fixture(chained=True)
        outputs["objc-otool"] = outputs["objc-otool"].replace("baseMethods    0x0", "baseMethods    0x100000000 __mh_execute_header")
        self.assert_unknown(self.analyze(data, outputs), "chained fixup slot coverage")

    def test_symbolic_null_method_list_cannot_be_assumed_empty(self):
        data, outputs = fixture()
        outputs["objc-otool"] = outputs["objc-otool"].replace("baseMethods    0x0", "baseMethods    0x0 _external_methods")
        self.assert_unknown(self.analyze(data, outputs), "unresolved symbolic identity")

    def test_missing_root_or_method_slot_does_not_become_zero(self):
        data, outputs = fixture()
        outputs["objc-otool"] = outputs["objc-otool"].replace("0000000100004058 0x1000081f0", "0000000100004060 0x1000081f0")
        self.assert_unknown(self.analyze(data, outputs), "root slots are reordered")
        data, outputs = fixture()
        outputs["objc-otool"] = outputs["objc-otool"].replace("            count   8", "            count   7")
        self.assert_unknown(self.analyze(data, outputs), "method-list header disagrees")

    def test_dyld_omission_and_duplicate_fail_multiset_reconciliation(self):
        data, outputs = fixture()
        row = "          0x100000c88  -[StickerBrowserViewController viewDidLoad]\n"
        for replacement in ("", row + row):
            with self.subTest(replacement=replacement):
                changed = {**outputs, "objc": outputs["objc"].replace(row, replacement)}
                self.assert_unknown(self.analyze(data, changed), "multisets disagree")

    def test_protocol_requirements_are_excluded_but_addressed_requirements_are_unknown(self):
        data, outputs = fixture()
        protocol = "        @protocol NSObject\n          -[NSObject description]\n        @end\n"
        result = self.analyze(data, {**outputs, "objc": outputs["objc"] + protocol})
        self.assertEqual(result["method_count"], 8)
        protocol = protocol.replace("-[NSObject", "0x100000c88 -[NSObject")
        self.assert_unknown(self.analyze(data, {**outputs, "objc": outputs["objc"] + protocol}), "addressed protocol")

    def test_unsupported_method_flags_and_bad_relative_pointer_are_unknown(self):
        data, outputs = fixture()
        struct.pack_into("<I", data, 0x13A0, 0xC000000C)
        self.assert_unknown(self.analyze(data, outputs), "unsupported method-list flags")
        data, outputs = fixture()
        struct.pack_into("<i", data, 0x13A8, 0)
        self.assert_unknown(self.analyze(data, outputs), "relative method field disagrees")

    def test_missing_fixups_unknown_format_or_wrong_arch_never_certifies(self):
        data, outputs = fixture()
        self.assert_unknown(self.analyze(data, {key: value for key, value in outputs.items() if key != "fixups"}),
                            "required independent evidence is missing")
        self.assert_unknown(self.analyze(data, {**outputs, "fixups": outputs["fixups"] + "unsupported variant format\n"}),
                            "unrecognized Apple fixup row")
        self.assert_unknown(self.analyze(data, outputs, architecture="x86_64"), "only matching thin")

    def test_artifact_sha_and_budget_failures_remain_unknown(self):
        data, outputs = fixture()
        self.assert_unknown(self.analyze(data, outputs, expected_sha="0" * 64), "artifact SHA-256")

        def expired():
            raise RuntimeError("case deadline exhausted")

        self.assert_unknown(self.analyze(data, outputs, check_budget=expired), "case deadline exhausted")
        with patch.object(oracle, "MAX_IO", 64):
            self.assert_unknown(self.analyze(data, outputs), "cumulative read budget")
        with patch.object(oracle, "MAX_RECORDS", 4):
            self.assert_unknown(self.analyze(data, outputs), "selector-reference budget")
        with patch.object(oracle, "MAX_METHOD_OUTPUT", 1024):
            self.assert_unknown(self.analyze(data, outputs), "method evidence output budget")

    def test_short_strings_reuse_bounded_cache_instead_of_charging_16k(self):
        data, outputs = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "fixture"
            binary.write_bytes(data)
            disk = oracle.Disk(binary, "arm64", lambda: None, hashlib.sha256(data).hexdigest())
            try:
                before = disk.io
                for _ in range(100):
                    self.assertEqual(disk.string(BASE + 0x1674), "v16@0:8")
                self.assertLessEqual(disk.io - before, 256)
                self.assertEqual(len(disk.strings), 1)
            finally:
                disk.stream.close()


if __name__ == "__main__":
    unittest.main()
