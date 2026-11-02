#!/usr/bin/env python3
"""Run every previously unqualified Lift case and reject incomplete evidence.

This profile owns Linux x86-64 execution, including real i386 processes and
unmasked SSE traps. The normal cross-platform suites keep their host guards.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "unittests/Inputs/lift-qualification.json"
PARITY_IMAGE = Path(
    "unittests/corpus/corpus/rust-eh/aarch64-unknown-linux-gnu/"
    "unwind/o2/cdylib/rust_eh_cdylib-aarch64-unknown-linux-gnu-unwind-o2.so"
)


def identities(document: dict) -> tuple[str, ...]:
    if document.get("schema") != 1:
        raise ValueError("unsupported qualification inventory schema")
    cases = document.get("tests", [])
    names = [case["name"] for case in cases]
    if len(names) != 108 or len(set(names)) != 108:
        raise ValueError("qualification requires exactly 108 distinct test identities")
    if any(not re.fullmatch(r"[A-Za-z0-9_]+\.[A-Za-z0-9_]+", n) for n in names):
        raise ValueError("invalid qualification test identity")
    counts = Counter(case.get("baseline") for case in cases)
    if dict(counts) != {"failed": 62, "skipped": 46}:
        raise ValueError("qualification must retain all 62 failures and 46 skips")
    # Keep the historical identity and outcome; a fixture rename must be explicit.
    current = [case.get("current_name", case["name"]) for case in cases]
    if (any(not isinstance(n, str) or not re.fullmatch(
            r"[A-Za-z0-9_]+\.[A-Za-z0-9_]+", n) for n in current)
            or len(set(current)) != len(names)):
        raise ValueError("invalid or duplicate current qualification identity")
    return tuple(current)


def audit(document: ET.Element, expected: tuple[str, ...], exit_code: int) -> dict:
    if document.tag != "testsuites":
        raise ValueError("expected GoogleTest XML testsuites")
    cases = list(document.iter("testcase"))
    names = [f"{case.get('classname')}.{case.get('name')}" for case in cases]
    if Counter(names) != Counter(expected):
        raise ValueError(
            "test identities differ: "
            f"missing={sorted(set(expected) - set(names))}, "
            f"unexpected={sorted(set(names) - set(expected))}, "
            f"duplicates={[n for n, c in Counter(names).items() if c != 1]}"
        )
    rejected = []
    for name, case in zip(names, cases):
        if (case.get("status") != "run" or case.get("result") != "completed"
                or any(child.tag in {"failure", "error", "skipped"} for child in case)):
            rejected.append(name)
    if rejected:
        raise ValueError(f"qualification cases did not pass: {', '.join(rejected)}")
    for element in [document, *document.findall("testsuite")]:
        for attribute in ("failures", "disabled", "errors"):
            if element.get(attribute) != "0":
                raise ValueError(f"GoogleTest reports nonzero or missing {attribute}")
        count = len(list(element.iter("testcase")))
        if element.get("tests") != str(count):
            raise ValueError("GoogleTest total differs from actual testcase count")
    if exit_code != 0:
        raise ValueError(f"test process failed with exit code {exit_code}")
    return {"passed": len(cases), "failed": 0, "skipped": 0, "tests": names}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--neverd", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    names = identities(json.loads(MANIFEST.read_text()))
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("this execution profile requires a native Linux x86-64 runner")
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    xml = output / "results.xml"
    # Clear prior success before any preflight that can reject this invocation.
    xml.unlink(missing_ok=True)
    (output / "qualification.json").unlink(missing_ok=True)
    binary, neverd = options.binary.resolve(), options.neverd.resolve()
    for path in (binary, neverd, ROOT / PARITY_IMAGE):
        if not path.is_file():
            raise ValueError(f"missing qualification input: {path}")
    tools = {}
    for name in ("clang", "gcc", "swiftc", "llvm-objcopy"):
        path = shutil.which(name)
        if not path:
            raise ValueError(f"missing qualification tool: {name}")
        tools[name] = {"path": str(Path(path).resolve()), "version": command(path, "--version")}
    clang = re.search(r"clang version (\d+)", tools["clang"]["version"])
    if not clang or int(clang[1]) < 23:
        raise ValueError("LLVM 23 generated IR requires Clang 23 or newer")
    if "clang" in tools["gcc"]["version"].lower():
        raise ValueError("GCC differential controls require actual GNU GCC")
    # The Rust runtime is a pinned, independently produced AArch64 program.
    # Decode its actual .text rather than supplying synthetic parity words.
    parity = output / "aarch64-rust-runtime.text"
    parity_source_digest = sha256(ROOT / PARITY_IMAGE)
    subprocess.run([tools["llvm-objcopy"]["path"], "--dump-section", f".text={parity}",
                    str(ROOT / PARITY_IMAGE), str(output / "parity-image.so")], check=True)
    if sha256(ROOT / PARITY_IMAGE) != parity_source_digest:
        raise ValueError("parity extraction changed the pinned corpus image")
    if parity.stat().st_size < 4096:
        raise ValueError("AArch64 parity text is missing or unexpectedly small")
    report = {
        "schema": 1, "commit": command("git", "-C", str(ROOT), "rev-parse", "HEAD"),
        "corpus_commit": command("git", "-C", str(ROOT / "unittests/corpus"), "rev-parse", "HEAD"),
        "host": {"system": platform.system(), "machine": platform.machine()},
        "binary_sha256": sha256(binary), "neverd_sha256": sha256(neverd),
        "inventory_sha256": sha256(MANIFEST), "tools": tools,
        "parity_image": str(PARITY_IMAGE), "parity_sha256": parity_source_digest,
    }
    env = dict(os.environ, NEVERD_PARITY_BIN=str(parity), NEVERD=str(neverd),
               NEVERD_SIGNATURE_CACHE="off",
               TMPDIR=str(output / "tmp"))
    Path(env["TMPDIR"]).mkdir(exist_ok=True)
    with (output / "execution.log").open("w") as log:
        result = subprocess.run([str(binary), "--gtest_filter=" + ":".join(names),
                                 f"--gtest_output=xml:{xml}"],
                                env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600)
    try:
        report["outcome"] = audit(ET.parse(xml).getroot(), names, result.returncode)
    except (ValueError, ET.ParseError, FileNotFoundError) as error:
        report["error"] = str(error)
        (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
        raise
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Lift qualification: {len(names)} passed, zero failed, zero skipped")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, subprocess.SubprocessError, OSError) as error:
        sys.exit(str(error))
