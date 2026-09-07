"""Bounded Mach-O container and recoverable language metadata inspection."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass

from .common import MobileError


ARCHITECTURES = {0x0100000C: "arm64", 12: "arm", 0x01000007: "x86_64", 7: "i386"}
_PREFERENCE = ("arm64", "arm", "x86_64", "i386")
_THIN = {b"\xce\xfa\xed\xfe": False, b"\xcf\xfa\xed\xfe": True}
_FAT = {
    b"\xca\xfe\xba\xbe": (">", False),
    b"\xbe\xba\xfe\xca": ("<", False),
    b"\xca\xfe\xba\xbf": (">", True),
    b"\xbf\xba\xfe\xca": ("<", True),
}
_MAX_ITEMS = 100_000
_MAX_STRING = 16_384


def _range(data: bytes, offset: int, size: int, what: str) -> None:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise MobileError(f"Mach-O {what} extends outside the file")


def _unpack(data: bytes, fmt: str, offset: int, what: str) -> tuple:
    _range(data, offset, struct.calcsize(fmt), what)
    return struct.unpack_from(fmt, data, offset)


def _name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


@dataclass(frozen=True)
class Section:
    name: str
    segment: str
    address: int
    size: int
    offset: int
    flags: int


class MachO:
    """One validated, little-endian native slice; metadata is best-effort."""

    def __init__(self, data: bytes):
        self.data = data
        if data[:4] not in _THIN:
            raise MobileError("expected a little-endian Mach-O executable")
        self.is64 = _THIN[data[:4]]
        self.pointer_size = 8 if self.is64 else 4
        header_size = 32 if self.is64 else 28
        header = _unpack(data, "<7I", 0, "header")
        _range(data, 0, header_size, "header")
        self.cpu_type, self.cpu_subtype, self.file_type = header[1:4]
        self.architecture = ARCHITECTURES.get(self.cpu_type)
        if self.architecture is None or bool(self.cpu_type & 0x01000000) != self.is64:
            raise MobileError(f"unsupported Mach-O CPU type 0x{self.cpu_type:x}")
        ncmds, sizeofcmds = header[4:6]
        if ncmds > _MAX_ITEMS or ncmds > sizeofcmds // 8:
            raise MobileError("invalid Mach-O load-command count")
        _range(data, header_size, sizeofcmds, "load commands")
        self.sections: list[Section] = []
        self.segments: list[tuple[int, int, int, int]] = []
        self.encryption: list[dict] = []
        self.chained_fixups = False
        chained_command_seen = False
        self.symtab: tuple[int, int, int, int] | None = None
        command_end = header_size + sizeofcmds
        cursor = header_size
        for _ in range(ncmds):
            cmd, size = _unpack(data, "<II", cursor, "load command")
            if size < 8 or size % 4 or size > command_end - cursor:
                raise MobileError("invalid Mach-O load-command size")
            if cmd in (1, 0x19):
                self._segment(cursor, size, cmd == 0x19)
            elif cmd in (0x21, 0x2C):
                if size < (24 if cmd == 0x2C else 20):
                    raise MobileError("truncated Mach-O encryption command")
                offset, length, cryptid = _unpack(data, "<III", cursor + 8, "encryption")
                _range(data, offset, length, "encrypted range")
                self.encryption.append({"offset": offset, "size": length, "cryptid": cryptid})
            elif cmd == 2:
                if size < 24 or self.symtab is not None:
                    raise MobileError("invalid Mach-O symbol-table command")
                self.symtab = _unpack(data, "<4I", cursor + 8, "symbol table")
                symoff, count, stroff, strsize = self.symtab
                if count > _MAX_ITEMS:
                    raise MobileError("Mach-O symbol count exceeds the metadata limit")
                _range(data, symoff, count * (16 if self.is64 else 12), "symbol table")
                _range(data, stroff, strsize, "string table")
            elif cmd == 0x80000034:
                if size < 16 or chained_command_seen:
                    raise MobileError("invalid or duplicate Mach-O chained-fixup command")
                chained_command_seen = True
                offset, length = _unpack(data, "<II", cursor + 8, "chained fixups")
                _range(data, offset, length, "chained fixups")
                self.chained_fixups = length != 0
            cursor += size
        if cursor != command_end:
            raise MobileError("Mach-O load commands do not fill their declared region")
        self._validate_mappings()

    def _segment(self, cursor: int, command_size: int, is64: bool) -> None:
        if is64 != self.is64:
            raise MobileError("Mach-O segment width disagrees with its header")
        size, secsize = (72, 80) if is64 else (56, 68)
        if command_size < size:
            raise MobileError("truncated Mach-O segment command")
        fmt = "<16s4Q4I" if is64 else "<16s8I"
        seg = _unpack(self.data, fmt, cursor + 8, "segment")
        segname = _name(seg[0])
        address, virtual_size, offset, file_size = seg[1:5]
        section_count = seg[7]
        if section_count > _MAX_ITEMS - len(self.sections) or section_count > (command_size - size) // secsize:
            raise MobileError("invalid Mach-O section count")
        _range(self.data, offset, file_size, "segment")
        if file_size > virtual_size or address + virtual_size > 1 << (64 if is64 else 32):
            raise MobileError("invalid Mach-O segment address range")
        self.segments.append((address, virtual_size, offset, file_size))
        for index in range(section_count):
            fmt = "<16s16s2Q8I" if is64 else "<16s16s9I"
            sec = _unpack(self.data, fmt, cursor + size + index * secsize, "section")
            name, owner = _name(sec[0]), _name(sec[1])
            va, length, fileoff, flags = sec[2], sec[3], sec[4], sec[8]
            if va < address or va - address > virtual_size or length > virtual_size - (va - address):
                raise MobileError("Mach-O section lies outside its segment")
            if (flags & 0xFF) not in (1, 12, 18):
                _range(self.data, fileoff, length, "section")
                if fileoff < offset or fileoff - offset > file_size or length > file_size - (fileoff - offset):
                    raise MobileError("Mach-O section file range lies outside its segment")
                if self.file_type != 1 and length and fileoff - offset != va - address:
                    raise MobileError("Mach-O section file and virtual mappings disagree")
            self.sections.append(Section(name, owner or segname, va, length, fileoff, flags))

    def _validate_mappings(self) -> None:
        ranges = sorted((va, va + size) for va, size, _, _ in self.segments if size)
        for previous, current in zip(ranges, ranges[1:]):
            if previous[1] > current[0]:
                raise MobileError("overlapping Mach-O virtual segments")

    @property
    def encrypted(self) -> bool:
        return any(item["cryptid"] != 0 for item in self.encryption)

    def offset(self, address: int, length: int = 1) -> int:
        for va, _, offset, size in self.segments:
            if address >= va and address - va <= size and length <= size - (address - va):
                return offset + address - va
        raise MobileError(f"unmapped Mach-O metadata address 0x{address:x}")

    def integer(self, address: int, size: int = 4, signed: bool = False) -> int:
        offset = self.offset(address, size)
        return int.from_bytes(self.data[offset:offset + size], "little", signed=signed)

    def pointer(self, address: int) -> int:
        return self.integer(address, self.pointer_size)

    def string(self, address: int) -> str:
        offset = self.offset(address)
        available = next(size - (address - va) for va, _, _, size in self.segments
                         if address >= va and address - va < size)
        end = self.data.find(b"\0", offset, offset + min(available, _MAX_STRING))
        if end < 0:
            raise MobileError("unterminated or oversized Mach-O metadata string")
        try:
            return self.data[offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise MobileError("invalid UTF-8 Mach-O metadata string") from exc

    def section(self, name: str) -> Section | None:
        found = [section for section in self.sections if section.name == name]
        if len(found) > 1:
            raise MobileError(f"ambiguous Mach-O metadata section {name}")
        if found and (found[0].flags & 0xFF) in (1, 12, 18):
            raise MobileError(f"Mach-O metadata section {name} is not file-backed")
        return found[0] if found else None

    def symbols(self) -> list[dict]:
        if self.symtab is None:
            return []
        offset, count, string_offset, string_size = self.symtab
        result = []
        for index in range(count):
            fmt = "<IBBHQ" if self.is64 else "<IBBHI"
            entry = _unpack(self.data, fmt, offset + index * (16 if self.is64 else 12), "symbol")
            string_index, kind, _, _, value = entry
            if not string_index or kind & 0xE0:
                continue
            if string_index >= string_size:
                raise MobileError("Mach-O symbol string index is out of bounds")
            start = string_offset + string_index
            end = self.data.find(b"\0", start, min(start + _MAX_STRING, string_offset + string_size))
            if end < 0:
                raise MobileError("unterminated or oversized Mach-O symbol name")
            name = self.data[start:end].decode("utf-8", errors="replace")
            result.append({"name": name, "address": f"0x{value:x}", "defined": (kind & 0xE) != 0})
        return result


def select_slice(data: bytes, architecture: str = "auto") -> tuple[MachO, list[str]]:
    if architecture not in ("auto", *_PREFERENCE):
        raise MobileError(f"unsupported iOS architecture: {architecture}")
    if data[:4] in _THIN:
        image = MachO(data)
        if architecture not in ("auto", image.architecture):
            raise MobileError(f"requested {architecture}, but Mach-O contains {image.architecture}")
        return image, [image.architecture]
    if data[:4] not in _FAT:
        raise MobileError("input is not a supported Mach-O binary")
    endian, fat64 = _FAT[data[:4]]
    count = _unpack(data, endian + "I", 4, "universal header")[0]
    entry_size = 32 if fat64 else 20
    if not count or count > 64:
        raise MobileError("invalid Mach-O universal architecture count")
    table_end = 8 + count * entry_size
    _range(data, 8, count * entry_size, "universal architecture table")
    slices = []
    for index in range(count):
        fmt = endian + ("IIQQII" if fat64 else "5I")
        entry = _unpack(data, fmt, 8 + index * entry_size, "universal architecture")
        cpu, subtype, offset, size, alignment = entry[:5]
        if not size or offset < table_end or alignment > 30 or offset % (1 << alignment):
            raise MobileError("invalid Mach-O universal slice range or alignment")
        _range(data, offset, size, "universal slice")
        slices.append((cpu, subtype, offset, size))
    ranges = sorted((item[2], item[2] + item[3]) for item in slices)
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise MobileError("overlapping Mach-O universal slices")
    available = [ARCHITECTURES.get(item[0], f"cpu-0x{item[0]:x}") for item in slices]
    chosen = architecture
    if chosen == "auto":
        chosen = next((name for name in _PREFERENCE if name in available), "")
    candidates = [item for item in slices if ARCHITECTURES.get(item[0]) == chosen]
    if not candidates:
        raise MobileError(f"requested architecture is unavailable; available: {', '.join(available)}")
    # Prefer the generic CPU subtype over an ISA extension (e.g. arm64e).
    candidates.sort(key=lambda item: (item[1] & 0x00FFFFFF, item[1]))
    cpu, subtype, offset, size = candidates[0]
    image = MachO(data[offset:offset + size])
    if image.cpu_type != cpu or image.cpu_subtype != subtype:
        raise MobileError("Mach-O universal entry disagrees with its slice header")
    return image, available


def _method_list(image: MachO, address: int, class_method: bool, budget: list[int]) -> list[dict]:
    if not address:
        return []
    flags, count = image.integer(address), image.integer(address + 4)
    relative = bool(flags & 0x80000000)
    direct = bool(flags & 0x40000000)
    minimum = 12 if relative else 3 * image.pointer_size
    stride = flags & 0xFFFC
    if stride < minimum or count > budget[0]:
        raise MobileError("invalid Objective-C method-list size or count")
    image.offset(address + 8, stride * count)
    budget[0] -= count
    methods = []
    for index in range(count):
        entry = address + 8 + stride * index
        if relative:
            name = entry + image.integer(entry, signed=True)
            if not direct:
                name = image.pointer(name)
            types = entry + 4 + image.integer(entry + 4, signed=True)
            imp = entry + 8 + image.integer(entry + 8, signed=True)
        else:
            name = image.pointer(entry)
            types = image.pointer(entry + image.pointer_size)
            imp = image.pointer(entry + 2 * image.pointer_size)
        methods.append({"selector": image.string(name), "type_encoding": image.string(types),
                        "implementation": f"0x{imp:x}", "class_method": class_method})
    return methods


def objc_metadata(image: MachO) -> dict:
    limitations = ["Declarations describe runtime metadata; method bodies are emitted separately as native C.",
                   "Properties, protocols, categories, and dynamically registered classes are not reconstructed."]
    result: dict = {"classes": [], "status": "section-absent", "limitations": limitations}
    section = image.section("__objc_classlist")
    if section is None:
        return result
    if image.chained_fixups or image.file_type == 1:
        result["status"] = "unsupported-pointer-layout"
        limitations.append("Objective-C pointer metadata requires resolved absolute pointers; chained fixups and relocatable objects are not decoded.")
        return result
    ptr = image.pointer_size
    if section.size % ptr or section.size // ptr > _MAX_ITEMS:
        raise MobileError("invalid Objective-C class-list size")
    addresses = [image.pointer(section.address + index * ptr) for index in range(section.size // ptr)]
    budget = [_MAX_ITEMS]
    classes = []
    result["status"] = "recovered"
    for address in dict.fromkeys(addresses):
        if not address:
            continue
        try:
            data = image.pointer(address + 4 * ptr) & ~(7 if image.is64 else 3)
            name_offset = 24 if image.is64 else 16
            name = image.string(image.pointer(data + name_offset))
            superclass = image.pointer(address + ptr)
            methods = _method_list(image, image.pointer(data + name_offset + ptr), False, budget)
            metaclass = image.pointer(address)
            if metaclass and metaclass != address:
                meta_data = image.pointer(metaclass + 4 * ptr) & ~(7 if image.is64 else 3)
                methods.extend(_method_list(image, image.pointer(meta_data + name_offset + ptr), True, budget))
            classes.append({"name": name, "address": f"0x{address:x}",
                            "superclass_address": f"0x{superclass:x}",
                            "root_class": bool(image.integer(data) & 2), "methods": methods})
        except MobileError as exc:
            result["status"] = "partial"
            limitations.append(f"Class at 0x{address:x} could not be fully decoded: {exc}")
    names = {entry["address"]: entry["name"] for entry in classes}
    for entry in classes:
        entry["superclass"] = names.get(entry["superclass_address"])
        entry["inheritance_status"] = ("root" if entry["root_class"] else
                                        "recovered" if entry["superclass"] else "unresolved")
    if any(entry["inheritance_status"] == "unresolved" for entry in classes):
        limitations.append("Some superclass pointers are unresolved; their declarations omit inheritance and are annotated.")
    result["classes"] = classes
    return result


def swift_metadata(image: MachO) -> dict:
    symbols = [symbol for symbol in image.symbols() if symbol["name"].lstrip("_").startswith(("$s", "$S", "T0"))]
    result: dict = {"symbols": symbols, "types": [], "status": "section-absent", "limitations": [
        "Swift symbol names are preserved in their mangled form.",
        "Only nominal type names are recovered; original Swift source and method bodies cannot be reconstructed from metadata alone."]}
    section = image.section("__swift5_types")
    if section is None:
        return result
    if section.size % 4 or section.size // 4 > _MAX_ITEMS:
        raise MobileError("invalid Swift type-reference section size")
    result["status"] = "recovered"
    kinds = {16: "class", 17: "struct", 18: "enum"}
    for index in range(section.size // 4):
        entry = section.address + index * 4
        try:
            record = image.integer(entry, signed=True)
            if not record:
                continue
            reference_kind = record & 3
            if reference_kind in (2, 3):
                raise MobileError("Objective-C interoperability type-reference records are not decoded")
            descriptor = entry + (record & ~3)
            if reference_kind == 1:
                if image.chained_fixups or image.file_type == 1:
                    raise MobileError("indirect type references require resolved absolute pointers")
                descriptor = image.pointer(descriptor)
            flags = image.integer(descriptor)
            kind = flags & 0x1F
            if kind not in kinds:
                result["status"] = "partial"
                result["limitations"].append(f"Swift type reference at 0x{entry:x} has an unsupported descriptor kind {kind}.")
                continue
            name = descriptor + 8 + image.integer(descriptor + 8, signed=True)
            result["types"].append({"name": image.string(name), "kind": kinds[kind],
                                    "descriptor": f"0x{descriptor:x}"})
        except MobileError as exc:
            result["status"] = "partial"
            result["limitations"].append(f"Swift type reference at 0x{entry:x} could not be decoded: {exc}")
    return result


_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")
_PRIMITIVES = {"v": "void", "c": "signed char", "C": "unsigned char", "s": "short",
               "S": "unsigned short", "i": "int", "I": "unsigned int", "l": "long",
               "L": "unsigned long", "q": "long long", "Q": "unsigned long long", "f": "float",
               "d": "double", "B": "BOOL", "@": "id", "#": "Class", ":": "SEL", "*": "char *"}


def _types(encoding: str) -> list[str] | None:
    """Decode scalar/object/pointer signatures; retain complex forms in JSON."""
    result = []
    cursor = 0
    while cursor < len(encoding):
        while cursor < len(encoding) and encoding[cursor] in "rnNoORV0123456789+-":
            cursor += 1
        if cursor == len(encoding):
            break
        pointers = 0
        while cursor < len(encoding) and encoding[cursor] == "^":
            pointers += 1
            cursor += 1
        if cursor == len(encoding) or encoding[cursor] not in _PRIMITIVES:
            return None
        marker = encoding[cursor]
        value = _PRIMITIVES[marker]
        cursor += 1
        if marker == "@" and cursor < len(encoding) and encoding[cursor] == '"':
            end = encoding.find('"', cursor + 1)
            if end < 0:
                return None
            # `id` is accurate for object encodings and needs no external declarations.
            cursor = end + 1
        if marker == "@" and cursor < len(encoding) and encoding[cursor] == "?":
            return None
        result.append(value + " *" * pointers)
    return result


def objc_header(metadata: dict) -> str:
    lines = ["// Recovered Objective-C declarations. See objc.json for coverage and raw encodings.",
             "#import <Foundation/Foundation.h>", ""]
    by_name: dict[str, dict] = {}
    for entry in metadata["classes"]:
        if _IDENTIFIER.fullmatch(entry["name"]):
            by_name.setdefault(entry["name"], entry)
    children: dict[str, list[str]] = {}
    parents: dict[str, str | None] = {}
    for name, entry in by_name.items():
        parent = entry.get("superclass")
        parents[name] = parent if parent in by_name and parent != name else None
        if parents[name]:
            children.setdefault(parent, []).append(name)
    ordered = [name for name in by_name if not parents[name]]
    cursor = 0
    while cursor < len(ordered):
        ordered.extend(children.get(ordered[cursor], []))
        cursor += 1
    present = set(ordered)
    for name in by_name:
        if name not in present:
            # Corrupt inheritance cycles must not make headers recurse forever.
            parents[name] = None
            ordered.append(name)
    for entry in by_name.values():
        lines.append(f"@class {entry['name']};")
    for name in ordered:
        entry = by_name[name]
        superclass = parents[name]
        suffix = f" : {superclass}" if superclass else ""
        if not superclass and not entry.get("root_class", False):
            lines.append("// Superclass could not be resolved; inheritance is omitted.")
        lines.extend(["", f"@interface {name}{suffix}"])
        for method in entry["methods"]:
            selector = method["selector"]
            parts = selector.split(":")
            arguments = selector.count(":")
            types = _types(method["type_encoding"])
            identifiers = parts[:-1] if arguments else parts
            if (not types or len(types) != arguments + 3 or
                    types[1] not in ("id", "Class") or types[2] != "SEL" or
                    (arguments and parts[-1]) or
                    not all(_IDENTIFIER.fullmatch(part) for part in identifiers)):
                lines.append("// Method declaration omitted: unsupported selector or type encoding; see objc.json.")
                continue
            prefix = "+" if method["class_method"] else "-"
            signature = selector if not arguments else " ".join(
                f"{part}:({types[index + 3]})arg{index}" for index, part in enumerate(parts[:-1]))
            lines.append(f"{prefix} ({types[0]}){signature};")
        lines.append("@end")
    return "\n".join(lines) + "\n"
