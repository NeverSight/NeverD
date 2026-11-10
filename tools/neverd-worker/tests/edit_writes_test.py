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
# A qword in the 32 bytes of data from GLOBAL.
ITEM = GLOBAL + 16
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


def qword_at(client, address):
    rows = ok(client, "listing", {"address": hex(address), "before": 0, "after": 1})["lines"]
    return any(int(row["item"], 16) == address and int(row["item_size"], 16) == 8
               for row in rows if row.get("kind") == "data")


def colour_of(client, address):
    return ok(client, "function_attributes", {"address": hex(address)}).get("color")


def source_of(client, address):
    return ok(client, "decompile", {"address": hex(address), "representation": "source",
                                    "offset": 0, "limit": 200})["text"]


def local_of(client, address):
    view = ok(client, "decompile", {"address": hex(address), "representation": "source",
                                    "offset": 0, "limit": 200})
    return next(name for name in view["code_names"] if name["kind"] == "local")


def local_name(address, local, name):
    return {"address": hex(address), "representation": "source", "kind": "name",
            "original": local["original"], "identity": local["identity"], "name": name}


def numbered_elf():
    """global_elf with mov eax, 0xB; ret at its entry: a number to format."""
    image, entry = global_elf()
    image = bytearray(image)
    image[176:182] = bytes.fromhex("b80b000000c3")
    return bytes(image), entry


def number_shown(client, address):
    lines = ok(client, "listing", {"address": hex(address), "before": 0, "after": 16})["lines"]
    shown = [line["text"] for line in lines
             if line.get("kind") == "insn" and int(line["address"], 16) == address]
    assert shown, lines
    return shown[0]


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

        # Staged comments committed ahead of an edit go behind the reply as the
        # edit does: the history holds them at once, before the edit, and a clean
        # exit writes both.
        client = opened(executable, path)
        refused = client.call("save", {"write_behind": "yes"})
        assert refused["status"] != "ok", refused
        assert refused["error"]["code"] == "invalid_request", refused
        ok(client, "annotation_set", {"address": hex(GLOBAL), "text": "committed behind"})
        assert ok(client, "save", {"write_behind": True}) == {"saved": True, "dirty": False}
        ok(client, "rename", {"address": hex(GLOBAL), "name": "after_behind_save"})
        steps = ok(client, "history", {"offset": 0, "limit": 512})
        kinds = [step["kind"] for step in steps["items"]]
        assert kinds[-2:] == ["annotation", "rename"], kinds
        client.close()
        assert not journal(path), journal(path)
        notes = json.loads((project_folder(path) / "annotations.json").read_text())
        assert any(row.get("text") == "committed behind" for row in notes), notes
        client = opened(executable, path)
        assert resolves(client, "after_behind_save")
        # Undo takes the edit away, then the comment.
        ok(client, "undo")
        assert resolves(client, "before_save_7")
        ok(client, "undo")
        ok(client, "save")
        notes = json.loads((project_folder(path) / "annotations.json").read_text())
        assert not any(row.get("text") == "committed behind" for row in notes), notes
        client.close()

        # Undo past a staged comment's own step commits the comments first when
        # asked to, and otherwise refuses as an edit does.
        client = opened(executable, path)
        ok(client, "rename", {"address": hex(GLOBAL), "name": "before_staged"})
        ok(client, "annotation_set", {"address": hex(GLOBAL), "text": "staged then undone"})
        assert ok(client, "undo")["dirty"]
        refused = client.call("undo")
        assert refused["status"] != "ok", refused
        assert refused["error"]["code"] == "unsaved_changes", refused
        refused = client.call("undo", {"commit_staged": "yes"})
        assert refused["error"]["code"] == "invalid_request", refused
        refused = client.call("undo", {"commit_staged": True, "limit": 0})
        assert refused["error"]["code"] == "invalid_request", refused
        assert resolves(client, "before_staged")
        undone = ok(client, "undo", {"commit_staged": True, "limit": 1})
        assert not undone["dirty"] and undone["saved"], undone
        assert len(undone["items"]) == 1 and undone["total"] > 1, undone
        assert resolves(client, "before_save_7")
        ok(client, "redo", {"commit_staged": True})
        assert resolves(client, "before_staged")
        client.close()
        assert not journal(path), journal(path)
        notes = json.loads((project_folder(path) / "annotations.json").read_text())
        assert not any(row.get("text") == "staged then undone" for row in notes), notes
        client = opened(executable, path)
        assert resolves(client, "before_staged")
        client.close()

        # A local's new name goes behind the reply as a name does, and a view
        # right behind it reads it.
        client = opened(executable, path)
        local = local_of(client, entry)
        ok(client, "code_edit", local_name(entry, local, "behind_local"))
        assert "behind_local" in source_of(client, entry)
        ok(client, "undo")
        assert "behind_local" not in source_of(client, entry)
        ok(client, "redo")
        client.close()
        assert not journal(path), journal(path)
        assert "behind_local" in (project_folder(path) / "code.json").read_text()
        client = opened(executable, path)
        assert "behind_local" in source_of(client, entry)
        client.close()

        # A kill at any moment leaves a project that opens and takes edits.
        for round_number in range(12):
            client = opened(executable, path)
            if round_number % 2:
                # Comments committed behind the reply, then names behind them.
                ok(client, "annotation_set",
                   {"address": hex(GLOBAL), "text": f"killed note {round_number}"})
                ok(client, "save", {"write_behind": True})
            if round_number % 3 == 2:
                # A local's name behind the reply too.
                ok(client, "code_edit", local_name(entry, local_of(client, entry),
                                                   f"killed_local_{round_number}"))
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

        # What an edit refused while the files lag behind undoes leaves the
        # engine as the history holds it: a data item refused right behind one
        # whose write fails, and function edits taken before a failed write is
        # found, stay, and reach the files once they take writes again.
        if os.name == "posix" and os.geteuid() != 0:
            client = opened(executable, path)
            folder_path = project_folder(path)
            mode = stat.S_IMODE(folder_path.stat().st_mode)
            ok(client, "analysis_snapshot")
            folder_path.chmod(0o500)
            try:
                ok(client, "item_define", {"address": hex(ITEM), "action": "data", "size": 8})
                refused = client.call("item_define", {"address": hex(GLOBAL), "action": "array",
                                                      "element_size": 8, "count": 3})
                assert refused["status"] != "ok", refused
                assert refused["error"]["code"] == "invalid_request", refused
                assert qword_at(client, ITEM)
            finally:
                folder_path.chmod(mode)
            ok(client, "rename", {"address": hex(GLOBAL), "name": "after_refused_item"})
            client.close()
            assert not journal(path), journal(path)
            client = opened(executable, path)
            assert qword_at(client, ITEM)
            # The entry is a function once a view has prepared it.
            ok(client, "decompile", {"address": hex(entry), "representation": "source",
                                     "offset": 0, "limit": 200})
            ok(client, "analysis_snapshot")
            folder_path.chmod(0o500)
            try:
                taken = []
                for index in range(200):
                    colour = "#%06x" % (0x102030 + index)
                    reply = client.call("function_edit", {"address": hex(entry),
                                                          "attributes": {"color": colour}})
                    if reply["status"] != "ok":
                        break
                    taken.append(colour)
                    time.sleep(0.01)
                assert reply["status"] != "ok", reply
                assert reply["error"]["code"] == "save_failed", reply
                assert taken, reply
            finally:
                folder_path.chmod(mode)
            # The engine kept them, and what reads it now writes them first.
            assert colour_of(client, entry) == taken[-1]
            ok(client, "rename", {"address": hex(GLOBAL), "name": "after_refused_colour"})
            client.close()
            assert not journal(path), journal(path)
            client = opened(executable, path)
            ok(client, "decompile", {"address": hex(entry), "representation": "source",
                                     "offset": 0, "limit": 200})
            assert colour_of(client, entry) == taken[-1]
            assert qword_at(client, ITEM)
            client.close()

            # A local's name goes behind the reply: it is taken while the files
            # take nothing, and written once they take writes again.
            client = opened(executable, path)
            local = local_of(client, entry)
            ok(client, "analysis_snapshot")
            folder_path.chmod(0o500)
            try:
                ok(client, "code_edit", local_name(entry, local, "taken_behind"))
                assert "taken_behind" in source_of(client, entry)
            finally:
                folder_path.chmod(mode)
            ok(client, "analysis_snapshot")
            client.close()
            assert not journal(path), journal(path)
            client = opened(executable, path)
            assert "taken_behind" in source_of(client, entry)
            client.close()

            # So do operand formats, which go behind the reply too.
            numbered = Path(folder) / "numbered.elf"
            image, start = numbered_elf()
            numbered.write_bytes(image)
            client = opened(executable, numbered)
            # Its instructions are the listing's once a view has prepared it.
            source_of(client, start)
            ok(client, "operand_format", {"address": hex(start), "action": "decimal"})
            ok(client, "analysis_snapshot")
            numbered_folder = project_folder(numbered)
            mode = stat.S_IMODE(numbered_folder.stat().st_mode)
            numbered_folder.chmod(0o500)
            spelled = {"hex": "0Bh", "binary": "1011b"}
            try:
                formats = []
                for index in range(200):
                    action = ("hex", "binary")[index % 2]
                    reply = client.call("operand_format", {"address": hex(start), "action": action})
                    if reply["status"] != "ok":
                        break
                    formats.append(action)
                    time.sleep(0.01)
                assert reply["status"] != "ok", reply
                assert reply["error"]["code"] == "save_failed", reply
                assert formats, reply
            finally:
                numbered_folder.chmod(mode)
            assert spelled[formats[-1]] in number_shown(client, start), number_shown(client, start)
            # What waits for the files writes them now.
            ok(client, "analysis_snapshot")
            client.close()
            assert not journal(numbered), journal(numbered)
            client = opened(executable, numbered)
            source_of(client, start)
            assert spelled[formats[-1]] in number_shown(client, start), number_shown(client, start)
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
