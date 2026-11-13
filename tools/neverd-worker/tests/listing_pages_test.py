#!/usr/bin/env python3
"""A client that keeps listing pages is told when one stands, or what changed of it.

A read that names no page (if_none_match absent) reads as it always has.  One that
names the page it keeps is answered that it stands, or, asking for what changed
(page_delta), with the changes the page takes; applied, they are the page a read
naming none gets.  A page this worker did not give reads whole.
"""
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).parent))
from source_variable_types_test import global_elf
from transport_test import Client

ENTRY, GLOBAL = 0x4000B0, 0x401000


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def applied(page, reply):
    """The page \\p reply's changes leave of \\p page, as the workbench applies them."""
    page = dict(page)
    for operation in reply["delta"]:
        if operation[0] == "set":
            page[operation[1]] = operation[2]
            continue
        assert operation[0] == "items", operation
        old, new, used = page[operation[1]], [], 0
        for run in operation[2]:
            how, value = run[0], run[1]
            if how == "same":
                assert value > 0 and used + value <= len(old), run
                new += old[used:used + value]
                used += value
            elif how == "skip":
                assert value > 0 and used + value <= len(old), run
                used += value
            elif how == "put":
                assert used + len(value) <= len(old), run
                new += value
                used += len(value)
            elif how == "new":
                new += value
            else:
                raise AssertionError(run)
        assert used == len(old), (used, len(old))
        page[operation[1]] = new
    page["etag"] = reply["etag"]
    return page


def without_name(page):
    return {key: value for key, value in page.items() if key != "etag"}


def run(executable):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "global.elf"
        image, entry = global_elf()
        assert entry == ENTRY, hex(entry)
        path.write_bytes(image)
        client = Client(executable)
        try:
            ok(client, "open", {"path": str(path), "analysis": False, "warm": False,
                                "write_behind": True})
            # The data's own line and the lines around it.
            window = {"address": hex(GLOBAL), "before": 8, "after": 24}
            plain = ok(client, "listing", window)
            assert "etag" not in plain, plain
            page = ok(client, "listing", dict(window, if_none_match=""))
            assert page["etag"] and page["lines"] == plain["lines"], page
            metadata = ok(client, "metadata")
            standing = metadata["listing_pages_unchanged"]
            changes = metadata["listing_page_deltas"]
            # The same lines read the same: the page stands, under its name.
            again = ok(client, "listing", dict(window, if_none_match=page["etag"]))
            assert again == {"etag": page["etag"], "unchanged": True}, again
            assert ok(client, "metadata")["listing_pages_unchanged"] == standing + 1
            # A name the window shows: what changed of the page, which the page
            # takes into the page a read naming none gets.
            ok(client, "rename", {"address": hex(GLOBAL), "name": "listing_pages_value"})
            changed = ok(client, "listing", dict(window, if_none_match=page["etag"],
                                                 page_delta=True))
            assert changed["base"] == page["etag"] and changed["etag"] != page["etag"], changed
            assert "lines" not in changed, changed
            followed = applied(page, changed)
            whole = ok(client, "listing", dict(window, if_none_match=""))
            assert without_name(followed) == without_name(whole), (followed, whole)
            assert any("listing_pages_value" in json.dumps(line) for line in followed["lines"]), followed
            assert len(json.dumps(changed)) * 4 < len(json.dumps(whole)), changed
            assert ok(client, "metadata")["listing_page_deltas"] == changes + 1
            # Without asking for what changed, a changed page reads whole.
            ok(client, "rename", {"address": hex(GLOBAL), "name": "listing_pages_again"})
            again = ok(client, "listing", dict(window, if_none_match=followed["etag"]))
            assert "lines" in again and again["etag"] != followed["etag"], again
            # A page this worker never gave reads whole, under a name of its own.
            unknown = ok(client, "listing", dict(window, if_none_match="no-such-page",
                                                 page_delta=True))
            assert "lines" in unknown and unknown["etag"] != "no-such-page", unknown
            assert without_name(unknown) == without_name(again), (unknown, again)
            # Two names a stretch apart, read at once: the lines between them
            # stand, and only the lines that changed are sent.
            base = unknown
            ok(client, "rename", {"address": hex(GLOBAL + 2), "name": "listing_pages_near"})
            ok(client, "rename", {"address": hex(GLOBAL + 12), "name": "listing_pages_far"})
            apart = ok(client, "listing", dict(window, if_none_match=base["etag"],
                                               page_delta=True))
            assert apart["base"] == base["etag"] and "lines" not in apart, apart
            whole = ok(client, "listing", dict(window, if_none_match=""))
            assert without_name(applied(base, apart)) == without_name(whole), (apart, whole)
            assert len(whole["lines"]) == len(base["lines"]), (base, whole)
            differing = sum(a != b for a, b in zip(base["lines"], whole["lines"]))
            runs = [run for operation in apart["delta"] if operation[0] == "items"
                    for run in operation[2]]
            sent = sum(len(run[1]) for run in runs if run[0] in ("put", "new"))
            assert 2 <= differing == sent, (differing, sent, apart)
            assert [run[0] for run in runs].count("put") >= 2, runs
            assert any(run[0] == "same" and 0 < i < len(runs) - 1
                       for i, run in enumerate(runs)), runs
        finally:
            client.close()


if __name__ == "__main__":
    run(Path(sys.argv[1]))
