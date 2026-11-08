#!/usr/bin/env python3
"""What an owner keeps of a view through an edit reads as a new reader of the project does.

An edit that reaches only some views leaves the others' documents as they are.  After each
edit every view must read, field for field, as a session that has just opened the project's
files reads it: the function renamed in its callers and its own view, the comment where its
address shows, data's new name in what reads it, and everything else as it was.  Two edits
with no view between them must each count: the first's forgetting is not lost to the second.
"""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

from listing_facts_test import ADDRESSES, DATA
from pe_listing_facts_test import fixture as functions_fixture
from render_ahead_test import done, render_ahead
from source_variable_types_test import global_elf, ok
from transport_test import Client

GLOBAL = 0x401000


def view(client, address):
    reply = client.call("decompile", {"address": hex(address), "representation": "source",
                                      "offset": 0, "limit": 200, "kept_only": True})
    if reply["status"] != "ok":
        return reply["error"]["code"]
    return {key: value for key, value in reply["payload"].items()
            if key not in ("revision", "project_id")}


def filled(clients, executable, path, data, visits=()):
    path.write_bytes(data)
    owner = clients.enter_context(Client(executable))
    ok(owner, "open", {"path": str(path), "analysis": False, "warm": False, "write_behind": True})
    restore = ok(owner, "analysis_snapshot")
    restore.update(call_summaries=True, render_ahead=True, render_store="write")
    with Client(executable) as replica:
        ok(replica, "analysis_restore", restore)
        render_ahead(replica, done)
        # The job takes functions of known sizes.  A visit keeps the others.
        for address in visits:
            ok(replica, "decompile", {"address": hex(address), "representation": "source",
                                      "offset": 0, "limit": 200})
    kept = ok(owner, "kept_renders")
    assert kept["complete"], kept
    return owner


def replica_view(client, address):
    reply = client.call("decompile", {"address": hex(address), "representation": "source",
                                      "offset": 0, "limit": 200})
    assert reply["status"] == "ok", reply
    return {key: value for key, value in reply["payload"].items()
            if key not in ("revision", "project_id")}


def store_reads(client):
    """How many views the owner has read from what is kept, not from a document it holds."""
    return client.call("kept_renders")["background"]["render_ahead"]["hits"]


def compare(executable, owner, path, functions, step):
    seen = {address: view(owner, address) for address in functions}
    ok(owner, "analysis_snapshot")
    with Client(executable) as fresh:
        ok(fresh, "open", {"path": str(path), "analysis": False, "warm": False, "read_only": True})
        for address, row in seen.items():
            other = view(fresh, address)
            assert other == row, (step, hex(address), other, row)
    return seen


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-view-edits-") as directory, ExitStack() as clients:
        path = Path(directory) / "edits.dll"
        owner = filled(clients, executable, path, functions_fixture())
        functions = list(ADDRESSES)

        def same_as_fresh(step):
            seen = compare(executable, owner, path, functions, step)
            assert not [row for row in seen.values() if isinstance(row, str)], (step, seen)
            return seen

        first = same_as_fresh("nothing")
        # A function prepared for an edit reads the same after it.
        assert ok(owner, "prepare_function", {"address": hex(functions[1])})["prepared"]
        assert same_as_fresh("prepared") == first
        # A function's name reads in its callers and in its own view, as it reads afresh.
        ok(owner, "rename", {"address": hex(functions[1]), "name": "renamed_function"})
        named = same_as_fresh("function name")
        for address in functions:
            assert "renamed_function" in named[address]["text"], named[address]["text"]
        # A comment shows at its address.
        ok(owner, "annotation_set", {"address": hex(functions[2]), "text": "a note here"})
        ok(owner, "save")
        noted = same_as_fresh("comment")
        assert "a note here" in noted[functions[2]]["text"], noted[functions[2]]
        assert noted[functions[1]] == named[functions[1]]
        # And on a line inside a function.
        inner = next(at for row in noted[functions[1]]["rows"] for at in row["addresses"]
                     if int(at, 16) != functions[1])
        ok(owner, "annotation_set", {"address": inner, "text": "a line's note"})
        ok(owner, "save")
        lined = same_as_fresh("comment on a line")
        assert "a line's note" in lined[functions[1]]["text"], lined[functions[1]]
        assert lined[functions[2]] == noted[functions[2]]
        ok(owner, "undo")
        ok(owner, "save")
        assert same_as_fresh("comment undone") == noted
        # A name another view spells and nothing has resolved leads there now.
        ok(owner, "rename", {"address": hex(functions[2]), "name": "__readeflags"})
        spelled = same_as_fresh("a name another view spells")
        assert any(name.get("original") == "__readeflags" and name.get("address") == hex(functions[2])
                   for name in spelled[functions[1]]["code_names"]), spelled[functions[1]]
        ok(owner, "undo")
        assert same_as_fresh("that name undone") == noted
        # A note on a function and a type nothing reads leave every view.
        ok(owner, "function_edit", {"address": hex(functions[2]), "attributes": {"color": "#204060"}})
        assert same_as_fresh("colour") == noted
        reply = ok(owner, "local_types_declare", {"language": "c", "source": "struct VIEW_NEW { unsigned value; };"})
        assert reply["unread"], reply
        assert same_as_fresh("new type") == noted
        # A page the client keeps reads the same while its document stands, and
        # the worker says so in place of sending it again.
        kept_page = owner.call("decompile", {"address": hex(functions[0]), "representation": "source",
                                             "offset": 0, "limit": 200, "kept_only": True,
                                             "if_none_match": ""})["payload"]
        ok(owner, "function_edit", {"address": hex(functions[2]), "attributes": {"color": "#405061"}})
        again = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                        "offset": 0, "limit": 200, "kept_only": True,
                                        "if_none_match": kept_page["etag"]})
        assert again["unchanged"] and again["etag"] == kept_page["etag"], again
        ok(owner, "annotation_set", {"address": hex(functions[0]), "text": "on the kept page"})
        ok(owner, "save")
        changed = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                          "offset": 0, "limit": 200, "kept_only": True,
                                          "if_none_match": kept_page["etag"]})
        assert not changed.get("unchanged") and "on the kept page" in changed["text"], changed
        assert changed["etag"] != kept_page["etag"]
        ok(owner, "undo")
        ok(owner, "save")
        ok(owner, "undo")
        assert same_as_fresh("page identity undone") == noted
        # What they reach none of, the owner's documents answer.
        reads = store_reads(owner)
        ok(owner, "function_edit", {"address": hex(functions[2]), "attributes": {"color": "#405060"}})
        ok(owner, "local_types_declare", {"language": "c", "source": "struct VIEW_KEPT { unsigned value; };"})
        ok(owner, "annotation_set", {"address": hex(functions[2]), "text": "another note"})
        ok(owner, "save")
        for address in functions:
            assert isinstance(view(owner, address), dict)
        assert store_reads(owner) == reads, (store_reads(owner), reads)
        ok(owner, "undo")
        ok(owner, "save")
        for _ in range(2):
            ok(owner, "undo")
        assert same_as_fresh("those undone") == noted
        # Back, as it was.
        for _ in range(3):
            ok(owner, "undo")
        ok(owner, "save")
        ok(owner, "undo")
        assert same_as_fresh("undone") == first
        # A name taken in place leaves what does not read it, and reaches a view that
        # has not read many edits since: past what a document can read of them it
        # builds again.  A listing reads the names again first, for it to be in place.
        ok(owner, "functions", {"offset": 0, "limit": 10})
        same_as_fresh("names read")
        reads = store_reads(owner)
        ok(owner, "rename", {"address": hex(functions[2]), "name": "after_many"})
        assert isinstance(view(owner, functions[0]), dict)
        assert store_reads(owner) == reads, (store_reads(owner), reads)
        for count in range(140):
            ok(owner, "annotation_set", {"address": hex(functions[3]), "text": f"note {count}"})
            ok(owner, "save")
        many = same_as_fresh("many edits")
        assert "after_many" in many[functions[2]]["text"], many[functions[2]]
        same_as_fresh("many read")
        # An edit each view must feel, then one that keeps them, with no view between.
        ok(owner, "rename", {"address": hex(functions[1]), "name": "again_function"})
        ok(owner, "function_edit", {"address": hex(functions[1]), "attributes": {"color": "#102030"}})
        again = same_as_fresh("rename then colour")
        assert "again_function" in again[functions[1]]["text"], again[functions[1]]["text"]
        ok(owner, "function_edit", {"address": hex(functions[1]), "attributes": {"type": "long long again_function(long long value)"}})
        ok(owner, "function_edit", {"address": hex(functions[2]), "attributes": {"color": "#304050"}})
        ok(owner, "function_edit", {"address": hex(functions[1]), "attributes": {"type": None}})
        ok(owner, "local_types_declare", {"language": "c", "source": "struct VIEW_NEXT { unsigned value; };"})
        compare(executable, owner, path, functions, "declaration then type")

        # A replica takes a function's new name and data's in one update as the
        # owner takes them, without reading the names again, and reads as a
        # replica restored afterwards.
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
        for address in functions:
            replica_view(replica, address)
        builds = ok(replica, "metadata")["listing_name_builds"]
        ok(owner, "rename", {"address": hex(functions[2]), "name": "replica_name"})
        ok(owner, "rename", {"address": hex(DATA), "name": "replica_data"})
        assert ok(replica, "analysis_update_names", ok(owner, "analysis_snapshot"))["updated"]
        taken = {address: replica_view(replica, address) for address in functions}
        assert ok(replica, "metadata")["listing_name_builds"] == builds
        names = [row["name"] for row in ok(replica, "functions", {"offset": 0, "limit": 10})["items"]]
        assert "replica_name" in names, names
        with Client(executable) as restored:
            ok(restored, "analysis_restore", ok(owner, "analysis_snapshot"))
            for address, row in taken.items():
                assert replica_view(restored, address) == row, hex(address)
        replica.close()

        # A name of data, in what reads it and in nothing else.
        path = Path(directory) / "global.elf"
        binary, entry = global_elf()
        owner = filled(clients, executable, path, binary, [entry])
        functions = [entry]
        before = compare(executable, owner, path, functions, "first")
        assert not [row for row in before.values() if isinstance(row, str)], before
        reader = [address for address, row in before.items() if "401000" in row["text"]]
        assert reader, before
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "renamed_global"})
        after = compare(executable, owner, path, functions, "data name")
        for address in reader:
            assert "renamed_global" in after[address]["text"], after[address]["text"]
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "renamed_again"})
        ok(owner, "item_define", {"address": hex(GLOBAL + 0x10), "action": "data", "size": 4})
        after = compare(executable, owner, path, functions, "data name then item")
        for address in reader:
            assert "renamed_again" in after[address]["text"], after[address]["text"]
        # Another address's name and an item leave the document that read neither.
        reads = store_reads(owner)
        ok(owner, "rename", {"address": hex(GLOBAL + 0x18), "name": "elsewhere"})
        ok(owner, "item_define", {"address": hex(GLOBAL + 0x14), "action": "data", "size": 2})
        assert view(owner, entry) == after[entry]
        assert store_reads(owner) == reads, (store_reads(owner), reads)
    print("what an owner keeps of a view through an edit reads as a new reader of the project does")


if __name__ == "__main__":
    run(sys.argv[1])
