#!/usr/bin/env python3
"""A name's reply does not wait for the disk, and nothing about the project shows it.

Whatever ends a worker, the project's files hold every edit whose write finished and are
never left torn: a clean exit writes what is queued, a kill leaves them as they were or a
journal that completes them.  What reads the files first waits for them, and a write that
fails fails the next edit that finds it, which keeps the project as it was until it can.
"""
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).parent))
from source_variable_types_test import global_elf
from transport_test import Client

GLOBAL = 0x401000
BURST = 40


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def opened(executable, path):
    client = Client(executable)
    ok(client, "open", {"path": str(path), "analysis": False, "warm": False,
                        "write_behind": True})
    return client


def project_folder(path):
    return next(path.parent.glob(path.name + "*.neverd"))


def journal(path):
    return [p for p in project_folder(path).rglob("journal.json")]


def names_of(client):
    return {row["name"] for row in ok(client, "names", {"offset": 0, "limit": 512})["items"]}


def resolves(client, name):
    reply = client.call("resolve", {"query": name})
    return reply["status"] == "ok" and int(reply["payload"]["address"], 16) == GLOBAL


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-edit-writes-") as folder:
        path = Path(folder) / "named.elf"
        image, entry = global_elf()
        path.write_bytes(image)

        # A burst, then a clean exit at once: all of it reaches the files.
        client = opened(executable, path)
        for index in range(BURST):
            ok(client, "rename", {"address": hex(GLOBAL), "name": f"burst_{index}"})
        client.close()
        assert not journal(path), journal(path)
        client = opened(executable, path)
        assert resolves(client, f"burst_{BURST - 1}")
        # Each is a step of the history.
        ok(client, "undo")
        assert resolves(client, f"burst_{BURST - 2}")
        ok(client, "redo")
        assert resolves(client, f"burst_{BURST - 1}")

        # What goes on from the name reads it, on and off the disk.
        ok(client, "rename", {"address": hex(GLOBAL), "name": "read_at_once"})
        view = ok(client, "decompile", {"address": hex(entry), "representation": "source",
                                        "offset": 0, "limit": 200})
        assert "read_at_once" in view["text"], view["text"]
        # An undo right behind it undoes it.
        ok(client, "rename", {"address": hex(GLOBAL), "name": "undone_at_once"})
        ok(client, "undo")
        assert resolves(client, "read_at_once")
        # A replica reads the project's files.
        for index in range(BURST):
            ok(client, "rename", {"address": hex(GLOBAL), "name": f"for_replica_{index}"})
        snapshot = ok(client, "analysis_snapshot")
        with Client(executable) as replica:
            ok(replica, "analysis_restore", snapshot)
            view = ok(replica, "decompile", {"address": hex(entry), "representation": "source",
                                             "offset": 0, "limit": 200})
            assert f"for_replica_{BURST - 1}" in view["text"], view["text"]
        # Another edit that writes at once writes after what was queued.
        for index in range(8):
            ok(client, "rename", {"address": hex(GLOBAL), "name": f"before_save_{index}"})
        ok(client, "annotation_set", {"address": hex(GLOBAL), "text": "a note"})
        ok(client, "save")
        assert not journal(path), journal(path)
        client.close()
        client = opened(executable, path)
        assert resolves(client, "before_save_7")
        client.close()

        # A kill at any moment leaves a project that opens and takes edits.
        for round_number in range(12):
            client = opened(executable, path)
            last = None
            for index in range(1 + round_number % 3):
                last = f"killed_{round_number}_{index}"
                ok(client, "rename", {"address": hex(GLOBAL), "name": last})
            time.sleep(round_number * 0.002)
            client.process.kill()
            client.process.wait()
            client = opened(executable, path)
            # The history and the files agree whichever edits got there.
            shown = ok(client, "listing", {"address": hex(GLOBAL), "before": 0, "after": 1})
            ok(client, "rename", {"address": hex(GLOBAL), "name": f"after_kill_{round_number}"})
            assert resolves(client, f"after_kill_{round_number}"), shown
            ok(client, "undo")
            ok(client, "redo")
            client.close()
            assert not journal(path), journal(path)

        # A write that fails fails the next edit, and goes through once it can.
        if os.name == "posix" and os.geteuid() != 0:
            client = opened(executable, path)
            folder_path = project_folder(path)
            mode = stat.S_IMODE(folder_path.stat().st_mode)
            ok(client, "rename", {"address": hex(GLOBAL), "name": "before_trouble"})
            ok(client, "metadata")
            # The queue is empty before the folder closes.
            ok(client, "analysis_snapshot")
            folder_path.chmod(0o500)
            try:
                # Those taken before the write is found to fail stay taken.
                taken = ["before_trouble"]
                refused = None
                for index in range(200):
                    name = f"in_trouble_{index}"
                    refused = client.call("rename", {"address": hex(GLOBAL), "name": name})
                    if refused["status"] != "ok":
                        break
                    taken.append(name)
                    time.sleep(0.01)
                assert refused["status"] != "ok", refused
                assert len(taken) > 1, refused
                assert refused["error"]["code"] == "save_failed", refused
                blocked = client.call("analysis_snapshot")
                assert blocked["status"] != "ok", blocked
            finally:
                folder_path.chmod(mode)
            ok(client, "rename", {"address": hex(GLOBAL), "name": "out_of_trouble"})
            client.close()
            assert not journal(path), journal(path)
            client = opened(executable, path)
            assert resolves(client, "out_of_trouble")
            for name in reversed(taken):
                ok(client, "undo")
                assert resolves(client, name), name
            client.close()

        # Files changed behind the worker stop the next edit.
        client = opened(executable, path)
        ok(client, "rename", {"address": hex(GLOBAL), "name": "before_foreign"})
        ok(client, "analysis_snapshot")
        renames = project_folder(path) / "renames.json"
        renames.write_text(renames.read_text().replace("before_foreign", "from_outside"))
        refused = client.call("rename", {"address": hex(GLOBAL), "name": "stopped"})
        assert refused["status"] != "ok" and refused["error"]["code"] == "foreign_edits", refused
        client.close()
    print("edit writes: a name's reply goes before the disk, and nothing about the project shows it")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
