#!/usr/bin/env python3
"""Local display names reuse analysis; obsolete and semantic edits do not."""
import copy
import json
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from analysis_snapshot_test import durable_files
from source_cache_test import A, B, C, ok, source
from transport_test import Client


def request(view, name):
    target = next(item for item in view["code_names"]
                  if item.get("kind") == "local" and item["original"] == "value")
    return {"address": A, "representation": "source", "kind": "name",
            "original": target["original"], "identity": target["identity"], "name": name}


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-local-name-") as directory, ExitStack() as clients:
        binary = Path(directory) / "source-cache.bin"
        binary.write_bytes(b"source cache fixture")
        owner = clients.enter_context(Client(executable))
        ok(owner, "open", {"path": str(binary), "analysis": False, "warm": False})
        original = source(owner)
        snapshot = ok(owner, "analysis_snapshot")
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        first = source(replica)
        source(owner, B)
        edit = request(original, "current_thread")
        assert ok(owner, "code_edit_prepare", request(original, "value"))["ready"]
        assert owner.call("code_edit", {**edit, "identity": "0x1"})["error"]["code"] == "stale_target"
        assert owner.call("code_edit", {**edit, "name": "function_0"})["error"]["code"] == "invalid_request"
        for name in ("current_thread", "thread_state", ""):
            edit = request(source(owner), name)
            assert ok(owner, "code_edit", edit)["local_names_only"]
            current = source(owner)
            assert current["fixture_prepares"] == original["fixture_prepares"]
            assert current["fixture_pages"] == original["fixture_pages"]
            assert (name or "value") + " = 1" in current["text"]
            snapshot = ok(owner, "analysis_snapshot")
            if name == "current_thread":
                # A different committed table can change after the owner
                # exports its snapshot, without touching code.json.
                for file, rows in (
                    ("renames.json", [{"addr": A, "original": "function_0",
                                       "renamed": "foreign_name"}]),
                    ("functions.json", [{"addr": B, "state": "deleted"}]),
                    ("types.json", [{"language": "c",
                                     "source": "typedef unsigned int ReviewWord;"}]),
                    ("libraries.json", {"added": ["foreign"], "removed": []}),
                    ("local/journal.json", {}),
                ):
                    sidecar = Path(str(binary) + ".neverd") / file
                    previous = sidecar.read_bytes() if sidecar.exists() else None
                    try:
                        sidecar.write_text(json.dumps(rows))
                        files = durable_files(directory)
                        response = replica.call("analysis_update_names", snapshot)
                        assert response["status"] == "error", (file, response)
                        assert response["error"]["code"] == "stale_snapshot", response
                        assert source(replica)["text"] == first["text"]
                        assert durable_files(directory) == files
                    finally:
                        if previous is None:
                            sidecar.unlink()
                        else:
                            sidecar.write_bytes(previous)
            before_files = durable_files(directory)
            assert ok(replica, "analysis_update_names", snapshot)["updated"]
            updated = source(replica)
            assert updated["text"] == current["text"]
            for field in ("fixture_instance", "fixture_prepares", "fixture_pages"):
                assert updated[field] == first[field], (field, updated, first)
            assert durable_files(directory) == before_files
            assert owner.call("analysis_update_names", snapshot)["error"]["code"] == "invalid_request"
        # Snapshots do not authorize a different image, stale sidecar, options or
        # type policy to reuse the retained analysis.
        wrong = copy.deepcopy(snapshot)
        wrong["input_sha256"] = "another input"
        assert replica.call("analysis_update_names", wrong)["error"]["code"] == "stale_snapshot"
        racing = copy.deepcopy(snapshot)
        ok(owner, "code_edit", request(source(owner), "new_name"))
        assert replica.call("analysis_update_names", racing)["error"]["code"] == "stale_snapshot"
        # Failed refreshes leave the earlier replica document unchanged.
        assert source(replica)["text"] == updated["text"]
        # A comment takes effect in place, and a source comment as one on the
        # image does, with nothing asked of the engine.
        current = source(owner)
        ok(owner, "code_edit", {"address": A, "representation": "source", "kind": "comment",
                                 "line": 0, "anchor": current["rows"][0]["code_anchor"],
                                 "text": "new source comment"})
        ok(owner, "annotation_set", {"address": A, "text": "new image comment"})
        current = source(owner)
        assert "new source comment" in current["text"], current
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        commented = source(replica)
        assert commented["text"] == current["text"], (commented, current)
        for field in ("fixture_instance", "fixture_prepares", "fixture_pages"):
            assert commented[field] == first[field], (field, commented, first)
        ok(owner, "save")
        # A function's new name, the same way but for its own view.
        other = source(replica, B)
        ok(owner, "rename", {"address": B, "name": "renamed_function"})
        current = source(owner, C)
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        for field in ("fixture_instance", "fixture_prepares", "fixture_pages"):
            assert source(replica)[field] == commented[field], field
        assert source(replica, C)["text"] == current["text"]
        renamed = source(replica, B)
        assert renamed["text"] == source(owner, B)["text"]
        assert "function_identity" not in renamed or renamed != other
        # A number format does not ask the analysis to start over.
        ok(owner, "operand_format", {"address": "0xffff800012340051", "operand": 1, "action": "decimal"})
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        assert source(replica)["text"] == source(owner)["text"]
        assert source(replica)["fixture_instance"] == first["fixture_instance"]
        # Taking a name away shows what the listing calls the function again,
        # which a replica restored under that name has not read.
        ordered = Path(directory) / "source-cache-ordered.bin"
        ordered.write_bytes(b"source cache fixture")
        named_owner = clients.enter_context(Client(executable))
        ok(named_owner, "open", {"path": str(ordered), "analysis": False, "warm": False})
        ok(named_owner, "rename", {"address": A, "name": "named_for_now"})
        named = clients.enter_context(Client(executable))
        ok(named, "analysis_restore", ok(named_owner, "analysis_snapshot"))
        listed = lambda client: json.dumps(ok(client, "listing", {"address": A, "before": 0, "after": 8}))
        assert "named_for_now" in source(named)["text"] and "named_for_now" in listed(named)
        ok(named_owner, "undo")
        assert ok(named, "analysis_update_names", ok(named_owner, "analysis_snapshot"))["updated"]
        assert source(named)["text"] == source(named_owner)["text"]
        assert "named_for_now" not in source(named)["text"] and "named_for_now" not in listed(named)
        # An edit to what the engine reads is refused: the caller restores afresh.
        ok(owner, "function_delete", {"address": C})
        assert replica.call("analysis_update_names", ok(owner, "analysis_snapshot"))["error"]["code"] == "stale_snapshot"
        signature = Path(directory) / "input.pat"
        signature.write_text("signature fixture")
        ok(owner, "signatures_load", {"path": str(signature)})
        signed_snapshot = ok(owner, "analysis_snapshot")
        signed = clients.enter_context(Client(executable))
        ok(signed, "analysis_restore", signed_snapshot)
        signature.write_text("signature fixture changed")
        assert signed.call("analysis_update_names", signed_snapshot)["error"]["code"] == "input_changed"
        large = Path(directory) / "source-cache-refresh-budget.bin"
        large.write_bytes(b"large source cache fixture")
        ok(owner, "open", {"path": str(large), "analysis": False})
        page = source(owner, limit=2048)
        assert not page["complete"] and len(page["text"].encode()) <= 2 * 1024 * 1024
        text = page["text"]
        while not page["complete"]:
            page = source(owner, offset=page["next_offset"], limit=2048)
            assert len(page["text"].encode()) <= 2 * 1024 * 1024
            text += page["text"]
        assert len(text.splitlines()) == 600


if __name__ == "__main__":
    run(sys.argv[1])
    print("Local-name cache reuse, readonly refresh, target and snapshot guards passed")
