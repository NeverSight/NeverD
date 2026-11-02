#!/usr/bin/env python3
"""The program's local types: a list, one type laid out, and source."""
from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import sys
import tempfile

from transport_test import Client


FIXTURES = Path(__file__).resolve().parents[3] / "unittests/types/fixtures"


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-local-types-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "local_types.exe"
        shutil.copyfile(FIXTURES / "local_types.exe", binary)
        shutil.copyfile(FIXTURES / "local_types.pdb", root / "local_types.pdb")

        owner = clients.enter_context(Client(executable))
        for operation in ("local_types", "local_type", "local_types_source"):
            assert operation in owner.hello["capabilities"]
        ok(owner, "open", {"path": str(binary), "analysis": False})

        # A console program also joins the user and kernel mode headers;
        # the PDB's own types are those of origin "pdb".
        listing = ok(owner, "local_types", {"filter": "device", "origin": "pdb"})
        assert listing["source"] == "pdb" and listing["dataModel"] == "LLP64"
        assert [row["name"] for row in listing["types"]] == ["_DEVICE"]
        headers = ok(owner, "local_types", {"filter": "device"})
        assert headers["types"][0]["name"] == "_DEVICE"
        assert any(row["origin"] == "header" for row in headers["types"]), headers["types"][:3]
        assert listing["types"][0]["size"] == 0x58

        # Exact widths unless asked.
        device = ok(owner, "local_type", {"name": "_DEVICE"})
        links = next(member for member in device["members"] if member["name"] == "Links")
        assert links["offset"] == 8 and links["refers"] == "_LIST_ENTRY"
        assert device["members"][0]["type"] == "uint16_t", device["members"][0]
        keywords = ok(owner, "local_type", {"name": "_DEVICE", "spelling": "keywords"})
        assert keywords["members"][0]["type"] == "unsigned short", keywords["members"][0]
        refused = owner.call("local_type", {"name": "_DEVICE", "spelling": "ida"})
        assert refused["status"] != "ok", refused

        rust = ok(owner, "local_types_source", {"language": "rust", "types": ["_PACKED"],
                                                "dependencies": False})
        assert rust["language"] == "rust" and "#[repr(C, packed(1))]" in rust["source"]
        # Python renders as ctypes, and declares nothing.
        python = ok(owner, "local_types_source", {"language": "python", "types": ["_PACKED"],
                                                  "dependencies": False})
        assert python["language"] == "python", python
        assert "class _PACKED(ctypes.Structure):" in python["source"], python
        assert '    _layout_ = "ms"\n' in python["source"], python
        unread = ok(owner, "local_types_declare", {"language": "python", "source": "X = 1"})
        assert unread["types"] == [] and "do not read" in unread["problems"][0]["message"], unread
        # JavaScript reads through a DataView, and declares nothing.
        script = ok(owner, "local_types_source", {"language": "javascript", "types": ["_PACKED"],
                                                  "dependencies": False})
        assert script["language"] == "javascript", script
        assert "export class _PACKED {" in script["source"], script
        unread = ok(owner, "local_types_declare", {"language": "javascript", "source": "export {}"})
        assert unread["types"] == [] and "do not read" in unread["problems"][0]["message"], unread
        # Lua reads through string.unpack, and declares nothing.
        lua = ok(owner, "local_types_source", {"language": "lua", "types": ["_PACKED"],
                                               "dependencies": False})
        assert lua["language"] == "lua" and 'record("_PACKED", 0x7)' in lua["source"], lua
        unread = ok(owner, "local_types_declare", {"language": "lua", "source": "return {}"})
        assert unread["types"] == [] and "do not read" in unread["problems"][0]["message"], unread
        # Prototypes render in C and C++ only, and fail elsewhere.
        refused = owner.call("local_types_source", {"language": "python", "prototypes": True})
        assert refused["status"] == "error" and "c and cpp" in refused["error"]["message"], refused

        # Declared types join the program's, and the project keeps them.
        assert "local_types_declare" in owner.hello["capabilities"]
        reply = owner.call("local_types_declare", {
            "language": "rust",
            "source": "#[repr(C)] pub struct OWNER { pub Device: *mut _DEVICE, pub Count: u32 }"})
        assert reply["status"] == "ok", reply
        declared = reply["payload"]
        assert declared["types"] == ["OWNER"] and declared["problems"] == [], declared
        # A type no prototype names leaves the decompilation and the views.
        assert declared["decompilation"] is False, declared
        assert ok(owner, "local_type", {"name": "OWNER"})["size"] == 16
        problem = ok(owner, "local_types_declare", {"language": "c", "source": "struct B {\n  NOPE x;\n};"})
        assert problem["problems"][0]["line"] == 2 and problem["problems"][0]["column"] == 3, problem
        forgot = ok(owner, "local_types_forget", {"names": ["_PACKED"]})
        assert forgot["removed"] == ["_PACKED"], forgot
        sidecar = Path(str(binary) + ".neverd/types.json")
        assert sidecar.exists()
        rows = json.loads(sidecar.read_text())
        assert [row.get("language") for row in rows] == ["rust", None], rows
        assert rows[1] == {"forget": ["_PACKED"]}
        bad = owner.call("local_types_forget", {"names": "_PACKED"})
        assert bad["status"] == "error" and bad["error"]["code"] == "invalid_request", bad
        # Each declaration and removal is a step of undo history, which takes
        # the project's file and the types back with it.
        history = ok(owner, "history")
        assert [item["kind"] for item in history["items"]][-2:] == ["types", "types"], history
        assert history["can_undo"], history
        ok(owner, "undo")
        assert [row.get("language") for row in json.loads(sidecar.read_text())] == ["rust"]
        assert [row["name"] for row in
                ok(owner, "local_types", {"filter": "packed", "origin": "pdb"})["types"]] == ["_PACKED"]
        ok(owner, "undo")
        assert json.loads(sidecar.read_text()) == []
        assert owner.call("local_type", {"name": "OWNER"})["status"] == "error"
        ok(owner, "redo")
        assert ok(owner, "local_type", {"name": "OWNER"})["size"] == 16
        ok(owner, "redo")
        assert json.loads(sidecar.read_text())[1] == {"forget": ["_PACKED"]}
        assert ok(owner, "local_types", {"filter": "packed", "origin": "pdb"})["types"] == []
        # A comment staged and unsaved keeps a declaration from committing.
        ok(owner, "annotation_set", {"address": hex(0x140001000), "text": "staged"})
        refused = owner.call("local_types_declare", {"source": "struct C { int c; };"})
        assert refused["error"]["code"] == "unsaved_changes", refused
        ok(owner, "save")
        # A prototype takes the decompilation, and every view reads anew.
        prototype = owner.call("local_types_declare", {
            "language": "c", "source": "long Probe(struct _DEVICE *Device);"})
        assert prototype["status"] == "ok", prototype
        assert prototype["payload"]["prototypes"] == ["Probe"], prototype
        assert prototype["payload"]["decompilation"] is True, prototype
        assert prototype["revision"] != reply["revision"], (prototype, reply)

        # The analysis replica reads the project's types as they were left,
        # and declares none.
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
        assert ok(replica, "local_type", {"name": "OWNER"})["size"] == 16
        assert ok(replica, "local_types", {"filter": "packed", "origin": "pdb"})["types"] == []
        assert "_PACKED" not in [row["name"] for row in
                                 ok(replica, "local_types", {"filter": "packed"})["types"]]
        refused = replica.call("local_types_declare", {"source": "struct C { int c; };"})
        assert refused["error"]["code"] == "read_only", refused

        # The records a pointer may point at, ranked by what the code does
        # through it: use() reads Device->Size through rcx, which only a
        # _DEVICE holds as a member of its width.
        assert "local_types_fit" in owner.hello["capabilities"]
        assert "operand_struct" in owner.hello["capabilities"]
        use = ok(owner, "resolve", {"query": "use"})["function_address"]
        lines = ok(owner, "listing", {"address": use, "before": 0, "after": 40})["lines"]
        read = next(line for line in lines if "[rcx+2]" in line["text"])
        fit = ok(owner, "local_types_fit", {"function": use, "address": read["address"], "limit": 3})
        assert fit["target"]["base"] == "rcx" and fit["target"]["offset"] == 2, fit
        best = fit["candidates"][0]
        assert best["name"] == "_DEVICE" and best["exact"] == 1, best
        assert best["fits"][0]["path"] == "Size" and best["fits"][0]["fit"] == "exact", best
        # A record of the Windows headers the access fits as exactly, such as
        # ACE_HEADER's AceSize, follows the program's own.
        assert best["origin"] == "pdb", best
        assert fit["total"] >= 3, fit
        assert all(c["exact"] == 0 or c["origin"] == "header"
                   for c in fit["candidates"][1:]), fit
        searched = ok(owner, "local_types_fit", {"text": "dev.lock.wait"})
        # The program's own record first; the headers' after it.
        assert searched["candidates"][0]["name"] == "_DEVICE", searched
        assert all(c["origin"] == "header" for c in searched["candidates"][1:]), searched
        assert searched["candidates"][0]["matchedMember"] == "Lock.Waiting", searched
        no_pointer = owner.call("local_types_fit", {"function": use, "address": use})
        assert no_pointer["status"] == "error", no_pointer
        assert no_pointer["error"]["code"] == "local_types_failed", no_pointer

        # T: the displacement as a structure offset into _DEVICE, kept with
        # the project and undone as one step.
        formatted = ok(owner, "operand_format", {"address": read["address"], "action": "struct",
                                                 "type": "_DEVICE"})
        assert formatted["operand"] == 1 and formatted["format"]["type"] == "_DEVICE", formatted
        line = ok(owner, "listing", {"address": read["address"], "before": 0, "after": 1})["lines"][0]
        assert "[rcx+_DEVICE.Size]" in line["text"], line
        member = next(span for span in line["spans"]
                      if line["text"].encode()[span[0]:span[0] + span[1]] == b"_DEVICE.Size")
        assert member[2] == 23, member
        # A register that points at the device's Links, as a list walk's
        # does: the bytes are the start of Links.Flink, six before the
        # register.
        shifted = ok(owner, "operand_format", {"address": read["address"], "action": "struct",
                                               "type": "_DEVICE", "delta": 6})
        assert shifted["format"]["delta"] == 6, shifted
        line = ok(owner, "listing", {"address": read["address"], "before": 0, "after": 1})["lines"][0]
        assert "[rcx+(_DEVICE.Links.Flink-6)]" in line["text"], line
        unknown = owner.call("operand_format", {"address": read["address"], "action": "struct",
                                                "type": "_NO_SUCH_TYPE"})
        assert unknown["status"] == "error" and unknown["error"]["code"] == "invalid_request", unknown
        signed = owner.call("operand_format", {"address": read["address"], "operand": 1,
                                               "action": "negate"})
        assert signed["status"] == "error", signed
        ok(owner, "undo")
        line = ok(owner, "listing", {"address": read["address"], "before": 0, "after": 1})["lines"][0]
        assert "[rcx+_DEVICE.Size]" in line["text"], line
        ok(owner, "operand_format", {"address": read["address"], "operand": 1, "action": "number"})
        line = ok(owner, "listing", {"address": read["address"], "before": 0, "after": 1})["lines"][0]
        assert "[rcx+2]" in line["text"], line

        # A number is an offset by itself: `cmp r9, 1` takes an address one
        # byte in, where no member of the program's records begins (some of
        # the Windows headers' do).
        compare = next(line for line in lines if line["text"].lstrip().startswith("cmp"))
        number = ok(owner, "local_types_fit", {"function": use, "address": compare["address"],
                                               "limit": 3})
        assert number["operand"] == 1 and "target" not in number, number
        assert number["accesses"] == [{"offset": 1, "size": 0, "use": "address"}], number
        assert len(number["candidates"]) == 3 and number["total"] > 3, number
        # Exact header matches can fill even the largest result page before
        # the program's partial matches. Search its known record explicitly;
        # do not depend on the number of declarations in the bundled headers.
        own = ok(owner, "local_types_fit", {"function": use, "address": compare["address"],
                                            "text": "_THREAD_LIKE", "limit": 3})
        assert own["operand"] == number["operand"] and "target" not in own, own
        assert own["accesses"] == number["accesses"], own
        record = next(c for c in own["candidates"] if c["name"] == "_THREAD_LIKE")
        assert record["origin"] == "pdb" and record["exact"] == 0, record
        assert record["fits"][0]["fit"] == "inside", record
        assert record["fits"][0]["path"] == "Header", record
        # Every access through a pointer at once, one step of the history.
        header = next(line for line in lines if "[r8+5]" in line["text"])
        flags = next(line for line in lines if "[rdx+8]" in line["text"])
        before = len(ok(owner, "history", {"offset": 0, "limit": 64})["items"])
        both = ok(owner, "operand_format", {"address": flags["address"], "action": "struct",
                                            "type": "_THREAD_LIKE", "also": [header["address"]]})
        assert both["also"] == 1 and both["saved"] is True, both
        shown = ok(owner, "listing", {"address": use, "before": 0, "after": 40})["lines"]
        assert any("[rdx+_THREAD_LIKE.MiscFlags]" in line["text"] for line in shown), shown
        assert any("[r8+(_THREAD_LIKE.Header+5)]" in line["text"] for line in shown), shown
        after = len(ok(owner, "history", {"offset": 0, "limit": 64})["items"])
        assert after == before + 1, (before, after)
        refused = owner.call("operand_format", {"address": flags["address"], "action": "hex",
                                                "also": [header["address"]]})
        assert refused["status"] == "error", refused
        ok(owner, "undo")
        shown = ok(owner, "listing", {"address": use, "before": 0, "after": 40})["lines"]
        assert not any("_THREAD_LIKE" in line["text"] for line in shown), shown

        # Y and Convert to struct * in the pseudocode: use's declaration,
        # where its parameters arrive, and the records the accesses through
        # its first one rank.
        assert "function_type" in owner.hello["capabilities"]
        declared = ok(owner, "function_type", {"address": use})
        # The signature prints _PACKED, which the project forgot above: its
        # declaration reads once the types hold it again.
        assert "'_PACKED' names no type" in declared["problems"][0]["message"], declared
        ok(owner, "local_types_declare", {"language": "c", "source": (
            "#pragma pack(push, 1)\nstruct _PACKED { unsigned char Tag; "
            "unsigned long Value; unsigned short Count; };\n#pragma pack(pop)")})
        declared = ok(owner, "function_type", {"address": use})
        assert declared["stated"] is False and "parameters" in declared, declared
        assert [p["location"] for p in declared["parameters"]][:2] == ["rcx", "rdx"], declared
        fitted = ok(owner, "local_types_fit", {"function": use, "parameter": 0, "limit": 2})
        assert fitted["target"]["base"] == "rcx", fitted
        assert fitted["candidates"][0]["name"] == "_DEVICE", fitted

        def decompiled():
            return ok(owner, "decompile", {"address": use, "representation": "c"})

        page = decompiled()
        locals_ = {n["original"]: n for n in page["code_names"] if n.get("kind") == "local"}
        assert locals_["Device"]["parameter"] == 0 and locals_["Thread"]["parameter"] == 1, locals_
        # The declaration Convert to struct * proposes, stated in one step.
        proposal = ok(owner, "function_type", {"address": use, "parameter": 0,
                                               "type": "_THREAD_LIKE *Device"})
        ok(owner, "function_edit", {"address": use, "attributes": {"type": proposal["declaration"]}})
        history = ok(owner, "history", {"offset": 0, "limit": 64})
        latest = next(item for item in history["items"] if item["index"] == history["cursor"] - 1)
        assert latest["kind"] == "function" and latest["after"]["type"] == proposal["declaration"], latest
        page = decompiled()
        assert "_THREAD_LIKE* Device" in page["text"] and "->Size" not in page["text"], page["text"]
        # A local keeps its name while the code keeps its locals, as when a
        # declaration renames a parameter.
        local = next(n for n in page["code_names"]
                     if n.get("kind") == "local" and "parameter" not in n)
        ok(owner, "code_edit", {"address": use, "representation": "c", "kind": "name",
                                "original": local["original"], "name": "size_bits",
                                "identity": local["identity"]})
        assert "size_bits" in decompiled()["text"]
        renamed = ok(owner, "function_type", {"address": use, "parameter": 4, "type": "int Mode"})
        ok(owner, "function_edit", {"address": use, "attributes": {"type": renamed["declaration"]}})
        page = decompiled()
        assert "Mode" in page["text"] and "size_bits" in page["text"], page["text"]
        assert page["code_edits_stale"] is False, page
        refused = owner.call("function_edit", {"address": use, "attributes": {"type": "int use(NOPE *x)"}})
        assert refused["status"] == "error", refused
        # Undo takes the declarations and the name back one step at a time.
        for _ in range(3):
            ok(owner, "undo")
        assert "Device->Size" in decompiled()["text"]

        missing = owner.call("local_type", {"name": "_NO_SUCH_TYPE"})
        assert missing["status"] == "error" and missing["error"]["code"] == "local_types_failed", missing
        malformed = owner.call("local_types_source", {"asserts": "yes"})
        assert malformed["status"] == "error" and malformed["error"]["code"] == "invalid_request", malformed

        # Shift+F11: the libraries a console program's types join.  The
        # kernel's left out and UEFI's added read the types again, and the
        # project keeps the choice outside the history.
        for operation in ("type_libraries", "type_libraries_set"):
            assert operation in owner.hello["capabilities"]

        def joined(listing):
            return [row["name"] for row in listing["libraries"] if row["joined"]]

        listing = owner.call("type_libraries", {})
        assert listing["status"] == "ok", listing
        assert joined(listing["payload"]) == ["windows-user-x64", "windows-kernel-x64"], listing
        kernel = next(row for row in listing["payload"]["libraries"]
                      if row["name"] == "windows-kernel-x64")
        assert kernel["image"] and kernel["types"] > 0 and kernel["compatible"], kernel
        assert ok(owner, "local_type", {"name": "_DRIVER_OBJECT"})["origin"] == "header"
        assert owner.call("local_type", {"name": "EFI_SYSTEM_TABLE"})["status"] == "error"
        steps = len(ok(owner, "history", {"offset": 0, "limit": 64})["items"])
        chosen = owner.call("type_libraries_set", {"added": ["uefi-x64"],
                                                   "removed": ["windows-kernel-x64"]})
        assert chosen["status"] == "ok" and chosen["revision"] != listing["revision"], chosen
        assert joined(chosen["payload"]) == ["windows-user-x64", "uefi-x64"], chosen
        assert owner.call("local_type", {"name": "_DRIVER_OBJECT"})["status"] == "error"
        assert ok(owner, "local_type", {"name": "EFI_SYSTEM_TABLE"})["origin"] == "header"
        kept = Path(str(binary) + ".neverd/libraries.json")
        assert json.loads(kept.read_text()) == {"added": ["uefi-x64"],
                                                "removed": ["windows-kernel-x64"]}
        assert len(ok(owner, "history", {"offset": 0, "limit": 64})["items"]) == steps
        # A library laid out otherwise, or one the program joins already, is
        # refused, and the choice stays.
        for added in ("linux-x64", "windows-user-x64"):
            refused = owner.call("type_libraries_set", {"added": [added]})
            assert refused["error"]["code"] == "type_libraries_failed", refused
        refused = owner.call("type_libraries_set", {"added": "uefi-x64"})
        assert refused["error"]["code"] == "invalid_request", refused
        assert json.loads(kept.read_text())["added"] == ["uefi-x64"]
        refused = replica.call("type_libraries_set", {})
        assert refused["error"]["code"] == "read_only", refused
        assert joined(ok(owner, "type_libraries_set", {})) == ["windows-user-x64",
                                                             "windows-kernel-x64"]
        assert json.loads(kept.read_text()) == {"added": [], "removed": []}
        # A choice another changed, as a pull brings it, is read once.
        assert ok(owner, "project_sync")["type_libraries"] is False
        kept.write_text('{"added": ["uefi-x64"], "removed": []}\n')
        synced = ok(owner, "project_sync")
        assert synced["type_libraries"] is True and synced["changed"] is True, synced
        assert joined(ok(owner, "type_libraries")) == ["windows-user-x64", "windows-kernel-x64",
                                                       "uefi-x64"]
        assert ok(owner, "project_sync")["type_libraries"] is False


if __name__ == "__main__":
    run(sys.argv[1])
