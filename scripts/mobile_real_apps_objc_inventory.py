"""Independent, fail-closed Apple-tool oracle for on-disk Objective-C methods.

This module is CI evidence analysis, not the NeverD runtime. It deliberately
does not import a NeverD parser or read a NeverD recovery report. ``known`` says
only that every supported disk declaration slot and method record reconciled;
it says nothing about dynamic registration, native/Swift completeness, or source
recovery. Unsupported pointer layouts and incomplete Apple output stay unknown.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import os
from pathlib import Path
import re
import stat
import struct


MAX_TEXT = 32 * 1024 * 1024
MAX_RECORDS = 200_000
MAX_IO = 64 * 1024 * 1024
MAX_METHOD_OUTPUT = 128 * 1024 * 1024
ROOT_SECTIONS = {"__objc_classlist", "__objc_catlist", "__objc_nlclslist", "__objc_nlcatlist"}
NONLAZY_SECTIONS = {"__objc_nlclslist", "__objc_nlcatlist"}
HEX = r"0x[0-9a-fA-F]+"


class UnknownInventory(ValueError):
    pass


def _lines(text):
    if not isinstance(text, str) or len(text) > MAX_TEXT or len(text.encode("utf-8")) > MAX_TEXT:
        raise UnknownInventory("Apple output exceeds text budget")
    lines = text.splitlines()
    if len(lines) > 1_000_000 or any(len(line) > 65536 for line in lines):
        raise UnknownInventory("Apple output exceeds line budget")
    return list(enumerate(lines, 1))


def apple_sections(text, check_budget=lambda: None):
    """Read just the documented otool -l Section blocks, with exact identities."""
    sections = {}
    current = None

    def finish():
        if current is None:
            return
        required = {"sectname", "segname", "addr", "size", "offset", "flags"}
        if not required.issubset(current):
            raise UnknownInventory("incomplete Apple section declaration")
        key = (current["segname"], current["sectname"])
        if key in sections or len(sections) >= 4096:
            raise UnknownInventory("duplicate or excessive Apple section declarations")
        sections[key] = current.copy()

    for number, line in _lines(text):
        check_budget()
        if line == "Section":
            finish()
            current = {"line": number}
        elif line.startswith("Load command "):
            finish()
            current = None
        elif current is not None:
            match = re.fullmatch(r"\s*(sectname|segname|addr|size|offset|flags)\s+(\S+)\s*", line)
            if match:
                key, value = match.groups()
                if key in current:
                    raise UnknownInventory("duplicate Apple section field")
                try:
                    current[key] = value if key.endswith("name") else int(value, 0)
                except ValueError as error:
                    raise UnknownInventory("invalid Apple section number") from error
    finish()
    return sections


def nonlazy_sections(text, check_budget=lambda: None):
    return [key for key in apple_sections(text, check_budget) if key[1] in NONLAZY_SECTIONS]


class Disk:
    """Small bounded Mach-O range reader; all metadata is checked against Apple."""

    def __init__(self, path, architecture, check_budget, expected_sha256):
        self.path = Path(path)
        self.stream = self.path.open("rb")
        self.identity = os.fstat(self.stream.fileno())
        self.size = self.identity.st_size
        self.expected_sha256 = expected_sha256
        self.check_budget = check_budget
        self.architecture = architecture
        self.io = 0
        self.strings = {}
        self.string_bytes = 0
        self.sections = {}
        self.uuid = None
        self.chained = False
        try:
            if not stat.S_ISREG(self.identity.st_mode) or self.size > 4 * 1024 * 1024 * 1024:
                raise UnknownInventory("input is not a bounded regular Mach-O evidence file")
            self.verify_identity()
            header = self.read(0, 32)
            magic, cpu, _, filetype, ncmds, command_bytes, _, _ = struct.unpack("<8I", header)
            if magic != 0xFEEDFACF or cpu != {"arm64": 0x100000C, "x86_64": 0x1000007}.get(architecture):
                raise UnknownInventory("only matching thin little-endian arm64/x86_64 Mach-O is supported")
            if filetype not in (2, 6, 8) or ncmds > 4096 or command_bytes > 4 * 1024 * 1024:
                raise UnknownInventory("unsupported Mach-O kind or load-command budget")
            commands = self.read(32, command_bytes)
            cursor = 0
            for _ in range(ncmds):
                self.check_budget()
                if cursor + 8 > len(commands):
                    raise UnknownInventory("truncated Mach-O load commands")
                command, size = struct.unpack_from("<II", commands, cursor)
                if size < 8 or size % 8 or size > len(commands) - cursor:
                    raise UnknownInventory("invalid Mach-O load-command range")
                data = commands[cursor:cursor + size]
                if command == 0x19:
                    self.segment(data)
                elif command == 0x1B:
                    if size != 24 or self.uuid is not None:
                        raise UnknownInventory("invalid or duplicate Mach-O UUID")
                    self.uuid = data[8:24].hex()
                elif command == 0x80000034:
                    self.chained = True
                elif command in (0x21, 0x2C):
                    if size < 20 or struct.unpack_from("<I", data, 16)[0]:
                        raise UnknownInventory("encrypted or malformed Mach-O encryption command")
                cursor += size
            if cursor != len(commands):
                raise UnknownInventory("unaccounted Mach-O load-command bytes")
            ordered = sorted((s["addr"], s["addr"] + s["size"]) for s in self.sections.values() if s["size"])
            if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
                raise UnknownInventory("overlapping Mach-O sections")
        except Exception:
            self.stream.close()
            raise

    def verify_identity(self):
        if not isinstance(self.expected_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", self.expected_sha256):
            raise UnknownInventory("artifact lacks a valid SHA-256 identity")
        digest = hashlib.sha256()
        self.stream.seek(0)
        remaining = self.size
        while remaining:
            self.check_budget()
            chunk = self.stream.read(min(1024 * 1024, remaining))
            if not chunk:
                raise UnknownInventory("artifact changed while verifying SHA-256")
            digest.update(chunk)
            remaining -= len(chunk)
        current = os.fstat(self.stream.fileno())
        pathname = self.path.stat()
        if any(getattr(current, field) != getattr(self.identity, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")) \
                or (pathname.st_dev, pathname.st_ino) != (current.st_dev, current.st_ino) \
                or digest.hexdigest() != self.expected_sha256:
            raise UnknownInventory("artifact SHA-256 or file identity changed since bundle inventory")

    def read(self, offset, count):
        self.check_budget()
        if offset < 0 or count < 0 or count > self.size - offset or count > MAX_IO - self.io:
            raise UnknownInventory("disk evidence range or cumulative read budget exceeded")
        self.io += count
        self.stream.seek(offset)
        data = self.stream.read(count)
        if len(data) != count:
            raise UnknownInventory("disk evidence changed or was truncated")
        return data

    def segment(self, data):
        if len(data) < 72:
            raise UnknownInventory("truncated segment")
        segment = data[8:24].split(b"\0")[0].decode("ascii", errors="strict")
        address, vmsize, offset, filesize, _, protection, count = struct.unpack_from("<QQQQIII", data, 24)
        if count > 4096 or len(data) != 72 + count * 80 or filesize > self.size - offset:
            raise UnknownInventory("invalid segment range")
        for index in range(count):
            self.check_budget()
            position = 72 + index * 80
            name = data[position:position + 16].split(b"\0")[0].decode("ascii", errors="strict")
            owner = data[position + 16:position + 32].split(b"\0")[0].decode("ascii", errors="strict")
            addr, size, fileoff, _, _, relocations, flags = struct.unpack_from("<QQIIIII", data, position + 32)
            key = (owner, name)
            file_backed = (flags & 0xFF) not in (1, 0xC, 0x12)
            if owner != segment or key in self.sections or len(self.sections) >= 4096 \
                    or addr < address or size > address + vmsize - addr:
                raise UnknownInventory("invalid section ownership or range")
            if file_backed and (fileoff != offset + addr - address or size > offset + filesize - fileoff):
                raise UnknownInventory("section file/VM mapping disagrees")
            self.sections[key] = {"segname": owner, "sectname": name, "addr": addr, "size": size,
                                  "offset": fileoff, "flags": flags, "file_backed": file_backed,
                                  "executable": bool(protection & 4), "relocations": relocations}

    def section(self, address, count=1):
        # At most 4096 sections; metadata calls are bounded separately. Retain a
        # sorted lookup rather than scanning all sections for every method.
        if not hasattr(self, "ordered"):
            from bisect import bisect_right
            self._bisect = bisect_right
            self.ordered = sorted((s["addr"], s) for s in self.sections.values() if s["size"])
            self.addresses = [item[0] for item in self.ordered]
        index = self._bisect(self.addresses, address) - 1
        if index < 0:
            raise UnknownInventory("metadata address is outside a declared section")
        section = self.ordered[index][1]
        if count < 0 or count > section["addr"] + section["size"] - address or not section["file_backed"]:
            raise UnknownInventory("metadata range is not file-backed")
        return section

    def at(self, address, count):
        section = self.section(address, count)
        if section["relocations"]:
            raise UnknownInventory("section relocations are not supported by this oracle")
        return self.read(section["offset"] + address - section["addr"], count)

    def pointer(self, address):
        return struct.unpack("<Q", self.at(address, 8))[0]

    def string(self, address):
        self.check_budget()
        if address in self.strings:
            return self.strings[address]
        section = self.section(address)
        count = min(16384, section["addr"] + section["size"] - address)
        data, terminated = bytearray(), False
        while len(data) < count:
            block = self.at(address + len(data), min(256, count - len(data)))
            end = block.find(b"\0")
            if end >= 0:
                data.extend(block[:end])
                terminated = True
                break
            data.extend(block)
        if not terminated:
            raise UnknownInventory("metadata string exceeds bounded NUL-terminated range")
        try:
            value = data.decode("utf-8")
        except UnicodeError as error:
            raise UnknownInventory("metadata string is not supported UTF-8") from error
        if not value or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise UnknownInventory("metadata string is empty or contains control characters")
        if len(self.strings) < 20_000 and len(data) <= 4 * 1024 * 1024 - self.string_bytes:
            self.strings[address] = value
            self.string_bytes += len(data)
        return value


def fixup_records(text, disk):
    records = {}
    header = False
    for number, line in _lines(text):
        disk.check_budget()
        stripped = line.strip()
        if stripped == "-fixups:":
            header = True
        elif not stripped or stripped.endswith(":") or stripped.startswith("segment "):
            continue
        else:
            match = re.fullmatch(r"\s*(\S+)\s+(\S+)\s+(" + HEX + r")\s+"
                                 r"(rebase|auth-rebase|bind|auth-bind|lazy-bind)\s+(.+?)\s*", line)
            if not header or not match:
                raise UnknownInventory(f"unrecognized Apple fixup row at line {number}")
            segment, section, address, kind, target = match.groups()
            address = int(address, 16)
            owner = disk.section(address, 8)
            if (owner["segname"], owner["sectname"]) != (segment, section) or address in records:
                raise UnknownInventory("duplicate or incorrectly located Apple fixup")
            if len(records) >= MAX_RECORDS:
                raise UnknownInventory("fixup record budget exceeded")
            records[address] = {"kind": kind, "target": target, "line": number}
    if not header:
        raise UnknownInventory("Apple fixup header is missing")
    return records


def _blocks(text, check_budget=lambda: None):
    sections, section, current = {}, None, None
    for number, line in _lines(text):
        check_budget()
        header = re.fullmatch(r"Contents of \(([^,]+),([^\)]+)\) section", line)
        if header:
            section = header.groups()
            if section in sections:
                raise UnknownInventory("duplicate otool Objective-C section")
            sections[section] = []
            current = None
        elif section and section[1] in ROOT_SECTIONS:
            root = re.fullmatch(r"([0-9A-Fa-f]{8,16})\s+(" + HEX + r")(?:\s+.*)?", line)
            if root:
                current = {"slot": int(root[1], 16), "address": int(root[2], 16),
                           "line": number, "lines": []}
                sections[section].append(current)
            elif current is not None:
                current["lines"].append((number, line))
            elif line.strip():
                raise UnknownInventory("unrecognized Objective-C root declaration")
        elif section:
            sections[section].append((number, line))
    return sections


def _field(lines, name, indent):
    matches = []
    expression = re.compile(r" {" + str(indent) + r"}" + re.escape(name) + r"\s+(" + HEX + r")(?:\s+(.*))?")
    for index, (number, line) in enumerate(lines):
        match = expression.fullmatch(line)
        if match:
            matches.append((index, int(match[1], 16), match[2] or "", number))
    if len(matches) != 1:
        raise UnknownInventory(f"missing or duplicate {name} metadata field")
    return matches[0]


def _table_lines(lines, index, indent):
    result = []
    for row in lines[index + 1:]:
        if row[1].strip() and len(row[1]) - len(row[1].lstrip()) <= indent:
            break
        if row[1].strip():
            result.append(row)
    return result


def _method_fields(rows, count, width, check_budget):
    if len(rows) != 2 + 3 * count:
        raise UnknownInventory("otool method count does not match emitted records")
    if not re.fullmatch(r"\s+entsize\s+" + str(width) + (r" \(relative\)" if width == 12 else "") + r"\s*", rows[0][1]) \
            or not re.fullmatch(r"\s+count\s+" + str(count) + r"\s*", rows[1][1]):
        raise UnknownInventory("otool method-list header disagrees with disk")
    result = []
    for index in range(count):
        check_budget()
        fields = {}
        for name, (number, line) in zip(("name", "types", "imp"), rows[2 + index * 3:5 + index * 3]):
            pattern = r"\s+" + name + r"\s+(" + HEX + r")"
            if width == 12:
                pattern += r" \((" + HEX + r")\)"
            pattern += r"(?:\s+(.*?))?\s*"
            match = re.fullmatch(pattern, line)
            if not match:
                raise UnknownInventory(f"unrecognized otool method field at line {number}")
            fields[name] = {"raw": int(match[1], 16), "target": int(match[2], 16) if width == 12 else int(match[1], 16),
                            "text": (match[3] if width == 12 else match[2]) or "", "line": number}
        result.append(fields)
    return result


def dyld_methods(text, check_budget=lambda: None):
    methods, context, header, count = Counter(), None, False, 0
    for number, line in _lines(text):
        check_budget()
        value = line.strip()
        if value == "-objc:":
            header = True
        elif value.startswith("@interface "):
            match = re.fullmatch(r"@interface ([^\s(:]+)(?:\(([^)]+)\))?(?:\s*:.*)?\s*", value)
            if not match or context is not None:
                raise UnknownInventory("unrecognized or nested Apple Objective-C interface")
            context = ("class", match[1], match[2] or "")
        elif value.startswith("@protocol "):
            if context is not None:
                raise UnknownInventory("nested Apple Objective-C protocol")
            context = ("protocol", "", "")
        elif value == "@end":
            if context is None:
                raise UnknownInventory("unmatched Apple Objective-C context terminator")
            context = None
        elif context and context[0] == "protocol":
            # Protocol requirements are not implementations. An addressed row
            # in this context is not a known format and cannot be discarded.
            if value.startswith("0x"):
                raise UnknownInventory("unexpected addressed protocol requirement")
        elif context:
            match = re.fullmatch(r"(" + HEX + r")\s+([+-])\[([^\s]+) (.+)\]", value)
            if not match or match[3] != context[1]:
                raise UnknownInventory(f"unrecognized Apple callable row at line {number}")
            key = (context[1], context[2], match[2] == "+", match[4], int(match[1], 16))
            methods[key] += 1
            count += 1
            if count > MAX_RECORDS:
                raise UnknownInventory("Apple callable record budget exceeded")
        elif value and not value.endswith(":"):
            raise UnknownInventory(f"unrecognized Apple Objective-C output at line {number}")
    if not header or context is not None:
        raise UnknownInventory("missing or incomplete Apple Objective-C output")
    return methods


class Reconcile:
    def __init__(self, disk, outputs, result):
        self.disk, self.outputs, self.result = disk, outputs, result
        self.fixups = fixup_records(outputs["fixups"], disk)
        self.blocks = _blocks(outputs["objc-otool"], disk.check_budget)
        self.methods = []
        self.method_bytes = 0
        self.declarations = {}
        self.selector_slots = {}

    def pointer(self, address, expected=None):
        raw = self.disk.pointer(address)
        fixup = self.fixups.get(address)
        if fixup:
            if fixup["kind"] != "rebase" or not re.fullmatch(HEX, fixup["target"]):
                raise UnknownInventory("metadata pointer requires unsupported bind/auth fixup")
            if int(fixup["target"], 16) != raw:
                raise UnknownInventory("classic pointer and Apple rebase target disagree")
        if expected is not None and raw != expected:
            raise UnknownInventory("disk pointer and otool declaration disagree")
        return raw

    def selectors(self):
        for key, section in self.disk.sections.items():
            if key[1] != "__objc_selrefs":
                continue
            rows = self.blocks.get(key)
            if rows is None or section["size"] % 8 or len(rows) != section["size"] // 8:
                raise UnknownInventory("selector-reference slots are incomplete in Apple output")
            if len(rows) > MAX_RECORDS:
                raise UnknownInventory("selector-reference budget exceeded")
            for index, (number, line) in enumerate(rows):
                match = re.fullmatch(r"\s+(" + HEX + r") (.+)", line)
                if not match:
                    raise UnknownInventory("unrecognized selector-reference row")
                slot = section["addr"] + index * 8
                target = self.pointer(slot, int(match[1], 16))
                text = self.disk.string(target)
                if text != match[2]:
                    raise UnknownInventory("selector text disagrees with disk")
                self.selector_slots[slot] = text

    def table(self, lines, field, indent, pointer_slot, owner, class_method, category, category_address):
        index, address, symbol, _ = _field(lines, field, indent)
        self.pointer(pointer_slot, address)
        rows = _table_lines(lines, index, indent)
        if address == 0:
            if rows or symbol:
                raise UnknownInventory("null method-list pointer has records or an unresolved symbolic identity")
            return
        flags, count = struct.unpack("<II", self.disk.at(address, 8))
        if flags not in (24, 0x8000000C) or count > MAX_RECORDS - len(self.methods):
            raise UnknownInventory("unsupported method-list flags or record budget")
        width = flags & 0xFFFF
        data = self.disk.at(address + 8, count * width) if count else b""
        fields = _method_fields(rows, count, width, self.disk.check_budget)
        for ordinal, values in enumerate(fields):
            self.disk.check_budget()
            entry = address + 8 + ordinal * width
            if width == 12:
                relative = struct.unpack_from("<iii", data, ordinal * width)
                targets = tuple(entry + i * 4 + value for i, value in enumerate(relative))
                for i, name in enumerate(("name", "types", "imp")):
                    if values[name]["raw"] != (relative[i] & 0xFFFFFFFF) or values[name]["target"] != targets[i]:
                        raise UnknownInventory("relative method field disagrees with disk")
                if targets[0] not in self.selector_slots:
                    raise UnknownInventory("relative method selector lacks a reconciled selector-reference slot")
                selector = self.selector_slots[targets[0]]
            else:
                targets = tuple(self.pointer(entry + i * 8, values[name]["target"])
                                for i, name in enumerate(("name", "types", "imp")))
                selector = self.disk.string(targets[0])
            encoding = self.disk.string(targets[1])
            if values["name"]["text"] and values["name"]["text"] != selector \
                    or values["types"]["text"] != encoding:
                raise UnknownInventory("method selector/type text disagrees with disk")
            if not self.disk.section(targets[2])["executable"]:
                raise UnknownInventory("method implementation is outside executable file-backed sections")
            # A shared long selector may appear only once in selrefs but many
            # times in JSON. Bound this amplification before appending records.
            cost = 1024 + 6 * sum(len(text.encode("utf-8")) for text in (owner[1], category, selector, encoding))
            if cost > MAX_METHOD_OUTPUT - self.method_bytes:
                raise UnknownInventory("Objective-C method evidence output budget exceeded")
            self.method_bytes += cost
            self.methods.append({"owner_address": hex(owner[0]), "owner": owner[1],
                                 "metaclass_address": hex(owner[2]), "class_method": class_method,
                                 "category_address": hex(category_address) if category_address else None,
                                 "category": category, "method_list": hex(address), "ordinal": ordinal,
                                 "selector": selector, "type_encoding": encoding, "implementation": hex(targets[2]),
                                 "evidence": {"command": "objc-otool", "line": values["imp"]["line"]}})

    def class_record(self, block):
        address, lines = block["address"], block["lines"]
        markers = [i for i, (_, line) in enumerate(lines) if line == "Meta Class"]
        if len(markers) != 1:
            raise UnknownInventory("class lacks exactly one metaclass declaration")
        instance, meta = lines[:markers[0]], lines[markers[0] + 1:]
        metaclass = self.pointer(address, _field(instance, "isa", 4)[1])
        if not metaclass:
            raise UnknownInventory("class has null metaclass")
        names = []
        for class_address, group, is_meta in ((address, instance, False), (metaclass, meta, True)):
            raw_ro = self.pointer(class_address + 32)
            ro = raw_ro & ~7
            if _field(group, "data", 4)[1] not in (raw_ro, ro):
                raise UnknownInventory("class data pointer disagrees with disk")
            flags = struct.unpack("<I", self.disk.at(ro, 4))[0]
            if bool(flags & 1) != is_meta:
                raise UnknownInventory("class/metaclass RO flag mismatch")
            name_target = self.pointer(ro + 24, _field(group, "name", 8)[1])
            name = self.disk.string(name_target)
            if name != _field(group, "name", 8)[2]:
                raise UnknownInventory("class name disagrees with disk")
            names.append(name)
            self.table(group, "baseMethods", 8, ro + 32, (address, name, metaclass), is_meta, "", 0)
        if names[0] != names[1]:
            raise UnknownInventory("class and metaclass names disagree")

    def category_record(self, block):
        address, lines = block["address"], block["lines"]
        name = self.disk.string(self.pointer(address, _field(lines, "name", 4)[1]))
        if name != _field(lines, "name", 4)[2]:
            raise UnknownInventory("category name disagrees with disk")
        _, displayed_owner, symbol, _ = _field(lines, "cls", 4)
        owner = self.disk.pointer(address + 8)
        if owner:
            self.pointer(address + 8, displayed_owner)
            metaclass = self.pointer(owner)
            ro = self.pointer(owner + 32) & ~7
            owner_name = self.disk.string(self.pointer(ro + 24))
        else:
            fixup = self.fixups.get(address + 8)
            match = re.fullmatch(r"_OBJC_CLASS_\$_([^\s]+)", symbol)
            if displayed_owner or not fixup or fixup["kind"] != "bind" or not match \
                    or not re.fullmatch(r"[^\s]+/" + re.escape(symbol) + r"(?: \[weak-import\])?", fixup["target"]):
                raise UnknownInventory("external category owner lacks an exact Apple bind identity")
            owner_name, metaclass = match[1], 0
        self.table(lines, "instanceMethods", 4, address + 16, (owner, owner_name, metaclass), False, name, address)
        self.table(lines, "classMethods", 4, address + 24, (owner, owner_name, metaclass), True, name, address)

    def roots(self):
        for key, section in self.disk.sections.items():
            if key[1].startswith("__objc_") and ("clslist" in key[1] or "catlist" in key[1] or "classlist" in key[1]) \
                    and key[1] not in ROOT_SECTIONS:
                raise UnknownInventory("unrecognized Objective-C declaration section")
            if key[1] not in ROOT_SECTIONS or key[1] in NONLAZY_SECTIONS:
                continue
            if section["size"] % 8:
                raise UnknownInventory("Objective-C root section has a partial pointer slot")
            count = section["size"] // 8
            blocks = self.blocks.get(key, [])
            if count != len(blocks) or count > MAX_RECORDS:
                raise UnknownInventory("Objective-C declared root slots are missing from otool output")
            for ordinal, block in enumerate(blocks):
                slot = section["addr"] + ordinal * 8
                if block["slot"] != slot or not block["address"]:
                    raise UnknownInventory("Objective-C root slots are reordered, missing, or null")
                target = self.pointer(slot, block["address"])
                kind = "class" if key[1] == "__objc_classlist" else "category"
                identity = (kind, target)
                if identity in self.declarations:
                    raise UnknownInventory("duplicate declaration roots require unsupported alias reconciliation")
                self.declarations[identity] = block
                self.result["root_slots"].append({"section": list(key), "slot": hex(slot),
                                                  "target": hex(target), "kind": kind,
                                                  "evidence": {"command": "objc-otool", "line": block["line"]}})
                (self.class_record if kind == "class" else self.category_record)(block)
        # Nonlazy roots refer to physical declarations, not additional methods.
        # Require their raw Apple section bytes as a separate independent view.
        for key, section in self.disk.sections.items():
            if key[1] not in NONLAZY_SECTIONS:
                continue
            raw = self.raw_section(key, section)
            if len(raw) % 8 or len(raw) // 8 > MAX_RECORDS:
                raise UnknownInventory("nonlazy section has incomplete or excessive slots")
            kind = "class" if key[1] == "__objc_nlclslist" else "category"
            for index in range(len(raw) // 8):
                slot = section["addr"] + index * 8
                target = self.pointer(slot)
                if (kind, target) not in self.declarations:
                    raise UnknownInventory("nonlazy slot points to a declaration absent from the reconciled roots")
                self.result["root_slots"].append({"section": list(key), "slot": hex(slot), "target": hex(target),
                                                  "kind": kind, "alias_of_declaration": True,
                                                  "evidence": {"command": "objc-raw:" + ":".join(key)}})

    def raw_section(self, key, section):
        label = "objc-raw:" + ":".join(key)
        if label not in self.outputs:
            raise UnknownInventory("nonlazy raw section evidence is missing: " + ":".join(key))
        data, expected, seen_header = bytearray(), section["addr"], False
        for _, line in _lines(self.outputs[label]):
            if line == "Contents of (" + ",".join(key) + ") section":
                if seen_header:
                    raise UnknownInventory("duplicate raw section header")
                seen_header = True
            elif not line.strip() or line.endswith(":"):
                continue
            else:
                digits = 2 if self.disk.architecture == "x86_64" else 8
                match = re.fullmatch(r"([0-9A-Fa-f]{8,16})\s+((?:[0-9A-Fa-f]{" + str(digits) + r"}(?:\s+|$))+)", line)
                if not seen_header or not match or int(match[1], 16) != expected:
                    raise UnknownInventory("unsupported Apple raw section format or address gap")
                for word in match[2].split():
                    data.extend(bytes.fromhex(word) if digits == 2 else struct.pack("<I", int(word, 16)))
                expected = section["addr"] + len(data)
                if len(data) > section["size"] or len(data) > MAX_IO:
                    raise UnknownInventory("Apple raw section exceeds disk section size")
        if not seen_header or len(data) != section["size"] or bytes(data) != self.disk.at(section["addr"], len(data)):
            raise UnknownInventory("Apple raw section is incomplete or disagrees with disk")
        return data

    def run(self):
        observed = apple_sections(self.outputs["load-commands"], self.disk.check_budget)
        if set(observed) != set(self.disk.sections):
            raise UnknownInventory("Apple and disk section inventories disagree")
        for key, section in self.disk.sections.items():
            if any(observed[key][field] != section[field] for field in ("addr", "size", "offset", "flags")):
                raise UnknownInventory("Apple and disk section ranges disagree")
        if any(key[1] in ROOT_SECTIONS | {"__objc_selrefs"} and key not in self.disk.sections for key in self.blocks):
            raise UnknownInventory("otool reports an Objective-C declaration section absent from disk")
        # Apple can display a raw zero chained pointer as the preferred base.
        # Until chain coverage itself is independently proven, even successful
        # -fixups output cannot certify that an omitted slot was actually null.
        if self.disk.chained:
            raise UnknownInventory("chained fixup slot coverage and null-pointer interpretation are not yet proven")
        self.selectors()
        self.roots()
        expected = Counter((m["owner"], m["category"], m["class_method"], m["selector"], int(m["implementation"], 16))
                           for m in self.methods)
        actual = dyld_methods(self.outputs["objc"], self.disk.check_budget)
        if expected != actual:
            raise UnknownInventory("disk/otool/dyld Objective-C method multisets disagree")


def objc_inventory(artifact, architecture, outputs, binary, check_budget=lambda: None):
    """Produce observations on failure; never convert unknown into a zero count."""
    result = {"schema_version": 1, "scope": "on-disk-objc-method-records", "status": "unknown",
              "denominator_known": False, "method_count": None, "observed_method_count": 0,
              "artifact": artifact["path"], "artifact_sha256": artifact["sha256"], "architecture": architecture,
              "uuid": None, "methods": [], "root_slots": [], "issues": [],
              "evidence_commands": sorted(outputs),
              "exclusions": ["dynamic runtime registration", "native callable completeness", "Swift callable completeness",
                             "protocol requirements without implementations", "source recovery or behavioral equivalence"]}
    disk, reconciler = None, None
    try:
        for label in ("load-commands", "objc", "objc-otool", "fixups"):
            if label not in outputs:
                raise UnknownInventory("required independent evidence is missing: " + label)
        disk = Disk(binary, architecture, check_budget, artifact["sha256"])
        result["uuid"] = disk.uuid
        reconciler = Reconcile(disk, outputs, result)
        reconciler.run()
        disk.verify_identity()
        result.update(status="known", denominator_known=True, method_count=len(reconciler.methods))
    except Exception as error:
        result["issues"].append(str(error))
    finally:
        if reconciler is not None:
            result["methods"] = reconciler.methods
            result["observed_method_count"] = len(reconciler.methods)
        if disk is not None:
            disk.stream.close()
    return result
