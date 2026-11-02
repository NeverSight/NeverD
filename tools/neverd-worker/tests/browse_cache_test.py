#!/usr/bin/env python3
"""Separate-process reuse, invalidation and damage recovery for browse facts."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import time

from pe_listing_facts_test import BASE, ENTRY, BODY, fixture, ok
from transport_test import Client


def browse(worker, path, **options):
    with Client(worker) as client:
        ok(client, "open", {"path": str(path), "analysis": False, **options})
        rows = [ok(client, "listing", {"address": hex(address), "before": 0,
                                       "after": 35})["lines"]
                for address in (ENTRY, BODY, BASE + 0x3000)]
        stats = ok(client, "metadata")["browse_cache"]
    return rows, stats


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-browse-cache-") as directory:
        path = Path(directory) / "entry.dll"
        path.write_bytes(fixture())
        folder = Path(str(path) + ".neverd/local/browse-cache")
        with Client(worker) as client:
            request = client.send("open", {"path": str(path), "browse_cache": "true"})
            rejected = client.response(request)
            assert rejected["status"] == "error", rejected
            assert rejected["error"]["code"] == "invalid_request", rejected
        reference, cold = browse(worker, path)
        assert cold["enabled"] and cold["hits"] == 0 and cold["misses"] >= 4, cold
        files = sorted(folder.glob("*.ndbc"))
        assert len(files) >= 4, files
        before = {file.name: (file.stat().st_mtime_ns, file.read_bytes()) for file in files}
        warm_rows, warm = browse(worker, path, read_only=True)
        assert warm_rows == reference, (reference, warm_rows)
        assert warm["hits"] >= 4 and warm["misses"] == 0, warm
        assert warm["writes"] == warm["pending"] == 0, warm
        assert before == {file.name: (file.stat().st_mtime_ns, file.read_bytes())
                          for file in folder.glob("*.ndbc")}

        # GUI startup asks global models before its first viewport. Those
        # reads must not force reconstruction ahead of a cached current page.
        gui_path = Path(directory) / "gui-order.dll"
        gui_path.write_bytes(fixture())
        previous = None
        for read_only in (False, True):
            with Client(worker) as client:
                ok(client, "open", {"path": str(gui_path), "analysis": False,
                                    "read_only": read_only})
                if not read_only:
                    overview = ok(client, "overview", {"buckets": 100})
                functions = ok(client, "functions")["items"]
                if read_only:
                    overview = ok(client, "overview", {"buckets": 100})
                page = ok(client, "listing", {
                    "address": hex(ENTRY), "recover_address": hex(ENTRY),
                    "after": 2, "complete_anchor_item": True})
                actual = [functions, overview["regions"], overview["buckets"], page["lines"]]
                if previous is not None:
                    assert actual == previous, (previous, actual)
                    stats = ok(client, "metadata")["browse_cache"]
                    assert stats["hits"] == 3 and stats["misses"] == 0, stats
                previous = actual

        # Closing the GUI's worker pipe is a clean exit too: pending facts
        # must be flushed rather than discarded by immediate termination.
        eof_path = Path(directory) / "eof.dll"
        eof_path.write_bytes(fixture())
        with Client(worker) as client:
            ok(client, "open", {"path": str(eof_path), "analysis": False})
            eof_rows = ok(client, "listing", {"address": hex(ENTRY), "after": 35})["lines"]
            client.process.stdin.close()
            assert client.process.wait(timeout=8) == 0
        with Client(worker) as client:
            ok(client, "open", {"path": str(eof_path), "analysis": False, "read_only": True})
            assert ok(client, "listing", {"address": hex(ENTRY), "after": 35})["lines"] == eof_rows
            assert ok(client, "metadata")["browse_cache"]["hits"] == 1

        # Same size and mtime must not defeat the source-byte identity.
        stamp = path.stat()
        original = path.read_bytes()
        changed = bytearray(original)
        changed[0x360] ^= 1
        path.write_bytes(changed)
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        _, different = browse(worker, path, read_only=True)
        assert different["hits"] == 0 and different["misses"] >= 4, different
        path.write_bytes(original)
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))

        # A truncated file or incorrect CBOR with a valid outer checksum is
        # disposable. The ordinary producer still returns the same facts.
        for index, file in enumerate(files):
            saved = file.read_bytes()
            if index % 2:
                payload = b"\xf6"  # CBOR null, invalid for every cached schema
                header = saved[:71] + hashlib.sha256(payload).hexdigest().encode() + b"\n"
                file.write_bytes(header + payload)
            else:
                file.write_bytes(saved[:8])
        rows, damaged = browse(worker, path)
        assert rows == reference, (reference, rows)
        assert damaged["invalid"] >= 4, damaged
        rows, repaired = browse(worker, path, read_only=True)
        assert rows == reference and repaired["misses"] == 0, repaired

        # Persisted type declarations are part of the identity even when the
        # declaration is unrelated to the currently visible function.
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            result = ok(client, "local_types_declare", {
                "language": "c", "source": "struct CACHE_RECORD { unsigned value; };"})
            assert not result["problems"], result
        _, typed = browse(worker, path, read_only=True)
        assert typed["hits"] == 0 and typed["misses"] >= 4, typed

        # A user edit must retire the current namespace as well as invalidate
        # the next process. Previously saved raw instruction text cannot leak.
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "listing", {"address": hex(ENTRY), "after": 12})
            hits = ok(client, "metadata")["browse_cache"]["hits"]
            ok(client, "rename", {"address": hex(ENTRY), "name": "user_entry"})
            edited = ok(client, "listing", {"address": hex(ENTRY), "after": 35})
            assert "user_entry" in "\n".join(row["text"] for row in edited["lines"]), edited
            stats = ok(client, "metadata")["browse_cache"]
            assert stats["hits"] == hits and stats["misses"] > 0, stats
        rows, edited_warm = browse(worker, path, read_only=True)
        assert "user_entry" in "\n".join(row["text"] for row in rows[0])

        # Comments are part of the identity as well. Once the function table
        # has read the identity, the first page after a new comment must not
        # come from the old namespace, nor be stored there with the comment.
        request = {"address": hex(ENTRY), "before": 0, "after": 35}
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "listing", request)
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "functions")
            ok(client, "annotation_set", {"address": hex(ENTRY), "text": "cached note"})
            noted = ok(client, "listing", request)
            assert "cached note" in "\n".join(row["text"] for row in noted["lines"]), noted
            deadline = time.monotonic() + 5
            while ok(client, "metadata")["browse_cache"]["pending"]:
                assert time.monotonic() < deadline
                time.sleep(.3)  # Leave the executor its idle write window.
            ok(client, "undo")
        rows, _ = browse(worker, path, read_only=True)
        assert "cached note" not in "\n".join(row["text"] for row in rows[0]), rows[0]

        # Explicit opt-out bypasses both cache reads and writes.
        count = len(list(folder.glob("*.ndbc")))
        rows, disabled = browse(worker, path, browse_cache=False)
        assert not disabled["enabled"] and disabled["hits"] == disabled["writes"] == 0, disabled
        assert len(list(folder.glob("*.ndbc"))) == count

        # A writer stopped between writing and publishing leaves a temporary
        # entry. The next flush removes an old one and keeps a current one.
        stale = folder / ("0" * 64 + ".ndbc.tmp-1-0")
        fresh = folder / ("1" * 64 + ".ndbc.tmp-2-0")
        for file in (stale, fresh):
            file.write_bytes(b"partial")
        old = time.time() - 24 * 3600
        os.utime(stale, (old, old))
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "listing", {"address": hex(BODY), "before": 3, "after": 17})
        assert not stale.exists() and fresh.exists()
        fresh.unlink()

        # Whole-program analysis is part of the identity: a document read
        # before it must not answer after it, even with the same function count.
        analyzed = Path(directory) / "analyzed.dll"
        analyzed.write_bytes(fixture())
        with Client(worker) as client:
            ok(client, "open", {"path": str(analyzed), "analysis": False})
            ok(client, "overview", {"buckets": 100})
        with Client(worker) as client:
            ok(client, "open", {"path": str(analyzed), "analysis": False})
            ok(client, "overview", {"buckets": 100})
            before = ok(client, "metadata")["browse_cache"]
            assert before["hits"] == 1, before
            ok(client, "analyze")
            ok(client, "overview", {"buckets": 100})
            after = ok(client, "metadata")["browse_cache"]
            assert after["hits"] == before["hits"], (before, after)

        # Replacing the image in a live worker retires the old listing's
        # deferred producers. Each project must keep its own names and facts.
        other = Path(directory) / "other.dll"
        other.write_bytes(fixture())
        with Client(worker) as client:
            for selected, name in ((path, "user_entry"), (other, "DllEntryPoint"),
                                   (path, "user_entry")):
                ok(client, "open", {"path": str(selected), "analysis": False})
                reply = ok(client, "listing", {"address": hex(ENTRY), "after": 35})
                text = "\n".join(row["text"] for row in reply["lines"])
                assert name in text, (selected, reply)
                if selected == other:
                    assert "user_entry" not in text, reply
        other_rows, _ = browse(worker, other, read_only=True)
        assert "DllEntryPoint" in "\n".join(row["text"] for row in other_rows[0])
        assert "user_entry" not in "\n".join(row["text"] for row in other_rows[0])

        # A cache directory that cannot be created must not fail a valid open
        # or listing. Keep the write error visible in structured diagnostics.
        blocked = Path(directory) / "blocked.dll"
        blocked.write_bytes(fixture())
        blocked_cache = Path(str(blocked) + ".neverd/local/browse-cache")
        blocked_cache.parent.mkdir(parents=True)
        blocked_cache.write_text("a file occupies the cache directory")
        with Client(worker) as client:
            ok(client, "open", {"path": str(blocked), "analysis": False})
            reply = ok(client, "listing", {"address": hex(BODY), "after": 35})
            assert any(row["kind"] == "insn" for row in reply["lines"]), reply
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                time.sleep(.3)  # Leave the executor its idle write window.
                state = ok(client, "metadata")["browse_cache"]
                if state["diagnostic"]:
                    break
            assert state["diagnostic"] and state["writes"] == 0, state
    print("Browse cache: fresh-process hits, identical facts, byte identity, read-only, damage, edits, comments, analysis, stale temporaries and opt-out passed")


if __name__ == "__main__":
    run(sys.argv[1])
