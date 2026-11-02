#!/usr/bin/env python3
"""Proven source edits and whole-function publication through the real worker."""
import json
import platform
import re
from pathlib import Path
import subprocess
import sys
import tempfile

from transport_test import Client
from function_edits_test import two_routine_elf


def call(worker, operation, payload=None):
    request = worker.send(operation, payload)
    return worker.next(lambda result: result.get("type") == "response"
                       and result.get("request_id") == request
                       and result.get("status") != "progress", timeout=120)


def ok(worker, operation, payload=None):
    result = call(worker, operation, payload)
    assert result["status"] == "ok", (operation, result)
    return result["payload"]


def returns_literal(source, value):
    match = re.search(r"\breturn\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*;", source)
    return match is not None and int(match.group(1), 0) == value


def run(executable, compiler):
    if not sys.platform.startswith("linux"):
        print("SKIP: native source-patch fixture requires a Linux ELF host")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-source-byte-patch-") as directory:
        root = Path(directory)
        data, entry = two_routine_elf()
        binary = root / "literal.elf"
        binary.write_bytes(data)
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(binary), "analysis": False})
            context = ok(worker, "source_patch_context", {"address": hex(entry)})
            source = context["source"]
            lines = source.splitlines()
            candidates = []
            for line, text in enumerate(lines):
                if "return" not in text or "7" not in text:
                    continue
                proof = call(worker, "patch_source_target", {
                    "function": hex(entry), "representation": "c", "line": line, "literal": 0})
                if proof["status"] == "ok":
                    candidates.append(proof["payload"])
            assert len(candidates) == 1, (source, candidates)
            proof = candidates[0]
            request = {"address": proof["address"], "function": hex(entry), "representation": "c",
                       "line": proof["line"], "literal": 0, "source_anchor": proof["source_anchor"],
                       "kind": "constant", "value": "0x2a"}
            preview = ok(worker, "patch_source_preview", request)
            assert preview["before"] == source and returns_literal(preview["after"], 42), preview
            assert ok(worker, "patches")["total"] == 0
            ok(worker, "patch_apply", request)
            current = ok(worker, "decompile", {"address": hex(entry), "representation": "c", "limit": 128})
            assert returns_literal(current["text"], 42), current
            rejected = call(worker, "patch_apply", {**request, "source_anchor": "old source"})
            assert rejected["status"] == "error", rejected
            assert call(worker, "source_patch_context", {"address": hex(entry)})["status"] == "error"
            ok(worker, "undo")
            current = ok(worker, "source_patch_context", {"address": hex(entry)})
            assert current["source"] == source
            bad = call(worker, "patch_source_target", {"function": hex(entry), "representation": "c",
                       "line": 0, "literal": 0})
            assert bad["status"] == "error", bad

        # A displayed constant produced by arithmetic is not a MOV operand.
        # Equal-looking values must never let a navigation anchor authorize it.
        folded = root / "folded.elf"
        folded.write_bytes(data[:120] + bytes.fromhex("b80700000083c005c3909090"))
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(folded), "analysis": False})
            source = ok(worker, "source_patch_context", {"address": hex(entry)})["source"]
            line = next(i for i, text in enumerate(source.splitlines()) if returns_literal(text, 12))
            refused = call(worker, "patch_source_target", {
                "function": hex(entry), "representation": "c", "line": line, "literal": 0})
            assert refused["status"] == "error", (source, refused)
            assert ok(worker, "patches")["total"] == 0

        original = root / "original"
        fixture = Path(__file__).resolve().parents[3] / "examples" / "source-patch" / "original.c"
        subprocess.run([compiler, "-O0", "-fno-omit-frame-pointer", "-funwind-tables", "-fPIE", "-pie",
                        "-Wl,--export-dynamic", "-Wl,--eh-frame-hdr", str(fixture), "-o", str(original)],
                       check=True, capture_output=True)
        assert subprocess.check_output([str(original), "1"], text=True).strip() == "2"
        with Client(executable) as worker:
            ok(worker, "open", {"path": str(original), "analysis": False})
            functions = ok(worker, "functions", {"filter": "target", "limit": 32})["items"]
            target = next(row["address"] for row in functions if row["name"] == "target")
            context = ok(worker, "source_patch_context", {"address": target})
            assert context["symbol"] and context["source"] and isinstance(context["bindings"], list), context
            if platform.machine() == "x86_64":
                # The listing owns var_4, so pasted IDA-style frame operands
                # arrive at LLVM MC with an explicit displacement binding.
                assembly = ok(worker, "patch_preview", {
                    "address": hex(int(target, 16) + 4), "kind": "assemble",
                    "text": "mov eax, dword ptr [rbp+var_4]", "allow_overlap": True})
                assert assembly["hex"].startswith("8b45fc"), assembly
            request = {"address": target, "source": "int replacement(int x) { return x + 41; }",
                       "symbol": "replacement", "compiler": "clang", "compiler_path": str(root / "missing-compiler"),
                       "path": str(root / "rewritten"), "bindings": []}
            refused = call(worker, "source_patch", request)
            assert refused["status"] == "error" and not Path(request["path"]).exists(), refused
            request["compiler_path"] = compiler
            report = ok(worker, "source_patch", request)
            assert report and Path(request["path"]).is_file(), report
            assert subprocess.check_output([request["path"], "1"], text=True).strip() == "42"
            assert subprocess.check_output([str(original), "1"], text=True).strip() == "2"
            assert call(worker, "source_patch", request)["status"] == "error"
        print("source literal proof, isolated preview, stale rejection, undo and native C replacement passed")


if __name__ == "__main__":
    sys.exit(run(sys.argv[1], sys.argv[2]))
