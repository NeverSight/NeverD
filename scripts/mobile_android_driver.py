#!/usr/bin/env python3
"""Run local Android verification requests through the owned artifact lifecycle.

The private configuration names fixed job slots and output parents. Requests
contain commands and immutable input hashes; sensitive paths stay outside Git.
Use named readers for encoding checks and frozen full-source audits. Reusing a
slot is refused until its readers finish, rather than creating another retry
directory. SDK staging promotes only the requested result before retiring work.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

try:
    from scripts.mobile_android_artifacts import ArtifactStore, digest, read_json, write_json
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_android_artifacts import ArtifactStore, digest, read_json, write_json


def validate_inputs(rows: list[dict]) -> list[Path]:
    paths = []
    for row in rows:
        path = Path(row["path"])
        if not path.is_absolute() or digest(path) != row["sha256"]:
            raise ValueError("frozen Android input changed")
        paths.append(path)
    return paths


def run_request(config: dict, request: dict) -> dict:
    slot = request["slot"]
    if slot not in config["slots"]:
        raise ValueError("request must reuse a configured job slot")
    inputs = validate_inputs(request.get("inputs", []))
    outputs = [Path(p) for p in request.get("output_directories", [])]
    parents = {Path(p).resolve() for p in config.get("output_parents", [])}
    if any(not p.is_absolute() or p.parent.resolve() not in parents for p in outputs):
        raise ValueError("output directory is outside the configured producer scope")
    store = ArtifactStore(Path(config["state_root"]), Path(config["work_root"]))
    publication = request.get("publish")
    retire = request.get("retire_sdk_aliases", [])
    if any(alias not in config.get("sdk_aliases", []) for alias in retire):
        raise ValueError("unknown retired SDK declaration role")
    if publication:
        if publication["alias"] not in config.get("sdk_aliases", []):
            raise ValueError("unknown SDK declaration role")
        output = Path(publication["file"])
        if output.is_absolute() or ".." in output.parts:
            raise ValueError("SDK result must be inside the job work directory")
    job = store.begin(slot, inputs, request.get("readers", []),
                      request.get("success_readers", []), outputs)
    if "context" in request:
        job.state["context"] = request["context"]
        with store.lock():
            write_json(store._record(slot), job.state)
    promoted = []

    def after_success(work):
        validate_inputs(request.get("inputs", []))
        if publication:
            source = work / publication["file"]
            if source.is_symlink():
                raise ValueError("SDK result must not redirect outside owned work")
            if not source.resolve().is_relative_to(work.resolve()):
                raise ValueError("SDK result escaped owned work")
            sha = digest(source)
            promoted.append(store.stage(publication["alias"], source, sha, {
                "request_sha256": request_hash(request), "inputs": request.get("inputs", [])}))
        for alias in retire:
            store.retire_cache(alias)

    result = job.run(request["command"], cwd=config["cwd"],
                     timeout=request.get("timeout_seconds"), after_success=after_success)
    validate_inputs(request.get("inputs", []))
    if promoted:
        result["staged_input"] = str(promoted[0])
        result["staged_input_sha256"] = digest(promoted[0])
    return result


def request_hash(request):
    return hashlib.sha256(json.dumps(request, sort_keys=True).encode()).hexdigest()


def finish_probe_receipt(config: dict, store: ArtifactStore) -> None:
    """Complete the frozen auditor's receipt, including after a driver restart."""
    record = store.root / "jobs/probe.json"
    if not record.exists():
        return
    state = read_json(record)
    if state["phase"] == "running" or "probe-receipt" not in state["readers"]:
        return
    if "command" not in state:
        store.release("probe", "probe-receipt")
        return
    command = state["command"]
    output = Path(command[command.index("-o") + 1])
    match = re.fullmatch(r"neverd-multidex-probe-(\d+)", output.name)
    if not match or output.parent != Path(config["probe"]["output_root"]):
        raise ValueError("probe output differs from its registered auditor path")
    number = int(match.group(1))
    inputs = {Path(row["path"]).resolve(): row["sha256"] for row in state["inputs"]}
    classpath = [command[i + 1] for i, arg in enumerate(command) if arg == "--classpath"]
    cwd = Path(config["cwd"])
    receipt = {"probe": number, "command": command, "exit_code": state["returncode"],
               "seconds": state.get("elapsed_seconds"),
               "input_sha256": inputs[(cwd / command[2]).resolve()],
               "cli_sha256": inputs[(cwd / command[0]).resolve()],
               "classpath": [{"path": p, "sha256": inputs[(cwd / p).resolve()]} for p in classpath],
               "lifecycle_phase": state["phase"]}
    with store.reader("probe", "probe-receipt") as work:
        raw = (work / "driver.log").read_bytes() if (work / "driver.log").exists() else b""
        receipt["stdout_sha256"] = hashlib.sha256(raw).hexdigest()
        if state["phase"] == "success":
            if not (output / "report.json").is_file():
                raise ValueError("successful probe did not emit the frozen audit input")
            log = {"status": "success", "report": str(output / "report.json"),
                   "report_sha256": digest(output / "report.json"),
                   "original_stdout_sha256": receipt["stdout_sha256"]}
        else:
            try:
                parsed = json.loads(raw)
                log = ({"error": parsed["error"]} if isinstance(parsed, dict) and "error" in parsed
                       else {"status": state["phase"], "original_stdout_sha256": receipt["stdout_sha256"],
                             "diagnostic_tail": raw[-16384:].decode(errors="replace")})
            except (json.JSONDecodeError, UnicodeDecodeError):
                log = {"status": state["phase"], "diagnostic_tail": raw[-16384:].decode(errors="replace")}
        # Retain two compact comparison records, not one full archive per retry.
        current = store.root / "probe-current.json"
        prior = read_json(current) if current.exists() else None
        result = {"receipt": receipt, "diagnostic": log}
        if prior is not None and prior["receipt"]["probe"] != number:
            write_json(store.root / "probe-previous.json", prior)
        write_json(current, result)
        ledger = store.root / "probe-compatibility.json"
        recorded = read_json(ledger) if ledger.exists() else {"current": {}, "retiring": {}}
        for path, sha in recorded["retiring"].items():
            if Path(path).exists():
                if digest(Path(path)) != sha:
                    raise ValueError("retired compatibility receipt changed")
                Path(path).unlink()
        old = recorded["current"]
        receipt_root = Path(config["probe"]["receipt_root"])
        paths = {receipt_root / f"neverd-multidex-probe-{number}.command.json": receipt,
                 receipt_root / f"neverd-multidex-probe-{number}.log": log}
        claimed = state["context"]["probe_compatibility_paths"]
        if set(claimed) != {str(path) for path in paths}:
            raise ValueError("probe receipt paths differ from the producer's prior ownership record")
        for path, data in paths.items():
            expected = hashlib.sha256((json.dumps(data, sort_keys=True, indent=2) + "\n").encode()).hexdigest()
            if path.exists() and str(path) not in old and digest(path) != expected:
                raise ValueError("partially completed probe receipt changed")
            if str(path) in old and digest(path) != old[str(path)]:
                raise ValueError("owned compatibility receipt changed")
            write_json(path, data)
        retiring = {p: sha for p, sha in old.items() if Path(p) not in paths}
        current_files = {str(path): digest(path) for path in paths}
        write_json(ledger, {"current": current_files, "retiring": retiring})
        for path, sha in retiring.items():
            if Path(path).exists():
                if digest(Path(path)) != sha:
                    raise ValueError("retired compatibility receipt changed")
                Path(path).unlink()
        write_json(ledger, {"current": current_files, "retiring": {}})
    store.release("probe", "probe-receipt")


def run_probe(config: dict, number: int, cli_receipt: Path) -> dict:
    settings = config["probe"]
    template = read_json(Path(settings["template"]))
    sdk = read_json(Path(settings["classpath_manifest"]))
    if sdk["status"] != "verified" or sdk["sdk_and_application_execution"]:
        raise ValueError("probe requires the already qualified declaration classpath")
    if number <= template["probe"]:
        raise ValueError("probe number must advance the frozen baseline")
    store = ArtifactStore(Path(config["state_root"]), Path(config["work_root"]))
    finish_probe_receipt(config, store)
    current = store.root / "probe-current.json"
    if current.exists() and number <= read_json(current)["receipt"]["probe"]:
        raise ValueError("probe number must advance the retained comparison")
    command = template["command"].copy()
    output_root = Path(settings["output_root"])
    command[command.index("-o") + 1] = str(output_root / f"neverd-multidex-probe-{number}")
    compatibility = [str(Path(settings["receipt_root"]) / f"neverd-multidex-probe-{number}{suffix}")
                     for suffix in (".command.json", ".log")]
    if any(Path(p).exists() for p in compatibility):
        raise ValueError("new probe receipt path already exists")
    old_classpath = [command[i + 1] for i, arg in enumerate(command) if arg == "--classpath"]
    if old_classpath != [row["path"] for row in sdk["classpath"]]:
        raise ValueError("probe template classpath differs from the qualified manifest")
    # Keep the frozen native argv verbatim, while recording input identities
    # against the same working directory used to start the native process.
    cwd = Path(config["cwd"])
    cli = cwd / command[0]
    if digest(cli) != read_json(cli_receipt)["sha256"]:
        raise ValueError("CLI changed since its completed build receipt")
    inputs = [{"path": str(cli), "sha256": digest(cli)},
              {"path": str(cwd / command[2]), "sha256": template["input_sha256"]}, *sdk["classpath"],
              {"path": settings["classpath_manifest"], "sha256": digest(Path(settings["classpath_manifest"]))}]
    request = {"slot": "probe", "command": command, "inputs": inputs,
               "output_directories": [str(output_root)], "readers": ["probe-receipt"],
               "success_readers": ["full-source-audit"], "timeout_seconds": 10860,
               "context": {"probe_compatibility_paths": compatibility}}
    result = run_request(config, request)
    finish_probe_receipt(config, store)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    commands = parser.add_subparsers(dest="action", required=True)
    run = commands.add_parser("run")
    run.add_argument("request", type=Path)
    release = commands.add_parser("release")
    release.add_argument("--slot", required=True)
    release.add_argument("--reader", required=True)
    commands.add_parser("recover")
    download = commands.add_parser("download")
    download.add_argument("--alias", required=True)
    download.add_argument("--url", required=True)
    download.add_argument("--sha256", required=True)
    probe = commands.add_parser("probe")
    probe.add_argument("--number", required=True, type=int)
    probe.add_argument("--cli-receipt", required=True, type=Path)
    args = parser.parse_args()
    config = read_json(args.config)
    if args.action == "run":
        result = run_request(config, read_json(args.request))
        print(json.dumps(result))
        return result["returncode"]
    if args.action == "probe":
        result = run_probe(config, args.number, args.cli_receipt)
        print(json.dumps(result))
        return result["returncode"]
    store = ArtifactStore(Path(config["state_root"]), Path(config["work_root"]))
    if "probe" in config:
        finish_probe_receipt(config, store)
    if args.action == "release":
        if args.slot not in config["slots"]:
            raise ValueError("unknown Android job slot")
        store.release(args.slot, args.reader)
    if args.action == "download":
        if args.alias not in config.get("sdk_aliases", []):
            raise ValueError("unknown SDK declaration role")
        print(store.download(args.alias, args.url, args.sha256))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
