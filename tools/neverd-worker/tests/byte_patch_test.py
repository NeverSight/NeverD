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


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", (operation, result)
    return result["payload"]


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
            original = ok(worker, "bytes", {"address": hex(at), "size": 1})["data"]
            request = {"address": hex(at), "kind": "bytes", "bytes": "2a"}
            plan = ok(worker, "patch_preview", request)
            assert plan["original"] == original and plan["current"] == "2a", plan
            assert ok(worker, "patches")["total"] == 0
            request["expected"] = original
            ok(worker, "patch_apply", request)
            assert ok(worker, "bytes", {"address": hex(at), "size": 1})["data"] == "2a"
            assert ok(worker, "patches", {"ranges_only": True})["items"] == [{"address": hex(at), "size": 1}]
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
                assert ok(replica, "patches", {"ranges_only": True})["items"] == [{"address": hex(at), "size": 1}]
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
            ok(worker, "patch_revert", {"address": hex(at), "ranges": [{"address": row["va"], "size": row["size"]} for row in rows]})
            assert ok(worker, "patches")["total"] == 0
            assert ok(worker, "patches", {"ranges_only": True})["items"] == []
            ok(worker, "undo")
            assert ok(worker, "patches")["total"] == len(rows)
        with Client(executable) as reopened:
            ok(reopened, "open", {"path": str(binary), "analysis": False})
            assert ok(reopened, "patches")["total"] == len(rows)
            ok(reopened, "undo")
            ok(reopened, "redo")
    print("patch preview, stale protection, history, replica, batch revert, reopen and export passed")


if __name__ == "__main__":
    run(sys.argv[1], "--real" in sys.argv[2:])
