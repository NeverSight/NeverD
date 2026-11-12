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
import json
import os
import sys
import tempfile
import time

from listing_facts_test import ADDRESSES, DATA
from pe_listing_facts_test import fixture as functions_fixture
from render_ahead_test import done, render_ahead
from source_variable_types_test import global_elf, ok
from transport_test import Client

GLOBAL = 0x401000
# NEVERD_TEST_CHECK_VIEW_CHANGES=1: the owner makes every page a patch's own
# account answers by comparing the documents too, and fails where they differ
# (open.check_view_changes); it then writes no patch into the document it is
# made of.
CHECK = os.environ.get("NEVERD_TEST_CHECK_VIEW_CHANGES") == "1"


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
    ok(owner, "open", {"path": str(path), "analysis": False, "warm": False, "write_behind": True,
                       "check_view_changes": CHECK})
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


def apply_delta(page, delta):
    """A kept page with what changed applied, as a client applies page_delta."""
    page = json.loads(json.dumps(page))
    for operation in delta["delta"]:
        kind, field = operation[0], operation[1]
        if kind == "set":
            page[field] = operation[2]
        elif kind == "unset":
            del page[field]
        elif kind == "lines":
            lines = page[field].split("\n")
            assert len(lines) == operation[2], (len(lines), operation)
            for line, text in operation[3]:
                lines[line] = text
            page[field] = "\n".join(lines)
        elif kind == "each":
            for item in page[field]:
                item.update({key: value for key, value in operation[2].items() if key in item})
        else:
            assert kind == "items", operation
            old, items, used = page[field], [], 0
            for run in operation[2]:
                if run[0] in ("same", "shift"):
                    for item in old[used:used + run[1]]:
                        item = dict(item) if run[0] == "shift" else item
                        for key, amount in (run[2].items() if run[0] == "shift" else ()):
                            item[key] += amount
                        items.append(item)
                    used += run[1]
                elif run[0] == "skip":
                    used += run[1]
                elif run[0] == "put":
                    items.extend(run[1])
                    used += len(run[1])
                else:
                    assert run[0] == "new", run
                    items.extend(run[1])
            assert used == len(old), (used, len(old))
            page[field] = items
    for key in ("etag", "project_id", "revision"):
        page[key] = delta[key]
    return page


def pages_of(client, address, limit, kept=None):
    """Every page of a view, limit lines each, as a client reads them that keeps none, or
    one that keeps the pages given and names each for what changed of it."""
    pages, offset = [], 0
    while True:
        request = {"address": hex(address), "representation": "source", "offset": offset,
                   "limit": limit, "kept_only": True, "if_none_match": ""}
        if kept:
            base = kept[len(pages)]
            assert base["offset"] == offset, (base["offset"], offset)
            request.update(if_none_match=base["etag"], page_delta=True)
        page = ok(client, "decompile", request)
        if page.get("unchanged"):
            assert page["etag"] == base["etag"], page
            page = dict(base, revision=page["revision"])
        elif "delta" in page:
            assert page["base"] == base["etag"], page
            page = apply_delta(base, page)
        pages.append(page)
        if page["complete"]:
            return pages
        offset = page["next_offset"]


def said(client):
    """How many pages a patch's own account answered, and how many patches were written
    into the document they were made of."""
    metadata = ok(client, "metadata")
    return metadata["named_view_changes_said"], metadata["named_view_documents_taken"]


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
        # A comment shows at its address, written into a copy of the document
        # that read it rather than decorated again.
        patches = ok(owner, "metadata")["named_view_comment_patches"]
        ok(owner, "annotation_set", {"address": hex(functions[2]), "text": "a note here"})
        ok(owner, "save")
        noted = same_as_fresh("comment")
        assert "a note here" in noted[functions[2]]["text"], noted[functions[2]]
        assert ok(owner, "metadata")["named_view_comment_patches"] > patches
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
        # The names its declarations give the stores are read behind its reply,
        # before the next request: the stores stand, and every view reads as it
        # did.
        settled = ok(owner, "metadata")["store_keeps_settled"]
        reply = ok(owner, "local_types_declare", {"language": "c", "source": "struct VIEW_NEW { unsigned value; };"})
        assert reply["unread"], reply
        assert ok(owner, "metadata")["store_keeps_settled"] == settled + 1
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
        # A client that applies what changed to the page it keeps reads that:
        # applied, it is the page a read naming none gets.  The edit's views
        # are read behind its reply as the client will ask for them, and that
        # read takes the answer.
        # The worker reads them only while no request waits, so one sent as the
        # edit's reply arrives can find it has read none: the client rests a
        # moment first, as one that shows a reply does, and where the worker
        # still read none it takes the page as it is and edits again.
        for attempt in range(40):
            before = ok(owner, "metadata")
            ok(owner, "annotation_set", {"address": hex(functions[0]),
                                         "text": f"on the kept page again {attempt}"})
            time.sleep(0.05)
            made = ok(owner, "metadata")["prepared_views_made"] - before["prepared_views_made"]
            ok(owner, "save")
            delta = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                            "offset": 0, "limit": 200, "kept_only": True,
                                            "if_none_match": changed["etag"], "page_delta": True})
            assert "delta" in delta and delta["base"] == changed["etag"], delta
            given = ok(owner, "metadata")["prepared_views_given"] - before["prepared_views_given"]
            # A view it read is the answer; one it did not read is read now.
            assert given == made and made in (0, 1), (made, given)
            if made:
                break
            changed = apply_delta(changed, delta)
        else:
            raise AssertionError("the worker read no view behind any of forty edits")
        assert len(json.dumps(delta)) < len(json.dumps(changed)) / 2, delta
        whole = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                        "offset": 0, "limit": 200, "kept_only": True,
                                        "if_none_match": ""})
        assert apply_delta(changed, delta) == whole
        assert "on the kept page again" in whole["text"], whole
        # An edit carries the reads its views make after it ("then"): the reply
        # answers each from the document the page it names is of, as a read made
        # right after it would be answered; a page of no document the worker
        # holds is the client's to read ("later"), and a read that is no page's
        # is refused alone, the edit standing.
        kept_read = {"address": hex(functions[0]), "representation": "source",
                     "offset": 0, "limit": 200, "kept_only": True,
                     "if_none_match": whole["etag"], "page_delta": True}
        made = ok(owner, "metadata")["prepared_views_made"]
        carried = owner.call("annotation_set", {
            "address": hex(functions[0]), "text": "carried with the edit",
            "then": [{"operation": "decompile", "payload": kept_read},
                     {"operation": "rename", "payload": {}},
                     {"operation": "decompile", "payload": {"address": hex(functions[0])}},
                     {"operation": "decompile", "payload": dict(kept_read, if_none_match="not a page")}]})
        assert carried["status"] == "ok", carried
        answers = carried["then"]
        assert [a["status"] for a in answers] == ["ok", "error", "error", "later"], answers
        assert answers[1]["error"]["code"] == "invalid_request", answers
        assert answers[2]["error"]["code"] == "invalid_request", answers
        # The page its reply gave is not read again behind it.
        assert ok(owner, "metadata")["prepared_views_made"] == made
        followed = apply_delta(whole, answers[0]["payload"])
        assert "carried with the edit" in followed["text"], followed
        assert followed == ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                                   "offset": 0, "limit": 200, "kept_only": True,
                                                   "if_none_match": ""})
        # A view of more pages than one carries the read of each.  The first
        # answered has the patch written into the document both name; the next
        # is answered from what that patch says it wrote, as a read made after
        # the edit is, rather than left to the client.
        def part(offset, tag):
            return {"address": hex(functions[0]), "representation": "source", "offset": offset,
                    "limit": 3, "kept_only": True, "if_none_match": tag, "page_delta": bool(tag)}
        def taken(page, answer):
            assert answer["status"] == "ok", answer
            payload = answer["payload"]
            if payload.get("unchanged"):
                assert payload["etag"] == page["etag"], (payload, page["etag"])
                return page["text"]
            return (apply_delta(page, payload) if "delta" in payload else payload)["text"]
        note = "carried with the edit"
        for turn, order in enumerate(((1, 0), (0, 1))):
            ok(owner, "save")
            document = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                               "offset": 0, "limit": 200, "kept_only": True,
                                               "if_none_match": ""})["text"].split("\n")
            noted_at = next(at for at, line in enumerate(document) if note in line)
            # The note's line in the first of two pages of three lines.
            starts = (max(0, noted_at - 1), max(0, noted_at - 1) + 3)
            assert starts[1] < len(document) - 1, (starts, len(document))
            pages = [ok(owner, "decompile", part(start, "")) for start in starts]
            assert note in pages[0]["text"] and pages[1]["text"], pages
            reads = ok(owner, "metadata")["following_reads"]
            note = f"on a view of two pages {turn}"
            both = owner.call("annotation_set", {
                "address": hex(functions[0]), "text": note,
                "then": [{"operation": "decompile", "payload": part(starts[at], pages[at]["etag"])}
                         for at in order]})
            assert both["status"] == "ok", both
            assert [a["status"] for a in both["then"]] == ["ok", "ok"], both["then"]
            assert ok(owner, "metadata")["following_reads"] == reads + 2
            answers = dict(zip(order, both["then"]))
            fresh = [ok(owner, "decompile", part(start, "")) for start in starts]
            for at in (0, 1):
                assert taken(pages[at], answers[at]) == fresh[at]["text"], (turn, at, answers[at])
            # What changed of each is sent, not the page: the note's line of
            # the first, and of the second no line, only where its text begins.
            assert "delta" in answers[0]["payload"] and note in fresh[0]["text"], answers[0]
            assert any(change[1] == "text" for change in answers[0]["payload"]["delta"]), answers[0]
            assert answers[1]["payload"].get("unchanged") or all(
                change[1] != "text" for change in answers[1]["payload"]["delta"]), answers[1]
        ok(owner, "undo")
        ok(owner, "save")
        ok(owner, "undo")
        ok(owner, "save")
        # The page as it stands once those are taken back.
        followed = ok(owner, "decompile", {"address": hex(functions[0]), "representation": "source",
                                           "offset": 0, "limit": 200, "kept_only": True,
                                           "if_none_match": ""})
        assert "carried with the edit" in followed["text"], followed
        # What an edit reaches none of, its reply answers as the page stands.
        ok(owner, "save")
        reads = ok(owner, "metadata")["following_reads"]
        coloured = owner.call("function_edit", {
            "address": hex(functions[2]), "attributes": {"color": "#405062"},
            "then": [{"operation": "decompile", "payload": dict(kept_read, if_none_match=followed["etag"])}]})
        assert coloured["status"] == "ok", coloured
        assert coloured["then"][0]["status"] == "ok", coloured["then"]
        assert coloured["then"][0]["payload"]["unchanged"], coloured["then"]
        assert coloured["then"][0]["payload"]["etag"] == followed["etag"], coloured["then"]
        assert ok(owner, "metadata")["following_reads"] == reads + 1
        refused = owner.call("annotation_set", {
            "address": "no address", "text": "refused with its reads",
            "then": [{"operation": "decompile", "payload": kept_read}]})
        assert refused["status"] != "ok" and "then" not in refused, refused
        too_many = owner.call("annotation_set", {
            "address": hex(functions[0]), "text": "too many reads",
            "then": [{"operation": "decompile", "payload": kept_read}] * 5})
        assert too_many["error"]["code"] == "invalid_request", too_many
        ok(owner, "undo")
        ok(owner, "save")
        ok(owner, "undo")
        ok(owner, "save")
        ok(owner, "undo")
        ok(owner, "save")
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
        # A function's declaration reaches the documents that read it and no
        # other: nothing calls the third function, and the others answer as they
        # were.  Its own reads as a new reader's does.
        reads = store_reads(owner)
        ok(owner, "function_edit", {"address": hex(functions[2]),
                                    "attributes": {"type": "long long declared_here(long long value)"}})
        for address in (functions[0], functions[1], functions[3]):
            assert view(owner, address) == noted[address], hex(address)
        assert store_reads(owner) == reads, (store_reads(owner), reads)
        compare(executable, owner, path, functions, "a declaration one view reads")
        ok(owner, "undo")
        assert same_as_fresh("declaration undone") == noted
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
        renames = ok(owner, "metadata")["named_view_name_patches"]
        ok(owner, "rename", {"address": hex(functions[2]), "name": "after_many"})
        assert isinstance(view(owner, functions[0]), dict)
        assert store_reads(owner) == reads, (store_reads(owner), reads)
        # The view that reads the name has it written into a copy of its
        # document, and reads as a new reader's does.
        in_place = same_as_fresh("a name in place")
        assert "after_many" in in_place[functions[2]]["text"], in_place[functions[2]]
        assert ok(owner, "metadata")["named_view_name_patches"] > renames
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
        again_views = compare(executable, owner, path, functions, "declaration then type")

        # A client that keeps a view's pages names each for what an edit changed
        # of it, and the patch that wrote the edit into the view's document says
        # that itself: a comment's line and row, a name's spelling, and the
        # spans either moved, whole or page by page.  Applied, each page is the
        # one a client that keeps none reads.
        def kept_pages(limit):
            return {address: pages_of(owner, address, limit) for address in functions}

        def read_again(kept, limit, step):
            for address in functions:
                again = pages_of(owner, address, limit, kept[address])
                afresh = pages_of(owner, address, limit)
                assert again == afresh, (step, limit, hex(address), again, afresh)
            return kept_pages(limit)

        for limit in (200, 3):
            kept = kept_pages(limit)
            assert limit == 200 or max(len(pages) for pages in kept.values()) > 2, kept
            before = said(owner)
            ok(owner, "annotation_set", {"address": hex(functions[0]), "text": "said of its page"})
            ok(owner, "save")
            kept = read_again(kept, limit, "a comment")
            ok(owner, "annotation_set", {"address": inner, "text": "said of a line, a longer one"})
            ok(owner, "save")
            kept = read_again(kept, limit, "a line's comment")
            ok(owner, "annotation_set", {"address": inner, "text": ""})
            ok(owner, "save")
            kept = read_again(kept, limit, "a comment taken away")
            ok(owner, "rename", {"address": hex(functions[1]), "name": "a_much_longer_function_name"})
            kept = read_again(kept, limit, "a longer name")
            ok(owner, "rename", {"address": hex(functions[1]), "name": "f"})
            kept = read_again(kept, limit, "a shorter name")
            assert said(owner)[0] > before[0], (said(owner), before)
            # Two edits with no read between them: the page a client keeps is of
            # no document a patch was made of, and is compared or read whole.
            ok(owner, "annotation_set", {"address": hex(functions[0]), "text": "the first of two"})
            ok(owner, "save")
            ok(owner, "annotation_set", {"address": hex(functions[0]), "text": "the second of two"})
            ok(owner, "save")
            kept = read_again(kept, limit, "two edits")
            # Carried by the edit, the read finds its document held by no one
            # else, and the patch is written into it: the other pages of the
            # document it was read what the patch says too.
            before = said(owner)
            pages = kept[functions[0]]
            carried = owner.call("annotation_set", {
                "address": hex(functions[0]), "text": "written into its document",
                "then": [{"operation": "decompile",
                          "payload": {"address": hex(functions[0]), "representation": "source",
                                      "offset": 0, "limit": limit, "kept_only": True,
                                      "if_none_match": pages[0]["etag"], "page_delta": True}}]})
            assert carried["status"] == "ok" and carried["then"][0]["status"] == "ok", carried
            assert "delta" in carried["then"][0]["payload"], carried
            first_page = apply_delta(pages[0], carried["then"][0]["payload"])
            ok(owner, "save")
            after = said(owner)
            assert after[0] > before[0], (after, before)
            assert after[1] == before[1] + (0 if CHECK else 1), (after, before)
            again = [first_page] + pages_of(owner, functions[0], limit, pages)[1:]
            assert again == pages_of(owner, functions[0], limit), (limit, again)
            # The pages after the first named the document the patch was written
            # into, which holds them no more: they read what it said.
            assert len(pages) == 1 or said(owner)[0] >= after[0] + len(pages) - 1, (
                said(owner), after, len(pages))
            for _ in range(8):
                ok(owner, "undo")
                ok(owner, "save")
            assert same_as_fresh("pages said undone") == again_views
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
        # A snapshot given before the owner's write reached the disk: the
        # replica takes a declaration and a name as the owner took them, reads
        # a new type from the file once it is there, and reads as a replica
        # restored once the files hold them.
        ok(owner, "function_edit", {"address": hex(functions[2]),
                                    "attributes": {"type": "long long replica_name(long long value)"}})
        ok(owner, "rename", {"address": hex(functions[1]), "name": "in_memory_name"})
        ok(owner, "local_types_declare", {"language": "c", "source": "struct IN_MEMORY { unsigned value; };"})
        snapshot = ok(owner, "analysis_snapshot", {"in_memory": True})
        snapshot["written"] = False
        assert ok(replica, "analysis_update_names", snapshot)["updated"]
        taken = {address: replica_view(replica, address) for address in functions}
        assert "in_memory_name" in taken[functions[1]]["text"], taken[functions[1]]
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
        patches = ok(owner, "metadata")["named_view_data_name_patches"]
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "renamed_global"})
        after = compare(executable, owner, path, functions, "data name")
        for address in reader:
            assert "renamed_global" in after[address]["text"], after[address]["text"]
        # The document that read it takes the name as respelling the engine text
        # its decoration read gives it, and again from the text that gave.
        assert ok(owner, "metadata")["named_view_data_name_patches"] > patches
        patches = ok(owner, "metadata")["named_view_data_name_patches"]
        ok(owner, "rename", {"address": hex(GLOBAL), "name": "renamed_twice"})
        after = compare(executable, owner, path, functions, "data name twice")
        for address in reader:
            assert "renamed_twice" in after[address]["text"], after[address]["text"]
        assert ok(owner, "metadata")["named_view_data_name_patches"] > patches
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
        # Undone step by step, and redone, each reads as a new reader of the
        # project does: a local's name, the data items and the names of data,
        # back to the first views and forward again.
        local = next(name for name in view(owner, entry)["code_names"] if name["kind"] == "local")
        ok(owner, "code_edit", {"address": hex(entry), "representation": "source", "kind": "name",
                                "original": local["original"], "identity": local["identity"],
                                "name": "undone_local"})
        named = compare(executable, owner, path, functions, "a local's name")
        assert "undone_local" in named[entry]["text"], named[entry]
        steps = len(ok(owner, "history", {"offset": 0, "limit": 64})["items"])
        assert steps == 7, steps

        def same_data(step):
            # The data the items and names lie in reads as a new reader's too.
            request = {"address": hex(GLOBAL + 0x10), "before": 4, "after": 12}
            mine = ok(owner, "listing", request)["lines"]
            with Client(executable) as fresh:
                ok(fresh, "open", {"path": str(path), "analysis": False, "warm": False,
                                   "read_only": True})
                theirs = ok(fresh, "listing", request)["lines"]
            assert mine == theirs, (step, mine, theirs)

        same_data("a local's name")
        for step in range(steps):
            ok(owner, "undo")
            undone = compare(executable, owner, path, functions, f"undo {step}")
            same_data(f"undo {step}")
        assert undone == before, (undone, before)
        for step in range(steps):
            ok(owner, "redo")
            redone = compare(executable, owner, path, functions, f"redo {step}")
            same_data(f"redo {step}")
        assert redone == named, (redone, named)
        # A data's new name, read by a client that keeps the pages of the view
        # that reads it: its spellings, its rows' anchors, its variable and the
        # identity of every local, as the patch says them; and carried by the
        # rename, written into the document it was read from.
        for limit in (200, 3):
            kept = pages_of(owner, entry, limit)
            before = said(owner)
            ok(owner, "rename", {"address": hex(GLOBAL), "name": f"said_global_{limit}"})
            again = pages_of(owner, entry, limit, kept)
            assert again == pages_of(owner, entry, limit), (limit, again)
            assert any(f"said_global_{limit}" in page["text"] for page in again), again
            assert said(owner)[0] > before[0], (said(owner), before)
            before = said(owner)
            carried = owner.call("rename", {
                "address": hex(GLOBAL), "name": f"a_longer_carried_global_{limit}",
                "then": [{"operation": "decompile",
                          "payload": {"address": hex(entry), "representation": "source",
                                      "offset": 0, "limit": limit, "kept_only": True,
                                      "if_none_match": again[0]["etag"], "page_delta": True}}]})
            assert carried["status"] == "ok" and carried["then"][0]["status"] == "ok", carried
            assert "delta" in carried["then"][0]["payload"], carried
            after = said(owner)
            assert after[0] > before[0], (after, before)
            assert after[1] == before[1] + (0 if CHECK else 1), (after, before)
            pages = [apply_delta(again[0], carried["then"][0]["payload"])]
            pages += pages_of(owner, entry, limit, again)[1:]
            assert pages == pages_of(owner, entry, limit), (limit, pages)
            compare(executable, owner, path, functions, f"data names said, {limit} lines a page")
    print("what an owner keeps of a view through an edit reads as a new reader of the project does")


if __name__ == "__main__":
    run(sys.argv[1])
