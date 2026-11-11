#!/usr/bin/env python3
"""Edits that change names read them again without a full listing build.

A rename, a data item, an undo and a comment let the listing read names and
items again over its last string scan, or not rebuild at all, and a function's
new name reads nothing again.  After each edit, every page must match what a
full rebuild (reload) shows.

Reads a binary file first -- one segment, no symbols, a function only its
call names and a string only its reference does -- then builds a benign glibc
program with the host C compiler and strips it, as distributions ship
programs.  Both are analyzed, never executed.  Exits 77 (skip) after the
binary file without a compiler and strip that build the program.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from transport_test import Client

SOURCE = r"""#include <stdio.h>
#include <stdlib.h>

static const char *pname;
static int calls;

static void report(const char *what) {
  ++calls;
  fprintf(stderr, "%s: %s (%d)\n", pname, what, calls);
}

int main(int argc, char **argv) {
  pname = argv[0];
  if (argc > 2)
    report("too many arguments");
  puts(argc > 1 ? argv[1] : "no argument given");
  return calls;
}
"""


# x86-64 code a binary file holds at BINARY_BASE: start calls add_seven at
# +0x10 and points at the text at +0x18.
BINARY_BASE = 0x400000
BINARY_CODE = bytes.fromhex(
    "e80b000000"  # call add_seven
    "488d3d0c000000"  # lea rdi, [rip + text]
    "c3909090"  # ret, padding
    "8d4707c3"  # add_seven: lea eax, [rdi + 7]; ret
    "00000000") + b"binary file text\0"


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def snapshot(client, anchors):
    """Every page an edit can change: listing pages and the tables."""
    pages = {}
    for anchor in anchors:
        pages[anchor] = ok(client, "listing",
                           {"address": anchor, "before": 40, "after": 120})["lines"]
    for table in ("functions", "names", "strings"):
        pages[table] = ok(client, table, {"offset": 0, "limit": 512})["items"]
    return pages


def same_as_full_rebuild(client, anchors, step, names=()):
    light = snapshot(client, anchors)
    found = {name: resolves(client, name) for name in names}
    ok(client, "reload")
    full = snapshot(client, anchors)
    for name, address in found.items():
        assert resolves(client, name) == address, f"{step}: {name} led to {address}"
    for key in light:
        if light[key] != full[key]:
            for index, (seen, rebuilt) in enumerate(zip(light[key], full[key])):
                if seen != rebuilt:
                    raise AssertionError(f"{step}: {key} differs at {index}: {seen} != {rebuilt}")
            raise AssertionError(f"{step}: {key} has {len(light[key])} rows, not {len(full[key])}")


def name_builds(client):
    return ok(client, "metadata")["listing_name_builds"]


def resolves(client, name):
    reply = client.call("resolve", {"query": name})
    return reply["payload"]["address"] if reply["status"] == "ok" else None


def graph_rows(client, address):
    metrics = {"char_width": 8, "line_height": 16}
    summary = ok(client, "cfg_summary", {"address": address, "metrics": metrics})
    return str(ok(client, "cfg_viewport", {
        "address": address, "layout_revision": summary["layout_revision"],
        "x": -1000000, "y": -1000000, "width": 2000000, "height": 2000000,
        "scale": 1, "metrics": metrics})["nodes"])


def rename_in_place(client, anchors, address, name, other, previous=None):
    """A function's new name changes what the listing shows of it and nothing
    it has read, and what shows it reads it at once."""
    snapshot(client, anchors)
    graph_rows(client, address)
    builds = name_builds(client)
    ok(client, "rename", {"address": address, "name": name})
    assert resolves(client, name) == address, name
    assert previous is None or resolves(client, previous) != address, previous
    # A name leads to one address: another function cannot take it.
    taken = client.call("rename", {"address": other, "name": name})
    assert taken["status"] != "ok", taken
    assert "is used at" in taken["error"]["message"], taken
    light = snapshot(client, anchors)
    assert name in str(light["functions"]), light["functions"]
    assert name in graph_rows(client, address), "a graph kept the old name"
    assert name_builds(client) == builds, "a function's new name read the names again"
    return light


def exported_function(executable, directory):
    """An export keeps finding a function under the name it exports it by."""
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler or not sys.platform.startswith("linux"):
        return
    source = Path(directory) / "exports.c"
    source.write_text("int exported_value(int x) { return x + 1; }\n"
                      "int other_value(int x) { return x + 2; }\n", encoding="ascii")
    library = Path(directory) / "libexports.so"
    built = subprocess.run([compiler, "-O1", "-shared", "-fPIC", str(source), "-o", str(library)],
                           capture_output=True, text=True)
    if built.returncode:
        return
    client = Client(executable)
    try:
        ok(client, "open", {"path": str(library)})
        address = resolves(client, "exported_value")
        other = resolves(client, "other_value")
        assert address and other, (address, other)
        anchors = [address, other]
        # The exports list reads the image's own names, in each order asked
        # for and as filtered, whatever the session calls the functions.
        exports = {"offset": 0, "limit": 100, "sort": "name"}
        listed = ok(client, "exports", exports)
        names = [row["name"] for row in listed["items"]]
        assert {"exported_value", "other_value"} <= set(names), listed
        assert names == sorted(names, key=str.lower), names
        down = ok(client, "exports", dict(exports, descending=True))
        downward = [row["name"] for row in down["items"]]
        assert sorted(downward) == sorted(names), (downward, names)
        assert downward == sorted(downward, key=str.lower, reverse=True), downward
        other_only = ok(client, "exports", dict(exports, filter="OTHER_V"))
        assert [row["name"] for row in other_only["items"]] == ["other_value"], other_only
        ok(client, "xrefs", {"address": address})
        def exported_name_taken():
            taken = client.call("rename", {"address": other, "name": "exported_value"})
            assert taken["status"] != "ok", taken
            assert "is used at" in taken["error"]["message"], taken

        rename_in_place(client, anchors, address, "renamed_export", other)
        assert ok(client, "exports", exports) == listed
        assert ok(client, "exports", dict(exports, descending=True)) == down
        assert resolves(client, "exported_value") == address
        exported_name_taken()
        same_as_full_rebuild(client, anchors, "exported function rename")
        assert resolves(client, "renamed_export") == address
        assert resolves(client, "exported_value") == address
        exported_name_taken()
        rename_in_place(client, anchors, address, "exported_value", other)
        same_as_full_rebuild(client, anchors, "exported function named as exported")
        assert resolves(client, "exported_value") == address
        assert resolves(client, "renamed_export") is None
    finally:
        client.close()


def binary_file(executable, directory):
    blob = Path(directory) / "blob.bin"
    blob.write_bytes(BINARY_CODE)
    start, callee, text = (hex(BINARY_BASE + offset) for offset in (0, 0x10, 0x18))
    client = Client(executable)
    try:
        ok(client, "open", {"path": str(blob), "loader": "binary", "processor": "x86_64",
                            "base": hex(BINARY_BASE)})
        ok(client, "xrefs", {"address": text})
        anchors = [start, callee, text]
        same_as_full_rebuild(client, anchors, "binary file open")
        # A first name in the padding between functions stops what decodes there.
        padding = hex(BINARY_BASE + 0xd)
        spotted = anchors + [padding]
        ok(client, "rename", {"address": padding, "name": "in_the_padding"})
        same_as_full_rebuild(client, spotted, "binary file name in padding", ("in_the_padding",))
        ok(client, "undo")
        same_as_full_rebuild(client, spotted, "binary file name in padding undone")
        rename_in_place(client, anchors, callee, "add_seven", start)
        view = ok(client, "decompile", {"address": start, "representation": "source",
                                        "offset": 0, "limit": 200})
        assert "add_seven" in view["text"], view
        same_as_full_rebuild(client, anchors, "binary file function rename")
        view = ok(client, "decompile", {"address": start, "representation": "source",
                                        "offset": 0, "limit": 200})
        assert "add_seven" in view["text"], view
        rename_in_place(client, anchors, callee, "add_seven_again", start, "add_seven")
        same_as_full_rebuild(client, anchors, "binary file second name")
        ok(client, "undo")
        same_as_full_rebuild(client, anchors, "binary file undone name")
        # The name the listing gives the entry gives way to the user's, and no
        # longer finds it.
        assert resolves(client, "start") == start
        ok(client, "rename", {"address": start, "name": "named_start"})
        assert resolves(client, "named_start") == start
        assert resolves(client, "start") is None
        same_as_full_rebuild(client, anchors, "binary file entry rename")
        assert resolves(client, "start") is None
        ok(client, "undo")
        assert resolves(client, "start") == start
        same_as_full_rebuild(client, anchors, "binary file entry undone")
        # A name leads to one address: another function cannot take it.
        taken = client.call("rename", {"address": start, "name": "add_seven"})
        assert taken["status"] != "ok", taken
        assert f"is used at {callee}" in taken["error"]["message"], taken
        ok(client, "rename", {"address": text, "name": "label_text"})
        same_as_full_rebuild(client, anchors, "binary file string rename")
        ok(client, "item_define", {"address": text, "action": "data", "size": 4})
        same_as_full_rebuild(client, anchors, "binary file string made data")
        ok(client, "undo")
        same_as_full_rebuild(client, anchors, "binary file undo")
    finally:
        client.close()


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-names-rebuild-") as directory:
        binary_file(executable, directory)
        exported_function(executable, directory)
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    strip = shutil.which("strip")
    if not compiler or not strip or not sys.platform.startswith("linux"):
        print("SKIP: needs a host C compiler and strip that build glibc programs")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-names-rebuild-") as directory:
        source = Path(directory) / "report.c"
        source.write_text(SOURCE, encoding="ascii")
        binary = Path(directory) / "report"
        built = subprocess.run([compiler, "-O1", "-fPIE", "-pie", str(source), "-o", str(binary)],
                               capture_output=True, text=True)
        if built.returncode or subprocess.run([strip, str(binary)]).returncode:
            print("SKIP: the host compiler cannot build the program:", built.stderr.strip())
            return 77
        client = Client(executable)
        try:
            ok(client, "open", {"path": str(binary)})
            main = ok(client, "resolve", {"query": "main"})["address"]
            # The reference index names data by its accesses.
            ok(client, "xrefs", {"address": main})
            strings = ok(client, "strings", {"offset": 0, "limit": 512})["items"]
            text = next(row for row in strings if row["text"] == "no argument given")
            # The program's own data the code accesses, which only its
            # automatic name names: beside the copy of stderr in .bss.
            stderr = client.call("resolve", {"query": "stderr"})
            if stderr["status"] != "ok":
                print("SKIP: the program reaches stderr through the GOT, not a copy in .bss")
                return 77
            bss = ok(client, "listing", {"address": stderr["payload"]["address"], "before": 0,
                                          "after": 40})["lines"]
            data = next(line["address"] for line in bss
                        if line["text"].startswith(("qword_", "dword_")))
            anchors = [main, text["address"], data]
            same_as_full_rebuild(client, anchors, "open")
            # The name only the listing gives it no longer finds it.
            assert resolves(client, "main") == main
            ok(client, "rename", {"address": main, "name": "report_main"})
            same_as_full_rebuild(client, anchors, "function rename", ("main", "report_main"))
            # A first name of data, and a name again, read nothing again.
            builds = name_builds(client)
            ok(client, "rename", {"address": data, "name": "program_first"})
            assert resolves(client, "program_first") == data
            ok(client, "rename", {"address": data, "name": "program_state"})
            assert resolves(client, "program_state") == data
            assert resolves(client, "program_first") is None
            # What takes either name leads to it, and the name given up is free.
            taken = client.call("rename", {"address": main, "name": "program_state"})
            assert taken["status"] != "ok" and "is used at" in taken["error"]["message"], taken
            light = snapshot(client, anchors)
            assert "program_state" in str(light[data]), light[data]
            assert name_builds(client) == builds, "a name of data read the names again"
            same_as_full_rebuild(client, anchors, "data rename", ("program_state", "program_first"))
            # A first name among loose bytes and in an unwritten run stops them there,
            # as a data item does and no longer does once it is gone.
            regions = {row["name"]: int(row.get("start") or row["address"], 16)
                       for row in ok(client, "regions", {})["items"]}
            loose = hex(regions[".data"] + 3)
            lines = ok(client, "listing", {"address": data, "before": 12, "after": 12})["lines"]
            aligned = next(int(line["address"], 16) for line in lines
                           if line["kind"] == "data" and line["text"].split()[:1] == ["align"])
            unwritten = hex(aligned + 2)
            spots = [loose, unwritten]
            spotted = anchors + spots
            snapshot(client, spotted)
            builds = name_builds(client)
            ok(client, "rename", {"address": loose, "name": "program_loose"})
            light = snapshot(client, spotted)
            assert name_builds(client) == builds, f"a first name at {loose} read the names again"
            ok(client, "rename", {"address": unwritten, "name": "program_unwritten"})
            light = snapshot(client, spotted)
            assert name_builds(client) == builds, f"a first name at {unwritten} read the names again"
            same_as_full_rebuild(client, spotted, "names among bytes",
                                 ("program_loose", "program_unwritten"))
            ok(client, "undo")
            ok(client, "undo")
            same_as_full_rebuild(client, spotted, "names among bytes undone")
            snapshot(client, spotted)
            builds = name_builds(client)
            ok(client, "item_define", {"address": hex(regions[".data"] + 2), "action": "data", "size": 2})
            ok(client, "item_define", {"address": unwritten, "action": "data", "size": 2})
            light = snapshot(client, spotted)
            assert name_builds(client) == builds, "a data item among bytes read the names again"
            same_as_full_rebuild(client, spotted, "items among bytes")
            snapshot(client, spotted)
            builds = name_builds(client)
            ok(client, "item_define", {"address": hex(regions[".data"] + 2), "action": "undefine"})
            ok(client, "item_define", {"address": unwritten, "action": "undefine"})
            light = snapshot(client, spotted)
            assert name_builds(client) == builds, "a data item going read the names again"
            same_as_full_rebuild(client, spotted, "items among bytes gone")
            for _ in range(4):
                ok(client, "undo")
            same_as_full_rebuild(client, spotted, "items among bytes undone")
            # And in the padding code's gaps align.
            code = ok(client, "listing", {"address": hex(regions[".text"]), "before": 0, "after": 200})["lines"]
            padding = next((int(line["address"], 16) for line in code
                            if line["text"].split()[:1] == ["align"] and int(line["item_size"], 16) > 2), None)
            assert padding is not None, "the program's code has no padding"
            spotted = anchors + [hex(padding)]
            ok(client, "rename", {"address": hex(padding + 1), "name": "program_padding"})
            same_as_full_rebuild(client, spotted, "name in padding", ("program_padding",))
            ok(client, "undo")
            same_as_full_rebuild(client, spotted, "name in padding undone")
            # A name inside an item ends it: the item is laid out again.
            builds = name_builds(client)
            inner = hex(int(data, 16) + 2)
            ok(client, "rename", {"address": inner, "name": "program_inner"})
            light = snapshot(client, anchors)
            assert name_builds(client) == builds + 1, "a name inside an item stayed in place"
            same_as_full_rebuild(client, anchors, "name inside an item", ("program_inner",))
            ok(client, "undo")
            same_as_full_rebuild(client, anchors, "name inside an item undone")
            # A string's first name moves the numbers of those named after it.
            builds = name_builds(client)
            ok(client, "rename", {"address": text["address"], "name": "usage_text"})
            light = snapshot(client, anchors)
            assert name_builds(client) == builds + 1, "a string's first name stayed in place"
            same_as_full_rebuild(client, anchors, "string rename")
            # A data item over bytes no string holds reads nothing else again,
            # and neither does it going, or one that stops a neighbour.
            builds = name_builds(client)
            ok(client, "item_define", {"address": data, "action": "data", "size": 4})
            light = snapshot(client, anchors)
            assert name_builds(client) == builds, "a data item read the names again"
            same_as_full_rebuild(client, anchors, "data item")
            builds = name_builds(client)
            ok(client, "item_define", {"address": hex(int(data, 16) + 4), "action": "data", "size": 2})
            ok(client, "item_define", {"address": data, "action": "undefine"})
            light = snapshot(client, anchors)
            assert name_builds(client) == builds, "a data item going read the names again"
            same_as_full_rebuild(client, anchors, "data item gone")
            ok(client, "undo")
            ok(client, "undo")
            same_as_full_rebuild(client, anchors, "data items undone")
            # One over a string's bytes takes the string with it: names read again.
            builds = name_builds(client)
            ok(client, "item_define", {"address": text["address"], "action": "data", "size": 4})
            light = snapshot(client, anchors)
            assert name_builds(client) == builds + 1, "a string went without its names"
            same_as_full_rebuild(client, anchors, "string made data")
            ok(client, "undo")
            same_as_full_rebuild(client, anchors, "undo")
            ok(client, "annotation_set", {"address": main, "text": "entry logic"})
            ok(client, "save")
            same_as_full_rebuild(client, anchors, "comment")
        finally:
            client.close()
    print("names rebuild: renames, a data item, undo and a comment show as a full rebuild does,"
          " in a binary file and a program")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
