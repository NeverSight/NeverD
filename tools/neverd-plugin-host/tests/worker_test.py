"""A real function report through the headless worker bridge and shared SDK."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

host, worker, library = map(lambda s: Path(s).resolve(), sys.argv[1:])
root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "tools/neverd-worker/tests"))
from pe_browse_test import image

with tempfile.TemporaryDirectory(prefix="nph-worker-") as temp:
    directory = Path(temp)
    package = directory / "workbench-explorer"
    subprocess.run([sys.executable, str(root / "pluginsdk/gui/examples/workbench-explorer/prepare.py"),
                    str(package)], check=True, capture_output=True)
    binary = directory / "fixture.exe"
    original, base = image()
    binary.write_bytes(original)
    env = dict(os.environ)
    key = "PATH" if os.name == "nt" else "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
    env[key] = str(library.parent) + os.pathsep + env.get(key, "")
    result = subprocess.run([str(host), "run", str(package / "plugin.json"), "report",
        "--worker", str(worker), "--input", str(binary), "--function", hex(base + 0x1000),
        "--address", hex(base + 0x1000), "--output", str(directory / "report")],
        capture_output=True, timeout=45, env=env)
    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    record = json.loads(result.stdout)
    assert record["state"] == "succeeded", record
    assert len(record["result"]["views"]) == 4, record
    assert len(record["result"]["artifacts"]) == 2, record
    assert record["context"]["project_id"] and record["context"]["loaded"], record
    assert json.loads((directory / "report/receipt.json").read_text(encoding="utf-8")) == record
    assert binary.read_bytes() == original
    observer = directory / "host-observer"
    subprocess.run([sys.executable, str(root / "pluginsdk/common/examples/host-observer/prepare.py"),
                    str(observer)], check=True, capture_output=True)
    observed = subprocess.run([str(host), "run", str(observer / "plugin.json"), "inspect",
        "--worker", str(worker), "--input", str(binary), "--output", str(directory / "observer-result")],
        capture_output=True, timeout=45, env=env)
    assert observed.returncode == 0, observed.stdout.decode("utf-8", errors="replace")
    data = json.loads((directory / "observer-result/host-report.json").read_text(encoding="utf-8"))
    assert data["context"]["loaded"] and data["metadata"], data
    assert data["notification"]["event"]["id"] == "task.changed", data
    assert not data["services"]["missing"], data
    assert binary.read_bytes() == original
    print("Headless real-worker report: 4 views, 2 artifacts, committed receipt, input preserved")
