#!/usr/bin/env python3
"""User command history, crash recovery, identity refusal and declarative extensions."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from transport_test import Client, BASE


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", (operation, result)
    return result["payload"]


def note(client):
    items = ok(client, "annotations")["items"]
    return items[0]["text"] if items else ""


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-history-test-") as directory:
        binary = Path(directory) / "fixture.bin"
        binary.write_bytes(b"history fixture")
        annotations = Path(str(binary) + ".neverd-annotations.json")
        history_path = Path(str(binary) + ".neverd-history.json")
        journal_path = Path(str(binary) + ".neverd-journal.json")
        client = Client(executable)
        ok(client, "open", {"path": str(binary)})
        assert not ok(client, "history")["can_undo"]
        ok(client, "annotation_set", {"address": BASE, "text": "one"})
        assert ok(client, "history")["can_undo"]
        assert ok(client, "undo")["dirty"] and note(client) == ""
        assert ok(client, "redo")["dirty"] and note(client) == "one"
        assert not history_path.exists(), "staged annotation history must not be autosaved"
        ok(client, "save")
        assert json.loads(history_path.read_text())["cursor"] == 1
        ok(client, "rename", {"address": BASE, "name": "renamed"})
        assert ok(client, "undo")["saved"]
        assert ok(client, "functions", {"filter": "renamed"})["total"] == 0
        assert ok(client, "redo")["saved"]
        assert ok(client, "functions", {"filter": "renamed"})["total"] == 1
        ok(client, "undo")
        ok(client, "annotation_set", {"address": BASE, "text": "two"})
        assert not ok(client, "history")["can_redo"], "new edit should remove old redo branch"
        assert client.call("rename", {"address": BASE, "name": "forbidden_autosave"})["error"]["code"] == "unsaved_changes"
        ok(client, "save")
        client.close()

        client = Client(executable)
        ok(client, "open", {"path": str(binary)})
        assert ok(client, "history")["cursor"] == 2
        ok(client, "undo")
        assert note(client) == "one"
        ok(client, "redo")
        ok(client, "save")
        annotations.write_text(json.dumps([dict(addr=BASE, text="foreign")]))
        ok(client, "annotation_set", {"address": BASE, "text": "staged"})
        assert client.call("save")["error"]["code"] == "foreign_edits"
        assert json.loads(annotations.read_text())[0]["text"] == "foreign"
        ok(client, "reload")
        status = ok(client, "history")
        assert not status["available"] and status["blocked_reason"] == "foreign_edits"
        assert client.call("undo")["error"]["code"] == "foreign_edits"
        ok(client, "history_reset")
        assert note(client) == "foreign" and not ok(client, "history")["can_undo"]
        client.close()

        binary.write_bytes(b"changed history fixture")
        client = Client(executable)
        ok(client, "open", {"path": str(binary)})
        assert ok(client, "history")["blocked_reason"] == "input_changed"
        assert client.call("annotation_set", {"address": BASE, "text": "bad replay"})["error"]["code"] == "input_changed"
        ok(client, "history_reset")
        client.close()

        # Simulate interruption after WAL + annotations replacement, before
        # history publication. Recovery must finish one known transaction.
        before_history = json.loads(history_path.read_text())
        before = before_history["state"]
        after = copy.deepcopy(before)
        after["annotations"] = [dict(addr=BASE, text="recovered")]
        after_history = copy.deepcopy(before_history)
        after_history.update(state=after, cursor=1,
                             commands=[dict(kind="annotation", address=BASE, before="foreign", after="recovered")])
        journal = dict(schema_version=1, source_sha256=before_history["source_sha256"],
                       before=before, after=after, before_history=before_history, after_history=after_history)
        journal_path.write_text(json.dumps(journal))
        annotations.write_text(json.dumps(after["annotations"]))
        reader = Client(executable)
        assert ok(reader, "open", {"path": str(binary), "read_only": True})["warnings"]
        assert reader.call("history")["error"]["code"] == "recovery_required"
        assert journal_path.exists()
        reader.close()
        writer = Client(executable)
        ok(writer, "open", {"path": str(binary)})
        assert not journal_path.exists() and note(writer) == "recovered"
        assert ok(writer, "history")["cursor"] == 1
        writer.close()

        # A third state is a foreign edit, never a recovery instruction.
        journal_path.write_text(json.dumps(journal))
        annotations.write_text(json.dumps([dict(addr=BASE, text="external after crash")]))
        conflict = Client(executable)
        assert ok(conflict, "open", {"path": str(binary)})["warnings"]
        assert conflict.call("history")["error"]["code"] == "foreign_edits"
        assert note(conflict) == "external after crash" and journal_path.exists()
        conflict.close()
        journal_path.unlink()

        # Registration validates the complete file before changing the registry.
        client = Client(executable)
        initial = ok(client, "contributions")
        assert len(initial["items"]) == 3
        manifest = dict(schema_version=1, namespace="sample", version="1.0",
                        contributions=[dict(id="sample:selected-code", title="Selected instructions", kind="panel",
                                            query=dict(operation="disasm", payload=dict(address="${address}", limit=3)))])
        path = Path(directory) / "manifest.json"
        path.write_text(json.dumps(manifest))
        registered = ok(client, "contribution_register", {"path": str(path)})
        assert registered["registry_revision"] != initial["registry_revision"]
        assert len(registered["items"]) == 4
        ok(client, "open", {"path": str(binary), "read_only": True})
        result = ok(client, "contribution_execute", {"id": "sample:selected-code", "address": BASE})
        assert result["contribution_id"] == "sample:selected-code" and len(result["result"]["items"]) == 3
        assert result["result"]["items"][0]["address"] == BASE
        for mutation in ("annotation_set", "rename", "open", "analyze", "contribution_execute"):
            invalid = copy.deepcopy(manifest)
            invalid["contributions"][0]["query"]["operation"] = mutation
            path.write_text(json.dumps(invalid))
            assert client.call("contribution_register", {"path": str(path)})["status"] == "error"
            assert len(ok(client, "contributions")["items"]) == 4
        invalid = copy.deepcopy(manifest)
        invalid["contributions"][0]["script"] = "arbitrary script is prohibited"
        path.write_text(json.dumps(invalid))
        assert client.call("contribution_register", {"path": str(path)})["error"]["code"] == "invalid_manifest"
        assert len(ok(client, "contribution_unregister", {"namespace": "sample"})["items"]) == 3
        assert client.call("contribution_execute", {"id": "sample:selected-code"})["error"]["code"] == "not_found"
        client.close()

        # A sidecar changed after load but before first history query must not
        # become an implicit baseline that overwrites someone else's comment.
        fresh = Path(directory) / "fresh.bin"
        fresh.write_bytes(b"fresh fixture")
        stale = Client(executable)
        ok(stale, "open", {"path": str(fresh)})
        fresh_annotations = Path(str(fresh) + ".neverd-annotations.json")
        fresh_annotations.write_text(json.dumps([dict(addr=BASE, text="external before first edit")]))
        assert not ok(stale, "history")["available"]
        assert stale.call("annotation_set", {"address": BASE, "text": "overwrite"})["error"]["code"] == "foreign_edits"
        ok(stale, "history_reset")
        assert note(stale) == "external before first edit"
        fresh.write_bytes(b"live input changed to a different length")
        assert stale.call("annotation_set", {"address": BASE, "text": "wrong binary"})["error"]["code"] == "input_changed"
        stale.close()
        # The same encoding must be used for write and read budgets. Pretty
        # indentation used to expand a valid 7.5 MiB history beyond the 8 MiB
        # reader limit after Save had already acknowledged success.
        large = Path(directory) / "large-state.bin"
        large.write_bytes(b"large user state fixture")
        large_annotations = Path(str(large) + ".neverd-annotations.json")
        large_annotations.write_text(json.dumps(
            [dict(addr=hex(int(BASE, 16) + index), text="x" * 220) for index in range(29000)],
            separators=(",", ":")))
        large_client = Client(executable)
        try:
            ok(large_client, "open", {"path": str(large)})
            ok(large_client, "save")
            assert Path(str(large) + ".neverd-history.json").stat().st_size <= 8 * 1024 * 1024
            ok(large_client, "reload")
            assert ok(large_client, "history")["available"], "Saved history must remain readable"
        finally:
            large_client.close()
        print("command undo/redo, saved history, branch replacement, foreign/input change refusal, WAL recovery and declarative contributions passed")


if __name__ == "__main__":
    run(sys.argv[1])
