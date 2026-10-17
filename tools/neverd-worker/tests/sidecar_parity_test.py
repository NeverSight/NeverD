#!/usr/bin/env python3
"""The CLI and the workbench write a project's edits as identical bytes, so a
project kept under version control diffs only where its edits differ."""
import subprocess
import sys
import tempfile
from pathlib import Path
from transport_test import Client

# Escapes both JSON writers must spell alike: a tab, a line break, a
# backspace and a form feed, beside text outside ASCII.
TEXT = "CLI note\twith a tab\r\nand a line\bwith a backspace\fform feed é 中文"


def run(worker, cli):
    with tempfile.TemporaryDirectory(prefix="neverd-sidecar-parity-") as directory:
        binary = Path(directory) / "arithmetic.evm"
        binary.write_text("600160020100", encoding="ascii")
        annotations = Path(str(binary) + ".neverd/annotations.json")
        renames = Path(str(binary) + ".neverd/renames.json")

        def execute(args):
            return subprocess.run(args, capture_output=True, timeout=30)

        for command in ([cli, "annotate", str(binary), "--add=0x0", "--text=" + TEXT],
                        [cli, "rename", str(binary), "--func=evm_entry", "--to=reviewed_entry"]):
            result = execute(command)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
        written = {path: path.read_bytes() for path in (annotations, renames)}
        for text in written.values():
            assert text.startswith(b"[\n  {\n    \"addr\": ") and text.endswith(b"\n]\n"), text

        for path in Path(directory).glob("arithmetic.evm.neverd-*.json"):
            path.unlink()
        client = Client(worker)
        try:
            def ok(operation, payload=None):
                reply = client.call(operation, payload or {})
                assert reply["status"] == "ok", (operation, reply)
                return reply["payload"]

            ok("open", {"path": str(binary)})
            ok("annotation_set", {"address": "0x0", "text": TEXT})
            ok("save")
            ok("rename", {"address": "0x0", "name": "reviewed_entry"})
        finally:
            client.close()
        for path, text in written.items():
            assert path.read_bytes() == text, (path.name, path.read_bytes(), text)
        print("CLI and workbench wrote identical annotation and rename sidecars")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
