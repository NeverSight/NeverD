#!/usr/bin/env python3
"""Large catalogs retain complete search/sort/seek and cancellable progress."""
import struct
import sys
import tempfile
from pathlib import Path

from transport_test import Client

COUNT = 1_000_001
BASE = 0x400000
PHRASES = (
    "unknown register",
    "unknown value",
    "unknown return value",
    "unresolved indirect branch",
    "Engine result exceeds the 32 MiB adapter budget",
    "Engine result exceeds the adapter budget",
)


def elf_with_functions():
    # mov eax, 1; ret, then one ret per remaining function. Every function is
    # STB_LOCAL so the catalog is not a million exports.
    text = b"\xb8\x01\x00\x00\x00\xc3" + b"\xc3" * (COUNT - 1)
    code_off = 64 + 56
    entry0 = BASE + code_off
    sym_count = COUNT + 1
    symbols = bytearray(sym_count * 24)
    view = memoryview(symbols)
    info = 2  # STB_LOCAL | STT_FUNC
    names = b"\0f\0"
    special = {}
    for index, name in ((5, "0x10"), (7, "0x2"),
                        (COUNT - 2, "needle"),
                        (COUNT - 1, "_ZN9LargeType4lastEv")):
        special[index] = len(names)
        names += name.encode() + b"\0"
    for index in range(COUNT):
        if index == 0:
            value, size = entry0, 6
        else:
            value, size = entry0 + 5 + index, 1
        struct.pack_into("<IBBHQQ", view, (index + 1) * 24,
                         special.get(index, 1), info, 0, 1, value, size)
    shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    body = bytearray(code_off) + text
    while len(body) % 8:
        body.append(0)
    sym_off = len(body)
    body += symbols
    str_off = len(body)
    body += names
    shstr_off = len(body)
    body += shstr
    while len(body) % 8:
        body.append(0)
    shoff = len(body)

    def shdr(name, kind, flags, addr, offset, size, link, info_value, align, entsize):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset, size,
                           link, info_value, align, entsize)

    body += shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    body += shdr(1, 1, 6, entry0, code_off, len(text), 0, 0, 1, 0)
    body += shdr(7, 2, 0, 0, sym_off, len(symbols), 3, sym_count, 8, 24)
    body += shdr(15, 3, 0, 0, str_off, len(names), 0, 0, 1, 0)
    body += shdr(23, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    filesz = code_off + len(text)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE, BASE, filesz, filesz, 0x1000)
    ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    ehdr = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, entry0, 64, shoff, 0,
                       64, 56, 1, 64, 5, 4)
    body[:64] = ehdr
    body[64:120] = phdr
    return bytes(body), entry0


def address(entry0, index):
    return entry0 if index == 0 else entry0 + 5 + index


def hx(value):
    return "0x" + format(value, "x")


def run(executable):
    image, entry0 = elf_with_functions()
    with tempfile.TemporaryDirectory(prefix="neverd-large-functions-") as directory:
        binary = Path(directory) / "many-functions.elf"
        binary.write_bytes(image)
        client = Client(executable)
        try:
            opened = client.call(
                "open",
                {"path": str(binary), "read_only": True, "analysis": False,
                 "debug_info": False, "browse_cache": False, "warm": False},
                timeout=240)
            assert opened["status"] == "ok", opened
            assert opened["payload"]["architecture"] == "x86_64", opened
            page = client.call("functions", {"limit": 2}, timeout=240)
            assert page["status"] == "ok", page
            payload = page["payload"]
            assert payload["total"] == COUNT, payload["total"]
            assert payload["complete"] is False
            assert payload["next_offset"] == 2
            assert [item["address"] for item in payload["items"]] == [
                hx(address(entry0, 0)), hx(address(entry0, 1))]
            tail = client.call("functions", {"offset": COUNT - 1, "limit": 2}, timeout=240)
            assert tail["status"] == "ok", tail
            assert tail["payload"]["complete"] is True
            assert [item["address"] for item in tail["payload"]["items"]] == [
                hx(address(entry0, COUNT - 1))]
            after = client.call("functions", {"from": hx(address(entry0, 0)), "limit": 1},
                                timeout=240)
            assert after["status"] == "ok", after
            assert after["payload"]["total"] == COUNT - 1
            assert after["payload"]["items"][0]["address"] == hx(address(entry0, 1))
            sought = client.call(
                "functions",
                {"seek_address": hx(address(entry0, 10)), "limit": 4},
                timeout=240)
            assert sought["status"] == "ok", sought
            assert sought["payload"]["selected_index"] == 10
            assert sought["payload"]["offset"] == 8
            assert sought["payload"]["selected_address"] == hx(address(entry0, 10))
            assert sought["payload"]["items"][0]["address"] == hx(address(entry0, 8))
            # Cancellation arrives after the scan starts, not while queued.
            request = client.send("functions", {"sort": "name", "limit": 2})
            client.next(lambda m: m.get("request_id") == request
                        and m.get("status") == "progress", timeout=240)
            cancellation = client.call("cancel", {"request_id": request})
            assert not cancellation["payload"]["requires_restart"], cancellation
            stopped = client.response(request, timeout=240)
            assert stopped.get("error", {}).get("code") == "cancelled", stopped
            assert stopped["calculation_stopped"] and not stopped["completed_before_cancellation"], stopped
            # The incomplete order must not be cached as a complete result.
            for descending, indexes in ((False, [7, 5]), (True, [5, 7])):
                result = client.call("functions", {
                    "sort": "name", "descending": descending,
                    "offset": 0 if descending else COUNT - 2, "limit": 2}, timeout=240)
                assert result["status"] == "ok", result
                assert [r["address"] for r in result["payload"]["items"]] == [
                    hx(address(entry0, i)) for i in indexes], result
            for query, index in (("needle", COUNT - 2),
                                 ("LargeType::last", COUNT - 1)):
                result = client.call("functions", {"filter": query, "limit": 2}, timeout=240)
                assert result["status"] == "ok", result
                assert result["payload"]["total"] == 1, result
                assert result["payload"]["items"][0]["address"] == hx(address(entry0, index))
            result = client.call("functions", {"filter": "absent-symbol"}, timeout=240)
            assert result["status"] == "ok" and result["payload"]["total"] == 0, result
            # A longer filter narrows the cached order; backspacing searches
            # the entire catalog again and must recover previously hidden rows.
            for query, expected in (("need", 1), ("needle", 1),
                                    ("needle-missing", 0), ("need", 1)):
                result = client.call("functions", {"filter": query}, timeout=240)
                assert result["status"] == "ok" and result["payload"]["total"] == expected, result
            for descending in (False, True):
                index = COUNT - 10 if descending else 10
                result = client.call("functions", {"filter": "f", "sort": "address",
                    "descending": descending, "from": hx(address(entry0, index)), "limit": 2}, timeout=240)
                assert result["status"] == "ok", result
                assert result["payload"]["items"][0]["address"] == hx(
                    address(entry0, index + (-1 if descending else 1))), result
            result = client.call("functions", {"filter": "needle", "seek_address":
                hx(address(entry0, COUNT - 2)), "limit": 2}, timeout=240)
            assert result["status"] == "ok" and result["payload"]["selected_index"] == 0, result
            names = client.call("names", {"limit": 2}, timeout=240)
            assert names["status"] == "ok", names
            assert names["payload"]["total"] > 1000000
            assert len(names["payload"]["items"]) == 2
            assert names["payload"]["complete"] is False
            assert names["payload"]["items"][0]["address"] == hx(address(entry0, 0))
            named = client.call("names", {"filter": "0x", "sort": "name", "limit": 2}, timeout=240)
            assert named["status"] == "ok", named
            assert [r["name"] for r in named["payload"]["items"]] == ["0x2", "0x10"], named
            named = client.call("names", {"filter": "needle", "sort": "address",
                "seek_address": hx(address(entry0, COUNT - 2)), "limit": 2}, timeout=240)
            assert named["status"] == "ok" and named["payload"]["selected_index"] == 0, named
            code = client.call(
                "decompile",
                {"address": hx(entry0), "representation": "c", "limit": 64},
                timeout=240)
            assert code["status"] == "ok", code
            text = code["payload"]["text"]
            assert text
            for phrase in PHRASES:
                assert phrase not in text, (phrase, text)
            print("large catalog: paging, search, sort, seek, cancellation, and decompile passed")
        finally:
            if client.process.poll() is None:
                try:
                    client.close(timeout=120)
                except Exception:
                    client.process.kill()
                    client.process.wait(timeout=30)


if __name__ == "__main__":
    run(sys.argv[1])
