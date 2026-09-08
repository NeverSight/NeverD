"""Bounded standard DEX reader for the shared Dalvik source model."""
from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import struct
import zlib

from .common import MobileError
from .dalvik_model import (Budget, Class, Field, FieldRef, Instruction, Method,
                           MethodRef, TryRegion, access_flags, descriptor, width)


def _bad(message: str) -> None:
    raise MobileError("Invalid DEX: " + message)


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class _Cursor:
    def __init__(self, data: bytes, offset: int, end: int, budget: Budget):
        self.data, self.pos, self.end, self.budget = data, offset, end, budget
        if not 0 <= offset <= end <= len(data):
            _bad("item lies outside its section")

    def take(self, size: int) -> bytes:
        self.budget.tick(1 + size // 16)
        if size < 0 or size > self.end - self.pos:
            _bad("truncated item")
        start = self.pos
        self.pos += size
        return self.data[start:self.pos]

    def integer(self, size: int) -> int:
        return int.from_bytes(self.take(size), "little")

    def u8(self) -> int: return self.integer(1)
    def u16(self) -> int: return self.integer(2)
    def u32(self) -> int: return self.integer(4)

    def leb(self, signed: bool = False) -> int:
        value = 0
        for index in range(5):
            byte = self.u8()
            value |= (byte & 0x7f) << (index * 7)
            if not byte & 0x80:
                bits = (index + 1) * 7
                if signed:
                    value = _signed(value, bits)
                    if not -(1 << 31) <= value < (1 << 31):
                        _bad("SLEB128 overflow")
                elif value > 0xffffffff:
                    _bad("ULEB128 overflow")
                return value
        _bad("unterminated LEB128")


@dataclass(frozen=True)
class _Value:
    kind: int
    value: object


# Opcode names and layouts are the standard DEX instruction vocabulary.
_OPS: dict[int, tuple[str, str, str | None]] = {}


def _op(code: int, name: str, form: str, reference: str | None = None) -> None:
    _OPS[code] = (name, form, reference)


for base, name in ((1, "move"), (4, "move-wide"), (7, "move-object")):
    for delta, suffix, form in ((0, "", "12x"), (1, "/from16", "22x"), (2, "/16", "32x")):
        _op(base + delta, name + suffix, form)
for code, name in enumerate(("move-result", "move-result-wide", "move-result-object", "move-exception"), 0xa):
    _op(code, name, "11x")
_op(0, "nop", "10x")
_op(0xe, "return-void", "10x")
for code, name in enumerate(("return", "return-wide", "return-object"), 0xf): _op(code, name, "11x")
for code, name, form in ((0x12, "const/4", "11n"), (0x13, "const/16", "21s"),
                         (0x14, "const", "31i"), (0x15, "const/high16", "21h"),
                         (0x16, "const-wide/16", "21s"), (0x17, "const-wide/32", "31i"),
                         (0x18, "const-wide", "51l"), (0x19, "const-wide/high16", "21h")):
    _op(code, name, form)
_op(0x1a, "const-string", "21c", "string")
_op(0x1b, "const-string/jumbo", "31c", "string")
_op(0x1c, "const-class", "21c", "type")
_op(0x1d, "monitor-enter", "11x")
_op(0x1e, "monitor-exit", "11x")
_op(0x1f, "check-cast", "21c", "type")
_op(0x20, "instance-of", "22c", "type")
_op(0x21, "array-length", "12x")
_op(0x22, "new-instance", "21c", "type")
_op(0x23, "new-array", "22c", "type")
_op(0x24, "filled-new-array", "35c", "type")
_op(0x25, "filled-new-array/range", "3rc", "type")
_op(0x26, "fill-array-data", "31t")
_op(0x27, "throw", "11x")
for code, form in ((0x28, "10t"), (0x29, "20t"), (0x2a, "30t")):
    _op(code, {0x28: "goto", 0x29: "goto/16", 0x2a: "goto/32"}[code], form)
_op(0x2b, "packed-switch", "31t")
_op(0x2c, "sparse-switch", "31t")
for code, name in enumerate(("cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double", "cmp-long"), 0x2d):
    _op(code, name, "23x")
for index, name in enumerate(("eq", "ne", "lt", "ge", "gt", "le")):
    _op(0x32 + index, "if-" + name, "22t")
    _op(0x38 + index, "if-" + name + "z", "21t")
for base, name, form, ref in ((0x44, "aget", "23x", None), (0x4b, "aput", "23x", None),
                             (0x52, "iget", "22c", "field"), (0x59, "iput", "22c", "field"),
                             (0x60, "sget", "21c", "field"), (0x67, "sput", "21c", "field")):
    for index, suffix in enumerate(("", "-wide", "-object", "-boolean", "-byte", "-char", "-short")):
        _op(base + index, name + suffix, form, ref)
for index, name in enumerate(("virtual", "super", "direct", "static", "interface")):
    _op(0x6e + index, "invoke-" + name, "35c", "method")
    _op(0x74 + index, "invoke-" + name + "/range", "3rc", "method")
for code, name in enumerate(("neg-int", "not-int", "neg-long", "not-long", "neg-float", "neg-double",
                             "int-to-long", "int-to-float", "int-to-double", "long-to-int", "long-to-float",
                             "long-to-double", "float-to-int", "float-to-long", "float-to-double",
                             "double-to-int", "double-to-long", "double-to-float", "int-to-byte", "int-to-char", "int-to-short"), 0x7b):
    _op(code, name, "12x")
_BINARY = [operation + "-" + typ for typ in ("int", "long", "float", "double")
           for operation in (("add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "ushr")
                             if typ in ("int", "long") else ("add", "sub", "mul", "div", "rem"))]
for index, name in enumerate(_BINARY):
    _op(0x90 + index, name, "23x")
    _op(0xb0 + index, name + "/2addr", "12x")
for index, name in enumerate(("add", "rsub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "ushr")):
    if index < 8: _op(0xd0 + index, name + "-int" + ("/lit16" if name != "rsub" else ""), "22s")
    _op(0xd8 + index, name + "-int/lit8", "22b")


class _Dex:
    def __init__(self, data: bytes, source_id: str, budget: Budget):
        self.data, self.source_id, self.budget = data, source_id, budget
        self.sections: dict[int, tuple[int, int, int]] = {}
        self.cache: dict[tuple[int, int], object] = {}
        self.ranges: dict[tuple[int, int], int] = {}
        self.active: set[tuple[int, int]] = set()
        self.contexts: dict[tuple[int, int], object] = {}
        self.strings: list[str] = []
        self.types: list[str] = []
        self.type_indices: dict[str, int] = {}
        self.protos: list[tuple[tuple[str, ...], str]] = []
        self.fields: list[FieldRef] = []
        self.methods: list[MethodRef] = []
        self.members: dict[str, tuple[str, ...]] = {}

    def cursor(self, offset: int, end: int | None = None) -> _Cursor:
        return _Cursor(self.data, offset, len(self.data) if end is None else end, self.budget)

    @staticmethod
    def at(values, index: int, name: str):
        if not 0 <= index < len(values): _bad(name + " index out of bounds")
        return values[index]

    def string(self, index: int) -> str: return self.at(self.strings, index, "string")
    def typ(self, index: int, *, void: bool = False) -> str:
        value = self.at(self.types, index, "type")
        if value == "V" and not void: _bad("void outside return type")
        return value

    def item(self, kind: int, offset: int, read, alignment: int = 1):
        key = (kind, offset)
        if key in self.cache: return self.cache[key]
        if key in self.active: _bad("cyclic data item reference")
        section = self.sections.get(kind)
        if not section or offset % alignment or not section[0] <= offset < section[1]:
            _bad(f"item 0x{kind:x} points outside its mapped section")
        reader = self.cursor(offset, section[1])
        self.active.add(key)
        try:
            result = read(reader)
        finally:
            self.active.remove(key)
        self.cache[key] = result
        self.ranges[key] = reader.pos
        return result

    def context(self, kind: int, offset: int, identity: object) -> None:
        key = (kind, offset)
        if key in self.contexts and self.contexts[key] != identity:
            _bad("shared data item has inconsistent declaration context")
        self.contexts[key] = identity

    def header(self) -> dict[int, tuple[int, int]]:
        self.budget.tick(1 + len(self.data) // 32)
        if len(self.data) > self.budget.limits.max_bytes: _bad("input exceeds byte limit")
        if len(self.data) < 112 or self.data[:4] != b"dex\n" or self.data[7] != 0:
            _bad("missing standard header")
        if self.data[4:7] not in (b"035", b"037", b"038", b"039", b"040"):
            _bad("unsupported DEX version (supported: 035, 037–040; 041 containers are unsupported)")
        self.version = int(self.data[4:7])
        head = self.cursor(8, 112)
        checksum, signature = head.u32(), head.take(20)
        if zlib.adler32(memoryview(self.data)[12:]) & 0xffffffff != checksum:
            _bad("checksum mismatch")
        if hashlib.sha1(memoryview(self.data)[32:]).digest() != signature:
            _bad("signature mismatch")
        size, header_size, endian = head.u32(), head.u32(), head.u32()
        if size != len(self.data) or header_size != 112: _bad("header/file size mismatch")
        if endian != 0x12345678: _bad("unsupported or invalid endian tag")
        link_size, link_off, map_off = head.u32(), head.u32(), head.u32()
        if link_size or link_off: _bad("statically linked DEX data is unsupported")
        tables = {kind: (head.u32(), head.u32()) for kind in range(1, 7)}
        data_size, data_off = head.u32(), head.u32()
        if data_off < 112 or data_off % 4 or data_size != size - data_off:
            _bad("invalid data section bounds")
        if map_off % 4 or not data_off <= map_off <= size - 4: _bad("invalid map offset")
        reader = self.cursor(map_off)
        count = reader.u32()
        if not 1 <= count <= 32: _bad("invalid section map size")
        entries = []
        fixed = {0: 112, 1: 4, 2: 4, 3: 12, 4: 8, 5: 8, 6: 32, 7: 4, 8: 8}
        variable = {0x1000, 0x1001, 0x1002, 0x1003, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006}
        for _ in range(count):
            kind, reserved, length, start = reader.u16(), reader.u16(), reader.u32(), reader.u32()
            if reserved or not length or start >= size or (entries and start <= entries[-1][1]):
                _bad("unordered, empty or invalid section map")
            if kind not in fixed and kind not in variable: _bad(f"unsupported map section 0x{kind:x}")
            if kind in (7, 8): _bad("method handles and custom call sites are unsupported")
            entries.append((kind, start, length))
        for index, (kind, start, length) in enumerate(entries):
            end = entries[index + 1][1] if index + 1 < len(entries) else size
            if kind in self.sections: _bad("duplicate map section")
            if kind in fixed and (start % 4 or length > (end - start) // fixed[kind]):
                _bad("overlapping or truncated fixed table")
            if kind >= 0x1000 and start < data_off: _bad("data item outside data section")
            self.sections[kind] = (start, end, length)
        if self.sections.get(0, ())[:1] != (0,) or self.sections[0][2] != 1:
            _bad("map lacks the unique header")
        if self.sections.get(0x1000, ())[:1] != (map_off,) or self.sections[0x1000][2] != 1 or reader.pos > self.sections[0x1000][1]:
            _bad("map does not describe itself")
        for kind, (length, start) in tables.items():
            section = self.sections.get(kind)
            if length == 0:
                if start or section: _bad("empty table has storage")
            elif not section or (section[0], section[2]) != (start, length) or start < 112 or section[1] > data_off:
                _bad("header and map tables disagree")
            if kind in (2, 3) and length > 65535: _bad("type/prototype table exceeds index limit")
            self.budget.tick(length)
        return tables

    def mutf8(self, reader: _Cursor) -> str:
        expected = reader.leb()
        if expected > reader.end - reader.pos: _bad("impossible UTF-16 string length")
        units = bytearray()
        length = 0
        while True:
            first = reader.u8()
            if first == 0: break
            if first < 0x80:
                unit = first
            elif 0xc0 <= first <= 0xdf:
                second = reader.u8()
                if second & 0xc0 != 0x80: _bad("invalid MUTF-8 continuation")
                unit = ((first & 31) << 6) | (second & 63)
                if unit < 0x80 and unit != 0: _bad("overlong MUTF-8")
            elif 0xe0 <= first <= 0xef:
                second, third = reader.u8(), reader.u8()
                if second & 0xc0 != 0x80 or third & 0xc0 != 0x80: _bad("invalid MUTF-8 continuation")
                unit = ((first & 15) << 12) | ((second & 63) << 6) | (third & 63)
                if unit < 0x800: _bad("overlong MUTF-8")
            else:
                _bad("invalid MUTF-8 leading byte")
            units += unit.to_bytes(2, "little")
            length += 1
            if length > expected: _bad("UTF-16 string length mismatch")
        if length != expected: _bad("UTF-16 string length mismatch")
        return units.decode("utf-16-le", errors="surrogatepass")

    def type_list(self, offset: int) -> tuple[str, ...]:
        if offset == 0: return ()
        def read(reader):
            size = reader.u32()
            if size > (reader.end - reader.pos) // 2: _bad("truncated type list")
            return tuple(self.typ(reader.u16()) for _ in range(size))
        return self.item(0x1001, offset, read, 4)

    def tables(self, tables: dict[int, tuple[int, int]]) -> None:
        def records(kind):
            count, offset = tables[kind]
            return count, self.cursor(offset, self.sections[kind][1] if count else offset)
        count, reader = records(1)
        for _ in range(count): self.strings.append(self.item(0x2002, reader.u32(), self.mutf8))
        keys = [value.encode("utf-16-be", errors="surrogatepass") for value in self.strings]
        if any(a >= b for a, b in zip(keys, keys[1:])): _bad("string IDs are duplicate or unordered")
        count, reader = records(2)
        previous = -1
        for _ in range(count):
            index = reader.u32()
            if index <= previous: _bad("type IDs are duplicate or unordered")
            previous = index
            self.types.append(descriptor(self.string(index), allow_void=True))
        count, reader = records(3)
        previous = None
        type_indices = {value: index for index, value in enumerate(self.types)}
        self.type_indices = type_indices
        for _ in range(count):
            shorty, result, parameters = reader.u32(), reader.u32(), reader.u32()
            returns, args = self.typ(result, void=True), self.type_list(parameters)
            expected = "".join("L" if typ.startswith(("L", "[")) else typ for typ in (returns, *args))
            if self.string(shorty) != expected: _bad("prototype shorty disagrees with descriptors")
            key = (result, tuple(type_indices[p] for p in args))
            if previous is not None and key <= previous: _bad("prototype IDs are duplicate or unordered")
            previous = key
            self.protos.append((args, returns))
        for kind in (4, 5):
            count, reader = records(kind)
            previous = None
            for _ in range(count):
                owner, typ, name = reader.u16(), reader.u16(), reader.u32()
                owner_name, member = self.typ(owner), self.string(name)
                if not owner_name.startswith("L") or not member: _bad("invalid member owner/name")
                key = (owner, name, typ)
                if previous is not None and key <= previous: _bad("member IDs are duplicate or unordered")
                previous = key
                if kind == 4: self.fields.append(FieldRef(owner_name, member, self.typ(typ)))
                else:
                    args, result = self.at(self.protos, typ, "prototype")
                    self.methods.append(MethodRef(owner_name, member, args, result))

    def encoded(self, reader: _Cursor, depth: int = 0) -> _Value:
        if depth > 64: _bad("encoded value nesting exceeds limit")
        tag = reader.u8()
        kind, arg = tag & 31, tag >> 5
        sizes = {0x00: 1, 0x02: 2, 0x03: 2, 0x04: 4, 0x06: 8, 0x10: 4, 0x11: 8,
                 0x15: 4, 0x16: 4, 0x17: 4, 0x18: 4, 0x19: 4, 0x1a: 4, 0x1b: 4}
        if kind in sizes:
            size = arg + 1
            if size > sizes[kind]: _bad("encoded value has invalid width")
            value = reader.integer(size)
            if kind in (0, 2, 4, 6): value = _signed(value, size * 8)
            elif kind in (0x10, 0x11):
                full = sizes[kind]
                bits = value << ((full - size) * 8)
                value = {"kind": "float-bits" if full == 4 else "double-bits", "bits": bits}
            elif kind == 0x17: value = self.string(value)
            elif kind == 0x18: value = self.typ(value, void=True)
            elif kind in (0x19, 0x1b): value = self.at(self.fields, value, "field")
            elif kind == 0x1a: value = self.at(self.methods, value, "method")
            elif kind == 0x15:
                args, result = self.at(self.protos, value, "prototype")
                value = "(" + "".join(args) + ")" + result
            elif kind == 0x16: _bad("encoded method handles are unsupported")
            return _Value(kind, value)
        if kind == 0x1f and arg <= 1: return _Value(kind, bool(arg))
        if arg: _bad("encoded value has invalid argument bits")
        if kind == 0x1e: return _Value(kind, None)
        if kind == 0x1c:
            count = reader.leb()
            self.budget.tick(count)
            return _Value(kind, tuple(self.encoded(reader, depth + 1) for _ in range(count)))
        if kind == 0x1d: return _Value(kind, self.annotation(reader, depth + 1))
        _bad(f"unknown encoded value 0x{kind:x}")

    def annotation(self, reader: _Cursor, depth: int = 0):
        if depth > 64: _bad("annotation nesting exceeds limit")
        typ = self.typ(reader.leb())
        if not typ.startswith("L"): _bad("annotation type is not a class")
        count = reader.leb()
        self.budget.tick(count)
        elements, previous = {}, -1
        for _ in range(count):
            name = reader.leb()
            if name <= previous: _bad("annotation elements are duplicate or unordered")
            previous = name
            elements[self.string(name)] = self.encoded(reader, depth + 1)
        return typ, elements

    def annotation_set(self, offset: int):
        if not offset: return []
        def read(reader):
            count = reader.u32()
            if count > (reader.end - reader.pos) // 4: _bad("truncated annotation set")
            result = []
            for _ in range(count):
                def annotation_item(r):
                    visibility = r.u8()
                    if visibility > 2: _bad("invalid annotation visibility")
                    return self.annotation(r)
                result.append(self.item(0x2004, reader.u32(), annotation_item))
            indices = [self.type_indices[typ] for typ, _ in result]
            if any(a >= b for a, b in zip(indices, indices[1:])): _bad("annotation types are duplicate or unordered")
            return result
        return self.item(0x1003, offset, read, 4)

    def annotations(self, cls: Class, offset: int) -> None:
        if not offset: return
        def read(reader):
            class_off = reader.u32()
            counts = (reader.u32(), reader.u32(), reader.u32())
            for kind, count in enumerate(counts):
                if count > (reader.end - reader.pos) // 8: _bad("truncated annotation directory")
                previous = -1
                for _ in range(count):
                    index, annotations = reader.u32(), reader.u32()
                    if index <= previous: _bad("annotation directory members unordered")
                    previous = index
                    reference = self.at(self.fields if kind == 0 else self.methods, index, "annotated member")
                    if reference.owner != cls.name: _bad("annotation directory owner mismatch")
                    if kind < 2: self.annotation_set(annotations)
                    else:
                        def parameters(r):
                            length = r.u32()
                            if length != len(reference.parameters): _bad("parameter annotation count mismatch")
                            return [self.annotation_set(r.u32()) for _ in range(length)]
                        self.item(0x1002, annotations, parameters, 4)
            return self.annotation_set(class_off), any(counts)
        annotations, has_members = self.item(0x2006, offset, read, 4)
        if has_members: self.context(0x2006, offset, cls.name)
        values = dict(annotations)
        enclosing = values.get("Ldalvik/annotation/EnclosingClass;")
        inner = values.get("Ldalvik/annotation/InnerClass;")
        if enclosing is not None:
            value = enclosing.get("value")
            if set(enclosing) != {"value"} or not value or value.kind != 0x18 or not str(value.value).startswith("L"):
                _bad("invalid EnclosingClass annotation")
            cls.enclosing = value.value
        if "Ldalvik/annotation/EnclosingMethod;" in values:
            _bad("method-local/anonymous class source context is unsupported")
        if inner is not None:
            name, flags = inner.get("name"), inner.get("accessFlags")
            if set(inner) != {"name", "accessFlags"} or not name or name.kind not in (0x17, 0x1e) or not flags or flags.kind != 4:
                _bad("invalid InnerClass annotation")
            cls.inner_name, cls.inner_access = name.value, access_flags(flags.value)
        if (inner is None) != (enclosing is None): _bad("incomplete inner class metadata")
        members = values.get("Ldalvik/annotation/MemberClasses;")
        if members is not None:
            value = members.get("value")
            if set(members) != {"value"} or not value or value.kind != 0x1c or any(v.kind != 0x18 or not str(v.value).startswith("L") for v in value.value):
                _bad("invalid MemberClasses annotation")
            names = tuple(v.value for v in value.value)
            if len(set(names)) != len(names): _bad("duplicate MemberClasses entry")
            self.members[cls.name] = names

    def payload(self, words: tuple[int, ...], pc: int):
        ident = words[pc]
        if pc % 2 or pc + 2 > len(words): _bad("unaligned/truncated payload")
        count = words[pc + 1]
        def value(at):
            if at + 2 > len(words): _bad("truncated payload value")
            return words[at] | words[at + 1] << 16
        if ident in (0x100, 0x200):
            size = 4 + count * 2 if ident == 0x100 else 2 + count * 4
            if pc + size > len(words): _bad("truncated switch payload")
            self.budget.tick(count)
            if ident == 0x100:
                first = _signed(value(pc + 2), 32)
                if count and first + count - 1 > 0x7fffffff: _bad("packed switch key overflow")
                keys, start = tuple(range(first, first + count)), pc + 4
            else:
                keys = tuple(_signed(value(pc + 2 + i * 2), 32) for i in range(count))
                if any(a >= b for a, b in zip(keys, keys[1:])): _bad("sparse switch keys unordered")
                start = pc + 2 + count * 2
            targets = tuple(_signed(value(start + i * 2), 32) for i in range(count))
            return size, ident, keys, targets, (), 0
        if ident == 0x300:
            element_width, count = count, value(pc + 2)
            if element_width not in (1, 2, 4, 8): _bad("invalid array payload element width")
            size = 4 + (count * element_width + 1) // 2
            if pc + size > len(words): _bad("truncated array payload")
            self.budget.tick(count * element_width)
            raw = b"".join(w.to_bytes(2, "little") for w in words[pc + 4:pc + size])
            data = tuple(int.from_bytes(raw[i * element_width:(i + 1) * element_width], "little") for i in range(count))
            return size, ident, (), (), data, element_width
        _bad("unknown payload pseudo-opcode")

    def instructions(self, words: tuple[int, ...], registers: int, outs: int):
        instructions, lengths, payloads = [], {}, {}
        pc = 0
        while pc < len(words):
            self.budget.tick()
            word, opcode = words[pc], words[pc] & 255
            if opcode == 0 and word:
                payload = self.payload(words, pc)
                payloads[pc] = payload
                pc += payload[0]
                continue
            if opcode not in _OPS: _bad(f"unsupported opcode 0x{opcode:02x} at code unit {pc}")
            name, form, pool = _OPS[opcode]
            size = int(form[0])
            if pc + size > len(words): _bad("truncated instruction")
            high = word >> 8
            tail = words[pc + 1:pc + size]
            regs, literal, target, index = (), None, None, None
            if form in ("10x", "20t", "30t", "32x") and high: _bad("nonzero reserved instruction bits")
            if form == "12x": regs = (high & 15, high >> 4)
            elif form == "11x": regs = (high,)
            elif form == "11n": regs, literal = (high & 15,), _signed(high >> 4, 4)
            elif form == "22x": regs = (high, tail[0])
            elif form == "32x": regs = (tail[0], tail[1])
            elif form == "23x": regs = (high, tail[0] & 255, tail[0] >> 8)
            elif form == "22b": regs, literal = (high, tail[0] & 255), _signed(tail[0] >> 8, 8)
            elif form in ("21s", "21h", "31i", "51l"):
                regs = (high,)
                raw = sum(value << (16 * i) for i, value in enumerate(tail))
                literal = _signed(raw, 16 * len(tail))
                if form == "21h": literal <<= 48 if opcode == 0x19 else 16
            elif form == "22s": regs, literal = (high & 15, high >> 4), _signed(tail[0], 16)
            elif form in ("21c", "31c"):
                regs, index = (high,), sum(value << (16 * i) for i, value in enumerate(tail))
            elif form == "22c": regs, index = (high & 15, high >> 4), tail[0]
            elif form in ("21t", "22t", "31t"):
                regs = (high & 15, high >> 4) if form == "22t" else (high,)
                target = pc + _signed(sum(value << (16 * i) for i, value in enumerate(tail)), 16 * len(tail))
            elif form in ("10t", "20t", "30t"):
                target = pc + (_signed(high, 8) if form == "10t" else _signed(sum(v << (16 * i) for i, v in enumerate(tail)), 16 * len(tail)))
            elif form == "35c":
                count, index = high >> 4, tail[0]
                if count > 5: _bad("invoke register count exceeds instruction format")
                regs = tuple((tail[1] >> (4 * i)) & 15 for i in range(min(count, 4)))
                if count == 5: regs += (high & 15,)
            elif form == "3rc":
                index = tail[0]
                regs = tuple(range(tail[1], tail[1] + high))
            if any(reg >= registers for reg in regs): _bad("instruction register exceeds frame")
            reference = None
            if pool == "string": literal = self.string(index)
            elif pool == "type": reference = self.typ(index)
            elif pool == "field": reference = self.at(self.fields, index, "field")
            elif pool == "method":
                reference = self.at(self.methods, index, "method")
                expected = sum(width(typ) for typ in reference.parameters) + (not name.startswith("invoke-static"))
                if len(regs) != expected or expected > outs: _bad("invoke argument words disagree with prototype/frame")
                word_index = int(not name.startswith("invoke-static"))
                for typ in reference.parameters:
                    if width(typ) == 2 and regs[word_index + 1] != regs[word_index] + 1:
                        _bad("invoke wide argument is not an adjacent register pair")
                    word_index += width(typ)
            if name.startswith("filled-new-array") and (not reference.startswith("[") or reference[1:] in ("J", "D")):
                _bad("filled-new-array requires a single-word array component")
            if name == "new-instance" and not reference.startswith("L"): _bad("new-instance requires class type")
            if name == "new-array" and not reference.startswith("["): _bad("new-array requires array type")
            if name in ("check-cast", "instance-of") and not reference.startswith(("L", "[")): _bad("reference operation requires object type")
            self.wide_registers(name, regs, registers)
            instructions.append(Instruction(pc, name, regs, literal, target, reference))
            lengths[pc] = size
            pc += size
        starts = set(lengths)
        result, used_payloads = [], set()
        for instruction in instructions:
            pc, name = instruction.pc, instruction.opcode
            if name in ("packed-switch", "sparse-switch", "fill-array-data"):
                payload = payloads.get(instruction.target)
                expected = {"packed-switch": 0x100, "sparse-switch": 0x200, "fill-array-data": 0x300}[name]
                if not payload or payload[1] != expected: _bad("instruction has missing/mismatched payload")
                used_payloads.add(instruction.target)
                targets = tuple(pc + relative for relative in payload[3])
                if any(target not in starts for target in targets): _bad("switch target is not an instruction boundary")
                instruction = replace(instruction, keys=payload[2], targets=targets, data=payload[4], element_width=payload[5])
            elif instruction.target is not None:
                if instruction.target not in starts: _bad("branch target is not an instruction boundary")
                if instruction.target == pc and name != "goto/32": _bad("zero branch displacement")
            result.append(instruction)
        if used_payloads != set(payloads): _bad("unreferenced payload")
        # Padding nops may precede a payload after a terminal instruction. Only
        # reachable fallthrough is forbidden; these alignment nops are not code.
        reachable, pending = set(), [0]
        by_pc = {ins.pc: ins for ins in result}
        while pending:
            self.budget.tick()
            at = pending.pop()
            if at in reachable: continue
            if at not in by_pc: _bad("execution falls into payload or beyond code")
            reachable.add(at)
            ins = by_pc[at]
            if ins.opcode.startswith("return") or ins.opcode == "throw": continue
            if ins.opcode.startswith("goto"):
                pending.append(ins.target)
            else:
                pending.append(at + lengths[at])
                if ins.opcode.startswith("if-"): pending.append(ins.target)
                pending.extend(ins.targets)
        return result, lengths

    @staticmethod
    def wide_registers(name: str, registers: tuple[int, ...], count: int) -> None:
        wide = set()
        base = name.split("/")[0]
        if base in ("move-wide",): wide = {0, 1}
        elif base in ("move-result-wide", "return-wide", "const-wide", "aget-wide", "aput-wide", "iget-wide", "iput-wide", "sget-wide", "sput-wide"):
            wide = {0}
        elif "-to-" in base:
            source, dest = base.split("-to-")
            if source in ("long", "double"): wide.add(1)
            if dest in ("long", "double"): wide.add(0)
        elif base in ("cmp-long", "cmpl-double", "cmpg-double"): wide = {1, 2}
        elif base.endswith(("-long", "-double")):
            wide = set(range(len(registers)))
            if base.startswith(("shl-", "shr-", "ushr-")): wide.discard(len(registers) - 1)
        if any(registers[index] + 1 >= count for index in wide): _bad("wide register pair exceeds frame")

    def debug_info(self, offset: int, registers: int, code_end: int, parameter_count: int) -> None:
        if not offset: return
        def read(reader):
            line, count = reader.leb(), reader.leb()
            if line < 1 or count != parameter_count: _bad("debug header disagrees with method")
            def optional_string():
                index = reader.leb() - 1
                if index >= 0: self.string(index)
            for _ in range(count): optional_string()
            address = 0
            while True:
                opcode = reader.u8()
                if opcode == 0: break
                if opcode == 1: address += reader.leb()
                elif opcode == 2: line += reader.leb(True)
                elif opcode in (3, 4):
                    if reader.leb() >= registers: _bad("debug register outside frame")
                    optional_string()
                    typ = reader.leb() - 1
                    if typ >= 0: self.typ(typ)
                    if opcode == 4: optional_string()
                elif opcode in (5, 6):
                    if reader.leb() >= registers: _bad("debug register outside frame")
                elif opcode in (7, 8): pass
                elif opcode == 9: optional_string()
                else:
                    address += (opcode - 10) // 15
                    line += (opcode - 10) % 15 - 4
                if address > code_end or line < 1: _bad("debug position outside method")
            return None
        # Identical debug streams may be shared by methods with different
        # frames. Recheck the stream against each method's actual bounds.
        self.cache.pop((0x2003, offset), None)
        self.item(0x2003, offset, read)

    def code_flow(self, instructions, lengths, tries) -> None:
        by_pc = {instruction.pc: instruction for instruction in instructions}
        handlers = {target for region in tries for _, target in region.handlers}
        explicit_targets = handlers | {target for ins in instructions for target in ins.targets}
        explicit_targets.update(ins.target for ins in instructions if ins.opcode.startswith(("if-", "goto")))
        previous = None
        for instruction in instructions:
            if instruction.opcode == "move-exception" and instruction.pc not in handlers:
                _bad("move-exception outside an exception handler entry")
            if instruction.opcode.startswith("move-result"):
                if instruction.pc in explicit_targets or previous is None or previous.pc + lengths[previous.pc] != instruction.pc:
                    _bad("move-result has an invalid control-flow predecessor")
                if previous.opcode.startswith("invoke-") and isinstance(previous.reference, MethodRef):
                    result = previous.reference.returns
                elif previous.opcode.startswith("filled-new-array"):
                    result = previous.reference
                else:
                    _bad("move-result does not immediately follow an invocation")
                expected = ("move-result-wide" if result in ("J", "D") else
                            "move-result-object" if result.startswith(("L", "[")) else "move-result")
                if result == "V" or instruction.opcode != expected: _bad("move-result kind disagrees with invocation result")
            previous = instruction
        pending, reached = [(0, False)] + [(target, True) for target in handlers], set()
        while pending:
            self.budget.tick()
            pc, exception_edge = pending.pop()
            if pc not in by_pc: _bad("normal or handler execution falls outside executable instructions")
            instruction = by_pc[pc]
            if instruction.opcode == "move-exception" and not exception_edge:
                _bad("normal execution enters move-exception")
            if pc in reached: continue
            reached.add(pc)
            if instruction.opcode.startswith("return") or instruction.opcode == "throw": continue
            if instruction.opcode.startswith("goto"):
                pending.append((instruction.target, False))
            else:
                pending.append((pc + lengths[pc], False))
                if instruction.opcode.startswith("if-"): pending.append((instruction.target, False))
                pending.extend((target, False) for target in instruction.targets)

    def code(self, offset: int, method: Method) -> None:
        self.context(0x2001, offset, (method.reference.parameters, method.reference.returns, "static" in method.access))
        def read(reader):
            registers, incoming, outgoing, tries_count = reader.u16(), reader.u16(), reader.u16(), reader.u16()
            debug, size = reader.u32(), reader.u32()
            if not size or size > (reader.end - reader.pos) // 2: _bad("empty/truncated method instructions")
            if incoming != method.incoming_words or incoming > registers: _bad("incoming register words disagree with method")
            raw = reader.take(size * 2)
            words = struct.unpack("<" + str(size) + "H", raw)
            instructions, lengths = self.instructions(words, registers, outgoing)
            starts = set(lengths)
            raw_tries = []
            if tries_count and size % 2:
                if reader.u16(): _bad("nonzero try alignment padding")
            for _ in range(tries_count):
                start, length, handler = reader.u32(), reader.u16(), reader.u16()
                end = start + length
                if not length or start not in starts or end > size or (end != size and end not in starts) or (raw_tries and start < raw_tries[-1][1]):
                    _bad("invalid or overlapping try range")
                raw_tries.append((start, end, handler))
            handlers = {}
            if tries_count:
                base = reader.pos
                count = reader.leb()
                self.budget.tick(count)
                for _ in range(count):
                    handler_offset = reader.pos - base
                    length = reader.leb(True)
                    self.budget.tick(abs(length))
                    entries, caught_types = [], set()
                    for _ in range(abs(length)):
                        typ, target = self.typ(reader.leb()), reader.leb()
                        if not typ.startswith("L") or target not in starts or typ in caught_types:
                            _bad("invalid or duplicate typed exception handler")
                        caught_types.add(typ)
                        entries.append((typ, target))
                    if length <= 0:
                        target = reader.leb()
                        if target not in starts: _bad("catch-all target is not an instruction")
                        entries.append((None, target))
                    handlers[handler_offset] = tuple(entries)
            tries = []
            for start, end, handler in raw_tries:
                if handler not in handlers: _bad("try references non-handler offset")
                tries.append(TryRegion(start, end, handlers[handler]))
            self.code_flow(instructions, lengths, tries)
            self.debug_info(debug, registers, size, len(method.reference.parameters))
            return registers, incoming, instructions, tries, size
        registers, incoming, instructions, tries, size = self.item(0x2001, offset, read, 4)
        if incoming != method.incoming_words: _bad("shared code item signature mismatch")
        method.registers, method.instructions, method.tries, method.code_end = registers, list(instructions), list(tries), size

    def class_data(self, cls: Class, offset: int) -> None:
        if not offset: return
        self.context(0x2000, offset, cls.name)
        def read(reader):
            counts = [reader.leb() for _ in range(4)]
            self.budget.tick(sum(counts))
            seen_fields, seen_methods = set(), set()
            for group, count in enumerate(counts):
                index = 0
                for member_index in range(count):
                    diff, flags = reader.leb(), reader.leb()
                    if member_index and diff == 0: _bad("duplicate class-data member index")
                    index += diff
                    access = access_flags(flags)
                    reference = self.at(self.fields if group < 2 else self.methods, index, "defined member")
                    if reference.owner != cls.name: _bad("class-data member owner mismatch")
                    seen = seen_fields if group < 2 else seen_methods
                    if index in seen: _bad("duplicate defined member")
                    seen.add(index)
                    if group < 2:
                        if ("static" in access) != (group == 0): _bad("field storage/access mismatch")
                        cls.fields.append(Field(reference, access))
                    else:
                        if "volatile" in access: access = (access - {"volatile"}) | {"bridge"}
                        if "transient" in access: access = (access - {"transient"}) | {"varargs"}
                        direct = bool(access & {"static", "private", "constructor"}) or reference.name in ("<init>", "<clinit>")
                        if direct != (group == 2): _bad("direct/virtual method classification mismatch")
                        code = reader.leb()
                        no_body = bool(access & {"native", "abstract"})
                        if bool(code) == no_body: _bad("method code/access mismatch")
                        method = Method(reference, frozenset(access), 0)
                        if code: self.code(code, method)
                        cls.methods.append(method)
            return None
        self.item(0x2000, offset, read)

    def static_values(self, cls: Class, offset: int) -> None:
        if not offset: return
        def read(reader):
            count = reader.leb()
            self.budget.tick(count)
            return tuple(self.encoded(reader) for _ in range(count))
        values = self.item(0x2005, offset, read)
        fields = [field for field in cls.fields if "static" in field.access]
        if len(values) > len(fields): _bad("static values exceed static field count")
        kinds = {"B": 0, "S": 2, "C": 3, "I": 4, "J": 6, "F": 0x10, "D": 0x11, "Z": 0x1f}
        for field, value in zip(fields, values):
            typ = field.reference.type
            if typ in kinds:
                if value.kind != kinds[typ]: _bad("static value type disagrees with field")
            elif value.kind == 0x17:
                if typ != "Ljava/lang/String;": _bad("string static value has incompatible field")
            elif value.kind != 0x1e:
                _bad("unsupported reference static initializer")
            field.value = value.value

    def parse(self) -> list[Class]:
        tables = self.header()
        self.tables(tables)
        count, offset = tables[6]
        if count > self.budget.limits.max_files: _bad("class inventory exceeds file limit")
        reader = self.cursor(offset, self.sections[6][1] if count else offset)
        classes, names = [], set()
        for _ in range(count):
            self.budget.tick()
            index, flags, parent, interfaces = reader.u32(), reader.u32(), reader.u32(), reader.u32()
            source, annotations, data, values = reader.u32(), reader.u32(), reader.u32(), reader.u32()
            name = self.typ(index)
            if not name.startswith("L") or name in names: _bad("invalid/duplicate class definition")
            names.add(name)
            superclass = None if parent == 0xffffffff else self.typ(parent)
            if superclass is not None and (not superclass.startswith("L") or superclass == name): _bad("invalid superclass")
            implemented = list(self.type_list(interfaces))
            if any(not typ.startswith("L") for typ in implemented) or len(set(implemented)) != len(implemented):
                _bad("invalid/duplicate interface")
            if source != 0xffffffff: self.string(source)
            cls = Class(name, superclass, access_flags(flags), self.source_id, implemented)
            self.class_data(cls, data)
            self.static_values(cls, values)
            self.annotations(cls, annotations)
            classes.append(cls)
        by_name = {cls.name: cls for cls in classes}
        for owner, members in self.members.items():
            for name in members:
                if name in by_name and by_name[name].enclosing != owner: _bad("MemberClasses/EnclosingClass disagreement")
        # Producers may retain an unused empty annotation set. Validate every
        # mapped set rather than treating lack of a reference as malformed.
        if 0x1003 in self.sections:
            offset, end, count = self.sections[0x1003]
            for _ in range(count):
                self.annotation_set(offset)
                offset = (self.ranges[(0x1003, offset)] + 3) & ~3
                if offset > end: _bad("annotation set exceeds mapped section")
        for kind, (start, end, count) in self.sections.items():
            if kind < 0x1000 or kind == 0x1000: continue
            ranges = sorted((offset, last) for (typ, offset), last in self.ranges.items() if typ == kind)
            if len(ranges) != count or not ranges or ranges[0][0] != start:
                _bad(f"mapped item inventory mismatch for section 0x{kind:x}")
            if any(last > next_start for (_, last), (next_start, _) in zip(ranges, ranges[1:])):
                _bad("overlapping variable-length data items")
        return classes


def parse_dex(data: bytes, *, input_id: str, budget: Budget) -> list[Class]:
    """Parse a standard DEX as typed declarations without executing input code."""
    if not isinstance(data, bytes): _bad("input must be bytes")
    return _Dex(data, input_id, budget).parse()
