"""Encode only the authored independent member class and lexical flags."""
from pathlib import Path
import struct
try:
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_member(root: Path, partial_finals: bool = False) -> dict:
    edits = []
    for path in sorted(root.rglob("*.class")):
        before = path.read_bytes()
        shape = class_records(before)
        data = bytearray(before)
        if path.name == "Scope$Cell.class":
            at = shape["pool_end"]
            flags = int.from_bytes(before[at:at + 2], "big")
            if flags != 0x0030:
                raise RuntimeError("Independent member binary flags changed")
            struct.pack_into(">H", data, at, flags | 0x1000)
            if partial_finals:
                pos = shape["pool_end"] + 6
                pos += 2 + 2 * int.from_bytes(before[pos:pos + 2], "big")
                count = int.from_bytes(before[pos:pos + 2], "big"); pos += 2
                found = 0
                for _ in range(count):
                    flags, name, descriptor, attributes = struct.unpack_from(">HHHH", before, pos)
                    if shape["strings"][name] in (b"value", b"token"):
                        expected = b"Ljava/lang/Object;" if shape["strings"][name] == b"value" else b"Lfixture/Carrier;"
                        if flags != 1 or shape["strings"][descriptor] != expected:
                            raise RuntimeError("Partial final field witness changed")
                        struct.pack_into(">H", data, pos, flags | 0x0010 | (0x1000 if shape["strings"][name] == b"token" else 0)); found += 1
                    pos += 8
                    for _ in range(attributes):
                        pos += 6 + int.from_bytes(before[pos + 2:pos + 6], "big")
                if found != 2:
                    raise RuntimeError("Partial final field witness omitted its declarations")
        changed = 0
        for name, start, stop in shape["rows"]:
            if name != b"InnerClasses": continue
            count = int.from_bytes(before[start + 6:start + 8], "big")
            if stop != start + 8 + count * 8:
                raise RuntimeError("Independent member lexical table changed")
            for at in range(start + 8, stop, 8):
                inner, outer, label, flags = struct.unpack_from(">HHHH", before, at)
                if inner and shape["strings"][shape["classes"][inner]] == b"fixture/Scope$Cell":
                    if flags != 0x001a:
                        raise RuntimeError("Independent member lexical flags changed")
                    struct.pack_into(">H", data, at + 6, (flags & ~0x0008) | 0x1000)
                    changed += 1
        if method_code(before) != method_code(bytes(data)):
            raise RuntimeError("Independent member flag encoding changed Code")
        path.write_bytes(data)
        if changed: edits.append({"file": path.relative_to(root).as_posix(), "inner_entries": changed})
    if not edits or not any(row["file"] == "fixture/Scope$Cell.class" for row in edits):
        raise RuntimeError("Independent member witness missed its declaration")
    return {"class_and_inner_synthetic_flags": "encoded", "inner_static_flag": "cleared",
            "all_code_and_descriptors": "unchanged", "partial_final_field": partial_finals, "lexical_entries": edits}
