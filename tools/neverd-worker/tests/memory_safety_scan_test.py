#!/usr/bin/env python3
"""Check the unified scan across the CLI, Python SDK and workbench worker."""
import argparse
from collections import Counter
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from call_site_types_test import pe
from transport_test import Client

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "pluginsdk/python"))
from neverd_plugin.api import Session
from neverd_plugin.ffi import HostAPI


def cli_report(cli, path, *flags, trace=False):
    env = dict(os.environ, NEVERD_NATIVE_PHASES="1" if trace else "0")
    run = subprocess.run([cli, "scan", *flags, str(path)], capture_output=True,
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


class BorrowedSession:
    def session_address(self, handle):
        return handle


def sdk_reports(host, path, catalog=None):
    handle = host.call("neverd_session_create")
    assert handle
    try:
        session = Session(handle, _native=BorrowedSession(), _host=host)
        session.load(path)
        return {name: session.scan(analyzer=name, sinks=str(catalog) if catalog else None)
                for name in ("both", "audit", "hunt")}
    finally:
        host.call("neverd_session_destroy", handle)


def observations(report, analyzer):
    return [obs["finding"] for group in report["findings"] for obs in group["observations"]
            if obs["analyzer"] == analyzer]


def verify(cli, worker, host, path, expected):
    reports = sdk_reports(host, path)
    combined, serialized = cli_report(cli, path, trace=True)
    assert combined["schema"] == "neverd.memory-safety-scan" and combined["schema_version"] == 1
    assert combined["verdict"] == expected, combined
    assert combined == reports["both"]
    assert cli_report(cli, path)[1] == serialized
    for name, other in (("audit", "hunt"), ("hunt", "audit")):
        single, _ = cli_report(cli, path, "--analyzer=" + name, trace=True)
        assert single == reports[name]
        assert single["analyzers"][other]["state"] == "disabled"
        assert not observations(single, other)
        assert findings(observations(single, name)) == findings(observations(combined, name))
        assert single["analyzers"][name] == combined["analyzers"][name]
    if expected == "SAFE":
        assert combined["coverage_complete"] and not combined["findings"]
    elif expected == "UNKNOWN":
        assert not combined["coverage_complete"] and not combined["findings"]
        assert all(row["state"] == "unsupported" and row["error"]
                   for row in combined["analyzers"].values())
    else:
        for name in ("audit", "hunt"):
            assert any(row["verdict"] == "UNSAFE" for row in observations(combined, name))
        limited, _ = cli_report(cli, path, "--max-call-depth=1",
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


def verify_direct_read_initialization(cli, worker, host, root):
    # All three read the caller's scalar field through RCX on Win64.
    cases = (
        ("borrowed", "8b 01 c3", 0, 1),                    # load 4
        ("initialized", "c7 01 07 00 00 00 0f b6 01 c3", 1, 0),  # store 4, load 1
        ("partial", "c6 01 07 8b 01 c3", 0, 1),            # store 1, load 4
    )
    for name, code, written, unresolved in cases:
        path = root / (name + ".exe")
        path.write_bytes(pe(bytes.fromhex(code)))
        report, _ = cli_report(cli, path, "--analyzer=audit")
        assert report["verdict"] == "SAFE" and report["coverage_complete"], report
        assert not report["findings"]
        assert report["analyzers"]["audit"]["direct_read_initialization"] == {
            "preceding_constant_store": written, "local_stack": 0,
            "outside_or_unresolved": unresolved,
        }, report
        assert "direct_read_initialization" not in report["analyzers"]["hunt"]
        if written:
            verify(cli, worker, host, path, "SAFE")

    # Reserve a real Win64 frame and address a slot through RDX.
    store = bytes.fromhex("c7 02 07 00 00 00")
    load = bytes.fromhex("0f b6 02")
    frame = bytes.fromhex("48 83 ec 28 48 8d 54 24 10")
    leave = bytes.fromhex("48 83 c4 28 c3")
    for code, expected in ((store + load, "SAFE"), (load + store, "UNSAFE")):
        path = root / ("frame-" + expected.lower() + ".exe")
        path.write_bytes(pe(frame + code + leave))
        report, _ = cli_report(cli, path, "--analyzer=audit")
        assert report["verdict"] == expected and report["coverage_complete"], report
        initialized = int(expected == "SAFE")
        assert report["analyzers"]["audit"]["direct_read_initialization"] == {
            "preceding_constant_store": initialized, "local_stack": 1 - initialized,
            "outside_or_unresolved": 0,
        }, report
        findings = [(g["property"], g["verdict"]) for g in report["findings"]]
        assert findings == ([] if initialized else [("uninitialized_read", "UNSAFE")])


def verify_cli_options(cli, clean, output):
    run = subprocess.run([cli, "scan", str(clean), "--json", "--analyzer=both", "-o", str(output)],
                         capture_output=True, text=True, timeout=90)
    assert run.returncode == 0 and not run.stdout, run.stderr
    assert output.read_text() == cli_report(cli, clean)[1]
    rejected = subprocess.run([cli, "scan", str(clean), "--analyzer=typo"],
                              capture_output=True, text=True, timeout=90)
    assert rejected.returncode == 1 and "analyzer" in rejected.stderr, rejected

    help_text = subprocess.check_output([cli, "--help"], text=True, timeout=90)
    names = {line.split(" - ", 1)[0].strip() for line in help_text.splitlines()}
    assert "scan" in names and names.isdisjoint({"audit", "hunt"})
    for retired in ("audit", "hunt"):
        run = subprocess.run([cli, retired, str(clean)], capture_output=True, text=True, timeout=90)
        assert run.returncode == 1 and not run.stdout and f"Unknown subcommand '{retired}'" in run.stderr, run


def verify_shared_catalog_scope(cli, host, mixed, catalog):
    # The fixture's puts calls take one argument. Both engines independently
    # report the unmet seven-argument catalog requirement at each call.
    catalog.write_text(json.dumps({"sinks": [{
        "name": "puts", "kind": "format", "fmt": 0,
        "effect": {"min_arity": 7, "max_arity": 7, "formats": ["elf"], "abis": ["sysv"]},
    }]}))
    flag = "--sinks=" + str(catalog)
    report, _ = cli_report(cli, mixed, flag)
    reports = sdk_reports(host, mixed, catalog)
    assert report == reports["both"]
    groups = [group for group in report["findings"] if group["scope"]["sink"] == "puts"]
    assert groups
    for group in groups:
        assert group["property"] == "format_string" and group["verdict"] == "UNKNOWN"
        assert group["scope"]["kind"] == "catalog_call_effects" and group["scope"]["arg_index"] == 0
        assert [row["analyzer"] for row in group["observations"]] == ["audit", "hunt"]
    for name in ("audit", "hunt"):
        merged = [ob["finding"] for group in groups for ob in group["observations"]
                  if ob["analyzer"] == name]
        assert findings(merged) == findings(row for row in observations(reports[name], name) if row["sink"] == "puts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli")
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--worker")
    parser.add_argument("--sdk", required=True, type=Path)
    args = parser.parse_args()
    host = HostAPI(ctypes.CDLL(str(args.sdk.resolve())))
    with tempfile.TemporaryDirectory(prefix="neverd-safety-scan-") as directory:
        root = Path(directory)
        mixed = root / args.fixture.name
        shutil.copyfile(args.fixture, mixed)
        clean = root / "clean.exe"
        clean.write_bytes(pe(bytes.fromhex("31 c0 c3")))  # xor eax,eax; ret
        incomplete = root / "incomplete.exe"
        incomplete.write_bytes(pe(bytes.fromhex("ff e0")))  # unresolved indirect jump
        for path, expected in ((clean, "SAFE"), (incomplete, "UNKNOWN"), (mixed, "UNSAFE")):
            verify(args.cli, args.worker, host, path, expected)
        # MOV EAX, ESI followed by an unsupported XACQUIRE LOCK XADD.
        refused = root / "refused.exe"
        refused.write_bytes(pe(bytes.fromhex("89 f0 f2 f0 0f c1 07 c3")))
        report, _ = cli_report(args.cli, refused)
        assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"]
        for row in report["analyzers"].values():
            assert row["state"] == "unsupported" and "unsupported instruction at 0x140001002" in row["error"], row
            assert "direct_read_initialization" not in row
        verify_direct_read_initialization(args.cli, args.worker, host, root)
        verify_cli_options(args.cli, clean, root / "report.json")
        verify_shared_catalog_scope(args.cli, host, mixed, root / "catalog.json")


if __name__ == "__main__":
    main()
