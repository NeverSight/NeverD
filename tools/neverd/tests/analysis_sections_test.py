#!/usr/bin/env python3
"""Real CLI/worker interop for persisted analysis-image additions."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "neverd-worker" / "tests"))
from native_mapping_test import native_elf
from pe_browse_test import image as pe_image
from transport_test import Client


def run(cli, worker=None):
    def command(*args, success=True, json_output=False):
        result = subprocess.run([cli, *map(str, args)], capture_output=True,
                                encoding="utf-8", timeout=20)
        assert (result.returncode == 0) == success, (args, result.returncode, result.stdout, result.stderr)
        if json_output:
            return json.loads(result.stdout)
        return result

    with tempfile.TemporaryDirectory(prefix="neverd-cli-sections-") as path:
        directory = Path(path) / "shared-state"
        directory.mkdir()
        variants = [("pe32.exe", *pe_image(False), False),
                    ("pe64.exe", *pe_image(True), False)]
        for arm64 in (False, True):
            base = 0xffff800000400000 if arm64 else 0x400000
            data, _ = native_elf(arm64, base)
            variants.append(("arm64.elf" if arm64 else "x64.elf", data, base, arm64))
        for name, original, base, arm64 in variants:
            binary = directory / name
            binary.write_bytes(original)
            added = base + 0x10000
            code = bytes.fromhex("40058052c0035fd6e0038052c0035fd6" if arm64 else
                                 "b82a000000c3b81f000000c3")
            width = 8 if arm64 else 6
            raw = directory / "code.bin"
            raw.write_bytes(code)
            description = {"schema": "neverd.analysis-section.v1", "name": ".cli_code",
                           "address": hex(added), "permissions": "rx", "functions": [
                               {"name": "cli_returns_42", "offset": "0x0", "size": width},
                               {"name": "cli_returns_31", "offset": hex(width), "size": width}]}
            request = directory / "description.json"
            request.write_text(json.dumps(description), encoding="utf-8")
            summary = command("analysis-sections", binary, "--json", json_output=True)
            assert summary["sections"] == []
            result = command("analysis-sections", binary, "--add", request, "--data", raw,
                             "--json", json_output=True)
            assert result["sections"][0]["name"] == ".cli_code", result
            sidecar = Path(str(binary) + ".neverd/analysis-sections.json")
            saved = sidecar.read_bytes()
            state = json.loads(saved)
            assert state["sample_sha256"] == hashlib.sha256(original).hexdigest()
            assert state["sections"][0]["data_hex"] == code.hex()
            sections = command("sections", binary, "--json", json_output=True)
            assert any(row["name"] == ".cli_code" and row["file_sz"] == 0
                       and row["materialized_sz"] == len(code) for row in sections), sections
            functions = command("funcs", binary, "--json", json_output=True)
            assert "cli_returns_42" in json.dumps(functions), functions
            instructions = command("disasm", binary, "--func", "cli_returns_42", "--json", json_output=True)
            assert int(instructions[0]["addr"], 16) == added, instructions
            text = command("decompile", binary, "--func", "cli_returns_42", "--json", json_output=True)
            source = "".join(p["text"] for p in text["pages"])
            assert "cli_returns_42" in source and "42" in source, source
            graph = command("cfg", binary, "--func", "cli_returns_31", "--json", json_output=True)
            assert graph["nodes"], graph
            assert "digraph" in command("cfg", binary, "--func", "cli_returns_31").stdout
            # Ordinary text and whole-image C paths must retain the same image.
            assert "cli_returns_42" in command("disasm", binary, "--func", "cli_returns_42").stdout
            assert "cli_returns_42" in command("decompile", binary).stdout
            for args in (("--add", request, "--data", raw), ("--remove", ".text"),
                         ("--data", raw), ("--add", request),
                         ("--add", request, "--data", raw, "--remove", ".cli_code")):
                command("analysis-sections", binary, *args, success=False)
                assert sidecar.read_bytes() == saved
            malformed = copy.deepcopy(state)
            malformed["sections"][0]["data_hex"] = "xyz"
            for invalid in (dict(state, sample_sha256="0" * 64), malformed):
                sidecar.write_text(json.dumps(invalid), encoding="utf-8")
                command("analysis-sections", binary, "--json", success=False)
                command("decompile", binary, "--func", "cli_returns_42", success=False)
            sidecar.write_bytes(saved + b"\0ignored")
            command("analysis-sections", binary, success=False)
            sidecar.write_text('[' * 100 + '0' + ']' * 100, encoding="utf-8")
            nested = command("analysis-sections", binary, success=False)
            assert "nesting" in nested.stderr, nested.stderr
            sidecar.write_bytes(saved)
            if worker:
                with Client(worker) as client:
                    reply = client.call("open", {"path": str(binary)})
                    assert reply["status"] == "ok", reply
                    restored = client.call("analysis_sections")
                    assert restored["payload"]["sections"][0]["name"] == ".cli_code"
                    # A CLI writer must not bypass a GUI/worker writer lease.
                    refused = command("analysis-sections", binary, "--remove", ".cli_code", success=False)
                    assert "writer lock" in refused.stderr.lower(), refused.stderr
                    assert sidecar.read_bytes() == saved
                    changed = client.call("analysis_section_remove", {"name": ".cli_code"})
                    assert changed["status"] == "ok", changed
                    assert client.call("save")["status"] == "ok"
                assert command("analysis-sections", binary, "--json", json_output=True)["sections"] == []
                command("analysis-sections", binary, "--add", request, "--data", raw)
            removed = command("analysis-sections", binary, "--remove", ".cli_code", "--json", json_output=True)
            assert removed["sections"] == []
            command("disasm", binary, "--func", "cli_returns_42", success=False)
            assert binary.read_bytes() == original
    print("analysis-sections CLI: add/list/remove, PE32/PE64/ELF x64/AArch64, native views, "
          "source identity and malformed-state rejection passed; " +
          ("worker writer-lock/state interop passed" if worker else
           "worker interop not run (worker target unavailable)"))


if __name__ == "__main__":
    run(*sys.argv[1:])
