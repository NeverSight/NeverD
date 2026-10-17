#!/usr/bin/env python3
"""A disposable analysis replica must preserve current edits without saving them."""
import copy
import json
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def durable_files(directory):
    # Windows refuses reads of the writer's exclusively locked runtime file.
    # Compare every input and every file of its project folder; the
    # persistent OS lock carries no edits.
    root = Path(directory)
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and not p.as_posix().endswith(".neverd/local/writer.lock")}


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-analysis-snapshot-") as directory, ExitStack() as clients:
        binary = Path(directory) / "code-edits.bin"
        binary.write_bytes(b"snapshot fixture")
        owner = clients.enter_context(Client(executable))
        ok(owner, "open", {"path": str(binary), "debug_info": False, "analysis": False})
        ok(owner, "rename", {"address": BASE, "name": "project_name"})
        ok(owner, "function_delete", {"address": hex(int(BASE, 16) + 16)})
        ok(owner, "code_edit", {"address": BASE, "representation": "c", "kind": "comment",
                                 "line": 0, "anchor": "#include <stdint.h>", "text": "source note"})
        ok(owner, "annotation_set", {"address": BASE, "text": "not saved"})
        snapshot = ok(owner, "analysis_snapshot")
        assert snapshot["open"]["read_only"] and not snapshot["open"]["analysis"]
        assert not snapshot["open"]["debug_info"]
        before = durable_files(directory)
        replica = clients.enter_context(Client(executable))
        assert ok(replica, "analysis_restore", snapshot)["read_only"]
        assert ok(replica, "functions", {"filter": "project_name"})["total"] == 1
        assert ok(replica, "annotations")["items"][0]["text"] == "not saved"
        assert replica.call("rename", {"address": BASE, "name": "forbidden"})["error"]["code"] == "read_only"
        assert replica.call("code_edit", {"address": BASE})["error"]["code"] == "read_only"
        assert ok(replica, "analysis_snapshot")["state"]["code_edits"] == snapshot["state"]["code_edits"]
        after = durable_files(directory)
        assert before == after, "Restoring analysis changed owner files"
        assert ok(owner, "metadata")["dirty"]
        assert owner.call("analysis_restore", snapshot)["error"]["code"] == "invalid_request"

        # A racing committed edit cannot be presented under the old snapshot.
        ok(owner, "save")
        ok(owner, "rename", {"address": BASE, "name": "new_project_name"})
        stale = clients.enter_context(Client(executable))
        assert stale.call("analysis_restore", snapshot)["error"]["code"] == "stale_snapshot"
        snapshot = ok(owner, "analysis_snapshot")
        wrong = copy.deepcopy(snapshot)
        wrong["input_sha256"] = "wrong input"
        other = clients.enter_context(Client(executable))
        assert other.call("analysis_restore", wrong)["error"]["code"] == "input_changed"

        # Signature replay is tied to the source that the owner loaded.
        signature = Path(directory) / "fixture.pat"
        signature.write_text("signature fixture")
        ok(owner, "signatures_load", {"path": str(signature)})
        snapshot = ok(owner, "analysis_snapshot")
        signed = clients.enter_context(Client(executable))
        ok(signed, "analysis_restore", snapshot)
        signature.write_text("signature fixture changed")
        changed = clients.enter_context(Client(executable))
        assert changed.call("analysis_restore", snapshot)["error"]["code"] == "input_changed"
        assert owner.call("analysis_snapshot")["error"]["code"] == "input_changed"

        # Refused code sidecars must not replace the previously loaded project.
        invalid = Path(directory) / "invalid.bin"
        invalid.write_bytes(b"fixture")
        Path(str(invalid) + ".neverd").mkdir(exist_ok=True)
        Path(str(invalid) + ".neverd/code.json").write_text(json.dumps([
            {"addr": BASE, "views": {}}, {"addr": BASE, "views": {}}]))
        assert owner.call("open", {"path": str(invalid)})["error"]["code"] == "invalid_request"
        assert Path(ok(owner, "metadata")["path"]).resolve() == binary.resolve()
        assert "source note" in ok(owner, "decompile", {"address": BASE, "representation": "c"})["text"]


if __name__ == "__main__":
    run(sys.argv[1])
    print("Analysis snapshot state, stale-input and read-only checks passed")
