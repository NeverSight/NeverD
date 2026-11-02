#!/usr/bin/env python3
"""GUI QA regressions at the worker boundary: ordered pages, cursors and saves."""
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from transport_test import BASE, Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def addresses(page):
    return [int(row["address"], 16) for row in page["items"]]


def run(executable):
    base = int(BASE, 16)
    with tempfile.TemporaryDirectory(prefix="neverd-functional-") as folder:
        binary = Path(folder) / "many-functions.bin"
        binary.write_bytes(b"fixture")
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            # Exercise both cache-warming orders; page boundaries must retain
            # the same direction, including a cursor far beyond the first 512.
            for directions in ((False, True, False), (True, False, True)):
                for descending in directions:
                    page = ok(client, "functions", {"sort": "address", "descending": descending, "limit": 8})
                    values = addresses(page)
                    assert values == sorted(values, reverse=descending), page
                    assert values[0] == base + (19999 * 16 if descending else 0), page
                    page = ok(client, "functions", {"sort": "address", "descending": descending,
                                                    "from": hex(base + 12000 * 16), "limit": 2})
                    assert addresses(page)[0] == base + (11999 if descending else 12001) * 16, page
            for descending, cursor in ((False, base + 19999 * 16), (True, base)):
                assert not ok(client, "functions", {"from": hex(cursor), "descending": descending})["items"]
            for descending in (False, True):
                target = base + 12000 * 16
                found = ok(client, "functions", {"seek_address": hex(target),
                           "sort": "address", "descending": descending, "limit": 97})
                selected = found["selected_index"] - found["offset"]
                assert addresses(found)[selected] == target, found
                assert found["offset"] % 97 == 0, found
            absent = client.call("functions", {"seek_address": hex(base + 1)})
            assert absent["error"]["code"] == "not_found", absent
            invalid = client.call("functions", {"seek_address": hex(base), "from": hex(base)})
            assert invalid["error"]["code"] == "invalid_request", invalid
            segment = ok(client, "segments", {"seek_address": hex(base + 17), "limit": 2})
            assert segment["selected_address"] == hex(base), segment
            assert addresses(segment)[segment["selected_index"] - segment["offset"]] == base, segment
            region = ok(client, "regions", {"seek_address": hex(base + 17), "limit": 2})
            assert region["selected_address"] == hex(base), region
            assert region["items"][region["selected_index"] - region["offset"]]["start"] == hex(base), region

        binary = Path(folder) / "strings-sort.bin"
        binary.write_bytes(b"fixture")
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            for sort in ("address", "length", "text", "type"):
                for descending in (False, True):
                    result = []
                    for offset in range(0, 600, 97):
                        page = ok(client, "strings", {"sort": sort, "descending": descending,
                                                       "offset": offset, "limit": 97})
                        result.extend(page["items"])
                    key = lambda row: (int(row["address"], 16) if sort == "address" else
                                       row["length"] if sort == "length" else row[sort].lower())
                    assert len(result) == 600
                    assert [key(row) for row in result] == sorted(map(key, result), reverse=descending), (sort, descending)
                    target = base + 523
                    found = ok(client, "strings", {"seek_address": hex(target),
                               "sort": sort, "descending": descending, "limit": 97})
                    selected = found["selected_index"] - found["offset"]
                    assert addresses(found)[selected] == target, (sort, descending, found)
            absent = client.call("strings", {"seek_address": hex(base + 700)})
            assert absent["error"]["code"] == "not_found", absent
            page = ok(client, "search", {"kind": "bytes", "pattern": "00 01", "limit": 7})
            assert not page["complete"] and len(page["items"]) == 7, page
            assert page["next_address"] == page["items"][-1]["address"], page
            continuation = ok(client, "search", {"kind": "bytes", "pattern": "00 01", "from": page["next_address"], "limit": 7})
            assert addresses(continuation)[0] == base + 7 * 16, continuation
            previous = ok(client, "search", {"kind": "bytes", "pattern": "00 01", "from": hex(base + 7 * 16), "backward": True, "limit": 7})
            assert addresses(previous) == [base + i * 16 for i in reversed(range(7))], previous
            assert previous["complete"], previous

        binary = Path(folder) / "strings-preview.bin"
        binary.write_bytes(b"fixture")
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            page = ok(client, "strings", {"sort": "length", "descending": True,
                                          "text_limit": 400, "filter": "needle-at-end", "limit": 256})
            assert page["total"] == 4 and len(page["items"]) == 4, page
            lengths = [row["length"] for row in page["items"]]
            assert lengths == sorted(lengths, reverse=True), lengths
            for row in page["items"]:
                assert row["text_truncated"] and row["text"] == "a" * 398 + "\u2026", row
            full = ok(client, "strings", {"limit": 1})["items"][0]
            assert not full["text_truncated"] and full["text"].endswith("needle-at-end")

        if os.name == "nt":
            windows_saves(executable, Path(folder))
    print("functional regressions: function/string ordering, exclusive cursors, search continuation and Windows saves passed")


def windows_saves(executable, folder):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    binary = folder / "reader-lock.bin"
    binary.write_bytes(b"fixture")
    annotations = Path(str(binary) + ".neverd/annotations.json")
    journal = Path(str(binary) + ".neverd/local/journal.json")
    with Client(executable) as client:
        ok(client, "open", {"path": str(binary)})
        ok(client, "annotation_set", {"address": BASE, "text": "original"})
        ok(client, "save")
        original = annotations.read_bytes()

        def reader():
            # A real Windows read handle without FILE_SHARE_DELETE reproduces
            # MoveFileEx sharing violations, as a concurrent reader can do.
            handle = kernel.CreateFileW(str(annotations), 0x80000000, 3, None, 3, 0, None)
            assert handle != ctypes.c_void_p(-1).value, ctypes.get_last_error()
            return handle

        ok(client, "annotation_set", {"address": BASE, "text": "transient"})
        handle = reader()
        released = threading.Thread(target=lambda: (time.sleep(0.15), kernel.CloseHandle(handle)))
        released.start()
        ok(client, "save")
        released.join()
        assert b"transient" in annotations.read_bytes() and not journal.exists()
        original = annotations.read_bytes()
        ok(client, "annotation_set", {"address": BASE, "text": "recoverable"})
        handle = reader()
        try:
            failed = client.call("save")
            assert failed["error"]["code"] == "save_failed", failed
            assert "Win32 error" in failed["error"]["message"], failed
            assert annotations.read_bytes() == original and journal.exists()
        finally:
            kernel.CloseHandle(handle)
        # No restart is needed for a transaction published by this worker.
        # A newer staged annotation and its history survive completion of the
        # interrupted write, rather than being replaced by its older snapshot.
        ok(client, "annotation_set", {"address": BASE, "text": "newer retry"})
        ok(client, "save")
        assert b"newer retry" in annotations.read_bytes() and not journal.exists()
        ok(client, "undo")
        ok(client, "save")
        assert b"recoverable" in annotations.read_bytes()
        ok(client, "redo")
        ok(client, "save")
        assert b"newer retry" in annotations.read_bytes()

        original = annotations.read_bytes()
        ok(client, "annotation_set", {"address": BASE, "text": "recoverable"})
        handle = reader()
        try:
            assert client.call("save")["error"]["code"] == "save_failed"
        finally:
            kernel.CloseHandle(handle)
        foreign = json.dumps([dict(addr=BASE, text="foreign edit")]).encode()
        annotations.write_bytes(foreign)
        assert client.call("save")["error"]["code"] == "foreign_edits"
        assert annotations.read_bytes() == foreign and journal.exists()
        annotations.write_bytes(original)
    with Client(executable) as recovered:
        ok(recovered, "open", {"path": str(binary)})
        assert b"recoverable" in annotations.read_bytes(), annotations.read_bytes()
        ok(recovered, "save")
        assert not journal.exists()


if __name__ == "__main__":
    run(sys.argv[1])
