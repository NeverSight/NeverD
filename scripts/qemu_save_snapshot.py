#!/usr/bin/env python3
"""Read registers and RAM blocks from an offline libvirt/QEMU save.

This is an independent, read-only format reader, not a QEMU restore or an
instruction executor. See docs/qemu-save-snapshot.md for its supported format,
source references and the distinction between RAM-block and guest addresses.
"""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import json
from pathlib import Path
import re
import struct
import sys


class SnapshotError(ValueError):
    """The save or requested observation is unsupported or incomplete."""


@dataclass(frozen=True)
class SnapshotLimits:
    # Bound each XML/JSON document and aggregate serialized device state,
    # independently of RAM. Expanded arrays also have a separate value bound.
    metadata_bytes: int = 16 * 1024 * 1024
    state_values: int = 1_000_000
    nesting: int = 32
    # A dense 64-bit page index takes 128 MiB at this supported RAM ceiling.
    ram_bytes: int = 64 * 1024 * 1024 * 1024
    # Observations are bounded; exporting the entire guest is not this API.
    read_bytes: int = 1024 * 1024


@dataclass(frozen=True)
class _Field:
    name: str
    size: int
    count: int | None
    integer: tuple[str, bool] | None
    layout: _Layout | None


@dataclass(frozen=True)
class _Layout:
    name: str
    version: int
    fields: tuple[_Field, ...]
    subsections: tuple[_Layout, ...]
    size: int
    values: int


@dataclass
class _RamBlock:
    pages: array
    present: int = 0
    records: int = 0


def _integer(value, label: str, maximum: int) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise SnapshotError(f"invalid {label}")
    return value


def _name(value, label: str) -> str:
    if not isinstance(value, str) or not value or "\0" in value:
        raise SnapshotError(f"invalid {label}")
    try:
        value.encode("utf-8")
    except UnicodeError as error:
        raise SnapshotError(f"invalid UTF-8 {label}") from error
    return value


def _object(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise SnapshotError(f"invalid {label}")
    return value


def _list(value, label: str) -> list:
    if not isinstance(value, list):
        raise SnapshotError(f"invalid {label}")
    return value


def _json_object(pairs) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise SnapshotError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _json_constant(value):
    raise SnapshotError(f"non-finite JSON number {value!r}")


def _layout(description: dict, limits: SnapshotLimits, depth: int = 0) -> _Layout:
    if depth >= limits.nesting:
        raise SnapshotError("VMState nesting exceeds inspection limit")
    description = _object(description, "VMState description")
    name = _name(description.get("vmsd_name", description.get("name")), "VMState name")
    version = _integer(description.get("version"), "VMState version", 0xffffffff)
    fields, names, size, values = [], set(), 0, 0
    for field in _list(description.get("fields"), "VMState fields"):
        field = _object(field, "VMState field")
        key = _name(field.get("name"), "field name")
        if "index" in field:
            index = _integer(field["index"], "field index", limits.state_values)
            key = f"{key}[{index}]"
        if key in names or key == "subsections":
            raise SnapshotError(f"duplicate/reserved field name {key!r}")
        names.add(key)
        width = _integer(field.get("size"), "field size", limits.metadata_bytes)
        count = None
        if "array_len" in field:
            count = _integer(field["array_len"], "array length", limits.state_values)
        kind = _name(field.get("type"), "field type")
        child, integer = None, None
        if kind == "struct" or "fields" in field:
            child = _layout(field.get("struct", field), limits, depth + 1)
            if child.size != width:
                raise SnapshotError(f"VMState size mismatch for {key!r}")
        else:
            match = re.fullmatch(r"(u?int)(8|16|32|64)( le| equal)?", kind)
            if match:
                if width != int(match[2]) // 8:
                    raise SnapshotError(f"integer width mismatch for {key!r}")
                integer = ("little" if match[3] == " le" else "big", match[1] == "int")
            elif kind == "bool":
                if width != 1:
                    raise SnapshotError(f"boolean width mismatch for {key!r}")
                integer = ("big", False)
        repeats = count if count is not None else 1
        size += width * repeats
        values += (1 + (child.values if child else 0)) * repeats
        if size > limits.metadata_bytes or values > limits.state_values:
            raise SnapshotError("VMState exceeds inspection limit")
        fields.append(_Field(key, width, count, integer, child))
    subsections, subnames = [], set()
    for sub in _list(description.get("subsections", []), "VMState subsections"):
        child = _layout(sub, limits, depth + 1)
        encoded = child.name.encode("utf-8")
        if len(encoded) > 255 or child.name in subnames:
            raise SnapshotError("invalid or duplicate subsection name")
        subnames.add(child.name)
        size += 6 + len(encoded) + child.size
        values += 1 + child.values
        if size > limits.metadata_bytes or values > limits.state_values:
            raise SnapshotError("VMState exceeds inspection limit")
        subsections.append(child)
    return _Layout(name, version, tuple(fields), tuple(subsections), size, values)


class QemuSaveSnapshot:
    """Index an immutable raw save once, then read bounded RAM-block ranges.

    The caller keeps the file immutable until close(). Names in the save are
    labels only, never host paths. No device model, page walk or guest code runs.
    """

    def __init__(self, path: str | Path, limits: SnapshotLimits = SnapshotLimits()):
        self.limits = limits
        self._file = open(path, "rb")
        try:
            self._file.seek(0, 2)
            self._end = self._file.tell()
            self.file_size = self._end
            if self.file_size >= 1 << 63:
                raise SnapshotError("save offsets exceed signed 64-bit page index")
            self._file.seek(0)
            self._read_header()
            description = self._read_description()
            self.page_size = description.get("page_size")
            if type(self.page_size) is not int or self.page_size != 4096:
                raise SnapshotError("only 4096-byte QEMU target pages are supported")
            configuration = _layout(description.get("configuration"), limits)
            if configuration.version != 1 or configuration.subsections:
                raise SnapshotError("unsupported migration configuration/extensions")
            devices = {}
            state_bytes, state_values = configuration.size, 1 + configuration.values
            for device in _list(description.get("devices"), "device descriptions"):
                device = _object(device, "device description")
                key = (_name(device.get("name"), "device name"),
                       _integer(device.get("instance_id"), "device instance", 0xffffffff))
                if key in devices:
                    raise SnapshotError(f"duplicate device description {key!r}")
                layout = _layout(device, limits)
                state_bytes += layout.size
                state_values += 1 + layout.values
                if state_bytes > limits.metadata_bytes or state_values > limits.state_values:
                    raise SnapshotError("device states exceed inspection limit")
                devices[key] = layout
            self._ram: dict[str, _RamBlock] = {}
            self._last_ram: _RamBlock | None = None
            self.cpus: dict[int, dict] = {}
            self.apics: dict[int, dict] = {}
            self.cpu_common: dict[int, dict] = {}
            self._read_stream(configuration, devices)
        except BaseException:
            self._file.close()
            raise

    def __enter__(self) -> QemuSaveSnapshot:
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self._file.close()

    def _read(self, size: int) -> bytes:
        if size < 0 or size > self._end - self._file.tell():
            raise SnapshotError(f"truncated save at file offset {self._file.tell():#x}")
        data = self._file.read(size)
        if len(data) != size:
            raise SnapshotError("save changed or was truncated during inspection")
        return data

    def _skip(self, size: int):
        if size < 0 or size > self._end - self._file.tell():
            raise SnapshotError(f"truncated save at file offset {self._file.tell():#x}")
        self._file.seek(size, 1)

    def _uint(self, size: int) -> int:
        return int.from_bytes(self._read(size), "big")

    def _string(self) -> str:
        try:
            return _name(self._read(self._uint(1)).decode("utf-8"), "stream name")
        except UnicodeError as error:
            raise SnapshotError("invalid UTF-8 stream name") from error

    def _read_header(self):
        header = self._read(92)
        if header[:16] != b"LibvirtQemudSave":
            raise SnapshotError("expected a completed libvirt save")
        if header[16:20] == struct.pack("<I", 2):
            byte_order = "<"
        elif header[16:20] == struct.pack(">I", 2):
            byte_order = ">"
        else:
            raise SnapshotError("only libvirt save version 2 is supported")
        _, length, running, compression, cookie, *reserved = struct.unpack(
            byte_order + "19I", header[16:])
        if compression != 0:
            raise SnapshotError("compressed libvirt saves are unsupported")
        if any(reserved) or running not in (0, 1):
            raise SnapshotError("unsupported libvirt save header")
        if not 0 < length <= self.limits.metadata_bytes or cookie >= length:
            raise SnapshotError("invalid libvirt metadata length/cookie offset")
        metadata = self._read(length)
        xml = metadata[:cookie] if cookie else metadata
        if not xml or xml[-1] != 0 or (cookie and metadata[-1] != 0):
            raise SnapshotError("unterminated libvirt metadata")
        try:
            self.domain_xml = xml.split(b"\0", 1)[0].decode("utf-8")
            self.cookie_xml = metadata[cookie:].split(b"\0", 1)[0].decode("utf-8") if cookie else None
        except UnicodeError as error:
            raise SnapshotError("invalid UTF-8 libvirt metadata") from error
        self.was_running = bool(running)
        self._stream_start = self._file.tell()

    def _read_description(self) -> dict:
        # JSON cannot contain raw NUL/0x06. The final EOF/VMDESC pair therefore
        # identifies the trailer without searching or loading the RAM payload.
        start = max(self._stream_start, self.file_size - self.limits.metadata_bytes - 6)
        self._file.seek(start)
        tail = self._read(self.file_size - start)
        marker = tail.rfind(b"\x00\x06")
        while marker >= 0:
            size = int.from_bytes(tail[marker + 2:marker + 6], "big")
            if len(tail) - marker >= 6 and marker + 6 + size == len(tail):
                break
            # The length word can itself contain the marker's byte pattern.
            marker = tail.rfind(b"\x00\x06", 0, marker)
        if marker < 0:
            raise SnapshotError("missing bounded VMState trailer or invalid trailer length")
        try:
            description = json.loads(tail[marker + 6:], object_pairs_hook=_json_object,
                                     parse_constant=_json_constant)
        except (ValueError, RecursionError) as error:
            raise SnapshotError(f"invalid VMState JSON: {error}") from error
        self._end = start + marker + 1  # Include EOF, exclude the description.
        self._file.seek(self._stream_start)
        return _object(description, "VMState document")

    def _state(self, layout: _Layout, keep: bool) -> dict:
        result = {}
        for field in layout.fields:
            values = []
            for _ in range(field.count if field.count is not None else 1):
                if field.layout:
                    value = self._state(field.layout, keep)
                elif keep:
                    value = self._read(field.size)
                    if field.integer:
                        order, signed = field.integer
                        value = int.from_bytes(value, order, signed=signed)
                else:
                    self._skip(field.size)
                    value = None
                if keep:
                    values.append(value)
            if keep:
                result[field.name] = values if field.count is not None else values[0]
        subsections = {}
        for sub in layout.subsections:
            if (self._uint(1) != 5 or self._string() != sub.name or
                    self._uint(4) != sub.version):
                raise SnapshotError(f"subsection header mismatch for {sub.name!r}")
            value = self._state(sub, keep)
            if keep:
                subsections[sub.name] = value
        if keep and subsections:
            result["subsections"] = subsections
        return result

    def _read_stream(self, configuration: _Layout, devices: dict):
        if self._read(8) != b"QEVM\0\0\0\x03" or self._uint(1) != 7:
            raise SnapshotError("expected QEMU v3 stream with configuration")
        config = self._state(configuration, True)
        name = config.get("name")
        if not isinstance(name, bytes) or config.get("len") != len(name):
            raise SnapshotError("invalid machine configuration name")
        try:
            self.machine = _name(name.decode("utf-8"), "machine name")
        except UnicodeError as error:
            raise SnapshotError("invalid machine configuration name") from error
        sections, seen, closed = {}, set(), set()
        destinations = {"cpu": self.cpus, "apic": self.apics, "cpu_common": self.cpu_common}
        while True:
            kind = self._uint(1)
            if kind == 0:
                if self._file.tell() != self._end or set(sections) != closed:
                    raise SnapshotError("premature EOF or unfinished migration sections")
                if seen - {("ram", 0)} != set(devices):
                    raise SnapshotError("device description/stream mismatch")
                if not self._ram or not self.cpus:
                    raise SnapshotError("save has no RAM or CPU state")
                return
            if kind == 8:
                command, length = self._uint(2), self._uint(2)
                if command != 11 or length != 0:
                    raise SnapshotError(f"unsupported migration command {command:#x}")
                continue
            if kind not in (1, 2, 3, 4):
                raise SnapshotError(f"unsupported migration section {kind:#x}")
            section_id = self._uint(4)
            if kind in (1, 4):
                key = (self._string(), self._uint(4))
                version = self._uint(4)
                if section_id in sections or key in seen:
                    raise SnapshotError("duplicate migration section")
                sections[section_id] = key
                seen.add(key)
                if key == ("ram", 0):
                    if version != 4:
                        raise SnapshotError("only RAM migration version 4 is supported")
                elif kind != 4 or key not in devices or version != devices[key].version:
                    raise SnapshotError(f"unsupported/mismatched device section {key!r}")
            else:
                if section_id not in sections or section_id in closed:
                    raise SnapshotError("unknown or completed migration section")
                key = sections[section_id]
                if key != ("ram", 0):
                    raise SnapshotError("only RAM supports partial migration sections")
            if key == ("ram", 0):
                self._read_ram_section()
            else:
                destination = destinations.get(key[0])
                value = self._state(devices[key], destination is not None)
                if destination is not None:
                    destination[key[1]] = {
                        "version": devices[key].version, "fields": value,
                        "subsection_versions": {sub.name: sub.version
                                                for sub in devices[key].subsections},
                    }
            if self._uint(1) != 0x7e or self._uint(4) != section_id:
                raise SnapshotError("migration section footer mismatch")
            if kind in (3, 4):
                closed.add(section_id)

    def _read_ram_section(self):
        while True:
            record = self._uint(8)
            flags, offset = record & (self.page_size - 1), record & -self.page_size
            if flags == 0x10:
                if offset:
                    raise SnapshotError("invalid RAM end marker")
                return
            if flags == 4:
                if self._ram or not 0 < offset <= self.limits.ram_bytes:
                    raise SnapshotError("invalid or oversized RAM inventory")
                remaining = offset
                while remaining:
                    name, size = self._string(), self._uint(8)
                    if name in self._ram or not 0 < size <= remaining or size % self.page_size:
                        raise SnapshotError("invalid RAM block extent")
                    self._ram[name] = _RamBlock(array("q", [0]) * (size // self.page_size))
                    remaining -= size
                continue
            if (flags & ~0x20) not in (2, 8):
                raise SnapshotError(f"unsupported RAM flags {flags:#x}")
            if not flags & 0x20:
                name = self._string()
                if name not in self._ram:
                    raise SnapshotError(f"unknown RAM block {name!r}")
                self._last_ram = self._ram[name]
            block = self._last_ram
            page = offset // self.page_size
            if block is None or page >= len(block.pages):
                raise SnapshotError("missing RAM block or page outside its extent")
            if flags & 8:
                location = self._file.tell() + 1
                self._skip(self.page_size)
            else:
                location = -(self._uint(1) + 1)
            if block.pages[page] == 0:
                block.present += 1
            block.pages[page] = location
            block.records += 1

    def expect_pc(self, cpu: int, pc: int):
        """Require captured env.eip (RIP in 64-bit code); never reposition it."""
        cpu = _integer(cpu, "CPU instance", 0xffffffff)
        pc = _integer(pc, "requested PC", 0xffffffffffffffff)
        if cpu not in self.cpus:
            raise SnapshotError(f"CPU {cpu} is absent")
        actual = self.cpus[cpu]["fields"].get("env.eip")
        if type(actual) is not int or not 0 <= actual <= 0xffffffffffffffff:
            raise SnapshotError("CPU has no decoded x86 instruction pointer")
        if actual != pc:
            raise SnapshotError(f"CPU {cpu} PC mismatch: saved {actual:#x}, requested {pc:#x}")

    def read_ram(self, block: str, offset: int, size: int) -> bytes:
        """Read RAM-block offsets, not physical/virtual addresses; missing pages fail."""
        if block not in self._ram:
            raise SnapshotError(f"unknown RAM block {block!r}")
        size = _integer(size, "RAM read size", self.limits.read_bytes)
        pages = self._ram[block].pages
        offset = _integer(offset, "RAM read offset", len(pages) * self.page_size)
        if size > len(pages) * self.page_size - offset:
            raise SnapshotError("RAM read outside block extent")
        result = bytearray()
        while size:
            page, within = divmod(offset, self.page_size)
            amount = min(size, self.page_size - within)
            location = pages[page]
            if location == 0:
                raise SnapshotError(f"RAM page {block!r}+{page * self.page_size:#x} is absent")
            if location < 0:
                result.extend(bytes([-location - 1]) * amount)
            else:
                self._file.seek(location - 1 + within)
                result.extend(self._read(amount))
            offset += amount
            size -= amount
        return bytes(result)

    def summary(self) -> dict:
        return {
            "schema": 1, "machine": self.machine, "was_running": self.was_running,
            "page_size": self.page_size, "file_size": self.file_size,
            "cpus": self.cpus, "apics": self.apics, "cpu_common": self.cpu_common,
            "ram_blocks": {
                name: {"size": len(block.pages) * self.page_size,
                       "pages_present": block.present, "page_records": block.records,
                       "index_bytes": len(block.pages) * block.pages.itemsize}
                for name, block in self._ram.items()
            },
        }


def _json_state(value):
    if isinstance(value, dict):
        return {key: _json_state(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_json_state(item) for item in value]
    if isinstance(value, bytes):
        return {"raw_hex": value.hex()}
    return hex(value) if type(value) is int else value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--cpu", type=int, help="select a captured CPU instance")
    parser.add_argument("--expect-pc", type=lambda value: int(value, 0),
                        help="require this captured x86 PC (requires --cpu)")
    parser.add_argument("--ram", nargs=3, metavar=("BLOCK", "OFFSET", "SIZE"),
                        help="read a bounded range by RAM-block offset")
    args = parser.parse_args(argv)
    if args.expect_pc is not None and args.cpu is None:
        parser.error("--expect-pc requires --cpu")
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            if args.cpu is not None and args.cpu not in snapshot.cpus:
                raise SnapshotError(f"CPU {args.cpu} is absent")
            if args.expect_pc is not None:
                snapshot.expect_pc(args.cpu, args.expect_pc)
            result = snapshot.summary()
            for category in ("cpus", "apics", "cpu_common"):
                result[category] = {
                    key: {**value, "fields": _json_state(value["fields"])}
                    for key, value in result[category].items()
                    if args.cpu is None or key == args.cpu
                }
            if args.ram:
                block, offset, size = args.ram
                try:
                    offset, size = int(offset, 0), int(size, 0)
                except ValueError as error:
                    raise SnapshotError("RAM offset/size must be decimal or prefixed integers") from error
                result["read"] = {"block": block, "offset": offset, "size": size,
                                  "hex": snapshot.read_ram(block, offset, size).hex()}
            json.dump(result, sys.stdout, indent=2)
            print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-snapshot: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
