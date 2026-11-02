#!/usr/bin/env python3
"""Real engine variable types: field projection, history and native isolation."""
from contextlib import ExitStack
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from transport_test import Client
from source_synthesis_test import elf, ENTRY


def ok(client, operation, payload=None):
    reply = client.call(operation, payload or {})
    assert reply["status"] == "ok", reply
    return reply["payload"]


def source(client, representation="c", entry=ENTRY):
    return ok(client, "decompile", {"address": hex(entry),
              "representation": representation, "limit": 2048})


def selected(view, name):
    target = next(item for item in view["code_names"]
                  if item["original"] == name and "variable_id" in item)
    return {"address": view["address"], "representation": view["representation"],
            "kind": "type", "variable_id": target["variable_id"],
            "snapshot": target["snapshot"]}


def global_elf():
    entry, address = 0x4000B0, 0x401000
    code = bytes.fromhex("488b05") + struct.pack("<i", address - entry - 7)
    code += bytes.fromhex("8b4804 034808 89c8 c3")
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, entry,
                         64, 0, 0, 64, 56, 2, 64, 0, 0)
    text = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000,
                       176 + len(code), 176 + len(code), 4096)
    data = struct.pack("<IIQQQQQQ", 1, 6, 4096, address, address, 32, 32, 4096)
    return (header + text + data + code).ljust(4096, b"\0") + bytes(32), entry


def run(executable, compiler=None):
    with tempfile.TemporaryDirectory(prefix="neverd-variable-types-") as directory, ExitStack() as stack:
        root = Path(directory)
        binary = root / "types.elf"
        # One call, two field reads from its result, kept live in rbx. The
        # helper's real body returns its first argument unchanged.
        binary.write_bytes(elf(bytes.fromhex(
            "53 e80f000000 4889c3 8b4304 034308 5b c3 90909090 4889f8 c3")))
        owner = stack.enter_context(Client(executable))
        ok(owner, "open", {"path": str(binary), "analysis": False})
        ok(owner, "local_types_declare", {"source":
           "struct QA_CONTEXT { unsigned int State; unsigned int Length; unsigned int Flags; };"})
        native = {rep: source(owner, rep)["text"] for rep in ("low", "med", "llvm")}
        edits = {}
        for representation, name in (("c", "v4_0"), ("llvmc", "call0")):
            before = source(owner, representation)
            request = selected(before, name)
            edits[representation] = request
            for text in ("uint32_t", "double", "struct MISSING_TYPE *"):
                refused = owner.call("source_type", {**request, "type": text})
                assert refused["status"] == "error", refused
            refused = owner.call("code_edit", {**request, "snapshot": "obsolete", "type": "QA_CONTEXT *"})
            assert refused["status"] == "error" and refused["error"]["code"] == "stale_target", refused
            ok(owner, "code_edit", {**request, "type": "struct QA_CONTEXT *"})
            after = source(owner, representation)
            assert "->Length" in after["text"] and "->Flags" in after["text"], after
            assert "struct QA_CONTEXT" in after["text"], after
            assert not after["stale_variable_types"], after
            assert after["variable_snapshot"] == before["variable_snapshot"]
            # Reset, undo reset, undo edit and redo: no rendered-name identity
            # is used to recover the original value.
            ok(owner, "code_edit", {**request, "reset": True})
            assert "->Length" not in source(owner, representation)["text"]
            ok(owner, "undo")
            assert "->Length" in source(owner, representation)["text"]
            ok(owner, "undo")
            assert "->Length" not in source(owner, representation)["text"]
            ok(owner, "redo")
            assert "->Length" in source(owner, representation)["text"]
        for representation, text in native.items():
            assert source(owner, representation)["text"] == text
        saved = json.loads(Path(str(binary) + ".neverd/code.json").read_text())
        assert len(saved) == 1 and set(saved[0]["views"]) == {"c", "llvmc"}, saved
        # A replica loads the committed policy and is never allowed to edit.
        replica = stack.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
        for representation in ("c", "llvmc"):
            assert "->Length" in source(replica, representation)["text"]
        refused = replica.call("code_edit", {**edits["c"], "reset": True})
        assert refused["status"] == "error" and refused["error"]["code"] == "read_only", refused
        # A changed source policy cannot borrow an old local assertion.
        altered = ok(owner, "decompile", {"address": hex(ENTRY), "representation": "c", "limit": 2048,
                     "source_options": {"symbolic": True}})
        assert altered["stale_variable_types"] and "->Length" not in altered["text"], altered
        # Removing a named type keeps the saved assertion diagnosable. Undoing
        # the declaration edit restores both field projection and the cache.
        ok(owner, "local_types_forget", {"names": ["QA_CONTEXT"]})
        assert source(owner)["stale_variable_types"]
        ok(owner, "undo")
        assert "->Length" in source(owner)["text"]
        replica.close()
        owner.close()
        owner = stack.enter_context(Client(executable))
        ok(owner, "open", {"path": str(binary), "analysis": False})
        for representation in ("c", "llvmc"):
            assert "->Length" in source(owner, representation)["text"]
        # The same global object is typed in both C routes. Its loaded value
        # is the pointer, never the address of an image backing byte array.
        global_binary = root / "global.elf"
        image, entry = global_elf()
        global_binary.write_bytes(image)
        ok(owner, "open", {"path": str(global_binary), "analysis": False})
        ok(owner, "local_types_declare", {"source":
           "struct QA_GLOBAL { unsigned int State; unsigned int Length; unsigned int Flags; };"})
        assert selected(source(owner, entry=entry), "qword_401000")
        llvm_view = source(owner, "llvmc", entry)
        # Shared image storage intentionally has no separate scalar C
        # declaration. Its emitter-owned inventory still names the exact
        # loaded object; the backing array must never inherit its type.
        variable = next(v for v in llvm_view["source_variables"]
                        if v.get("address") == "0x401000")
        assert variable["id"] == "global:4198400" and variable["bytes"] == 8, variable
        assert not variable["ambiguous"], variable
        assert not any(n.get("variable_id") for n in llvm_view["code_names"]
                       if n.get("address") == "0x400000"), llvm_view
        request = {"address": llvm_view["address"], "representation": "llvmc", "kind": "type",
                   "variable_id": variable["id"], "snapshot": llvm_view["variable_snapshot"]}
        global_native = {rep: source(owner, rep, entry)["text"] for rep in ("low", "med", "llvm")}
        ok(owner, "code_edit", {**request, "type": "struct QA_GLOBAL *"})
        for representation in ("c", "llvmc"):
            after = source(owner, representation, entry)
            assert "->Length" in after["text"] and "->Flags" in after["text"], after
            if representation == "llvmc":
                assert "extern uint8_t unk_400000[4128]" in after["text"], after
                assert "uint64_t qword_401000;" not in after["text"], after
                assert "(uintptr_t)unk_400000)->" not in after["text"], after
            if compiler:
                # The provider updates the pointer cell between calls. Both
                # projections must read the new value, not the cell's address
                # or a duplicated global disconnected from the image bytes.
                storage = ("uint8_t unk_400000[4128];" if representation == "llvmc" else "")
                cell = "unk_400000 + 4096" if representation == "llvmc" else "&qword_401000"
                harness = "\n#include <string.h>\n" + storage + "\n" + r'''
int main(void) {
    uint32_t first[3] = {0, 7, 9}, second[3] = {0, 11, 13};
    uintptr_t pointer = (uintptr_t)first;
    memcpy(CELL, &pointer, sizeof(pointer));
    if (start() != 16) return 1;
    pointer = (uintptr_t)second;
    memcpy(CELL, &pointer, sizeof(pointer));
    return start() != 24;
}
'''
                harness = harness.replace("CELL", cell)
                for level in ("O0", "O2"):
                    c_file = root / ("global-" + representation + "-" + level + ".c")
                    program = c_file.with_suffix(".exe")
                    c_file.write_text(after["text"] + harness)
                    if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                        command = [compiler, "/nologo", "/TC", "/std:c11", "/Od" if level == "O0" else "/O2",
                                   str(c_file), "/Fe:" + str(program)]
                    else:
                        command = [compiler, "-std=c11", "-fno-strict-aliasing", "-" + level,
                                   str(c_file), "-o", str(program)]
                    built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
                    assert built.returncode == 0, built.stdout + built.stderr + after["text"]
                    subprocess.run([str(program)], cwd=root, check=True, timeout=10)
        for representation, text in global_native.items():
            assert source(owner, representation, entry)["text"] == text
        ok(owner, "undo")
        for representation in ("c", "llvmc"):
            assert "->Length" not in source(owner, representation, entry)["text"]
        ok(owner, "redo")
        assert "->Length" in source(owner, entry=entry)["text"]
        # Transplant a project's type/code tables onto another image with the
        # same addresses and instruction graph. A data-only file change must
        # not let the old global pointer assertion bind to that new image.
        foreign = root / "foreign.elf"
        changed_image = bytearray(global_binary.read_bytes())
        changed_image[-1] = 1
        foreign.write_bytes(changed_image)
        project = Path(str(foreign) + ".neverd")
        project.mkdir()
        for table in ("code.json", "types.json"):
            project.joinpath(table).write_bytes(Path(str(global_binary) + ".neverd", table).read_bytes())
        ok(owner, "open", {"path": str(foreign), "analysis": False})
        for representation in ("c", "llvmc"):
            view = source(owner, representation, entry)
            assert "->Length" not in view["text"] and view["stale_variable_types"], view
        typed_call = root / "typed-call.elf"
        typed_call.write_bytes(binary.read_bytes())
        ok(owner, "open", {"path": str(typed_call), "analysis": False})
        ok(owner, "analyze")
        # Discovery finds the helper the entry calls.
        assert ok(owner, "resolve", {"query": hex(ENTRY + 21)})["function_address"] == hex(ENTRY + 21)
        ok(owner, "local_types_declare", {"source":
            "struct QA_PREVIOUS { unsigned int OldState; unsigned int OldLength; unsigned int OldFlags; };"
            "struct QA_CHOSEN { unsigned int State; unsigned int Length; unsigned int Flags; };"})
        ok(owner, "function_edit", {"address": hex(ENTRY + 21),
                                    "attributes": {"type": "struct QA_PREVIOUS *helper(uint64_t input)"}})
        for representation in ("c", "llvmc"):
            before = source(owner, representation)
            if representation == "c":
                assert "->OldLength" in before["text"], before["text"]
            names = {n["original"] for n in before["code_names"] if "variable_id" in n}
            variable = next(v for v in before["source_variables"]
                            if v["bytes"] == 8 and not v["parameter"] and v["name"] in names)
            request = selected(before, variable["name"])
            ok(owner, "code_edit", {**request, "type": "struct QA_CHOSEN *"})
            after = source(owner, representation)["text"]
            assert "->Length" in after and "->Flags" in after and "->OldLength" not in after, after
        phi = root / "phi.elf"
        phi.write_bytes(elf(bytes.fromhex("4889f8 85f6 7404 4883c010 8b4804 034808 89c8 c3")))
        ok(owner, "open", {"path": str(phi), "analysis": False})
        ok(owner, "local_types_declare", {"source":
           "struct QA_CONTEXT { unsigned int State; unsigned int Length; unsigned int Flags; };"})
        harness = r'''
#include <string.h>
int main(void) {
    _Alignas(8) uint8_t data[64] = {0};
    uint32_t a = 7, b = 9, c = 11, d = 13;
    memcpy(data + 4, &a, 4); memcpy(data + 8, &b, 4);
    memcpy(data + 20, &c, 4); memcpy(data + 24, &d, 4);
    return start((uintptr_t)data, 0) != 16 || start((uintptr_t)data, 1) != 24;
}
'''
        for representation, name in (("c", "v0"), ("llvmc", "RAX_2_00")):
            request = selected(source(owner, representation), name)
            ok(owner, "code_edit", {**request, "type": "struct QA_CONTEXT *"})
            after = source(owner, representation)["text"]
            assert "->Length" in after and "->Flags" in after, after
            if compiler:
                c_file, executable = root / (representation + ".c"), root / (representation + ".exe")
                c_file.write_text(after + harness)
                if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                    command = [compiler, "/nologo", "/TC", "/std:c11", str(c_file), "/Fe:" + str(executable)]
                else:
                    command = [compiler, "-std=c11", "-fno-strict-aliasing", str(c_file), "-o", str(executable)]
                built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
                assert built.returncode == 0, built.stdout + built.stderr
                subprocess.run([str(executable)], cwd=root, check=True, timeout=10)
        print("Source variable types: fields, validation, native isolation, history, replicas and reopen passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc")
    arguments = parser.parse_args()
    run(arguments.worker, arguments.cc)
