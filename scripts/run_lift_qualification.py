#!/usr/bin/env python3
"""Run every previously unqualified Lift case and reject incomplete evidence.

This profile owns Linux x86-64 execution, including real i386 processes and
unmasked SSE traps. The normal cross-platform suites keep their host guards.
"""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


if __package__:
    from . import verification_report
else:
    import verification_report


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "unittests/Inputs/lift-qualification.json"
PARITY_IMAGE = Path(
    "unittests/corpus/corpus/rust-eh/aarch64-unknown-linux-gnu/"
    "unwind/o2/cdylib/rust_eh_cdylib-aarch64-unknown-linux-gnu-unwind-o2.so"
)


def identities(document: dict) -> tuple[str, ...]:
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise ValueError("unsupported qualification inventory schema")
    cases = document.get("tests", [])
    if not isinstance(cases, list) or not all(
            isinstance(case, dict) and isinstance(case.get("name"), str)
            and isinstance(case.get("baseline"), str) for case in cases):
        raise ValueError("qualification inventory requires named test outcomes")
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


class QualificationCasesFailed(ValueError):
    """All rejected identities, with the first case's recorded diagnostic."""

    def __init__(self, rejected: list[str], name: str, reason: str, blocked: bool,
                 missing: str | None):
        self.stage = {"path": ["case audit", name], "state": "blocked" if blocked else "failed",
                      "diagnostic": reason}
        if missing or blocked:
            self.stage["missing_evidence"] = missing or f"completed execution of {name}"
        super().__init__(f"qualification cases did not pass: {', '.join(rejected)}"
                         f" ({name}: {reason})")


class QualificationError(ValueError):
    def __init__(self, diagnostic: str, missing_evidence: str | None = None):
        super().__init__(diagnostic)
        self.missing_evidence = missing_evidence


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
    first_failure = None
    for name, case in zip(names, cases):
        problem = next((child for child in case if child.tag in
                        {"failure", "error", "skipped"}), None)
        status, result = case.get("status"), case.get("result")
        if status == "run" and result == "completed" and problem is None:
            continue
        rejected.append(name)
        if first_failure is None:
            reason = ""
            if problem is not None:
                reason = ((problem.get("message") or "").strip()
                          or "".join(problem.itertext()).strip() or problem.tag)
            blocked = (problem.tag == "skipped" if problem is not None
                       else status != "run" or result != "completed")
            required = case.find("./properties/property[@name='neverd_missing_evidence']")
            missing = required.get("value") if required is not None else None
            first_failure = (name, reason or f"status={status}, result={result}", blocked, missing)
    if first_failure is not None:
        raise QualificationCasesFailed(rejected, *first_failure)
    for element in [document, *document.findall("testsuite")]:
        for attribute in ("failures", "disabled", "errors"):
            if element.get(attribute) != "0":
                raise ValueError(f"GoogleTest reports nonzero or missing {attribute}")
        count = len(list(element.iter("testcase")))
        if element.get("tests") != str(count):
            raise ValueError("GoogleTest total differs from actual testcase count")
    if exit_code != 0:
        raise QualificationError(f"test process failed with exit code {exit_code}")
    return {"passed": len(cases), "failed": 0, "skipped": 0, "tests": names}


RESULT_CHECKS = {
    "results": "fresh GoogleTest XML from the test process",
    "result metadata": "valid run-level execution evidence when supplied",
    "case audit": "a successful result for every discovered test identity",
}


def recorded_execution(document: ET.Element) -> dict:
    # A property on a single child test cannot speak for the entire run.
    properties = document.findall("./properties/property[@name='neverd_execution_evidence']")
    if not properties:
        return {}
    if len(properties) != 1 or properties[0].get("value") is None:
        raise ValueError("expected one run-level neverd_execution_evidence value")
    return verification_report.execution_evidence(json.loads(properties[0].get("value")))


class QualificationReport:
    def __init__(self, requirements: dict[str, str]):
        self.requirements = requirements
        self.stages = {name: {"path": [name], "state": "not_run"} for name in requirements}
        self.observations = {}
        self.exit_code = None

    @contextmanager
    def check(self, name: str):
        stage = self.stages[name]
        stage["state"] = "unknown"
        try:
            yield
        except QualificationCasesFailed as error:
            self.stages[name] = error.stage
            raise
        except QualificationError as error:
            stage.update(state="failed", diagnostic=str(error))
            if error.missing_evidence:
                stage.update(state="blocked", missing_evidence=error.missing_evidence)
            raise
        except (OSError, ValueError, ET.ParseError, subprocess.SubprocessError) as error:
            diagnostic = str(error)
            if isinstance(error, subprocess.CalledProcessError) and error.stdout:
                diagnostic += "\n" + str(error.stdout).strip()
            if name == "results" and self.exit_code is not None:
                diagnostic += f" (process exit {self.exit_code})"
            stage.update(state="blocked", diagnostic=diagnostic,
                         missing_evidence=self.requirements[name])
            raise
        else:
            stage["state"] = "passed"

    def run_tests(self, args: list[str], xml: Path, names: tuple[str, ...], **options) -> dict:
        with self.check("results"):
            result = subprocess.run([*args, "--gtest_output=xml:" + str(xml)], **options)
            self.exit_code = result.returncode
            document = ET.parse(xml).getroot()
        with self.check("result metadata"):
            self.observations = recorded_execution(document)
        with self.check("case audit"):
            return audit(document, names, result.returncode)

    def write(self, path: Path):
        result = verification_report.summarize({"schema": verification_report.SCHEMA,
                                                "stages": list(self.stages.values()),
                                                **self.observations})
        with tempfile.TemporaryDirectory(dir=path.parent) as directory:
            temporary = Path(directory) / path.name
            temporary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            temporary.replace(path)


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
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    paths = [output / name for name in ("results.xml", "qualification.json", "verification.json")]
    if any(path in (options.binary.resolve(), options.neverd.resolve()) for path in paths):
        raise ValueError("qualification output must not overwrite its input binaries")
    for path in paths:
        path.unlink(missing_ok=True)
    progress = QualificationReport({
        "inventory": "the complete qualification inventory",
        "host": "a native Linux x86-64 runner",
        "inputs": "both test binaries and the pinned parity fixture",
        "toolchain": "the required compiler and object-copy tool versions",
        "parity": "the text section of the pinned parity fixture",
        "provenance": "source revisions and input hashes",
        "execution files": "a writable execution log and temporary directory",
        **RESULT_CHECKS,
    })
    try:
        return run_profile(options, output, progress)
    finally:
        progress.write(output / "verification.json")


def run_profile(options, output: Path, progress: QualificationReport) -> int:
    with progress.check("inventory"):
        names = identities(json.loads(MANIFEST.read_text()))
    with progress.check("host"):
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            raise ValueError("this execution profile requires a native Linux x86-64 runner")
    with progress.check("inputs"):
        binary, neverd = options.binary.resolve(), options.neverd.resolve()
        for path in (binary, neverd, ROOT / PARITY_IMAGE):
            if not path.is_file():
                raise QualificationError(f"missing qualification input: {path}", str(path))
    with progress.check("toolchain"):
        tools = {}
        for name in ("clang", "gcc", "swiftc", "llvm-objcopy"):
            path = shutil.which(name)
            if not path:
                raise QualificationError(f"missing qualification tool: {name}", name)
            tools[name] = {"path": str(Path(path).resolve()), "version": command(path, "--version")}
        clang = re.search(r"clang version (\d+)", tools["clang"]["version"])
        if not clang or int(clang[1]) < 23:
            raise QualificationError("LLVM 23 generated IR requires Clang 23 or newer",
                                     "Clang 23 or newer")
        if "clang" in tools["gcc"]["version"].lower():
            raise QualificationError("GCC differential controls require actual GNU GCC", "GNU GCC")
    with progress.check("parity"):
        # The Rust runtime is a pinned, independently produced AArch64 program.
        # Decode its actual .text rather than supplying synthetic parity words.
        parity = output / "aarch64-rust-runtime.text"
        parity_source_digest = sha256(ROOT / PARITY_IMAGE)
        subprocess.run([tools["llvm-objcopy"]["path"], "--dump-section", f".text={parity}",
                        str(ROOT / PARITY_IMAGE), str(output / "parity-image.so")], check=True)
        if sha256(ROOT / PARITY_IMAGE) != parity_source_digest:
            raise QualificationError("parity extraction changed the pinned corpus image")
        if parity.stat().st_size < 4096:
            raise ValueError("AArch64 parity text is missing or unexpectedly small")
    with progress.check("provenance"):
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
    try:
        with progress.check("execution files"):
            Path(env["TMPDIR"]).mkdir(exist_ok=True)
            log = (output / "execution.log").open("w")
        with log:
            report["outcome"] = progress.run_tests(
                [str(binary), "--gtest_filter=" + ":".join(names)], output / "results.xml",
                names, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=3600)
    except (ValueError, ET.ParseError, OSError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        if isinstance(error, QualificationCasesFailed):
            report["first_blocker"] = error.stage
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
