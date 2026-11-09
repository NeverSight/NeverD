#!/usr/bin/env python3
"""A kept render answers while what it read of the user's facts reads the same.

Each render says which names of data and which functions' declarations it asked
for.  A new name or declaration elsewhere leaves it answering, one it read sends
the view back to the engine and the job renders it again, and the views read as a
fresh replica's do from then on.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import time

from pe_listing_facts_test import fixture as functions_fixture
from render_ahead_test import done, render_ahead
from source_variable_types_test import global_elf, ok
from transport_test import Client

GLOBAL = 0x401000


def view(client, address, **options):
    return client.call("decompile", {"address": hex(address), "offset": 0, "limit": 200,
                                     "representation": "source", **options})


def page(client, address):
    reply = view(client, address)
    assert reply["status"] == "ok", reply
    return {key: value for key, value in reply["payload"].items()
            if key not in ("revision", "project_id")}


def content(reply):
    return {key: value for key, value in reply["payload"].items()
            if key not in ("revision", "project_id")}


def kept(client, address):
    return view(client, address, kept_only=True)


def sessions(clients, executable, path):
    owner = clients.enter_context(Client(executable))
    ok(owner, "open", {"path": str(path), "analysis": False, "warm": False,
                               "write_behind": True})
    snapshot = ok(owner, "analysis_snapshot")
    snapshot.update(call_summaries=True, render_ahead=True, render_store="write")
    replica = clients.enter_context(Client(executable))
    ok(replica, "analysis_restore", snapshot)
    return owner, replica


def store_folders(path):
    roots = list(path.parent.glob(path.name + "*/local/render-ahead"))
    return sorted(p.name for p in roots[0].iterdir()) if roots else []


def fresh_page(executable, owner, address):
    with Client(executable) as fresh:
        ok(fresh, "analysis_restore", ok(owner, "analysis_snapshot"))
        return page(fresh, address)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-kept-reads-") as directory, ExitStack() as clients:
        root = Path(directory)

        # The name of data.
        named = root / "named.elf"
        image, entry = global_elf()
        named.write_bytes(image)
        owner, replica = sessions(clients, executable, named)
        render_ahead(replica, done)
        # The job takes functions of known sizes.  A visit keeps this one.
        original = page(replica, entry)
        folders = store_folders(named)
        assert len(folders) == 1, folders

        def owner_reads(accept):
            time.sleep(1.1)  # kListingFactsRefreshMilliseconds
            reply = kept(owner, entry)
            assert accept(reply), reply
            return reply

        owner_reads(lambda reply: reply["status"] == "ok")
        # Another address's name is nothing it read.
        ok(owner, "rename", {"address": hex(GLOBAL + 16), "name": "elsewhere"})
        owner_reads(lambda reply: reply["status"] == "ok")
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        assert page(replica, entry)["text"] == original["text"]
        # Its own is, and the page declares it: spelling it there reads as a render
        # does, field for field, with no engine asked.
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "the_global"})
        reply = kept(owner, entry)
        assert reply["status"] == "ok", reply
        assert store_folders(named) == folders
        rendered = fresh_page(executable, owner, entry)
        assert "the_global" in rendered["text"] and "qword_401000" not in rendered["text"], rendered
        assert content(reply) == rendered
        # A local's new name builds the document again from that page.
        local = next(name for name in reply["payload"]["code_names"] if name["kind"] == "local")
        ok(owner, "code_edit", {"address": hex(entry), "representation": "source", "kind": "name",
                                "original": local["original"], "identity": local["identity"],
                                "name": "a_local_name"})
        again = kept(owner, entry)
        assert again["status"] == "ok" and "a_local_name" in again["payload"]["text"], again
        assert content(again) == fresh_page(executable, owner, entry)
        ok(owner, "undo")
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        assert page(replica, entry) == rendered
        # A name the page has another use of is the engine's to step around.
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "t3"})
        assert kept(owner, entry)["error"]["code"] == "not_kept"
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        stepped = page(replica, entry)
        assert stepped == fresh_page(executable, owner, entry)
        # The visit keeps what it rendered in its place.
        reply = owner_reads(lambda reply: reply["status"] == "ok")
        assert reply["payload"]["text"] == stepped["text"]
        # A page read while the global shared a local's spelling tells them apart
        # only in a render.
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "the_same_global"})
        assert kept(owner, entry)["error"]["code"] == "not_kept"
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        again = page(replica, entry)
        assert "the_same_global" in again["text"]
        assert again == fresh_page(executable, owner, entry)
        # A name again, over the page that render kept.
        reply = owner_reads(lambda reply: reply["status"] == "ok")
        assert reply["payload"]["text"] == again["text"]
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "a_third_global"})
        reply = kept(owner, entry)
        assert reply["status"] == "ok", reply
        rendered = fresh_page(executable, owner, entry)
        assert "a_third_global" in rendered["text"] and content(reply) == rendered
        # A name reaches more than a spelling where the C spells it otherwise, and
        # where a library header declares it.
        for name in ("int", "a.dotted.name", "stdout"):
            ok(owner, "rename", {"address": hex(GLOBAL), "name": name})
            refused = kept(owner, entry)
            assert refused["error"]["code"] == "not_kept", (name, refused)
            ok(owner, "undo")
        # Back through those names: the page kept under one reads as it did.
        ok(owner, "undo")
        reply = kept(owner, entry)
        assert reply["status"] == "ok" and reply["payload"]["text"] == again["text"], reply
        ok(owner, "undo")
        assert kept(owner, entry)["error"]["code"] == "not_kept"
        ok(owner, "undo")
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        rendered = fresh_page(executable, owner, entry)
        assert page(replica, entry)["text"] == rendered["text"]
        # The name given up reads as the image names it, which is the engine's to say.
        ok(owner, "undo")
        time.sleep(1.1)  # kListingFactsRefreshMilliseconds
        assert kept(owner, entry)["error"]["code"] == "not_kept"
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        undone = page(replica, entry)
        assert undone["text"] == original["text"], undone["text"]
        assert undone == fresh_page(executable, owner, entry)
        assert store_folders(named) == folders
        replica.close()
        owner.close()
        # A later session reads what the last one kept.
        owner, replica = sessions(clients, executable, named)
        render_ahead(replica, done)
        owner_reads(lambda reply: reply["status"] == "ok")
        assert page(replica, entry)["text"] == original["text"]
        replica.close()
        owner.close()

        # A function's declaration reaches the function and its callers.
        typed = root / "typed.dll"
        typed.write_bytes(functions_fixture())
        owner, replica = sessions(clients, executable, typed)
        state = render_ahead(replica, done)
        assert state["rendered"] > 2, state
        target, caller = 0x140001040, 0x140001100
        before = {address: page(replica, address) for address in (target, caller)}
        folders = store_folders(typed)
        # A function nothing calls: only it reads its declaration.
        ok(owner, "function_edit", {"address": hex(caller), "attributes": {
            "type": "long long sub_140001100(long long renamed_argument)"}})
        assert store_folders(typed) == folders
        assert kept(owner, caller)["error"]["code"] == "not_kept"
        assert kept(owner, target)["status"] == "ok"
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        state = render_ahead(replica, lambda s: done(s) and s["rendered"] == 1)
        shown = page(replica, caller)
        assert "renamed_argument" in shown["text"], shown["text"]
        assert shown == fresh_page(executable, owner, caller)
        assert page(replica, target) == fresh_page(executable, owner, target)
        # A function something calls: its callers read it.
        ok(owner, "function_edit", {"address": hex(target), "attributes": {
            "type": "long long DllEntryPoint_0(void)"}})
        assert store_folders(typed) == folders
        assert kept(owner, target)["error"]["code"] == "not_kept"
        assert kept(owner, caller)["error"]["code"] == "not_kept"
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        state = render_ahead(replica, lambda s: done(s) and s["rendered"] >= 2)
        for address in (target, caller):
            assert page(replica, address) == fresh_page(executable, owner, address), hex(address)
        # They are taken back, and they read as they did.
        ok(owner, "undo")
        ok(owner, "undo")
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        render_ahead(replica, done)
        for address, shown in before.items():
            assert page(replica, address)["text"] == shown["text"], hex(address)
        assert store_folders(typed) == folders
        # A type nothing reads: the owner says which store its views are kept
        # under as it takes it, and the next view it reads from that store
        # does not read that again, and reads as a fresh replica does.
        listed = ok(owner, "functions", {"offset": 0, "limit": 100})["items"]
        other = next(int(item["address"], 16) for item in listed
                     if int(item["address"], 16) not in (target, caller))
        reads = ok(owner, "metadata")["view_store_name_reads"]
        assert ok(owner, "local_types_declare",
                  {"language": "c", "source": "struct KEPT_STORE { int value; };"})["unread"]
        reply = kept(owner, other)
        assert reply["status"] == "ok", reply
        assert ok(owner, "metadata")["view_store_name_reads"] == reads
        assert content(reply) == fresh_page(executable, owner, other)
    print("a kept render answers while what it read of the user's facts reads the same")


if __name__ == "__main__":
    run(sys.argv[1])
