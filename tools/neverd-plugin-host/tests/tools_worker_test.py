"""External two-tool plugin, deterministic caller, actual ND project receipts."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

host, worker, library = map(lambda s: Path(s).resolve(), sys.argv[1:])
root = Path(__file__).resolve().parents[3]
example = root / "pluginsdk/common/examples/tool-roundtrip"
sys.path[:0] = [str(example), str(root / "tools/neverd-worker/tests")]
from caller import Consumer
from pe_browse_test import image

key = "PATH" if os.name == "nt" else "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
os.environ[key] = str(library.parent) + os.pathsep + os.environ.get(key, "")

with tempfile.TemporaryDirectory(prefix="tools-roundtrip-") as temp:
    directory = Path(temp)
    package = directory / "插件"
    subprocess.run([sys.executable, str(example / "prepare.py"), str(package)], check=True, capture_output=True)
    original, base = image()
    binary = directory / "fixture.exe"
    binary.write_bytes(original)
    entry = hex(base + 0x1000)

    def consumer(name, writable=False):
        command = [str(host), "tools", str(package / "plugin.json"), str(package / "tools.json"),
                   "--worker", str(worker), "--input", str(binary), "--function", entry,
                   "--output", str(directory / name)]
        if writable:
            command.append("--allow-project-writes")
        return Consumer(command)

    read_only = consumer("readonly")
    try:
        listed = read_only.request("resolve", session="test", step="readonly")
        write = next(t for t in listed["tools"] if t["id"] == "apply-name")
        assert not write["available"] and write["reason"]["code"] == "service_unavailable", write
        denied = read_only.request("call", set=listed["set"], registration=write["registration"], call_id="denied")
        assert denied["status"] == "error", denied
    finally:
        read_only.close()

    client = consumer("writable", True)
    try:
        listed = client.request("resolve", session="test", step="review")
        tools = {t["id"]: t for t in listed["tools"]}
        assert all(t["available"] for t in tools.values()), tools
        first = client.request("call", set=listed["set"], registration=tools["report"]["registration"],
                               call_id="report", parameters={"limit": 8})
        parent = first["value"]["operation"]
        repeated = client.request("call", set=listed["set"], registration=tools["report"]["registration"],
                                  call_id="report", parameters={"limit": 8})
        assert repeated["value"]["operation"] == parent
        conflict = client.request("call", set=listed["set"], registration=tools["report"]["registration"],
                                  call_id="report", parameters={"limit": 9})
        assert conflict["error"]["code"] == "call_conflict", conflict
        report = client.done(parent)
        assert report["state"] == "succeeded", report
        assert 0 < report["data"]["instruction_count"] <= 8
        artifact = report["artifacts"][0]
        contents = (directory / "writable" / parent / artifact["path"]).read_bytes()
        assert hashlib.sha256(contents).hexdigest() == artifact["sha256"]
        assert len(json.loads(contents)["instructions"]) == report["data"]["instruction_count"]
        second = client.request("call", set=listed["set"], registration=tools["apply-name"]["registration"],
                                call_id="apply", parameters={"name": "tools_reviewed_entry"}, parent_operation=parent)
        applied = client.done(second["value"]["operation"])
        assert applied["state"] == "succeeded", applied
        assert applied["data"]["saved"] and len(applied["receipts"]) == 1, applied
        receipt = applied["receipts"][0]["reply"]
        assert receipt["status"] == "ok" and receipt["payload"]["name"] == "tools_reviewed_entry", receipt
        assert receipt["revision"] != report["context"]["revision"]
        retry = client.request("call", set=listed["set"], registration=tools["apply-name"]["registration"],
                               call_id="apply", parameters={"name": "tools_reviewed_entry"}, parent_operation=parent)
        assert retry["value"]["operation"] == applied["operation"], retry
        stale = client.request("call", set=listed["set"], registration=tools["report"]["registration"], call_id="stale")
        assert stale["error"]["code"] == "context_changed", stale
        fresh = client.request("resolve", session="test", step="fresh")
        tools2 = {t["id"]: t for t in fresh["tools"]}
        old_report = client.request("call", set=fresh["set"], registration=tools2["apply-name"]["registration"],
                                    call_id="old-report", parameters={"name": "must_not_commit"}, parent_operation=parent)
        assert old_report["error"]["code"] == "context_changed", old_report
        third = client.request("call", set=fresh["set"], registration=tools2["report"]["registration"], call_id="verify")
        verified = client.done(third["value"]["operation"])
        assert verified["state"] == "succeeded" and verified["data"]["current_name"] == "tools_reviewed_entry", verified
        assert any(e.get("event") == "operation" and e["value"]["progress"] for e in client.events)
        assert client.request("inspect", operation="unknown")["error"]["code"] == "outcome_unknown"
        client.request("release", set=listed["set"])
        released = client.request("call", set=listed["set"], registration=tools["report"]["registration"], call_id="released")
        assert released["error"]["code"] == "set_released", released
        saved = json.loads((directory / "writable" / applied["operation"] / "receipt.json").read_text(encoding="utf-8"))
        assert saved["receipts"] == applied["receipts"]
    finally:
        client.close()

    reopened = consumer("reopened")
    try:
        listed = reopened.request("resolve", session="test", step="persisted")
        tool = next(t for t in listed["tools"] if t["id"] == "report")
        request = reopened.request("call", set=listed["set"], registration=tool["registration"], call_id="persisted")
        value = reopened.done(request["value"]["operation"])
        assert value["state"] == "succeeded" and value["data"]["current_name"] == "tools_reviewed_entry", value
        assert reopened.request("inspect", operation=parent)["error"]["code"] == "outcome_unknown"
    finally:
        reopened.close()
    assert binary.read_bytes() == original
    print("Tools roundtrip passed: external report, exact dedup, committed rename, stale-report refusal, persistent readback")
