#!/usr/bin/env python3
"""Real native C ABI mapped pages through worker IPC; old engines skip explicitly."""
from pathlib import Path
import struct
import sys
import tempfile
from transport_test import Client


def native_elf(arm64, base):
    code = bytes.fromhex("e0008052c0035fd6" if arm64 else "b807000000c3")
    entry = base + 120
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 183 if arm64 else 62, 1,
                         entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, 120 + len(code), 120 + len(code), 4096)
    return header + segment + code, entry


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-native-map-") as directory:
        for arm64 in (False, True):
            client = Client(executable)
            try:
                data, entry = native_elf(arm64, 0xffff800000400000 if arm64 else 0x400000)
                binary = Path(directory) / ("arm64.elf" if arm64 else "x86.elf")
                binary.write_bytes(data)
                assert client.call("open", {"path": str(binary), "read_only": True})["status"] == "ok"
                decoded = client.call("disasm", {"address": hex(entry), "limit": 2})
                assert decoded["status"] == "ok", decoded
                instruction_addresses = {row["address"] for row in decoded["payload"]["items"]}
                for stage in ("low", "med"):
                    full = client.call("decompile", {"address": hex(entry), "representation": stage, "limit": 2048})
                    assert full["status"] == "ok", full
                    result = full["payload"]
                    if result["mapping_status"] == "unavailable_engine_api":
                        print("SKIP: matching engine predates additive instruction mapping API")
                        return 77
                    assert result["mapping_status"] == "instruction_anchors", result
                    assert result["complete"]
                    mapped = [row for row in result["rows"] if row["addresses"]]
                    assert mapped, (stage, result)
                    assert all(set(row["addresses"]) <= instruction_addresses for row in mapped), (decoded, mapped)
                    assert all(address == address.lower() for row in mapped for address in row["addresses"])
                    text, rows, offset = "", [], 0
                    while True:
                        reply = client.call("decompile", {"address": hex(entry), "representation": stage, "offset": offset, "limit": 2})
                        assert reply["status"] == "ok", reply
                        page = reply["payload"]
                        assert page["revision"] == reply["revision"] and page["project_id"] == reply["project_id"]
                        text += page["text"]
                        rows.extend(page["rows"])
                        if page["complete"]:
                            break
                        assert page["next_offset"] > offset
                        offset = page["next_offset"]
                    assert text == result["text"] and rows == result["rows"]
                for stage in ("c", "high", "llvm"):
                    reply = client.call("decompile", {"address": hex(entry), "representation": stage, "limit": 2})
                    assert reply["status"] == "ok", reply
                    assert reply["payload"]["mapping_status"] == "unsupported_representation"
                    assert reply["payload"]["rows"] == []
            finally:
                client.close()
        print("real native x86/AArch64 high-VA mapping: Low/Med exact instruction anchors, canonical hex, stable paged rows and explicit C/High/LLVM mapping status passed")
        return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1]))
