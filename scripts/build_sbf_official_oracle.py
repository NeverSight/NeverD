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


try:
    from .sbf_artifacts import ArtifactWorkspace, run_owned
    from .sbf_corpus_common import sha256
except ImportError:
    from sbf_artifacts import ArtifactWorkspace, run_owned
    from sbf_corpus_common import sha256


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
    return run_owned(command, check=True, text=True, **kwargs)


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

    build_cached_oracle(root, output, revision, toolchain, arguments.cargo,
                        source, protocol, faults, execution_constants, versions)

    print(f"built {output} from anza-xyz/sbpf {revision}")


def build_cached_oracle(root, output, revision, toolchain, cargo,
                        source, protocol, faults, execution_constants, versions):
    """Reuse one owned checkout/target; Cargo still validates every invocation."""
    output.parent.mkdir(parents=True, exist_ok=True)
    if os.name != "posix":
        # Preserve the existing disposable Windows build. POSIX leases are not
        # a cross-platform guarantee about inherited child handles.
        with tempfile.TemporaryDirectory(prefix="neverd-sbpf-oracle-", dir=output.parent) as temporary:
            build_oracle(Path(temporary), None, root, output, revision, toolchain,
                         cargo, source, protocol, faults, execution_constants, versions)
        return
    with ArtifactWorkspace(output.parent) as artifacts:
        cache = artifacts.retained_path("oracle-cache")
        if cache is None:
            artifacts.begin("oracle-cache")
            cache = artifacts.own(output.parent / ".sbpf-oracle-build", directory=True)
            artifacts.keep("oracle-cache")
            artifacts.finish()
        artifacts.begin("oracle-publication", {"revision": revision, "toolchain": toolchain})
        build_oracle(cache, artifacts, root, output, revision, toolchain, cargo,
                     source, protocol, faults, execution_constants, versions)


def build_oracle(cache, artifacts, root, output, revision, toolchain, cargo,
                 source, protocol, faults, execution_constants, versions):
    checkout, target = cache / "upstream", cache / "target"
    inputs = {"source": str(root.resolve()), "revision": revision, "toolchain": toolchain}
    previous = cache / "inputs.json"
    values = {f"{EXAMPLE_NAME}.rs": source.read_text(),
              PROTOCOL_RS_NAME: render_rust_protocol(protocol),
              FAULTS_RS_NAME: render_rust_faults(faults),
              EXECUTION_CONSTANTS_RS_NAME: render_rust_execution_constants(execution_constants),
              VERSIONS_RS_NAME: render_rust_versions(versions)}
    if checkout.exists():
        origin = run(["git", "-C", str(checkout), "remote", "get-url", "origin"],
                     capture_output=True).stdout.strip()
        if Path(origin).resolve() != root.resolve():
            raise RuntimeError("oracle cache belongs to another source checkout")
        changed = run(["git", "-C", str(checkout), "status", "--porcelain=v1",
                       "--untracked-files=all"], capture_output=True).stdout.splitlines()
        generated = {"examples/" + n for n in values}
        if any(line[3:] not in generated for line in changed):
            raise RuntimeError("oracle cache has unrelated changes; preserve them")
        head = run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture_output=True).stdout.strip()
        if head != revision:
            run(["git", "-C", str(checkout), "fetch", "--no-tags", "origin", revision])
            run(["git", "-C", str(checkout), "checkout", "--detach", revision])
    else:
        # A private shared-object clone does not register or prune worktrees in
        # the caller's repository. Partial creation remains a disposable bundle.
        staging = cache / "checkout-staging"
        if artifacts:
            artifacts.own(staging, directory=True)
        else:
            staging.mkdir()
        run(["git", "clone", "--shared", "--no-checkout", str(root), str(staging / "repo")])
        run(["git", "-C", str(staging / "repo"), "checkout", "--detach", revision])
        (staging / "repo").rename(checkout)
    if previous.exists() and json.loads(previous.read_text()) != inputs:
        if target.is_symlink():
            raise RuntimeError("oracle target cache is a symlink; preserve it")
        if target.exists():
            shutil.rmtree(target)
    temporary = cache / "inputs.tmp"
    temporary.write_text(json.dumps(inputs) + "\n")
    temporary.replace(previous)
    (checkout / "examples").mkdir(exist_ok=True)
    for name, value in values.items():
        path = checkout / "examples" / name
        if not path.exists() or path.read_text() != value:
            path.write_text(value, encoding="utf-8")
    environment = os.environ.copy()
    environment["NEVERD_SBPF_ORACLE_REVISION"] = revision
    run([cargo, f"+{toolchain}", "build", "--manifest-path", str(checkout / "Cargo.toml"),
         "--locked", "--release", "--example", EXAMPLE_NAME, "--target-dir", str(target)],
        env=environment)
    suffix = ".exe" if os.name == "nt" else ""
    binary = target / "release/examples" / f"{EXAMPLE_NAME}{suffix}"
    digest = sha256(binary)
    if not output.exists() or sha256(output) != digest:
        staged = (artifacts.own(output.with_name(f".{output.name}.tmp")) if artifacts
                  else cache / "publication.tmp")
        shutil.copy2(binary, staged)
        if sha256(staged) != digest:
            raise RuntimeError("oracle publication copy differs")
        os.replace(staged, output)
    if artifacts:
        artifacts.state["entries"][artifacts.run_id]["metadata"].update(
            output=str(output), output_sha256=digest, cache=str(cache))


if __name__ == "__main__":
    main()
