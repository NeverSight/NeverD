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


def paged_view(client, entry, stage):
    text, rows, offset = "", [], 0
    while True:
        reply = client.call("decompile", {"address": hex(entry), "representation": stage,
                                           "offset": offset, "limit": 2})
        assert reply["status"] == "ok", reply
        page = reply["payload"]
        assert page["revision"] == reply["revision"] and page["project_id"] == reply["project_id"]
        text += page["text"]
        rows.extend(page["rows"])
        if page["complete"]:
            return text, rows
        assert page["next_offset"] > offset
        offset = page["next_offset"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-native-map-") as directory:
        for arm64 in (False, True):
            client = Client(executable)
            try:
                data, entry = native_elf(arm64, 0xffff800000400000 if arm64 else 0x400000)
                binary = Path(directory) / ("arm64.elf" if arm64 else "x86.elf")
                binary.write_bytes(data)
                opened = client.call("open", {"path": str(binary), "read_only": True})
                assert opened["status"] == "ok"
                assert opened["analysis_state"] == "not_analyzed"
                assert opened["payload"]["function_count"] == 0
                decoded = client.call("disasm", {"address": hex(entry), "limit": 2})
                assert decoded["status"] == "ok", decoded
                instruction_addresses = {row["address"] for row in decoded["payload"]["items"]}
                for stage in ("low", "med"):
                    full = client.call("decompile", {"address": hex(entry), "representation": stage, "limit": 2048})
                    assert full["status"] == "ok", full
                    assert full["analysis_state"] == "not_analyzed", full
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
                    text, rows = paged_view(client, entry, stage)
                    assert text == result["text"] and rows == result["rows"], (stage, result["text"], text)
                # Low above was the first request for an undiscovered entry.
                # Give Med the same cold context, starting with a small page:
                # preparing either IR may discover its workbench display name.
                with Client(executable) as cold:
                    fresh = cold.call("open", {"path": str(binary), "read_only": True})
                    assert fresh["status"] == "ok", fresh
                    assert fresh["payload"]["function_count"] == 0, fresh
                    text, rows = paged_view(cold, entry, "med")
                    reply = cold.call("decompile", {"address": hex(entry), "representation": "med", "limit": 2048})
                    assert reply["status"] == "ok", reply
                    full = reply["payload"]
                    assert text == full["text"] and rows == full["rows"], ("cold med", full["text"], text)
                # Function views prepare only their requested entry. The
                # explicit whole-image pipeline publishes complete analysis.
                analyzed = client.call("analyze")
                assert analyzed["status"] == "ok", analyzed
                assert analyzed["analysis_state"] == "complete", analyzed
                # The shared session publishes its discovered function to lists.
                functions = client.call("functions")
                assert functions["status"] == "ok", functions
                assert functions["revision"] != opened["revision"]
                assert functions["payload"]["total"] == 1, functions
                function = functions["payload"]["items"][0]
                assert function["address"] == hex(entry)
                assert function["size"] == (8 if arm64 else 6)
                for query in (function["name"], hex(entry + 1)):
                    resolved = client.call("resolve", {"query": query})
                    assert resolved["status"] == "ok", resolved
                    assert resolved["payload"]["function_address"] == hex(entry)
                for stage in ("c", "llvmc"):
                    reply = client.call("decompile", {"address": hex(entry), "representation": stage, "limit": 2048})
                    assert reply["status"] == "ok", reply
                    assert reply["payload"]["mapping_status"] in ("library_regions", "instruction_anchors"), reply
                    assert reply["payload"]["library_regions"] == []
                    assert reply["payload"]["byte_offset"] == 0
                    result = reply["payload"]
                    if result["mapping_status"] == "instruction_anchors":
                        mapped = [row for row in result["rows"] if row["addresses"]]
                        assert mapped, result
                        assert all(set(row["addresses"]) <= instruction_addresses for row in mapped), result
                        returned = [row for row in mapped if "return" in row.get("code_anchor", "")]
                        assert returned, result
                        assert returned[0]["addresses"][0] == hex(entry + (4 if arm64 else 5)), result
                        # Paging and worker presentation edits preserve the exact anchors.
                        text, rows, offset = "", [], 0
                        while True:
                            page = client.call("decompile", {"address": hex(entry), "representation": stage,
                                                              "offset": offset, "limit": 2})["payload"]
                            text += page["text"]
                            rows.extend(page["rows"])
                            if page["complete"]:
                                break
                            offset = page["next_offset"]
                        assert text == result["text"] and rows == result["rows"]
                    else:
                        assert all(row["addresses"] == [] for row in result["rows"])
                for stage in ("high", "llvm"):
                    reply = client.call("decompile", {"address": hex(entry), "representation": stage, "limit": 2})
                    assert reply["status"] == "ok", reply
                    assert reply["payload"]["mapping_status"] == "unsupported_representation"
                    assert reply["payload"]["rows"] == []
            finally:
                client.close()
        print("real native x86/AArch64: recovered function navigation, high-VA Low/Med instruction anchors, stable paged rows and explicit C/High/LLVM mapping status passed")
        return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1]))
