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
        assert run.stderr.count("stage=machine_med_ir event=begin") == 1, run.stderr
        assert "stage=llvm_emission" not in run.stderr, run.stderr
    return report, run.stdout


def findings(rows):
    return Counter(json.dumps(row, sort_keys=True) for row in rows)


def verify(cli, worker, path, expected):
    legacy = {name: cli_report(cli, path, name, trace=True)[0] for name in ("audit", "hunt")}
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


def verify_cli_options(cli, clean, output):
    run = subprocess.run([cli, "scan", str(clean), "--json", "--analyzer=both", "-o", str(output)],
                         capture_output=True, text=True, timeout=90)
    assert run.returncode == 0 and not run.stdout, run.stderr
    assert output.read_text() == cli_report(cli, clean, "scan")[1]
    for command, selection in (("scan", "typo"), ("audit", "both"), ("hunt", "both")):
        rejected = subprocess.run([cli, command, str(clean), "--analyzer=" + selection],
                                  capture_output=True, text=True, timeout=90)
        assert rejected.returncode == 1 and "analyzer" in rejected.stderr, rejected


def verify_shared_catalog_scope(cli, mixed, catalog):
    # The fixture's puts calls take one argument. Both engines independently
    # report the unmet seven-argument catalog requirement at each call.
    catalog.write_text(json.dumps({"sinks": [{
        "name": "puts", "kind": "format", "fmt": 0,
        "effect": {"min_arity": 7, "max_arity": 7, "formats": ["elf"], "abis": ["sysv"]},
    }]}))
    flag = "--sinks=" + str(catalog)
    reports = {name: cli_report(cli, mixed, name, flag)[0]
               for name in ("scan", "audit", "hunt")}
    groups = [group for group in reports["scan"]["findings"] if group["scope"]["sink"] == "puts"]
    assert groups
    for group in groups:
        assert group["property"] == "format_string" and group["verdict"] == "UNKNOWN"
        assert group["scope"]["kind"] == "catalog_call_effects" and group["scope"]["arg_index"] == 0
        assert [row["analyzer"] for row in group["observations"]] == ["audit", "hunt"]
    for name in ("audit", "hunt"):
        merged = [ob["finding"] for group in groups for ob in group["observations"]
                  if ob["analyzer"] == name]
        assert findings(merged) == findings(row for row in reports[name]["findings"] if row["sink"] == "puts")


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
        # MOV EAX, ESI followed by an unsupported XACQUIRE LOCK XADD.
        refused = root / "refused.exe"
        refused.write_bytes(pe(bytes.fromhex("89 f0 f2 f0 0f c1 07 c3")))
        report, _ = cli_report(args.cli, refused, "scan")
        assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"]
        for row in report["analyzers"].values():
            assert row["state"] == "unsupported" and "unsupported instruction at 0x140001002" in row["error"], row
        verify_cli_options(args.cli, clean, root / "report.json")
        verify_shared_catalog_scope(args.cli, mixed, root / "catalog.json")


if __name__ == "__main__":
    main()
