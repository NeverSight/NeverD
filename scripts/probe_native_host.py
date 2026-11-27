#!/usr/bin/env python3
"""Preserve native KVM/WHP setup evidence without claiming guest execution."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "scripts/probe_native_host.cpp", "scripts/NativeHostProbe.def",
    "lib/emulation/backends/kvm/KvmProtocol.def",
    "lib/emulation/backends/whp/WhpProtocol.def",
)


def definitions(root: Path = ROOT) -> tuple[dict[str, int], list[tuple[str, str, str]], dict[str, str]]:
    text = (root / "scripts/NativeHostProbe.def").read_text(encoding="utf-8")
    values = {name: int(value, 0) for name, value in re.findall(
        r"^NEVERD_HOST_VALUE\((\w+), (\w+)\)$", text, re.M)}
    stages = re.findall(r"^NEVERD_HOST_STAGE\((\w+), (\w+), (\w+)\)$", text, re.M)
    resources = dict(re.findall(r"^NEVERD_HOST_RESOURCE\((\w+), (\w+)\)$", text, re.M))
    if not values or not stages or len({name for _, name, _ in stages}) != len(stages):
        raise ValueError("invalid native host probe definition")
    return values, stages, resources


def parse_evidence(text: str, code: int, backend: str, architecture: str,
                   root: Path = ROOT) -> tuple[str, list[dict]]:
    values, stages, resources = definitions(root)
    availability = set(re.findall(
        r"^NEVERD_HOST_AVAILABILITY_STAGE\((\w+)\)$",
        (root / "scripts/NativeHostProbe.def").read_text(encoding="utf-8"), re.M))
    scope = {"kvm": "Kvm", "whp": "Whp"}[backend]
    selected = [(name, phase) for group, name, phase in stages
                if group in {"All", scope, scope + architecture}]
    prepares = [name for name, phase in selected if phase == "Prepare"]
    cleanups = [name for name, phase in selected if phase == "Cleanup"]
    records = []
    for line in text.splitlines():
        match = re.fullmatch(r"(\w+)\t(ok|unavailable|failed)\t([0-9]+)\t([0-9]+)", line)
        if not match:
            raise ValueError(f"invalid probe record: {line!r}")
        stage, status, error, value = match.groups()
        if max(int(error), int(value)) > (1 << 64) - 1:
            raise ValueError("probe record exceeds its wire width")
        records.append({"stage": stage, "status": status, "code": int(error), "value": int(value)})
    if not records or len({record["stage"] for record in records}) != len(records):
        raise ValueError("missing or duplicate native probe records")
    first = records[0]
    if (first["stage"] != "NativeArchitecture"
            or first["value"] != values[architecture + "Architecture"]):
        raise ValueError("probe executable architecture disagrees with the requested host")

    reached, cleanup = [], []
    failed_prepare = False
    for record in records:
        name, status = record["stage"], record["status"]
        if status == "unavailable" and name not in availability:
            raise ValueError("setup failures cannot be availability skips")
        if name in cleanups:
            cleanup.append(name)
            if status == "unavailable":
                raise ValueError("resource cleanup cannot be an availability skip")
            continue
        if cleanup or failed_prepare or len(reached) == len(prepares) or name != prepares[len(reached)]:
            raise ValueError("probe preparation is not the declared prefix")
        reached.append(name)
        failed_prepare = status != "ok"
    acquired = {resources[record["stage"]] for record in records
                if record["stage"] in resources and record["status"] == "ok"}
    if cleanup != [name for name in cleanups if name in acquired]:
        raise ValueError("probe did not report every acquired resource cleanup")
    failures = [record for record in records if record["status"] != "ok"]
    if not failures:
        if code != values["ReadyExit"] or reached != prepares:
            raise ValueError("successful exit omitted required host setup evidence")
        return "setup_ready", records
    failed = any(record["status"] == "failed" for record in failures)
    expected = values["FailedExit" if failed else "UnavailableExit"]
    if code != expected:
        raise ValueError("probe exit status disagrees with its failure evidence")
    return "failed" if failed else "unavailable", records


def host_matches(backend: str, architecture: str) -> bool:
    machine = {"x86_64": "X64", "amd64": "X64", "aarch64": "ARM64", "arm64": "ARM64"}.get(
        platform.machine().lower())
    return (machine == architecture
            and platform.system() == {"kvm": "Linux", "whp": "Windows"}[backend])


def run(output: Path, backend: str, architecture: str, compiler: str | None,
        require_setup: bool) -> int:
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    values, _, _ = definitions()
    report = {
        "backend": backend, "architecture": architecture,
        "host": {"system": platform.system(), "machine": platform.machine(),
                 "release": platform.release(), "version": platform.version()},
        "status": "failed", "guest_execution_verified": False,
        "commands": [], "records": [],
    }

    def git(*args: str) -> str:
        return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()

    def command(args: list[str], name: str, timeout: int) -> subprocess.CompletedProcess:
        report["commands"].append(args)
        try:
            result = subprocess.run(args, cwd=output, capture_output=True, text=True,
                                    errors="replace", timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            (output / (name + ".stdout")).write_bytes(error.stdout or b"")
            (output / (name + ".stderr")).write_bytes(error.stderr or b"")
            raise
        (output / (name + ".stdout")).write_text(result.stdout, encoding="utf-8")
        (output / (name + ".stderr")).write_text(result.stderr, encoding="utf-8")
        report[name + "_returncode"] = result.returncode
        return result

    try:
        report["commit"] = git("rev-parse", "HEAD")
        report["source_dirty"] = bool(git("status", "--porcelain"))
        if report["source_dirty"]:
            raise ValueError("native evidence requires a committed source tree")
        if not host_matches(backend, architecture):
            raise ValueError("native probe requires the matching host OS and ISA")
        report["source_sha256"] = {
            source: hashlib.sha256((ROOT / source).read_bytes()).hexdigest() for source in SOURCES
        }
        selected = compiler or ("cl" if backend == "whp" else "c++")
        path = shutil.which(selected)
        if not path:
            raise ValueError(f"native compiler is unavailable: {selected}")
        program = output / ("probe.exe" if backend == "whp" else "probe")
        source = str(ROOT / SOURCES[0])
        if backend == "whp":
            arguments = [path, "/nologo", "/std:c++17", "/W4", "/O2", "/EHsc",
                         source, "/Fe:" + str(program)]
        else:
            arguments = [path, "-std=c++17", "-Wall", "-Wextra", "-O2", source, "-o", str(program)]
        report["stage"] = "build"
        build = command(arguments, "build", values["BuildTimeoutSeconds"])
        build.check_returncode()
        report["program_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        report["stage"] = "probe"
        result = command([str(program)], "probe", values["ProbeTimeoutSeconds"])
        if result.stderr:
            raise ValueError("native probe wrote unexpected stderr")
        report["status"], report["records"] = parse_evidence(
            result.stdout, result.returncode, backend, architecture)
        if backend == "kvm" and architecture == "X64" and platform.system() == "Linux":
            widths = command([str(program), "--address-widths"], "address-widths",
                             values["ProbeTimeoutSeconds"])
            report["address_widths"] = widths.stdout.splitlines()
            cpu = Path("/proc/cpuinfo").read_text().split("\n\n", 1)[0]
            fields = dict(line.split(":", 1) for line in cpu.splitlines() if ":" in line)
            report["host"]["cpu"] = {k.strip(): v.strip() for k, v in fields.items()
                                     if k.strip() in ("vendor_id", "model name", "address sizes")}
            parameter = Path("/sys/module/kvm/parameters/allow_smaller_maxphyaddr")
            try:
                report["allow_smaller_maxphyaddr"] = parameter.read_text().strip()
            except (FileNotFoundError, PermissionError):
                pass
        report["stage"] = "complete"
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        try:
            report["final_commit"] = git("rev-parse", "HEAD")
            report["final_source_dirty"] = bool(git("status", "--porcelain"))
            if report["final_source_dirty"] or report["final_commit"] != report.get("commit"):
                report["status"] = "failed"
                report["error"] = "source changed during native host validation"
        except (OSError, subprocess.SubprocessError) as error:
            report["status"] = "failed"
            report["error"] = str(error)
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
    return int(report["status"] == "failed" or (require_setup and report["status"] != "setup_ready"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("kvm", "whp"), required=True)
    parser.add_argument("--arch", choices=("X64", "ARM64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler")
    parser.add_argument("--require-setup", action="store_true")
    args = parser.parse_args()
    return run(args.output, args.backend, args.arch, args.compiler, args.require_setup)


if __name__ == "__main__":
    sys.exit(main())
