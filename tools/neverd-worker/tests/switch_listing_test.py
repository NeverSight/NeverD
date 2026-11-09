#!/usr/bin/env python3
"""A recovered switch lists its cases as code; skipped junk stays bytes.

One PE holds two functions whose bytes after an indirect jump differ in kind:

* an MSVC x64 SEH termination handler whose selector is sign-extended from
  the establisher frame before an unsigned 32-bit range check, dispatching
  through an image-relative table that its unwind range also covers.  Every
  case is code the recovered body reaches.
* an obfuscated constant branch whose junk byte would swallow its target if
  decoded linearly.  Only the target is code; the junk byte stays a byte.
* MSVC's CRT character classifier, a two-level switch: the character indexes
  a byte table whose entry selects a slot of an image-relative table, both
  after the code in its unwind range.  The slots list as `rva` entries and
  the index table four entries a line.

* a function whose instruction after a nop has its address taken, as a C++
  catch funclet returns the address its parent continues at: it lists under
  its own `loc_` label with that reference.

Both switches carry a classic listing's annotations: the case count on the
bounds check's compare, the default's `def_` name, each case's values on its
code and on the branches to it, and the tables named in their loads, which
read them, beside the image base the loads add (`unk_140000000`, the header).
"""
from pathlib import Path
import re
import struct
import sys
import tempfile

from pe_browse_test import image
from transport_test import Client


BASE = 0x140000000
SWITCH = BASE + 0x1000
JUNK = BASE + 0x1100
TWO_LEVEL = BASE + 0x1140
TAKEN = BASE + 0x11E0
CASES = 6
# The table slot each character selects: cases for 0..10 and 20h, the
# default for the rest, as the CRT's table holds them.
SELECTED = [0, 1, 2, 0, 3, 1, 2, 3, 3, 1, 2] + [4] * 21 + [0]


def switch_code():
    """The handler's bytes, its case addresses, the end of its code, its
    table's address, and the addresses of its compare, table load, image base
    load and default."""
    code = bytearray()
    code += bytes.fromhex("4055")          # push rbp
    code += bytes.fromhex("4883ec20")      # sub rsp, 20h
    code += bytes.fromhex("488bea")        # mov rbp, rdx
    code += bytes.fromhex("488b4540")      # mov rax, [rbp+40h]
    code += bytes.fromhex("486308")        # movsxd rcx, dword ptr [rax]
    compare = SWITCH + len(code)
    code += bytes.fromhex("83f905")        # cmp ecx, 5
    code += bytes.fromhex("0f87")          # ja epilogue
    to_epilogue = len(code)
    code += bytes(4)
    image_base = SWITCH + len(code)
    code += bytes.fromhex("488d15")        # lea rdx, [rip+__ImageBase]
    code += struct.pack("<i", BASE - (SWITCH + len(code) + 4))
    load = SWITCH + len(code)
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
    marks = {"compare": compare, "load": load, "image_base": image_base,
             "default": SWITCH + epilogue}
    return bytes(code), jump, cases, code_end, table, marks


def junk_code():
    """mov rax, target-0Bh; add rax, 0Bh; jmp rax; db 0E9h; target: retn."""
    target = JUNK + 0x11
    code = (bytes.fromhex("48b8") + struct.pack("<Q", target - 0xB) +
            bytes.fromhex("4883c00b") + bytes.fromhex("ffe0") + b"\xe9" +
            b"\xc3")
    assert JUNK + len(code) - 1 == target
    return code, JUNK + 0x10, target


def two_level_code():
    """The classifier's bytes, dispatch, case and default addresses, the
    first padding byte, both tables' addresses and the addresses of its
    compare and its two loads."""
    code = bytearray()
    code += bytes.fromhex("4883ec28")      # sub rsp, 28h
    compare = TWO_LEVEL + len(code)
    code += bytes.fromhex("80f920")        # cmp cl, 20h
    code += bytes.fromhex("77")            # ja default
    to_default = len(code)
    code += bytes(1)
    code += bytes.fromhex("0fb6c1")        # movzx eax, cl
    code += bytes.fromhex("488d15")        # lea rdx, [rip+__ImageBase]
    code += struct.pack("<i", BASE - (TWO_LEVEL + len(code) + 4))
    index_load = TWO_LEVEL + len(code)
    code += bytes.fromhex("0fb68402")      # movzx eax, byte ptr [rdx+rax+index]
    index_field = len(code)
    code += bytes(4)
    load = TWO_LEVEL + len(code)
    code += bytes.fromhex("8b8c82")        # mov ecx, [rdx+rax*4+table]
    table_field = len(code)
    code += bytes(4)
    code += bytes.fromhex("4803ca")        # add rcx, rdx
    jump = TWO_LEVEL + len(code)
    code += bytes.fromhex("ffe1")          # jmp rcx
    slots = []
    for value in (1, 2, 3, 5):
        slots.append(TWO_LEVEL + len(code))
        code += bytes([0xB0, value])         # mov al, value
        code += bytes.fromhex("4883c428c3")  # add rsp, 28h; retn
    slots.append(TWO_LEVEL + len(code))
    code[to_default] = len(code) - (to_default + 1)
    code += bytes.fromhex("32c04883c428c3")  # xor al, al; add rsp, 28h; retn
    padding = TWO_LEVEL + len(code)
    # int3, then nops up to the table, as MSVC aligns it.
    if len(code) % 4:
        code += b"\xcc"
    while len(code) % 4:
        code += b"\x90"
    table = TWO_LEVEL + len(code)
    struct.pack_into("<I", code, table_field, table - BASE)
    for slot in slots:
        code += struct.pack("<I", slot - BASE)
    index = TWO_LEVEL + len(code)
    struct.pack_into("<I", code, index_field, index - BASE)
    code += bytes(SELECTED)
    marks = {"compare": compare, "index_load": index_load, "load": load}
    return bytes(code), jump, slots, padding, table, index, marks


def taken_code():
    """lea rax, [rip+1]; nop; target: xor eax, eax; retn -- the address of
    target taken."""
    return bytes.fromhex("488d0501000000" "90" "31c0" "c3"), TAKEN + 8


def fixture():
    data = bytearray(image()[0])
    optional = 0x98
    directories = optional + 112
    switch, jump, cases, code_end, table, marks = switch_code()
    junk, junk_byte, target = junk_code()
    two_level, two_level_jump, slots, padding, slot_table, index, \
        two_level_marks = two_level_code()
    taken, taken_target = taken_code()
    data[0x200:0x400] = b"\xcc" * 0x200
    data[0x200:0x200 + len(switch)] = switch
    data[0x300:0x300 + len(junk)] = junk
    data[0x340:0x340 + len(two_level)] = two_level
    assert 0x340 + len(two_level) <= 0x3E0
    data[0x3E0:0x3E0 + len(taken)] = taken
    # The third section holds the unwind records instead of relocations.
    section = optional + 0xF0 + 80
    data[section:section + 8] = b".pdata\0\0"
    data[0x600:0x800] = bytes(0x200)
    records = [(0x1000, table + 4 * CASES - BASE, 0x3060),
               (0x1100, 0x1100 + len(junk), 0x3070),
               (0x1140, 0x1140 + len(two_level), 0x3080),
               (0x11E0, 0x11E0 + len(taken), 0x3090)]
    for number, record in enumerate(records):
        struct.pack_into("<III", data, 0x600 + number * 12, *record)
    for at in (0x660, 0x670, 0x680, 0x690):
        data[at:at + 4] = bytes([1, 0, 0, 0])
    struct.pack_into("<II", data, directories + 3 * 8, 0x3000, 12 * len(records))
    return (bytes(data), jump, cases, code_end, table, marks, junk_byte,
            target,
            (two_level_jump, slots, padding, slot_table, index,
             two_level_marks), taken_target)


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


def refs_to(client, address):
    """The (address, type) of each reference to an address."""
    return {(item["address"], item["type"]) for item in
            ok(client, "xrefs", {"address": hex(address),
                                 "direction": "to"})["items"]}


def insn(by_address, address, text):
    """The instruction line at an address."""
    lines = [row for row in by_address.get(address, []) if row["kind"] == "insn"]
    assert lines, (hex(address), text)
    return lines[0]["text"]


def case_mark(jump, values):
    """The comment a classic listing gives the code of a switch's cases."""
    return "jumptable %016X %s" % (jump, values)


def check_two_level(client, jump, slots, padding, table, index, marks):
    """The classifier's cases are code, its slots `rva` entries and its index
    table lines of four entries; no byte of either table lists alone."""
    rows = rows_at(client, TWO_LEVEL, 140)
    text = "\n".join(row["text"] for row in rows)
    by_address = {}
    for row in rows:
        by_address.setdefault(int(row["address"], 16), []).append(row)
    for slot in slots:
        assert any(row["kind"] == "insn" for row in by_address.get(slot, [])), \
            (hex(slot), text)
    assert any("switch jump" in row["text"] for row in by_address[jump]), text
    assert any(re.search(r"\balign\s+4\b", row["text"])
               for row in by_address.get(padding, [])), text
    head = [row for row in by_address[table] if row["kind"] == "data"]
    assert head and head[0]["text"].split()[0] == "jpt_%X" % jump, text
    # The table's comment follows the references to it.
    assert any("jump table for switch statement" in row["text"]
               for row in by_address[table]), text
    for number, slot in enumerate(slots):
        lines = [row for row in by_address.get(table + 4 * number, [])
                 if row["kind"] == "data"]
        assert lines and "rva " in lines[0]["text"] and \
            lines[0].get("target") == hex(slot), (number, text)
    head = [row for row in by_address[index] if row["kind"] == "data"]
    assert head and head[0]["text"].split()[0] == "byte_%X" % index, text
    assert any("indirect table for switch statement" in row["text"]
               for row in by_address[index]), text
    for first in range(0, len(SELECTED), 4):
        values = ", ".join(str(value) for value in SELECTED[first:first + 4])
        lines = [row for row in by_address.get(index + first, [])
                 if row["kind"] == "data"]
        assert lines and re.search(r"\bdb\s+" + re.escape(values) + r"(\s|$)",
                                   lines[0]["text"]), (first, text)
    # Both tables tile with their slots and index lines, nothing else.
    items = sorted({(int(row["item"], 16), int(row["item_size"], 16))
                    for row in rows if row["kind"] == "data" and
                    table <= int(row["item"], 16) < index + len(SELECTED)})
    assert items == [(table + 4 * number, 4) for number in range(len(slots))] + \
        [(index + first, min(4, len(SELECTED) - first))
         for first in range(0, len(SELECTED), 4)], (items, text)
    # The compare counts the cases; the loads name the tables they read, and
    # each slot's code names the selector values that reach it, the
    # default's with the values past the table's covered ones.
    assert insn(by_address, marks["compare"], text).endswith(
        "; switch 33 cases"), text
    assert "ds:(byte_%X - 140000000h)[rdx+rax]" % index in \
        insn(by_address, marks["index_load"], text), text
    assert "ds:(jpt_%X - 140000000h)[rdx+rax*4]" % jump in \
        insn(by_address, marks["load"], text), text
    for slot, values in zip(slots, ("cases 0,3,32", "cases 1,5,9",
                                    "cases 2,6,10", "cases 4,7,8",
                                    "default case, cases 11-31")):
        assert insn(by_address, slot, text).endswith(
            "; " + case_mark(jump, values)), (hex(slot), text)
    assert any(row["kind"] == "label" and
               row["text"].startswith("def_%X:" % jump)
               for row in by_address[slots[-1]]), text
    assert (hex(marks["index_load"]), "r") in refs_to(client, index), text
    assert (hex(marks["load"]), "r") in refs_to(client, table), text
    for address in (table, index):
        assert any("\u2191r" in row["text"] for row in by_address[address]), \
            (hex(address), text)


def run(worker):
    binary, jump, cases, code_end, table, marks, junk_byte, target, \
        two_level, taken_target = fixture()
    with tempfile.TemporaryDirectory(prefix="neverd-switch-listing-") as directory:
        path = Path(directory) / "switch.dll"
        path.write_bytes(binary)
        with Client(worker) as client:
            ok(client, "open", {"path": str(path)})
            ok(client, "analyze")
            # A cross-reference request completes the reference index, so
            # the listing shows each table's readers and labels its cases.
            assert (hex(marks["load"]), "r") in refs_to(client, table)
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
            assert any("rva " in row["text"] for row in by_address[table]), text
            assert any("jump table for switch statement" in row["text"]
                       for row in by_address[table]), text
            for slot, case in enumerate(cases):
                assert any(row.get("target") == hex(case)
                           for row in by_address.get(table + 4 * slot, [])), \
                    (slot, text)
            # The compare counts the cases and the branch past the table
            # goes to the default; the load names the table, the image base
            # reads as the header's name, and each case's code names its
            # value.
            assert insn(by_address, marks["compare"], text).endswith(
                "; switch 6 cases"), text
            branch = insn(by_address, marks["compare"] + 3, text)
            assert "def_%X" % jump in branch and \
                branch.endswith("; " + case_mark(jump, "default case")), text
            assert insn(by_address, marks["image_base"], text).endswith(
                "unk_140000000"), text
            load = insn(by_address, marks["load"], text)
            assert "ds:(jpt_%X - 140000000h)[rdx+rcx*4]" % jump in load and \
                ";" not in load, text
            for value, case in enumerate(cases):
                assert insn(by_address, case, text).endswith(
                    "; " + case_mark(jump, "case %d" % value)), (value, text)
            assert any(row["kind"] == "label" and
                       row["text"].startswith("def_%X:" % jump)
                       for row in by_address[marks["default"]]), text
            assert insn(by_address, marks["default"], text).endswith(
                "; " + case_mark(jump, "default case")), text
            assert any("\u2191r" in row["text"] for row in by_address[table]), \
                text

            rows = rows_at(client, JUNK, 20)
            text = "\n".join(row["text"] for row in rows)
            assert any(int(row["address"], 16) == junk_byte and "db" in row["text"]
                       and "0E9h" in row["text"] for row in rows), text
            assert any(int(row["address"], 16) == target and row["kind"] == "insn"
                       and "ret" in row["text"] for row in rows), text

            check_two_level(client, *two_level)

            # The instruction whose address is taken lists under its label
            # with the reference that takes it.
            rows = rows_at(client, TAKEN, 12)
            text = "\n".join(row["text"] for row in rows)
            assert any(int(row["address"], 16) == taken_target and
                       row["kind"] == "label" and
                       row["text"].startswith("loc_%X:" % taken_target) and
                       "\u2191o" in row["text"] for row in rows), text


if __name__ == "__main__":
    run(sys.argv[1])
