#!/usr/bin/env python3
"""Recovered ownership survives the detailed-instruction retention bound."""
from pathlib import Path
import struct
import sys
import tempfile

from transport_test import Client


def fixture(arm64, count):
    base = 0x400000 if arm64 else 0x140000000
    entry = base + 120
    code = bytearray()
    addresses = []
    for index in range(count):
        addresses.append(entry + len(code))
        if index % 4096 == 4095:
            code += bytes.fromhex("01000014" if arm64 else "eb00")
        else:
            code += (bytes.fromhex("1f2003d5") if arm64 else
                     bytes.fromhex("0f1f00") if index % 2 else b"\x90")
    branch = entry + len(code)
    code += bytes.fromhex("05000014" if arm64 else "eb10")
    hole = entry + len(code)
    code += b"\xff" * 16
    tail = entry + len(code)
    code += bytes.fromhex("c0035fd6" if arm64 else "c3")
    image = bytearray(120) + code
    while len(image) % 8:
        image.append(0)
    symbols = len(image)
    image += bytes(24) + struct.pack("<IBBHQQ", 1, 0x12, 0, 1, entry, len(code))
    strings = len(image)
    image += b"\0large_function\0"
    section_names = len(image)
    names = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    image += names
    while len(image) % 8:
        image.append(0)
    sections = len(image)
    for section in (
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, entry, 120, len(code), 0, 0, 1, 0),
        (7, 2, 0, 0, symbols, 48, 3, 1, 8, 24),
        (15, 3, 0, 0, strings, 16, 0, 0, 1, 0),
        (23, 3, 0, 0, section_names, len(names), 0, 0, 1, 0),
    ):
        image += struct.pack("<IIQQQQIIQQ", *section)
    image[:64] = struct.pack(
        "<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
        2, 183 if arm64 else 62, 1, entry, 64, sections, 0,
        64, 56, 1, 64, 5, 4)
    image[64:120] = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                120 + len(code), 120 + len(code), 4096)
    return image, entry, addresses, branch, hole, tail


def run(worker):
    retained = 1 << 18  # kListingMaxRecoveredInstructions in ResourceLimits.h
    with tempfile.TemporaryDirectory(prefix="neverd-recovered-pages-") as directory:
        for arm64, count in ((True, retained * 2 + 3), (False, retained + 29)):
            image, entry, addresses, branch, hole, tail = fixture(arm64, count)
            binary = Path(directory) / ("arm64.elf" if arm64 else "x64.elf")
            binary.write_bytes(image)
            with Client(worker) as client:
                opened = client.call("open", {"path": str(binary), "analysis": False,
                                              "read_only": True, "warm": False}, timeout=30)
                assert opened["status"] == "ok", opened

                def page(address):
                    response = client.call("listing", {"address": hex(address),
                        "recover_address": hex(address), "before": 3, "after": 48,
                        "opcode_bytes": 8}, timeout=90)
                    assert response["status"] == "ok", response
                    return response["payload"]["lines"]

                # Start beyond the bound, then cross it both ways, revisit the
                # entry, and return to the tail. No navigation order can drop
                # ownership or reinterpret operand bytes as another stream.
                first = page(tail)
                for address in (tail, addresses[retained], addresses[retained - 1],
                                addresses[retained + 1], entry, branch, tail):
                    rows = page(address)
                    instruction = next((row for row in rows if row["kind"] == "insn"
                                        and int(row["address"], 16) == address), None)
                    assert instruction is not None, (arm64, hex(address), rows)
                    assert instruction.get("function_address") == hex(entry), instruction
                    size = int(instruction["item_size"], 16)
                    offset = address - entry + 120
                    expected = " ".join(f"{byte:02X}" for byte in image[offset:offset + size])
                    assert instruction["text"].startswith(expected), instruction
                    if address == branch:
                        assert instruction["flow"] == "jump", instruction
                        assert f"locret_{tail:X}" in instruction["text"], instruction
                    assert not any("recovery failed" in row.get("text", "") for row in rows)
                last = page(tail)
                assert first == last
                assert not any(row["kind"] == "insn" and
                               hole <= int(row["address"], 16) < tail for row in page(hole))
                if not arm64:
                    address = addresses[retained + 1]
                    rows = page(address + 1)
                    assert any(row["kind"] == "insn" and
                               int(row["address"], 16) == address and
                               row.get("function_address") == hex(entry) for row in rows)
                print(f"{'AArch64' if arm64 else 'x64'}: {count + 2} recovered instructions; "
                      "paged tail, boundary crossing, holes and ownership passed", flush=True)


if __name__ == "__main__":
    run(sys.argv[1])
