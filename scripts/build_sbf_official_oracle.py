#!/usr/bin/env python3
"""Build the process-external oracle from NeverD's pinned Anza sbpf tree."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


EXAMPLE_NAME = "neverd_sbf_official_oracle"
PROTOCOL_RS_NAME = "SBFOfficialOracleProtocol.rs"
FAULTS_RS_NAME = "SBFOfficialExecutionFaults.rs"
EXECUTION_CONSTANTS_RS_NAME = "SBFOfficialExecutionConstants.rs"
VERSIONS_RS_NAME = "SBFOfficialOracleVersions.rs"
REVISION_PATTERN = re.compile(
    r'SBF_UPSTREAM_SOURCE\(\s*SBPFMain,\s*"[^"]+",\s*'
    r'"([0-9a-f]{40})"\s*\)',
    re.MULTILINE,
)
TEST_VECTORS_REVISION_PATTERN = re.compile(
    r'SBF_UPSTREAM_SOURCE\(\s*FiredancerTestVectors,\s*"[^"]+",\s*'
    r'"([0-9a-f]{40})"\s*\)',
    re.MULTILINE,
)
TOOLCHAIN_PATTERN = re.compile(
    r'SBF_UPSTREAM_TOOLCHAIN\(\s*OfficialOracleRust,\s*"rust",\s*'
    r'"([^"]+)"\s*\)',
    re.MULTILINE,
)
PROTOCOL_PATTERN = re.compile(
    r"SBF_OFFICIAL_ORACLE_STRING\(\s*[A-Za-z0-9_]+\s*,\s*"
    r'([A-Z][A-Z0-9_]*)\s*,\s*"([^"]*)"\s*\)',
    re.MULTILINE,
)
VERSION_PATTERN = re.compile(
    r"SBF_VERSION\(\s*([A-Za-z][A-Za-z0-9_]*)\s*,\s*[^,]+,\s*"
    r'"([^"]+)"',
    re.MULTILINE,
)
FAULT_PATTERN = re.compile(
    r"SBF_OFFICIAL_EXECUTION_FAULT\(\s*[A-Za-z0-9_]+\s*,\s*"
    r'([A-Z][A-Z0-9_]*)\s*,\s*"([^"]*)"\s*\)',
    re.MULTILINE,
)
EXECUTION_CONSTANT_PATTERN = re.compile(
    r"SBF_OFFICIAL_EXECUTION_CONSTANT\(\s*[A-Za-z0-9_]+\s*,\s*"
    r"([A-Z][A-Z0-9_]*)\s*,\s*([a-z][a-z0-9_]*)\s*,\s*"
    r"[A-Za-z_][A-Za-z0-9_:]*\s*,\s*([^\s,)]+)\s*\)",
    re.MULTILINE,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    metadata = parser.add_mutually_exclusive_group()
    metadata.add_argument("--print-pinned-revision", action="store_true")
    metadata.add_argument("--print-test-vectors-revision", action="store_true")
    metadata.add_argument("--print-toolchain", action="store_true")
    parser.add_argument("--sbpf-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--toolchain")
    return parser.parse_args()


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, text=True, **kwargs)


def upstream_metadata_value(
    repository: Path, pattern: re.Pattern[str], description: str
) -> str:
    sources = repository / "include/neverd/sbf/runtime/SBFUpstreamSources.def"
    matches = pattern.findall(sources.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one {description} in {sources}, found {len(matches)}"
        )
    return matches[0]


def pinned_revision(repository: Path) -> str:
    return upstream_metadata_value(repository, REVISION_PATTERN, "SBPFMain revision")


def pinned_toolchain(repository: Path) -> str:
    return upstream_metadata_value(
        repository, TOOLCHAIN_PATTERN, "official oracle Rust toolchain"
    )


def pinned_test_vectors_revision(repository: Path) -> str:
    return upstream_metadata_value(
        repository,
        TEST_VECTORS_REVISION_PATTERN,
        "FiredancerTestVectors revision",
    )


def render_rust_protocol(protocol: Path) -> str:
    records = PROTOCOL_PATTERN.findall(protocol.read_text(encoding="utf-8"))
    if not records:
        raise RuntimeError(f"cannot find protocol records in {protocol}")
    names = [name for name, _ in records]
    if len(names) != len(set(names)):
        raise RuntimeError(f"duplicate Rust protocol identifier in {protocol}")
    lines = [
        "// Generated from NeverD's SBFOfficialOracleProtocol.def.",
        "// Do not edit this detached-worktree artifact.",
        "",
    ]
    for name, value in records:
        lines.extend(
            (
                "#[allow(dead_code)]",
                f"const {name}: &str = {json.dumps(value)};",
            )
        )
    return "\n".join(lines) + "\n"


def render_rust_versions(versions: Path) -> str:
    records = VERSION_PATTERN.findall(versions.read_text(encoding="utf-8"))
    if not records:
        raise RuntimeError(f"cannot find concrete SBF versions in {versions}")
    names = [name for name, _ in records]
    spellings = [spelling for _, spelling in records]
    if len(names) != len(set(names)) or len(spellings) != len(set(spellings)):
        raise RuntimeError(f"duplicate SBF version identity in {versions}")
    lines = [
        "// Generated from NeverD's SBFVersions.def.",
        "// A missing upstream enum variant intentionally fails the oracle build.",
        "",
        "fn parse_generated_version(value: &str) -> Option<SBPFVersion> {",
        "    match value {",
    ]
    for name, spelling in records:
        lines.append(f"        {json.dumps(spelling)} => Some(SBPFVersion::{name}),")
    lines.extend(("        _ => None,", "    }", "}", ""))
    return "\n".join(lines)


def render_rust_faults(faults: Path) -> str:
    records = FAULT_PATTERN.findall(faults.read_text(encoding="utf-8"))
    if not records:
        raise RuntimeError(f"cannot find execution fault records in {faults}")
    names = [name for name, _ in records]
    spellings = [spelling for _, spelling in records]
    if len(names) != len(set(names)) or len(spellings) != len(set(spellings)):
        raise RuntimeError(f"duplicate execution fault identity in {faults}")
    lines = [
        "// Generated from NeverD's SBFOfficialExecutionFaults.def.",
        "// Do not edit this detached-worktree artifact.",
        "",
    ]
    for name, spelling in records:
        lines.extend(
            (
                "#[allow(dead_code)]",
                f"const {name}: &str = {json.dumps(spelling)};",
            )
        )
    return "\n".join(lines) + "\n"


def render_rust_execution_constants(constants: Path) -> str:
    records = EXECUTION_CONSTANT_PATTERN.findall(constants.read_text(encoding="utf-8"))
    if not records:
        raise RuntimeError(f"cannot find execution constants in {constants}")
    names = [name for name, _, _ in records]
    if len(names) != len(set(names)):
        raise RuntimeError(f"duplicate Rust execution constant in {constants}")
    lines = [
        "// Generated from NeverD's SBFOfficialExecutionConstants.def.",
        "// Do not edit this detached-worktree artifact.",
        "",
    ]
    for name, rust_type, value in records:
        lines.extend(
            (
                "#[allow(dead_code)]",
                f"const {name}: {rust_type} = {value};",
            )
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    arguments = parse_arguments()
    repository = Path(__file__).resolve().parents[1]
    if arguments.print_pinned_revision:
        print(pinned_revision(repository))
        return
    if arguments.print_test_vectors_revision:
        print(pinned_test_vectors_revision(repository))
        return
    if arguments.print_toolchain:
        print(pinned_toolchain(repository))
        return
    if arguments.sbpf_root is None or arguments.output is None:
        raise RuntimeError("--sbpf-root and --output are required when building")
    root = arguments.sbpf_root.resolve()
    output = arguments.output.resolve()
    source = repository / "unittests/sbf/oracle/SBFOfficialOracle.rs"
    protocol = repository / "unittests/sbf/SBFOfficialOracleProtocol.def"
    faults = repository / "unittests/sbf/SBFOfficialExecutionFaults.def"
    execution_constants = repository / "unittests/sbf/SBFOfficialExecutionConstants.def"
    versions = repository / "include/neverd/sbf/image/SBFVersions.def"
    revision = pinned_revision(repository)
    toolchain = arguments.toolchain or pinned_toolchain(repository)

    for required in (
        root / ".git",
        root / "Cargo.toml",
        root / "Cargo.lock",
        source,
        protocol,
        faults,
        execution_constants,
        versions,
    ):
        if not required.exists():
            raise RuntimeError(f"required path does not exist: {required}")
    run(["git", "-C", str(root), "cat-file", "-e", f"{revision}^{{commit}}"])

    with tempfile.TemporaryDirectory(prefix="neverd-sbpf-oracle-") as temporary:
        temporary_root = Path(temporary)
        checkout = temporary_root / "upstream"
        target = temporary_root / "target"
        worktree_added = False
        try:
            run(
                [
                    "git",
                    "-C",
                    str(root),
                    "worktree",
                    "add",
                    "--detach",
                    str(checkout),
                    revision,
                ]
            )
            worktree_added = True
            shutil.copy2(source, checkout / "examples" / f"{EXAMPLE_NAME}.rs")
            (checkout / "examples" / PROTOCOL_RS_NAME).write_text(
                render_rust_protocol(protocol), encoding="utf-8"
            )
            (checkout / "examples" / FAULTS_RS_NAME).write_text(
                render_rust_faults(faults), encoding="utf-8"
            )
            (checkout / "examples" / EXECUTION_CONSTANTS_RS_NAME).write_text(
                render_rust_execution_constants(execution_constants), encoding="utf-8"
            )
            (checkout / "examples" / VERSIONS_RS_NAME).write_text(
                render_rust_versions(versions), encoding="utf-8"
            )
            environment = os.environ.copy()
            environment["NEVERD_SBPF_ORACLE_REVISION"] = revision
            run(
                [
                    arguments.cargo,
                    f"+{toolchain}",
                    "build",
                    "--manifest-path",
                    str(checkout / "Cargo.toml"),
                    "--locked",
                    "--release",
                    "--example",
                    EXAMPLE_NAME,
                    "--target-dir",
                    str(target),
                ],
                env=environment,
            )
            suffix = ".exe" if os.name == "nt" else ""
            binary = target / "release" / "examples" / f"{EXAMPLE_NAME}{suffix}"
            output.parent.mkdir(parents=True, exist_ok=True)
            staged_output = output.with_name(f".{output.name}.tmp")
            shutil.copy2(binary, staged_output)
            os.replace(staged_output, output)
        finally:
            if worktree_added:
                subprocess.run(
                    [
                        "git",
                        "-C",
                        str(root),
                        "worktree",
                        "remove",
                        "--force",
                        str(checkout),
                    ],
                    check=False,
                )

    print(f"built {output} from anza-xyz/sbpf {revision}")


if __name__ == "__main__":
    main()
