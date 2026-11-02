#!/usr/bin/env python3
"""Real PE listing: entry contracts, frame facts, metadata and padding bounds."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile

from pe_browse_test import image
from transport_test import Client


BASE = 0x140000000
ENTRY = BASE + 0x1000
BODY = BASE + 0x1040


def fixture():
    binary, _ = image()
    data = bytearray(binary)
    struct.pack_into("<H", data, 0x96, 0x2022)
    optional = 0x98
    directories = optional + 112
    struct.pack_into("<II", data, directories + 3 * 8, 0x3000, 48)
    section = optional + 0xF0 + 80
    data[section:section + 8] = b".pdata\0\0"
    data[0x200:0x400] = b"\xcc" * 0x200
    data[0x200:0x205] = b"\xe9" + struct.pack("<i", BODY - ENTRY - 5)
    code = bytes.fromhex(
        "488da42480ffffff"  # lea rsp,[rsp-80h]
        "48896c2410"        # mov [rsp+10h],rbp
        "4889e5"            # mov rbp,rsp
        "9c8f4500"          # pushfq; pop [rbp]
        "4c897578"          # mov [rbp+78h],r14
        "488b4578"          # mov rax,[rbp+78h]
        "488d6d08"          # lea rbp,[rbp+8]
        "488b4570"          # mov rax,[rbp+70h], the same slot
        "488d6df8"          # lea rbp,[rbp-8]
        "31ed"              # xor ebp,ebp: ends the proved frame alias
        "488b4578"          # mov rax,[rbp+78h], now not a frame access
        "488b6c2410"        # mov rbp,[rsp+10h]
        "488da42480000000c3")
    data[0x240:0x240 + len(code)] = code
    adjusted = bytes.fromhex("4883c110") + b"\xe9" + struct.pack("<i", 0x1040 - 0x1109)
    data[0x300:0x309] = adjusted
    data[0x340] = 0xC3
    ranges = [(0x1000, 0x1030, 0x3060),
              (0x1040, 0x1040 + len(code), 0x3070),
              (0x1100, 0x1109, 0x3090), (0x1140, 0x1141, 0x3080)]
    data[0x600:0x800] = bytes(0x200)
    for index, record in enumerate(ranges):
        struct.pack_into("<III", data, 0x600 + index * 12, *record)
    for at in (0x660, 0x670, 0x690):
        data[at:at + 4] = bytes([1, 0, 0, 0])
    data[0x680:0x684] = bytes([0x21, 0, 0, 0])
    struct.pack_into("<III", data, 0x684, *ranges[1])
    return bytes(data)


def ok(client, operation, payload=None):
    request = client.send(operation, payload or {})
    result = client.next(lambda row: row.get("type") == "response" and
                         row.get("request_id") == request and
                         row.get("status") != "progress", timeout=60)
    assert result["status"] == "ok", result
    return result["payload"]


def listing(client, address, after=55):
    rows = ok(client, "listing", {"address": hex(address), "after": after})["lines"]
    return rows, "\n".join(row["text"] for row in rows)


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-pe-listing-facts-") as directory:
        path = Path(directory) / "entry.dll"
        binary = fixture()
        path.write_bytes(binary)
        digest = hashlib.sha256(binary).hexdigest()
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            first = ok(client, "listing", {"address": hex(ENTRY), "after": 1,
                                           "complete_anchor_item": True})["lines"]
            assert any(row["kind"] == "insn" and row["address"] == hex(ENTRY)
                       for row in first), first
            assert all(row["item"] == hex(ENTRY) for row in first), first
            _, initial_record = listing(client, BASE + 0x3000, 8)
            assert "RUNTIME_FUNCTION <rva DllEntryPoint," in initial_record, initial_record
            ok(client, "resolve", {"query": hex(BODY)})
            _, text = listing(client, ENTRY)
            assert "DllEntryPoint" in text and "Attributes: thunk" in text, text
            assert "HINSTANCE" in text and "fdwReason" in text and "lpReserved" in text, text
            assert "bp-based frame" not in text.split("DllEntryPoint   endp")[0], text
            assert "db 2Bh dup(0CCh)" in text, text
            signature = ok(client, "function_type", {"address": hex(ENTRY)})
            assert len(signature["parameters"]) == 3, signature
            assert [parameter["location"] for parameter in signature["parameters"]] == ["rcx", "edx", "r8"], signature
            rows, text = listing(client, BODY)
            assert "bp-based frame" in text and "var_s78" in text, text
            loads = [row["text"] for row in rows if "mov" in row["text"] and "rax," in row["text"]]
            assert any("[rbp+var_s78]" in line for line in loads), loads
            assert any("[rbp-8+var_s78]" in line for line in loads), loads
            assert any("[rbp+78h]" in line for line in loads), loads
            # The frame's saved base still has an exact RSP interpretation.
            assert any("var_s10" in row["text"] for row in rows), text
            _, adjusted = listing(client, BASE + 0x1100, 12)
            assert "Attributes: thunk" not in adjusted, adjusted
            refs = ok(client, "xrefs", {"address": hex(ENTRY)})["items"]
            assert any(row["from"] == hex(BASE + 0x3000) and
                       row.get("metadata_role") == "range_begin" for row in refs), refs
            assert all(not row.get("function") for row in refs
                       if row.get("provenance") == "metadata"), refs
            from_record = ok(client, "xrefs", {"address": hex(BASE + 0x3000), "direction": "from"})["items"]
            assert {row.get("metadata_role") for row in from_record} == {
                "range_begin", "range_end", "unwind_info"}, from_record
            chain = ok(client, "xrefs", {"address": hex(BODY)})["items"]
            assert any(row["from"] == hex(BASE + 0x3084) and
                       row.get("metadata_role") == "range_begin" for row in chain), chain
            _, record = listing(client, BASE + 0x3000, 8)
            assert "RUNTIME_FUNCTION <rva DllEntryPoint," in record, record
            assert record == initial_record, (initial_record, record)
            end_refs = ok(client, "xrefs", {"address": hex(BASE + 0x1030)})["items"]
            assert any(row.get("metadata_role") == "range_end" for row in end_refs), end_refs
            assert not ok(client, "functions", {"filter": hex(BASE + 0x1030)[2:]})["items"]
            ok(client, "rename", {"address": hex(ENTRY + 0x18), "name": "padding_marker"})
            rows, _ = listing(client, ENTRY + 5, 12)
            assert all(not (int(row["item"], 16) < ENTRY + 0x18 <
                            int(row["item"], 16) + int(row["item_size"], 16))
                       for row in rows), rows
            item = ok(client, "item_define", {"address": hex(ENTRY + 0x18), "action": "data", "size": 1})
            assert item["size"] == 1 and item["saved"], item
            rows, _ = listing(client, ENTRY + 0x18, 3)
            assert int(rows[0]["item"], 16) == ENTRY + 0x18 and int(rows[0]["item_size"], 16) == 1, rows
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    print("PE listing facts: frame aliases/clobbers, DLL contract, pure thunk, typed RVA roles, chain records, padding and user boundaries passed")


if __name__ == "__main__":
    run(sys.argv[1])
