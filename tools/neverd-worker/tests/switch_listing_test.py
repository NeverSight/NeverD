#!/usr/bin/env python3
"""A recovered switch lists its cases as code; skipped junk stays bytes.

One PE holds two functions whose bytes after an indirect jump differ in kind:

* an MSVC x64 SEH termination handler whose selector is sign-extended from
  the establisher frame before an unsigned 32-bit range check, dispatching
  through an image-relative table that its unwind range also covers.  Every
  case is code the recovered body reaches.
* an obfuscated constant branch whose junk byte would swallow its target if
  decoded linearly.  Only the target is code; the junk byte stays a byte.
"""
from pathlib import Path
import struct
import sys
import tempfile

from pe_browse_test import image
from transport_test import Client


BASE = 0x140000000
SWITCH = BASE + 0x1000
JUNK = BASE + 0x1100
CASES = 6


def switch_code():
    """The handler's bytes, its case addresses and its table's address."""
    code = bytearray()
    code += bytes.fromhex("4055")          # push rbp
    code += bytes.fromhex("4883ec20")      # sub rsp, 20h
    code += bytes.fromhex("488bea")        # mov rbp, rdx
    code += bytes.fromhex("488b4540")      # mov rax, [rbp+40h]
    code += bytes.fromhex("486308")        # movsxd rcx, dword ptr [rax]
    code += bytes.fromhex("83f905")        # cmp ecx, 5
    code += bytes.fromhex("0f87")          # ja epilogue
    to_epilogue = len(code)
    code += bytes(4)
    code += bytes.fromhex("488d15")        # lea rdx, [rip+__ImageBase]
    code += struct.pack("<i", BASE - (SWITCH + len(code) + 4))
    code += bytes.fromhex("8b8c8a")        # mov ecx, [rdx+rcx*4+table]
    table_field = len(code)
    code += bytes(4)
    code += bytes.fromhex("4803ca")        # add rcx, rdx
    jump = SWITCH + len(code)
    code += bytes.fromhex("ffe1")          # jmp rcx
    cases, exits = [], []
    for case in range(CASES):
        cases.append(SWITCH + len(code))
        code += bytes([0xC7, 0x45, 0x20, case, 0, 0, 0])  # mov [rbp+20h], case
        if case + 1 < CASES:
            code += bytes([0xEB, 0])                       # jmp epilogue
            exits.append(len(code) - 1)
    epilogue = len(code)
    for at in exits:
        code[at] = epilogue - (at + 1)
    struct.pack_into("<i", code, to_epilogue, epilogue - (to_epilogue + 4))
    code += bytes.fromhex("4883c4205dc3")  # add rsp, 20h; pop rbp; retn
    code_end = SWITCH + len(code)
    while len(code) % 4:
        code += b"\xcc"
    table = SWITCH + len(code)
    struct.pack_into("<I", code, table_field, table - BASE)
    for case in cases:
        code += struct.pack("<I", case - BASE)
    return bytes(code), jump, cases, code_end, table


def junk_code():
    """mov rax, target-0Bh; add rax, 0Bh; jmp rax; db 0E9h; target: retn."""
    target = JUNK + 0x11
    code = (bytes.fromhex("48b8") + struct.pack("<Q", target - 0xB) +
            bytes.fromhex("4883c00b") + bytes.fromhex("ffe0") + b"\xe9" +
            b"\xc3")
    assert JUNK + len(code) - 1 == target
    return code, JUNK + 0x10, target


def fixture():
    data = bytearray(image()[0])
    optional = 0x98
    directories = optional + 112
    switch, jump, cases, code_end, table = switch_code()
    junk, junk_byte, target = junk_code()
    data[0x200:0x400] = b"\xcc" * 0x200
    data[0x200:0x200 + len(switch)] = switch
    data[0x300:0x300 + len(junk)] = junk
    # The third section holds the unwind records instead of relocations.
    section = optional + 0xF0 + 80
    data[section:section + 8] = b".pdata\0\0"
    data[0x600:0x800] = bytes(0x200)
    records = [(0x1000, table + 4 * CASES - BASE, 0x3060),
               (0x1100, 0x1100 + len(junk), 0x3070)]
    for index, record in enumerate(records):
        struct.pack_into("<III", data, 0x600 + index * 12, *record)
    for at in (0x660, 0x670):
        data[at:at + 4] = bytes([1, 0, 0, 0])
    struct.pack_into("<II", data, directories + 3 * 8, 0x3000, 12 * len(records))
    return bytes(data), jump, cases, code_end, table, junk_byte, target


def ok(client, operation, payload=None):
    request = client.send(operation, payload or {})
    result = client.next(lambda row: row.get("type") == "response" and
                         row.get("request_id") == request and
                         row.get("status") != "progress", timeout=60)
    assert result["status"] == "ok", result
    return result["payload"]


def rows_at(client, address, after):
    return ok(client, "listing", {"address": hex(address), "before": 0,
                                  "after": after})["lines"]


def run(worker):
    binary, jump, cases, code_end, table, junk_byte, target = fixture()
    with tempfile.TemporaryDirectory(prefix="neverd-switch-listing-") as directory:
        path = Path(directory) / "switch.dll"
        path.write_bytes(binary)
        with Client(worker) as client:
            ok(client, "open", {"path": str(path)})
            ok(client, "analyze")
            rows = rows_at(client, SWITCH, 80)
            text = "\n".join(row["text"] for row in rows)
            by_address = {}
            for row in rows:
                by_address.setdefault(int(row["address"], 16), []).append(row)
            for case in cases:
                assert any(row["kind"] == "insn" for row in by_address.get(case, [])), \
                    (hex(case), text)
            # No byte between the dispatch and the end of the code is data.
            for address, lines in by_address.items():
                if jump < address < code_end:
                    assert all(row["kind"] != "data" for row in lines), \
                        (hex(address), text)
            assert any("switch jump" in row["text"] for row in by_address[jump]), text
            assert any("jump table for switch statement" in row["text"] and
                       "rva " in row["text"] for row in by_address[table]), text
            for slot, case in enumerate(cases):
                assert any(row.get("target") == hex(case)
                           for row in by_address.get(table + 4 * slot, [])), \
                    (slot, text)

            rows = rows_at(client, JUNK, 20)
            text = "\n".join(row["text"] for row in rows)
            assert any(int(row["address"], 16) == junk_byte and "db" in row["text"]
                       and "0E9h" in row["text"] for row in rows), text
            assert any(int(row["address"], 16) == target and row["kind"] == "insn"
                       and "ret" in row["text"] for row in rows), text


if __name__ == "__main__":
    run(sys.argv[1])
