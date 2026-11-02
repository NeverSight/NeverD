#!/usr/bin/env python3
"""Recovered branch targets, skipped bytes and editable function instructions."""
import json
from pathlib import Path
import struct
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def verify_function_views(client, address, entry):
    """A fragment selects the same source, IR and graph as its proven owner."""
    address, entry = hex(address), hex(entry)
    for representation in ("c", "llvmc", "low", "med", "high", "llvm", "source"):
        request = {"address": address, "representation": representation, "limit": 2048}
        source = ok(client, "decompile", request)
        assert source["address"] == entry and source["text"], source
        if representation in ("c", "llvmc", "source"):
            assert "unknown register" not in source["text"], source
            assert "unknown value" not in source["text"], source
        owner = ok(client, "decompile", {**request, "address": entry})
        assert source["text"] == owner["text"], (source, owner)
        for selected in (address, entry):
            page = ok(client, "decompile", {**request, "address": selected, "offset": 1, "limit": 3})
            assert page["address"] == entry, page
            assert page["text"].splitlines() == source["text"].splitlines()[1:4], page
    graph = ok(client, "cfg", {"address": address})
    assert any(node["start"] == entry for node in graph["nodes"]), graph
    summary = ok(client, "cfg_summary", {"address": address})
    assert summary["address"] == entry, summary
    owner = ok(client, "cfg_summary", {"address": entry})
    assert owner["layout_revision"] == summary["layout_revision"], (summary, owner)
    viewport = ok(client, "cfg_viewport", {"address": address,
                  "layout_revision": summary["layout_revision"], "scale": 1,
                  **summary["bounds"]})
    assert viewport["snapshot_complete"] and viewport["complete"], viewport
    resolved = ok(client, "resolve", {"query": address})
    assert resolved["function_address"] == entry, resolved
    inventory = ok(client, "functions", {"limit": 64})
    assert inventory["complete"], inventory
    entries = {row["address"] for row in inventory["items"]}
    assert entry in entries and (address == entry or address not in entries), inventory


def verify_saved_code_navigation(executable, directory, image, entry, fragments):
    """Saved code is still eligible for bounded function-owner recovery."""
    for offset, size in fragments:
        target = entry + offset
        for kind, state in (("code", None), ("code", "deleted"),
                            ("code", "created"), ("undefined", None), ("byte", None)):
            for preceding in (False, True):
                binary = Path(directory) / f"saved-{offset}-{kind}-{state}-{preceding}.elf"
                binary.write_bytes(image)
                project = Path(str(binary) + ".neverd")
                project.mkdir()
                items = [{"addr": hex(target), "kind": kind,
                          "size": 1 if kind == "byte" else size}]
                (project / "items.json").write_text(json.dumps(items))
                functions = [{"addr": hex(target), "state": state}] if state else []
                (project / "functions.json").write_text(json.dumps(functions))
                with Client(executable) as client:
                    ok(client, "open", {"path": str(binary), "analysis": False,
                                        "warm": False, "read_only": True})
                    # Literal GUI navigation reaches listing before resolve or
                    # decompile can warm the recovered instruction index.
                    request = {"address": hex(target - int(preceding)),
                               "recover_address": hex(target), "before": 0, "after": 16}
                    page = ok(client, "listing", request)
                    at = [row for row in page["lines"] if row["address"] == hex(target)
                          and row["kind"] == "insn"]
                    if kind != "code":
                        assert not at, page
                    else:
                        expected = hex(target if state == "created" else entry)
                        assert at and all(row.get("function_address") == expected
                                          for row in at), (binary.name, page)
                        assert ok(client, "resolve", {"query": hex(target)})[
                            "function_address"] == expected
                        if state != "created":
                            for route in ("c", "llvmc"):
                                source = ok(client, "decompile", {"address": hex(target),
                                            "representation": route, "limit": 2048})
                                assert source["address"] == expected, source
                                assert "__builtin_trap" not in source["text"], source
                    again = ok(client, "listing", request)
                    selected = lambda rows: [(row["item_size"], row.get("function_address"))
                                             for row in rows if row["address"] == hex(target)
                                             and row["kind"] == "insn"]
                    assert selected(again["lines"]) == selected(page["lines"]), (page, again)
                assert json.loads((project / "items.json").read_text()) == items
                assert json.loads((project / "functions.json").read_text()) == functions


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-constant-branch-") as directory:
        binary = Path(directory) / "branches.elf"
        base, entry = 0x400000, 0x400078
        code = bytes.fromhex("b87800400083c00bffe0e9c390909090")
        header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
                             2, 62, 1, entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                              120 + len(code), 120 + len(code), 4096)
        binary.write_bytes(header + segment + code)
        sidecar = Path(str(binary) + ".neverd/items.json")
        rows = lambda: json.loads(sidecar.read_text()) if sidecar.exists() else []

        def listing(client, address):
            return ok(client, "listing", {"address": hex(address),
                                          "before": 0, "after": 12})["lines"]

        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            ok(client, "analyze")
            initial = listing(client, entry + 10)
            assert any(row["address"] == hex(entry + 10) and "db" in row["text"]
                       and "0E9h" in row["text"] for row in initial), initial
            assert any(row["address"] == hex(entry + 11) and row["kind"] == "insn"
                       and "ret" in row["text"] for row in initial), initial
            outgoing = ok(client, "xrefs", {"address": hex(entry + 8), "direction": "from"})
            assert [(row["to"], row["kind"]) for row in outgoing["items"]] == [
                (hex(entry + 11), "jump")], outgoing
            incoming = ok(client, "xrefs", {"address": hex(entry + 11), "direction": "to"})
            assert any(row["from"] == hex(entry + 8) for row in incoming["items"]), incoming
            count = ok(client, "functions", {"offset": 0, "limit": 1})["total"]
            # U must work on an automatic instruction without deleting its function.
            changed = ok(client, "item_define", {"address": hex(entry), "action": "undefine"})
            assert changed["saved"] and changed["size"] == 5, changed
            assert rows()[0]["kind"] == "undefined", rows()
            assert not any(row["address"] == hex(entry) and row["kind"] == "insn"
                           for row in listing(client, entry)), listing(client, entry)
            jumped = ok(client, "listing", {"address": hex(entry), "before": 0,
                         "after": 12, "recover_address": hex(entry)})
            assert not any(row["address"] == hex(entry) and row["kind"] == "insn"
                           for row in jumped["lines"]), jumped
            assert ok(client, "functions", {"offset": 0, "limit": 1})["total"] == count
            ok(client, "undo")
            assert rows() == []
            ok(client, "redo")
            saved = rows()
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            ok(client, "analyze")
            assert rows() == saved
            assert not any(row["address"] == hex(entry) and row["kind"] == "insn"
                           for row in listing(client, entry)), listing(client, entry)
            ok(client, "item_define", {"address": hex(entry), "action": "code"})
            assert rows()[0]["kind"] == "code", rows()
            assert any(row["address"] == hex(entry) and row["kind"] == "insn"
                       for row in listing(client, entry)), listing(client, entry)

        # A jump table embedded before the default block remains data with
        # references after the recovered body replaces the raw code index.
        # The explicit 32-bit write bounds the complete RDI address index.
        switch = Path(directory) / "switch.elf"
        code = (bytes.fromhex("89ff83ff017729ff24fd98004000b80b000000c3b816000000c3")
                + b"\x90" * 6 + struct.pack("<QQ", entry + 14, entry + 20)
                + bytes.fromhex("b821000000c3"))
        switch_segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                     120 + len(code), 120 + len(code), 4096)
        switch.write_bytes(header + switch_segment + code)
        with Client(executable) as client:
            ok(client, "open", {"path": str(switch)})
            ok(client, "analyze")
            expected = {(hex(entry + 7), "ijump"), (hex(entry + 32), "offset")}
            for _ in range(2):
                refs = ok(client, "xrefs", {"address": hex(entry + 14)})
                assert expected <= {(row["from"], row["kind"])
                                    for row in refs["items"]}, refs
                listing(client, entry)

        # The first navigation can land on a chunk beyond another function.
        # Follow two arithmetic transfers, including a backward one, retaining
        # the entry's frame. Never treat the intervening independent body as
        # part of that frame or erase its reference-index facts.
        scattered = Path(directory) / "scattered.elf"
        code = bytearray(b"\xcc" * 0x80)
        start = (bytes.fromhex("554889e54883ec10897df8e820000000b8")
                 + struct.pack("<I", entry + 0x71) + bytes.fromhex("ffe0"))
        code[:len(start)] = start
        code[0x30:0x36] = bytes.fromhex("e82b000000c3")
        code[0x60:0x66] = bytes.fromhex("b84d000000c3")
        code[0x50:0x59] = bytes.fromhex("e98b45f883c001c9c3")
        code[0x70:0x78] = (b"\xe9\xb8" + struct.pack("<I", entry + 0x51)
                            + bytes.fromhex("ffe0"))
        scattered_segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                        120 + len(code), 120 + len(code), 4096)
        scattered.write_bytes(header + scattered_segment + code)
        verify_saved_code_navigation(executable, directory, scattered.read_bytes(), entry,
                                     ((0x71, 5), (0x51, 3)))
        for first, navigation in ((entry + 0x71, "resolve"),
                                  (entry + 0x51, "resolve"),
                                  (entry + 0x71, "listing"),
                                  (entry + 0x51, "listing"),
                                  (entry + 0x71, "decompile"),
                                  (entry + 0x51, "decompile")):
            with Client(executable) as client:
                ok(client, "open", {"path": str(scattered), "analysis": False, "warm": False})
                if navigation == "decompile":
                    verify_function_views(client, first, entry)
                if navigation == "listing":
                    # A literal GUI jump bypasses the expression resolver.
                    initial = ok(client, "listing", {"address": hex(first),
                                 "recover_address": hex(first), "before": 0, "after": 12})
                    assert any(row["address"] == hex(first) and row["kind"] == "insn"
                               and row["function_address"] == hex(entry)
                               for row in initial["lines"]), initial
                resolved = ok(client, "resolve", {"query": hex(first)})
                assert resolved["function_address"] == hex(entry), resolved
                for target in (entry + 0x71, entry + 0x51):
                    assert ok(client, "resolve", {"query": hex(target)})[
                        "function_address"] == hex(entry)
                    fragment_end = entry + (0x78 if target == entry + 0x71 else 0x59)
                    fragment = [row for row in listing(client, target)
                                if target <= int(row["address"], 16) < fragment_end]
                    assert any(row["address"] == hex(target) and row["kind"] == "insn"
                               for row in fragment), fragment
                    assert sum("START OF FUNCTION CHUNK" in row["text"]
                               for row in fragment) == 1, fragment
                    assert sum("END OF FUNCTION CHUNK" in row["text"]
                               for row in fragment) == 1, fragment
                    assert not any(row["text"].endswith("endp") for row in fragment), fragment
                if navigation != "decompile":
                    verify_function_views(client, first, entry)
                independent_source = ok(client, "decompile", {"address": hex(entry + 0x30)})
                assert independent_source["address"] == hex(entry + 0x30), independent_source
                assert ok(client, "resolve", {"query": hex(entry + 0x30)})[
                    "function_address"] == hex(entry + 0x30)
                outgoing = ok(client, "xrefs", {"address": hex(entry + 0x76),
                                                "direction": "from"})
                assert any(row["to"] == hex(entry + 0x51) for row in outgoing["items"]), outgoing
                independent = ok(client, "xrefs", {"address": hex(entry + 0x60)})
                assert any(row["from"] == hex(entry + 0x30) and row["kind"] == "call"
                           for row in independent["items"]), independent
                # An edit rebuilds ownership. A viewport refresh starts before
                # the cursor but must recover the cursor's original function.
                ok(client, "rename", {"address": hex(entry), "name": "fragment_owner"})
                refreshed = ok(client, "listing", {"address": hex(first - 1),
                               "recover_address": hex(first), "before": 0, "after": 16})
                assert any(row["address"] == hex(first) and row["kind"] == "insn"
                           and row["function_address"] == hex(entry)
                           for row in refreshed["lines"]), refreshed
                preceding = ok(client, "listing", {"address": hex(first + (5 if first == entry + 0x71 else 3)),
                               "before": 12, "after": 1})
                assert any(row["address"] == hex(first) and row["kind"] == "insn"
                           for row in preceding["lines"]), preceding

        # The same saved-item contract on ARM64: save the frame/LR, call an
        # independent helper, then follow ADR/BR forward and backward before
        # restoring the original frame and returning seven.
        arm64 = bytearray(0x78)
        arm64[:24] = bytes.fromhex("fd7bbfa9fd0300910a000094e00080520903001020011fd6")
        arm64[0x30:0x38] = bytes.fromhex("20018052c0035fd6")
        arm64[0x50:0x58] = bytes.fromhex("fd7bc1a8c0035fd6")
        arm64[0x70:0x78] = bytes.fromhex("09ffff1020011fd6")
        arm64_header = bytearray(header)
        struct.pack_into("<H", arm64_header, 18, 183)
        arm64_segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                    120 + len(arm64), 120 + len(arm64), 4096)
        verify_saved_code_navigation(executable, directory,
                                     arm64_header + arm64_segment + arm64, entry,
                                     ((0x70, 4), (0x50, 4)))

        # A cold fragment before the entry must not become the stack tracker's
        # initial block merely because its address sorts first.
        backward = Path(directory) / "backward.elf"
        code = bytearray(b"\xcc" * 0x50)
        code[0:9] = bytes.fromhex("e98b45f883c001c9c3")
        start = (bytes.fromhex("554889e54883ec10897df8b8")
                 + struct.pack("<I", entry + 1) + bytes.fromhex("ffe0"))
        code[0x30:0x30 + len(start)] = start
        backward_header = bytearray(header)
        struct.pack_into("<Q", backward_header, 24, entry + 0x30)
        backward_segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                                       120 + len(code), 120 + len(code), 4096)
        backward.write_bytes(backward_header + backward_segment + code)
        with Client(executable) as client:
            ok(client, "open", {"path": str(backward)})
            resolved = ok(client, "resolve", {"query": hex(entry + 1)})
            assert resolved["function_address"] == hex(entry + 0x30), resolved
            lines = listing(client, entry + 1)
            assert any(row["address"] == hex(entry + 1) and "var_" in row["text"]
                       for row in lines), lines
            assert sum("START OF FUNCTION CHUNK" in row["text"] for row in lines) == 1, lines
            assert sum("END OF FUNCTION CHUNK" in row["text"] for row in lines) == 1, lines
            verify_function_views(client, entry + 1, entry + 0x30)
            verify_function_views(client, entry + 3, entry + 0x30)

        # A rejected lift must leave the native bytes inspectable, with an
        # explicit linear-view warning rather than a fabricated recovered CFG.
        broken = Path(directory) / "incomplete.elf"
        broken.write_bytes(header + segment + bytes.fromhex("b80100000060") + bytes(10))
        with Client(executable) as client:
            ok(client, "open", {"path": str(broken)})
            ok(client, "analyze")
            ok(client, "function_create", {"address": hex(entry)})
            lines = ok(client, "listing", {"address": hex(entry), "before": 0,
                                           "after": 60})["lines"]
            assert any("control-flow recovery failed" in row["text"] for row in lines), lines
            assert any(row["address"] == hex(entry) and row["kind"] == "insn"
                       for row in lines), lines

        # Native recovery must not replace ordinary bytecode ownership.
        bytecode = Path(directory) / "arithmetic.evm"
        bytecode.write_text("600160020100", encoding="ascii")
        with Client(executable) as client:
            ok(client, "open", {"path": str(bytecode)})
            ok(client, "analyze")
            resolved = ok(client, "resolve", {"query": "0x2"})
            assert resolved["function_address"] == "0x0", resolved


if __name__ == "__main__":
    run(sys.argv[1])
