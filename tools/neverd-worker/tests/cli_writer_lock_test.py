#!/usr/bin/env python3
"""Cross-process real CLI/worker writer exclusion; never executes target code."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from transport_test import Client


def run(worker, cli):
    with tempfile.TemporaryDirectory(prefix="neverd-cli-writer-lock-") as directory:
        binary = Path(directory) / "arithmetic.evm"
        binary.write_text("600160020100", encoding="ascii")
        annotations = Path(str(binary) + ".neverd-annotations.json")
        renames = Path(str(binary) + ".neverd-renames.json")
        commands = [[cli, "annotate", str(binary), "--add=0x0", "--text=CLI note"],
                    [cli, "rename", str(binary), "--func=evm_entry", "--to=reviewed_entry"]]
        def execute(args):
            return subprocess.run(args, capture_output=True, text=True, timeout=15)
        client = Client(worker)
        try:
            opened = client.call("open", {"path": str(binary)})
            assert opened["status"] == "ok" and not opened["payload"]["read_only"], opened
            for command in commands:
                result = execute(command)
                assert result.returncode != 0, (command, result.stdout, result.stderr)
                assert "writer" in (result.stderr + result.stdout).lower(), result
                assert not annotations.exists() and not renames.exists(), "Rejected writer created a sidecar"
            info = execute([cli, "info", str(binary), "--json"])
            assert info.returncode == 0, (info.stdout, info.stderr)
            assert json.loads(info.stdout), "Read-only CLI metadata must remain available"
        finally:
            client.close()
        for command in commands:
            result = execute(command)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
        assert json.loads(annotations.read_text())[0]["text"] == "CLI note"
        assert json.loads(renames.read_text())[0]["renamed"] == "reviewed_entry"
        assert binary.read_text() == "600160020100"
        print("Real CLI/worker: two CLI writes refused without sidecars while worker owns lock; info succeeds; both edits succeed after worker closes")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
