#!/usr/bin/env python3
"""Real-engine x64 PE functions as .pdata describes them; the fixture is never
executed.

The first function's record covers the int3 its compiler pads it with after
its return; the second jumps to a cold part far below it, which a record
chained to its own covers, and which takes the address of a string.  The
entry point is a third, which analysis sizes by its code rather than by its
record.  The first two are exported, the second under two names, as a
linker that folds identical functions exports them.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from transport_test import Client
from constant_branch_test import verify_function_views

BASE = 0x140000000
PADDED, SPLIT, ENTRY, COLD = 0x1000, 0x1010, 0x1020, 0x1100
TEXT = 0x2100
COLD_SIZE = 10
EXPORTS = 0x2180
EXPORTED = (("Padded", PADDED), ("Split", SPLIT), ("SplitAlias", SPLIT))
UNW_FLAG_CHAININFO = 4


def image():
    data = bytearray(0x800)
    optional, optional_size = 0x98, 240
    directories = optional + 112
    def u16(at, value): struct.pack_into("<H", data, at, value)
    def u32(at, value): struct.pack_into("<I", data, at, value)
    data[:2] = b"MZ"
    u32(0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    u16(0x84, 0x8664)
    u16(0x86, 3)
    u16(0x94, optional_size)
    u16(0x96, 0x22)
    u16(optional, 0x20b)
    u32(optional + 16, ENTRY)
    struct.pack_into("<Q", data, optional + 24, BASE)
    u32(optional + 32, 0x1000)
    u32(optional + 36, 0x200)
    u32(optional + 56, 0x4000)
    u32(optional + 60, 0x200)
    u16(optional + 68, 3)
    u32(directories - 4, 16)
    # Code, unwind information, and the RUNTIME_FUNCTIONs.
    sections = ((b".text", 0x60000020), (b".rdata", 0x40000040), (b".pdata", 0x40000040))
    for index, (name, flags) in enumerate(sections):
        section = optional + optional_size + index * 40
        data[section:section + len(name)] = name
        u32(section + 8, 0x200)
        u32(section + 12, 0x1000 * (index + 1))
        u32(section + 16, 0x200)
        u32(section + 20, 0x200 * (index + 1))
        u32(section + 36, flags)
    text = 0x200
    data[text:text + 0x200] = b"\xcc" * 0x200
    def code(rva, encoded): data[text + rva - 0x1000:text + rva - 0x1000 + len(encoded)] = encoded
    code(PADDED, b"\x48\xc7\xc0\x2a\x00\x00\x00\xc3\xcc")  # mov rax, 42; ret; int3
    code(SPLIT, b"\xe9" + struct.pack("<i", COLD - (SPLIT + 5)))  # jmp cold
    code(ENTRY, b"\xc3")  # ret
    # lea rcx, [rip + TEXT]; xor eax, eax; ret
    code(COLD, b"\x48\x8d\x0d" + struct.pack("<i", TEXT - (COLD + 7)) + b"\x31\xc0\xc3")
    rdata = 0x400
    data[rdata:rdata + 4] = bytes([1, 0, 0, 0])
    data[rdata + 0x10:rdata + 0x14] = bytes([1, 0, 0, 0])
    data[rdata + 0x20:rdata + 0x24] = bytes([1 | UNW_FLAG_CHAININFO << 3, 0, 0, 0])
    struct.pack_into("<III", data, rdata + 0x24, SPLIT, SPLIT + 5, 0x2010)
    data[rdata + 0x30:rdata + 0x34] = bytes([1, 0, 0, 0])
    data[rdata + TEXT - 0x2000:rdata + TEXT - 0x2000 + 13] = b"ColdPathText\0"
    records = ((PADDED, PADDED + 9, 0x2000), (SPLIT, SPLIT + 5, 0x2010),
               (ENTRY, ENTRY + 1, 0x2030), (COLD, COLD + COLD_SIZE, 0x2020))
    for index, record in enumerate(records):
        struct.pack_into("<III", data, 0x600 + 12 * index, *record)
    u32(directories + 3 * 8, 0x3000)
    u32(directories + 3 * 8 + 4, 12 * len(records))
    # The export directory: its address table, sorted name pointers and
    # ordinals, then the names.  Each name has its own ordinal, as a linker
    # that folds two functions into one exports both.
    def at(rva): return rdata + rva - 0x2000
    functions = [rva for _, rva in EXPORTED]
    table, pointers, ordinals = EXPORTS + 0x30, EXPORTS + 0x3c, EXPORTS + 0x48
    names, module = EXPORTS + 0x50, b"parts.exe\0"
    struct.pack_into("<IIHHIIIIIII", data, at(EXPORTS), 0, 0, 0, 0,
                     names + sum(len(n) + 1 for n, _ in EXPORTED), 1, len(functions),
                     len(EXPORTED), table, pointers, ordinals)
    for index, rva in enumerate(functions):
        u32(at(table) + 4 * index, rva)
    cursor = names
    for index, (name, rva) in enumerate(EXPORTED):
        u32(at(pointers) + 4 * index, cursor)
        u16(at(ordinals) + 2 * index, index)
        data[at(cursor):at(cursor) + len(name) + 1] = name.encode() + b"\0"
        cursor += len(name) + 1
    data[at(cursor):at(cursor) + len(module)] = module
    u32(directories, EXPORTS)
    u32(directories + 4, cursor + len(module) - EXPORTS)
    return bytes(data)


def lines(client, address, after):
    listed = client.call("listing", {"address": hex(BASE + address), "before": 0, "after": after})
    assert listed["status"] == "ok", listed
    return listed["payload"]["lines"]


def cli_parts(cli, worker, path, entries):
    def call(operation, *arguments):
        result = subprocess.run([cli, operation, str(path), "--no-debug", "--json", *arguments],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert result.returncode == 0, (operation, result.stdout, result.stderr)
        return json.loads(result.stdout)

    # The CLI selects an existing function and decodes its linear bytes. It
    # must retain the same independent entries and references as the worker;
    # a cold part's address must never become another callable identity.
    for _ in range(2):
        functions = call("funcs")
        assert {int(row["addr"], 16) for row in functions} == entries, functions
        for name in ("Split", "SplitAlias", hex(BASE + SPLIT)):
            instructions = call("disasm", "--func", name)
            assert instructions and int(instructions[0]["addr"], 16) == BASE + SPLIT, instructions
            assert instructions[0]["mnemonic"] == "jmp", instructions
            assert instructions[0]["bytes"] == "e9eb000000", instructions
        refs = call("xrefs", "--addr", hex(BASE + TEXT))
        assert any(int(row["from"], 16) == BASE + COLD and
                   int(row["to"], 16) == BASE + TEXT for row in refs), refs

    # A CLI function rename persists through the shared SDK store. A new
    # worker still displays its cold part under that same owner and name.
    call("rename", "--func", "Split", "--to", "RenamedSplit")
    functions = call("funcs")
    assert {int(row["addr"], 16) for row in functions} == entries, functions
    assert next(row["name"] for row in functions if int(row["addr"], 16) == BASE + SPLIT) == "RenamedSplit", functions
    with Client(worker) as client:
        opened = client.call("open", {"path": str(path), "analysis": False, "warm": False})
        assert opened["status"] == "ok", opened
        cold = lines(client, COLD, 16)
        assert any("START OF FUNCTION CHUNK FOR RenamedSplit" in row["text"] for row in cold), cold
        body = [row for row in cold if row["kind"] == "insn" and
                BASE + COLD <= int(row["address"], 16) < BASE + COLD + COLD_SIZE]
        assert len(body) == 3 and all(row["function_address"] == hex(BASE + SPLIT) for row in body), body


def run(worker, cli=None):
    with tempfile.TemporaryDirectory(prefix="neverd-pe-parts-") as directory:
        path = Path(directory) / "parts.exe"
        path.write_bytes(image())
        with Client(worker) as client:
            assert client.call("open", {"path": str(path), "analysis": False, "warm": False})["status"] == "ok"
            listed = client.call("functions", {"limit": 10})["payload"]["items"]
            entries = {int(item["address"], 16) for item in listed}
            assert {BASE + PADDED, BASE + SPLIT} <= entries, listed
            assert BASE + COLD not in entries, listed

            # Exports name the functions their symbols leave unnamed, and each
            # name a function is exported under finds it.
            names = {int(item["address"], 16): item["name"] for item in listed}
            assert names[BASE + PADDED] == "Padded" and names[BASE + SPLIT] == "Split", listed
            for name, rva in EXPORTED:
                resolved = client.call("resolve", {"query": name})
                assert resolved["status"] == "ok" and resolved["payload"]["address"] == hex(BASE + rva), resolved

            # The padded function ends at its return; its int3 is padding.
            padded = [" ".join(line["text"].split()) for line in lines(client, PADDED, 40)]
            footer = next(i for i, text in enumerate(padded) if text.endswith(" endp"))
            assert padded[footer - 1] == "retn", padded
            assert not any(text == "int3" for text in padded[:footer]), padded

            # The jump reaches the cold part by a label, and the part lists
            # as a chunk of its function.
            split = [" ".join(line["text"].split()) for line in lines(client, SPLIT, 12)]
            assert "jmp loc_140001100" in split, split
            cold = lines(client, COLD, 16)
            texts = [" ".join(line["text"].split()) for line in cold]
            assert "; START OF FUNCTION CHUNK FOR Split" in texts, texts
            assert "; END OF FUNCTION CHUNK FOR Split" in texts, texts
            assert sum("END OF FUNCTION CHUNK FOR Split" in text for text in texts) == 1, texts
            assert not any(text.endswith(" endp") for text in texts), texts
            assert any(text.startswith("loc_140001100:") for text in texts), texts
            body = [line for line in cold if line["kind"] == "insn"
                    and BASE + COLD <= int(line["address"], 16) < BASE + COLD + COLD_SIZE]
            assert len(body) == 3 and all(line["function_address"] == hex(BASE + SPLIT) for line in body), body
            resolved = client.call("resolve", {"query": hex(BASE + COLD + 7)})
            assert resolved["payload"]["function_address"] == hex(BASE + SPLIT), resolved

            # Reading a cold part must select its owner's recovered CFG, not
            # register the part as a transient independent function. Alternate
            # both ranges as a listing refresh/prefetch does, and force the
            # inventory check that would otherwise invalidate every page.
            generation = None
            for _ in range(4):
                for rva in (SPLIT, COLD, PADDED):
                    page = client.call("listing", {
                        "address": hex(BASE + rva), "before": 0, "after": 12})
                    assert page["status"] == "ok", page
                    metadata = client.call("metadata")["payload"]
                    assert metadata["function_count"] == len(entries), metadata
                    current = page["payload"]["generation"]
                    if generation is not None:
                        assert current == generation, (generation, current)
                    generation = current
                    inventory = client.call("functions", {"limit": 10})["payload"]["items"]
                    assert {int(item["address"], 16) for item in inventory} == entries, inventory
                    assert not any("control-flow recovery failed" in row["text"]
                                   for row in page["payload"]["lines"]), page

            verify_function_views(client, BASE + COLD, BASE + SPLIT)
            verify_function_views(client, BASE + COLD + 7, BASE + SPLIT)

            # The part's reference to the string is its function's: the
            # string lists it, and so do its references.
            refs = client.call("xrefs", {"address": hex(BASE + TEXT)})["payload"]["items"]
            assert any(item["from"] == hex(BASE + COLD) for item in refs), refs
            strings = client.call("string_references", {"filter": "ColdPath"})["payload"]["items"]
            assert any(item["address"] == hex(BASE + COLD) for item in strings), strings
        if cli:
            cli_parts(cli, worker, path, entries)
            print("CLI/worker cold-chunk entries, instructions, references and function rename persistence passed")
    print("x64 PE padding, chained chunks and exported names list as IDA lists them")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cli")
    args = parser.parse_args()
    run(args.worker, args.cli)
