#!/usr/bin/env python3
"""Real-engine PE browsing; generated fixtures are never executed."""
from pathlib import Path
import hashlib
import struct
import sys
import tempfile
from transport_test import Client


def image(wide=True, invalid=False, legacy=False):
    data = bytearray(0x800)
    optional, optional_size = 0x98, 240 if wide else 224
    directories = optional + (112 if wide else 96)
    base, width = (0x140000000, 8) if wide else (0x774c0000, 4)
    def u16(at, value): struct.pack_into("<H", data, at, value)
    def u32(at, value): struct.pack_into("<I", data, at, value)
    def word(at, value): struct.pack_into("<Q" if wide else "<I", data, at, value)
    data[:2] = b"MZ"
    u32(0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    u16(0x84, 0x8664 if wide else 0x14c)
    u16(0x86, 3)
    u16(0x94, optional_size)
    u16(0x96, 2)
    u16(optional, 0x20b if wide else 0x10b)
    u32(optional + 16, 0x88f41000 if invalid else 0x1000)
    word(optional + (24 if wide else 28), base)
    u32(optional + 32, 0x1000)
    u32(optional + 36, 0x200)
    u32(optional + 56, 0x4000)
    u32(optional + 60, 0x200)
    u16(optional + 68, 3)
    u32(directories - 4, 16)
    for index, name in enumerate((b".text", b".idata", b".reloc")):
        section = optional + optional_size + index * 40
        data[section:section + len(name)] = name
        u32(section + 8, 0x200)
        u32(section + 12, 0x1000 * (index + 1))
        u32(section + 16, 0x200)
        u32(section + 20, 0x200 * (index + 1))
        u32(section + 36, 0x60000020 if not index else 0xc0000040)
    code = (b"\x48\xb8" + struct.pack("<Q", 42) + b"\xc3"
            if wide else b"\xb8\x2a\0\0\0\xc3")
    data[0x200:0x200 + len(code)] = code
    u32(directories + 8, 0x2000)
    u32(directories + 12, 40)
    u32(0x400, 0x2100)
    u32(0x40c, 0x2080)
    u32(0x410, 0x88fb7a93 if invalid else 0x2180)
    data[0x480:0x48c] = b"example.dll\0"
    word(0x500, (1 << (width * 8 - 1)) | 1)
    word(0x580, (1 << (width * 8 - 1)) | 1)
    if legacy:
        assert wide
        u32(directories + 5 * 8, 0x3000)
        u32(directories + 5 * 8 + 4, 20)
        struct.pack_into("<IIHIIH", data, 0x600, 0x1000, 10, 0xa002,
                         0x2000, 10, 0xa1c0)
    return bytes(data), base


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-pe-browse-") as directory:
        with Client(worker) as client:
            for name, wide, invalid, legacy in (
                    ("legacy-go-layout.exe", True, False, True),
                    ("inconsistent-dump.exe", False, True, False),
                    ("clean.exe", True, False, False)):
                path = Path(directory) / name
                data, base = image(wide, invalid, legacy)
                path.write_bytes(data)
                before = hashlib.sha256(data).hexdigest()
                opened = client.call("open", {"path": str(path), "read_only": True})
                assert opened["status"] == "ok", opened
                payload = opened["payload"]
                codes = {item["code"] for item in payload["loader_diagnostics"]}
                if legacy:
                    assert codes == {"pe.relocations_word_aligned"}, payload
                    assert payload["warnings"], payload
                elif invalid:
                    assert payload["entry_address"] == "0x0", payload
                    assert payload["import_count"] == 0, payload
                    # Invalid loader writes also prevent proving that PE
                    # headers retain their original bytes in the image.
                    assert codes == {"pe.entry_out_of_range", "pe.headers_unmapped",
                                     "pe.import_descriptor_invalid"}, payload
                    assert len(payload["warnings"]) == 3, payload
                else:
                    assert not codes and not payload["warnings"], payload
                assembly = client.call("disasm", {"address": hex(base + 0x1000), "limit": 4})
                assert assembly["status"] == "ok" and assembly["payload"]["items"], assembly
                metadata = client.call("metadata")
                assert metadata["status"] == "ok", metadata
                assert metadata["payload"]["loader_diagnostics"] == payload["loader_diagnostics"]
                assert hashlib.sha256(path.read_bytes()).hexdigest() == before
            print("real PE worker: legacy relocations, invalid dump metadata, diagnostic reset, mapped disassembly and source preservation passed")


if __name__ == "__main__":
    run(sys.argv[1])
