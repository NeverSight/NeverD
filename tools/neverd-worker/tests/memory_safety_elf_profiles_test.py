#!/usr/bin/env python3
"""Check scan findings in linked MIPS, PowerPC and RISC-V ELF images."""

import argparse
import ctypes
import json
from pathlib import Path
import shutil
import tempfile

from memory_safety_scan_test import cli_report, observations, sdk_reports
from neverd_plugin.ffi import HostAPI
from transport_test import Client


PROFILES = (
    "mips", "mipsel", "mips64", "mips64el", "mips64n32", "mips64n32el",
    "mips64o32", "mips64o32el", "powerpc", "powerpcle", "powerpc64",
    "powerpc64le", "powerpc64-elfv1", "powerpc64le-elfv1", "riscv32", "riscv64",
    "riscv32-ilp32d", "riscv64-lp64d",
)
HISTORICAL = {"mips64o32", "mips64o32el"}


def translation_unavailable(report, name):
    assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"], name
    assert not report["findings"], name
    for analyzer in report["analyzers"].values():
        if analyzer["state"] != "disabled":
            assert analyzer["state"] == "unsupported", name
            assert "unresolved-memory-translation" in analyzer["error"], name


def verify_frame(cli, image, name):
    reports = {mode: cli_report(cli, image, *flags)[0] for mode, flags in (
        ("both", ()), ("audit", ("--analyzer=audit",)), ("hunt", ("--analyzer=hunt",)),
    )}
    both, audit, hunt = (reports[k] for k in ("both", "audit", "hunt"))
    assert both["format"] == "ELF" and both["arch"] == name, name
    assert both["schema"] == "neverd.memory-safety-scan" and both["schema_version"] == 1, name
    for mode in ("audit", "hunt"):
        assert both["analyzers"][mode] == reports[mode]["analyzers"][mode], name
        assert observations(both, mode) == observations(reports[mode], mode), name
    assert audit["analyzers"]["hunt"]["state"] == hunt["analyzers"]["audit"]["state"] == "disabled", name

    if name in HISTORICAL:
        for report in reports.values():
            translation_unavailable(report, name)
    else:
        assert both["coverage_complete"] and both["verdict"] == audit["verdict"] == "UNSAFE", name
        assert hunt["coverage_complete"] and hunt["verdict"] == "SAFE" and not hunt["findings"], name
        # Only first reads the slot before initialization. Second initializes
        # a slot of the same size before reading it.
        found = [(f["function"], f["class"], f["verdict"]) for f in observations(both, "audit")]
        assert found == [("first", "uninitialized_read", "UNSAFE")], (name, found)
    return reports


def verify_ownership(cli, directory, name, temporary):
    def scan(variant, *flags):
        image = directory / "ownership" / (name + "-" + variant)
        return cli_report(cli, image, *flags)[0]

    for flags in (("--analyzer=audit",), ()):
        report = scan("released", *flags)
        audit = report["analyzers"]["audit"]
        assert report["verdict"] == "SAFE" and report["coverage_complete"], (name, report)
        assert audit["scanned"] == 1 and audit["unknown"] == 0 and not report["findings"], name

    for variant in ("unreleased", "free-null"):
        report = scan(variant, "--analyzer=audit")
        assert report["coverage_complete"] and report["verdict"] == "UNSAFE", (name, report)
        audit = report["analyzers"]["audit"]
        assert audit["scanned"] == 1 and audit["unknown"] == 0, (name, report)
        assert [(f["function"], f["name"], f["class"], f["verdict"])
                for f in observations(report, "audit")] == [
            ("sample", "malloc", "heap_leak", "UNSAFE")
        ], (name, report)

    # Publication to a global requires a stronger write-set proof. A change
    # in the version provider also removes the known allocation contract.
    report = scan("unrelated-store", "--analyzer=audit")
    assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"], (name, report)
    model = temporary.with_suffix(".json")
    model.write_text(json.dumps({"sinks": [
        {"name": "free", "kind": "free", "handle": 0, "release_may_fail": True}
    ]}))
    report = scan("released", "--analyzer=audit", "--sinks=" + str(model))
    assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"], (name, report)

    original = (directory / "ownership" / (name + "-released")).read_bytes()
    assert original.count(b"libc.so.6\0") == 1, name
    temporary.write_bytes(original.replace(b"libc.so.6\0", b"libc.so.5\0"))
    report, _ = cli_report(cli, temporary, "--analyzer=audit")
    assert report["verdict"] == "UNKNOWN" and not report["coverage_complete"], (name, report)


def verify_import(cli, image, name, catalog, arity):
    catalog.write_text(json.dumps({"sinks": [{
        "name": "record_label", "kind": "format", "fmt": 0,
        "effect": {"min_arity": arity, "max_arity": arity},
    }]}))
    report, _ = cli_report(cli, image, "--sinks=" + str(catalog))
    assert report["format"] == "ELF" and report["arch"] == name, name
    if name in HISTORICAL:
        translation_unavailable(report, name)
        return
    assert report["coverage_complete"] and report["analyzers"]["hunt"]["scanned"] == 1, name
    assert len(report["findings"]) == 1, (name, report["findings"])
    group = report["findings"][0]
    assert group["property"] == "format_string" and group["scope"]["sink"] == "record_label", name
    items = group["observations"]
    assert all(i["finding"]["function"] == "read_label" for i in items), name
    if arity == 1:
        assert report["verdict"] == group["verdict"] in {"SAFE", "UNKNOWN"}, name
        assert [i["analyzer"] for i in items] == ["hunt"], name
        if name in ("powerpc64", "powerpc64le"):
            # The linked caller branches to read_label's ELFv2 local entry,
            # and the format argument resolves to the immutable literal "OK".
            assert report["verdict"] == group["verdict"] == "SAFE", name
            assert items[0]["finding"]["detail"] == "format string is a mapped constant", name
        elif name in ("riscv32-ilp32d", "riscv64-lp64d"):
            assert group["verdict"] == "UNKNOWN", name
            assert items[0]["finding"]["detail"] == "dynamic call-site analysis is unavailable for this ABI", name
    else:
        assert [i["analyzer"] for i in items] == ["audit", "hunt"], name
        assert all(i["finding"]["verdict"] == "UNKNOWN" for i in items), name
        assert report["verdict"] == "UNKNOWN", name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli")
    parser.add_argument("directory", type=Path)
    parser.add_argument("--worker")
    parser.add_argument("--sdk", type=Path, required=True)
    args = parser.parse_args()
    host = HostAPI(ctypes.CDLL(str(args.sdk.resolve())))
    with tempfile.TemporaryDirectory(prefix="neverd-elf-scan-") as temporary:
        image = Path(temporary) / "program"
        catalog = Path(temporary) / "catalog.json"
        for name in PROFILES:
            shutil.copyfile(args.directory / name, image)
            reports = verify_frame(args.cli, image, name)
            if name in ("mips64n32el", "powerpc64-elfv1", "riscv32", "riscv64-lp64d"):
                assert reports == sdk_reports(host, image), name
                if args.worker:
                    with Client(args.worker) as worker:
                        reply = worker.call("open", {"path": str(image), "analysis": False,
                                                     "warm": False, "read_only": True}, timeout=90)
                        assert reply["status"] == "ok", (name, reply)
                        reply = worker.call("memory_safety_scan", timeout=90)
                        assert reply["status"] == "ok" and reply["payload"] == reports["both"], name
            if name in ("mips64n32", "mips64n32el"):
                verify_ownership(args.cli, args.directory, name, image)
            if not name.endswith("-elfv1"):
                shutil.copyfile(args.directory / "imports" / name, image)
                for arity in (1, 7):
                    verify_import(args.cli, image, name, catalog, arity)


if __name__ == "__main__":
    main()
