#!/usr/bin/env python3
"""Byte patches cross preview, writer history, sidecars, replicas and export."""
import json
import os
import stat
from pathlib import Path
import sys
import tempfile

from transport_test import Client
from function_edits_test import two_routine_elf
from pe_browse_test import image


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", (operation, result)
    return result["payload"]


def patch_pages(worker, expected):
    total = len(expected)
    for offset, limit in ((0, 0), (0, 1), (1, 1), (0, 512),
                          (max(0, total - 1), 512), (total, 128), (total + 1, 128)):
        items = expected[offset:offset + limit]
        page = ok(worker, "patches", {"offset": offset, "limit": limit})
        assert page == {"items": items, "total": total, "offset": offset,
                        "complete": offset + len(items) >= total}, page
        assert all(row["address"] == row["va"] for row in page["items"])
    for payload, code in (({"offset": -1}, "invalid_request"),
                           ({"offset": "1"}, "invalid_request"),
                           ({"limit": -1}, "invalid_request"),
                           ({"limit": 513}, "budget_exceeded")):
        refused = worker.call("patches", payload)
        assert refused["status"] != "ok" and refused["error"]["code"] == code, refused
    assert ok(worker, "patches")["items"] == expected
    ranges = [{"address": row["va"], "size": row["size"]} for row in expected]
    for _ in range(2):
        assert ok(worker, "patches", {"ranges_only": True}) == {
            "items": ranges, "total": total, "complete": True}


def run(executable, real=False):
    data, entry = two_routine_elf() if real else (bytes(range(16)), 0xffff800012340000)
    at = entry + 1
    with tempfile.TemporaryDirectory(prefix="neverd-patch-worker-") as directory:
        binary = Path(directory) / "patch.bin"
        binary.write_bytes(data)
        if real and os.name == "posix":
            binary.chmod(0o751)
        with Client(executable) as worker:
            assert "patch_apply" in worker.hello["capabilities"]
            ok(worker, "open", {"path": str(binary), "analysis": False})
            ok(worker, "annotation_set", {"address": hex(at), "text": "keep this note"})
            ok(worker, "save")
            annotations, = Path(directory).rglob("annotations.json")
            note_bytes = annotations.read_bytes()
            note_time = annotations.stat().st_mtime_ns
            original = ok(worker, "bytes", {"address": hex(at), "size": 1})["data"]
            request = {"address": hex(at), "kind": "bytes", "bytes": "2a"}
            plan = ok(worker, "patch_preview", request)
            assert plan["original"] == original and plan["current"] == "2a", plan
            assert ok(worker, "patches")["total"] == 0
            patch_pages(worker, [])
            request["expected"] = original
            ok(worker, "patch_apply", request)
            assert ok(worker, "bytes", {"address": hex(at), "size": 1})["data"] == "2a"
            assert ok(worker, "patches", {"ranges_only": True}) == {
                "items": [{"address": hex(at), "size": 1}], "total": 1, "complete": True}
            last = ok(worker, "history")["items"][-1]
            assert last["kind"] == "patch" and last["details_omitted"]
            assert int(last["address"], 16) == at, last
            assert "current" not in last["after"] and last["changed_rows"] == 1, last
            saved = list(Path(directory).rglob("patches.json"))
            assert len(saved) == 1 and json.loads(saved[0].read_text())
            assert binary.read_bytes() == data
            before = ok(worker, "history")
            stale = worker.call("patch_apply", request)
            assert stale["status"] == "error", stale
            assert ok(worker, "history") == before
            snapshot = ok(worker, "analysis_snapshot")
            with Client(executable) as replica:
                ok(replica, "analysis_restore", snapshot)
                assert ok(replica, "bytes", {"address": hex(at), "size": 1})["data"] == "2a"
                assert ok(replica, "patches", {"ranges_only": True}) == {
                    "items": [{"address": hex(at), "size": 1}], "total": 1, "complete": True}
                denied = replica.call("patch_apply", {**request, "expected": "2a"})
                assert denied["error"]["code"] == "read_only", denied
            assert int(ok(worker, "undo")["address"], 16) == at
            assert ok(worker, "bytes", {"address": hex(at), "size": 1})["data"] == original
            assert int(ok(worker, "redo")["address"], 16) == at
            assert ok(worker, "patches")["total"] == 1
            output = Path(directory) / "patched.bin"
            ok(worker, "patch_export", {"path": str(output)})
            offset = at - (0x400000 if real else 0xffff800012340000)
            expected = bytearray(data)
            expected[offset] = 0x2a
            assert output.read_bytes() == expected
            if real and os.name == "posix":
                assert stat.S_IMODE(output.stat().st_mode) == 0o751
            assert worker.call("patch_export", {"path": str(output)})["status"] == "error"
            assert binary.read_bytes() == data
            ok(worker, "patch_apply", {"address": hex(at + 2), "kind": "bytes", "bytes": "55"})
            rows = ok(worker, "patches")["items"]
            patch_file, = Path(directory).rglob("patches.json")
            patch_text, patch_time = patch_file.read_bytes(), patch_file.stat().st_mtime_ns
            patch_pages(worker, rows)
            assert patch_file.read_bytes() == patch_text and patch_file.stat().st_mtime_ns == patch_time
            ok(worker, "patch_revert", {"address": hex(at), "ranges": [{"address": row["va"], "size": row["size"]} for row in rows]})
            assert ok(worker, "patches")["total"] == 0
            assert ok(worker, "patches", {"ranges_only": True}) == {
                "items": [], "total": 0, "complete": True}
            ok(worker, "undo")
            assert ok(worker, "patches")["total"] == len(rows)
            assert annotations.read_bytes() == note_bytes
            assert annotations.stat().st_mtime_ns == note_time, "Patch history rewrote unchanged annotations"
        with Client(executable) as reopened:
            ok(reopened, "open", {"path": str(binary), "analysis": False})
            assert ok(reopened, "patches")["total"] == len(rows)
            ok(reopened, "undo")
            ok(reopened, "redo")
            assert annotations.read_bytes() == note_bytes
            assert annotations.stat().st_mtime_ns == note_time
            # Skipping an unchanged table's write must not skip its conflict check.
            foreign = json.loads(note_bytes)
            foreign[0]["text"] = "external edit"
            annotations.write_text(json.dumps(foreign))
            before = ok(reopened, "bytes", {"address": hex(at), "size": 1})["data"]
            refused = reopened.call("patch_apply", {"address": hex(at), "kind": "bytes", "bytes": "66"})
            assert refused["error"]["code"] == "foreign_edits", refused
            assert ok(reopened, "bytes", {"address": hex(at), "size": 1})["data"] == before
            assert json.loads(annotations.read_text()) == foreign
    print("patch preview, stale protection, history, replica, batch revert, reopen and export passed")


def split_row_history(executable):
    data, entry = two_routine_elf()
    with tempfile.TemporaryDirectory(prefix="neverd-patch-merge-history-") as directory:
        binary = Path(directory) / "input.elf"
        binary.write_bytes(data)
        states = [(data[-12:].hex(), [])]
        expected = bytearray(data[-12:])
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(binary), "analysis": False})
            # Insert before/after existing rows, then split an interior range.
            for offset, replacement in ((2, b"\x90" * 4), (0, b"\x91"),
                                        (8, b"\x92"), (3, b"\x93" * 2)):
                old = expected[offset:offset + len(replacement)].hex()
                ok(worker, "patch_apply", {"address": hex(entry + offset),
                                           "kind": "bytes", "bytes": replacement.hex(),
                                           "expected": old, "allow_overlap": True})
                expected[offset:offset + len(replacement)] = replacement
                assert ok(worker, "bytes", {"address": hex(entry), "size": 12})["data"] == expected.hex()
                rows = ok(worker, "patches")["items"]
                assert [int(row["addr"], 16) for row in rows] == sorted(int(row["addr"], 16) for row in rows)
                states.append((expected.hex(), rows))
            for operation, sequence in (("undo", reversed(states[:-1])),
                                         ("redo", states[1:])):
                for bytes_, rows in sequence:
                    ok(worker, operation)
                    assert ok(worker, "bytes", {"address": hex(entry), "size": 12})["data"] == bytes_
                    assert ok(worker, "patches")["items"] == rows
            rows = states[-1][1]
            assert len(rows) >= 3, rows
            ok(worker, "patch_revert", {"address": rows[0]["va"], "ranges": [
                {"address": row["va"], "size": row["size"]} for row in (rows[0], rows[-1])]})
            reverted = ok(worker, "patches")["items"]
            ok(worker, "undo")
            assert ok(worker, "patches")["items"] == rows
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(binary), "analysis": False})
            assert ok(worker, "bytes", {"address": hex(entry), "size": 12})["data"] == states[-1][0]
            ok(worker, "redo")
            assert ok(worker, "patches")["items"] == reverted
            ok(worker, "undo")
            assert ok(worker, "patches")["items"] == states[-1][1]
        assert binary.read_bytes() == data
    print("ordered patch inserts, split rows, batch removal and reopened undo/redo passed")


def noncanonical_history_row(executable):
    # Generic history retains unknown fields and permits a missing kind.
    # Such a command can be persisted by undo even when the SDK refuses to
    # load it. A later canonical edit must still be able to replace that row.
    data, entry = two_routine_elf()
    for missing_kind in (False, True):
        with tempfile.TemporaryDirectory(prefix="neverd-patch-history-kind-") as directory:
            binary = Path(directory) / "input.elf"
            binary.write_bytes(data)
            with Client(executable) as worker:
                ok(worker, "open", {"path": str(binary), "analysis": False})
                for byte in ("91", "92"):
                    ok(worker, "patch_apply", {"address": hex(entry + 1),
                                               "kind": "bytes", "bytes": byte})
            history, = Path(directory).rglob("history.json")
            document = json.loads(history.read_text())
            before = document["commands"][-1]["before"]
            before["extra"] = "retained history field"
            if missing_kind:
                before.pop("kind")
                assert len(before) == 7
            history.write_text(json.dumps(document))
            with Client(executable) as worker:
                ok(worker, "open", {"path": str(binary), "analysis": False})
                undo = worker.call("undo")
                if missing_kind:
                    assert undo["status"] == "error" and undo["error"]["code"] == "reload_failed", undo
                    assert undo["payload"]["saved"], undo
                else:
                    assert undo["status"] == "ok", undo
                result = ok(worker, "patch_apply", {"address": hex(entry + 1),
                                                      "kind": "bytes", "bytes": "93"})
                assert result["saved"], result
                sidecar, = Path(directory).rglob("patches.json")
                row, = json.loads(sidecar.read_text())
                assert set(row) == {"addr", "file_off", "va", "size", "original", "current", "kind"}
                assert row["kind"] == "bytes" and row["current"] == "93", row
                assert ok(worker, "bytes", {"address": hex(entry + 1), "size": 1})["data"] == "93"
            with Client(executable) as worker:
                ok(worker, "open", {"path": str(binary), "analysis": False})
                assert ok(worker, "bytes", {"address": hex(entry + 1), "size": 1})["data"] == "93"
            assert binary.read_bytes() == data
    print("noncanonical history rows, optional kind, canonical replacement and reopen passed")


def pe_export(executable):
    data = image()[0]
    with tempfile.TemporaryDirectory(prefix="neverd-pe-export-") as directory:
        binary = Path(directory) / "驱动.sys"
        binary.write_bytes(data)
        output = Path(directory) / "补丁.sys"
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(binary), "analysis": False})
            ok(worker, "patch_apply", {"address": "0x140001002", "kind": "bytes",
                                       "bytes": "2b", "expected": "2a"})
            ok(worker, "patch_export", {"path": str(output)})
            expected = bytearray(data)
            expected[0x202] = 0x2b
            assert output.read_bytes() == expected
            assert worker.call("patch_export", {"path": str(output)})["status"] == "error"
            assert output.read_bytes() == expected
            assert worker.call("patch_export", {"path": str(binary)})["status"] == "error"
            assert binary.read_bytes() == data
            assert sorted(path.name for path in Path(directory).glob("*.tmp-*")) == []
    print("PE export, UTF-8 paths, exclusive output, unchanged input and temporary cleanup passed")


def range_projection(executable):
    for base in (0x1000, 0xffff800012340000):
        for count in (0, 1, 16, 31, 32, 33, 64):
            with tempfile.TemporaryDirectory(prefix="neverd-patch-ranges-") as directory:
                root = Path(directory)
                binary = root / "input.bin"
                original = b"\xcc" * (count + 16)
                binary.write_bytes(original)
                options = {"path": str(binary), "loader": "binary", "processor": "x86_64",
                           "base": hex(base), "analysis": False}
                with Client(executable) as seed:
                    ok(seed, "open", options)
                    ok(seed, "patch_apply", {"address": hex(base), "kind": "bytes", "bytes": "90"})
                sidecar, = root.rglob("patches.json")
                history, = root.rglob("history.json")
                history.unlink()
                rows = [{"addr": hex(i), "file_off": hex(i), "va": hex(base + i),
                         "size": 1, "original": "cc", "current": "90",
                         "kind": "nop" if i % 2 else "bytes"} for i in range(count)]
                saved = json.dumps(rows, separators=(",", ":")).encode()
                sidecar.write_bytes(saved)
                timestamp = sidecar.stat().st_mtime_ns
                expected = {"items": [{"address": hex(base + i), "size": 1}
                                       for i in range(count)],
                            "total": count, "complete": True}
                with Client(executable) as worker:
                    ok(worker, "open", options)
                    for _ in range(2):
                        assert ok(worker, "patches", {"ranges_only": True}) == expected
                    assert sidecar.read_bytes() == saved
                    assert sidecar.stat().st_mtime_ns == timestamp
                assert binary.read_bytes() == original
    print("small/streamed range projection, high addresses and unchanged source/state passed")


def patch_budget_boundary(executable):
    # Patch state reserves half the advertised frame allowance. Construct an
    # exact compact-JSON boundary independently of the native size counter.
    def compact(rows):
        return json.dumps(rows, separators=(",", ":"), sort_keys=True).encode()

    def fixture(limit):
        for base in (0xffff800012340000, 0xfff800012340000):
            for kind in ("bytes", "nop", "assemble"):
                for size in range(limit // 4 - 256, limit // 4):
                    offset = size + 16
                    rows = [{"addr": "0x0", "file_off": "0x0", "va": hex(base),
                             "size": size, "original": "", "current": "", "kind": kind},
                            {"addr": hex(offset), "file_off": hex(offset), "va": hex(base + offset),
                             "size": 1, "original": "cc", "current": "91", "kind": "bytes"}]
                    if len(compact(rows)) + size * 4 == limit:
                        rows[0].update(original="cc" * size, current="90" * size)
                        assert len(compact(rows)) == limit
                        return base, size, offset, rows
        raise AssertionError("Could not construct exact patch budget boundary")

    for excess in (0, 1):
        with tempfile.TemporaryDirectory(prefix="neverd-patch-budget-") as directory:
            root = Path(directory)
            binary = root / "input.bin"
            with Client(executable) as seed:
                limit = seed.hello["max_frame_bytes"] // 2
                base, size, offset, expected = fixture(limit + excess)
                original = b"\xcc" * (size + 64)
                binary.write_bytes(original)
                options = {"path": str(binary), "loader": "binary", "processor": "x86_64",
                           "base": hex(base), "analysis": False}
                ok(seed, "open", options)
                ok(seed, "patch_apply", {"address": hex(base), "kind": "bytes", "bytes": "90"})
            sidecar, = root.rglob("patches.json")
            history, = root.rglob("history.json")
            history.unlink()
            before = json.loads(compact(expected))
            before[1]["current"] = "90"
            before_text = compact(before)
            sidecar.write_bytes(before_text)
            with Client(executable) as worker:
                ok(worker, "open", options)
                reply = worker.call("patch_apply", {"address": hex(base + offset), "kind": "bytes",
                                                     "bytes": "91", "expected": "90"})
                if excess:
                    assert reply["error"]["code"] == "patch_limit", reply
                    assert sidecar.read_bytes() == before_text
                    assert not history.exists() and not list(root.rglob("journal.json"))
                else:
                    assert reply["status"] == "ok" and reply["payload"]["saved"], reply
                    assert json.loads(sidecar.read_bytes()) == expected
                    assert len(compact(json.loads(sidecar.read_bytes()))) == limit
                    ok(worker, "undo")
                    assert json.loads(sidecar.read_bytes()) == before
                assert ok(worker, "bytes", {"address": hex(base + offset), "size": 1})["data"] == "90"
            assert binary.read_bytes() == original
    print("exact patch budget accepted, one-byte excess refused, live and persisted rollback passed")


if __name__ == "__main__":
    run(sys.argv[1], "--real" in sys.argv[2:])
    if "--real" in sys.argv[2:]:
        split_row_history(sys.argv[1])
        noncanonical_history_row(sys.argv[1])
        pe_export(sys.argv[1])
        range_projection(sys.argv[1])
        patch_budget_boundary(sys.argv[1])
