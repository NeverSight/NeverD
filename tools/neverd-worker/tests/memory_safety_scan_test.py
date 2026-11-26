#!/usr/bin/env python3
"""Compare the static CLI and workbench scan with both independent legacy reports."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from call_site_types_test import pe
from transport_test import Client


def cli_report(cli, path, command, *flags, trace=False):
    env = dict(os.environ, NEVERD_NATIVE_PHASES="1" if trace else "0")
    run = subprocess.run([cli, command, *flags, str(path)], capture_output=True,
                         text=True, timeout=90, env=env)
    report = json.loads(run.stdout)
    assert run.returncode == {"SAFE": 0, "UNKNOWN": 1, "UNSAFE": 2}[report["verdict"]], run.stderr
    if trace:
        assert run.stderr.count("phase=session_load event=begin") == 1, run.stderr
        assert run.stderr.count("stage=low_ir event=begin") == 1, run.stderr
    return report, run.stdout


def findings(rows):
    return Counter(json.dumps(row, sort_keys=True) for row in rows)


def verify(cli, worker, path, expected):
    legacy = {name: cli_report(cli, path, name)[0] for name in ("audit", "hunt")}
    combined, serialized = cli_report(cli, path, "scan", trace=True)
    assert combined["schema"] == "neverd.memory-safety-scan" and combined["schema_version"] == 1
    assert combined["verdict"] == expected, combined
    assert cli_report(cli, path, "scan")[1] == serialized
    reports = {"both": combined}
    for name, old in legacy.items():
        assert old["track"] == name and old["schema_version"] == 1 and "analyzers" not in old
        single, _ = cli_report(cli, path, "scan", "--analyzer=" + name)
        other = "audit" if name == "hunt" else "hunt"
        assert single["analyzers"][other]["state"] == "disabled"
        for report in (single, combined):
            actual = [obs["finding"] for row in report["findings"]
                      for obs in row["observations"] if obs["analyzer"] == name]
            assert findings(actual) == findings(old["findings"]), (name, report)
            summary = report["analyzers"][name]
            for key in ("scanned", "skipped", "budget_hit"):
                assert summary[key] == old[key], (key, summary, old)
        assert single["analyzers"][name] == combined["analyzers"][name]
        reports[name] = single
    if expected == "SAFE":
        assert combined["coverage_complete"] and not combined["findings"]
    elif expected == "UNKNOWN":
        assert not combined["coverage_complete"] and not combined["findings"]
        assert all(row["state"] == "unsupported" and row["error"]
                   for row in combined["analyzers"].values())
    else:
        assert all(any(row["verdict"] == "UNSAFE" for row in v["findings"])
                   for v in legacy.values())
        limited, _ = cli_report(cli, path, "scan", "--max-call-depth=1",
                                "--max-summary-iterations=1")
        assert not limited["coverage_complete"]
        assert any(row["state"] == "budget_exhausted" for row in limited["analyzers"].values())
    if worker:
        with Client(worker) as client:
            assert "memory_safety_scan" in client.hello["capabilities"]
            opened = client.call("open", {"path": str(path), "analysis": False, "warm": False}, timeout=90)
            assert opened["status"] == "ok", opened
            for name, report in reports.items():
                payload = {} if name == "both" else {"analyzer": name}
                response = client.call("memory_safety_scan", payload, timeout=90)
                assert response["status"] == "ok", response
                assert response["payload"] == report, (name, response["payload"], report)
            assert client.call("memory_safety_scan", {"analyzer": "typo"})["error"]["code"] == "invalid_request"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli")
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--worker")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="neverd-safety-scan-") as directory:
        root = Path(directory)
        mixed = root / args.fixture.name
        shutil.copyfile(args.fixture, mixed)
        clean = root / "clean.exe"
        clean.write_bytes(pe(bytes.fromhex("31 c0 c3")))  # xor eax,eax; ret
        incomplete = root / "incomplete.exe"
        incomplete.write_bytes(pe(bytes.fromhex("ff e0")))  # unresolved indirect jump
        for path, expected in ((clean, "SAFE"), (incomplete, "UNKNOWN"), (mixed, "UNSAFE")):
            verify(args.cli, args.worker, path, expected)


if __name__ == "__main__":
    main()
