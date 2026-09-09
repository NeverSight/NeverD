#!/usr/bin/env python3
"""Qualify the complete pinned mobile matrix, never just its initial baseline.

All paths in case evidence are relative to the directory containing result.json.
Every stage needs details.evidence (nonempty artifact references). Recompile and
behavior additionally need details.proof, referencing versioned JSON documents.

The independent-recompile proof binds source_reports, input_sources, outputs,
command_ids and dependency_manifest to the actual hashed command inputs. Its
dependency manifest lists inputs [{path, origin}] and no original implementation
inputs. Non-recovered inputs must match the app's acceptance_dependencies locks
[{origin, sha256}]; this is not inferred from a filename or an empty self-report.

The behavior-comparison proof binds original_artifact and rebuilt_artifact to
successful original/rebuilt command IDs, original_results/rebuilt_results and
expected_assertions. Result paths must be stdout of the respective commands;
independent UI/XCTest adapters can emit normalized JSON there. Each result
document has assertions [{id, obligation,
status:'pass', value}]. The expected document fixes IDs, obligations and values;
all app behavior_obligations must be covered. Runtime identity is explicit.

Both proofs also carry schema_version=1, case_id, consumer_commit,
source_commit and manifest_sha256. Qualification requirement evidence references
{case_id, path} and must contain a matching qualification proof. This protocol
checks evidence consistency; it is not a claim of universal semantic equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys


STAGES = ("provenance", "original_build", "inventory", "recovery", "recompile", "behavior")
BAD_STATES = {"failed", "failure", "error", "incomplete", "partial", "skip", "skipped",
              "cancelled", "canceled", "timeout", "timed-out", "unknown", "unclassified",
              "unsupported", "unrecovered", "not-run"}
JSON_LIMIT = 32 * 1024 * 1024
SOURCE_EXTENSIONS = {"android": {".java"}, "ios": {".c", ".cc", ".cpp", ".m", ".mm", ".swift"}}


class GateError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def object_value(value, label):
    require(isinstance(value, dict), f"{label} must be an object")
    return value


def names(value, label, *, empty=False):
    require(isinstance(value, list) and (empty or value), f"{label} must be a nonempty list")
    require(all(isinstance(item, str) and item.strip() for item in value), f"{label} contains an invalid name")
    require(len(set(value)) == len(value), f"{label} contains duplicate names")
    return value


def exact_schema(value, label):
    object_value(value, label)
    require(type(value.get("schema_version")) is int and value["schema_version"] == 1,
            f"{label} has an unsupported schema")


def digest_file(path):
    digest, size = hashlib.sha256(), 0
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode), f"not a regular artifact: {path}")
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
        after = os.fstat(stream.fileno())
    require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
            and size == after.st_size, f"artifact changed during hashing: {path}")
    return digest.hexdigest(), size


def load_json(path):
    require(path.is_file() and not path.is_symlink(), f"missing or symlink JSON evidence: {path}")
    require(path.stat().st_size <= JSON_LIMIT, f"JSON evidence exceeds size limit: {path}")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise GateError(f"non-finite JSON number: {value}")

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=invalid_constant)


def relative_name(value):
    require(isinstance(value, str) and value and "\\" not in value
            and ":" not in value and not any(ord(c) < 32 for c in value), "unsafe artifact path")
    parts = value.split("/")
    require(not value.startswith("/") and all(part not in ("", ".", "..") for part in parts),
            f"artifact path escapes its result root: {value!r}")
    return value


def clean_states(value, label):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in ("status", "coverage_status", "outcome", "conclusion") and isinstance(item, str):
                require(item.lower() not in BAD_STATES, f"{label} contains {key}={item}")
            if key in ("truncated", "skipped", "timed_out", "cancelled"):
                require(item in (False, None), f"{label} contains {key}")
            if key.endswith("_count") and key.startswith(("unknown_", "unclassified_", "unrecovered_", "skipped_", "unsupported_")):
                require(type(item) is int and item == 0, f"{label} contains nonzero/invalid {key}")
            clean_states(item, label)
    elif isinstance(value, list):
        for item in value:
            clean_states(item, label)


class CaseEvidence:
    def __init__(self, path, record, case, app, consumer, manifest_sha):
        self.path, self.root, self.record = path, path.parent, record
        self.case, self.app, self.consumer, self.manifest_sha = case, app, consumer, manifest_sha
        self.artifacts, self.commands = {}, {}

    def artifact(self, name, *, nonempty=False):
        name = relative_name(name)
        require(name in self.artifacts, f"unrecorded artifact reference: {name}")
        require(not nonempty or self.artifacts[name]["size"] > 0, f"empty evidence artifact: {name}")
        return self.root / name

    def document(self, name):
        return load_json(self.artifact(name, nonempty=True))

    def identity(self, proof, kind):
        exact_schema(proof, kind)
        require(proof.get("kind") == kind, f"expected {kind} proof, not a log or unrelated JSON")
        for key, value in (("case_id", self.case["id"]), ("consumer_commit", self.consumer),
                           ("source_commit", self.app["source_commit"]), ("manifest_sha256", self.manifest_sha)):
            require(proof.get(key) == value, f"{kind} {key} differs from the qualified case")

    def check_artifacts(self):
        rows = self.record.get("artifacts")
        require(isinstance(rows, list) and rows, "case has no artifact inventory")
        for row in rows:
            object_value(row, "artifact")
            name = relative_name(row.get("path"))
            require(name != "result.json" and name not in self.artifacts, f"duplicate/self-referential artifact: {name}")
            require(type(row.get("size")) is int and row["size"] >= 0, f"invalid artifact size: {name}")
            require(isinstance(row.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", row["sha256"]),
                    f"invalid artifact digest: {name}")
            path = self.root / name
            require(path.resolve().is_relative_to(self.root.resolve()), f"artifact escapes result root: {name}")
            require(path.is_file() and not path.is_symlink(), f"missing/symlink artifact: {name}")
            actual_sha, actual_size = digest_file(path)
            require((actual_sha, actual_size) == (row["sha256"], row["size"]), f"artifact SHA/size mismatch: {name}")
            self.artifacts[name] = row
        actual = set()
        def traversal_error(error):
            raise GateError(f"cannot inventory case artifact directory: {error}")

        for directory, dirs, files in os.walk(self.root, followlinks=False, onerror=traversal_error):
            for name in [*dirs, *files]:
                require(not (Path(directory) / name).is_symlink(), "symlink inside case evidence")
            for name in files:
                path = Path(directory) / name
                if path != self.path:
                    actual.add(path.relative_to(self.root).as_posix())
        require(actual == set(self.artifacts), "artifact inventory omits or invents files")

    def check_commands(self):
        rows = self.record.get("commands")
        require(isinstance(rows, list) and rows, "case has no executed commands")
        for row in rows:
            object_value(row, "command")
            key = row.get("id")
            require(isinstance(key, str) and key and key not in self.commands, "missing or duplicate command id")
            require(row.get("status") == "success" and type(row.get("exitcode")) is int and row["exitcode"] == 0,
                    f"command {key} failed, timed out or did not execute")
            require(isinstance(row.get("argv"), list) and row["argv"]
                    and all(isinstance(arg, str) and arg for arg in row["argv"]), f"invalid command argv: {key}")
            self.artifact(row.get("stdout"))
            self.artifact(row.get("stderr"))
            clean_states(row, f"command {key}")
            self.commands[key] = row

    def command_paths(self, ids):
        ids = names(ids, "proof command_ids")
        root = self.record.get("work_directory")
        require(isinstance(root, str) and root.startswith("/"), "case lacks original absolute work_directory")
        base = PurePosixPath(root)
        paths = set()
        for key in ids:
            require(key in self.commands, f"proof references an unknown command: {key}")
            row = self.commands[key]
            cwd = row.get("cwd") or root
            require(isinstance(cwd, str) and cwd.startswith("/"), f"invalid command cwd: {key}")
            for argument in row["argv"]:
                value = PurePosixPath(argument)
                if not value.is_absolute():
                    value = PurePosixPath(cwd) / value
                if ".." in value.parts:
                    continue
                if value.is_relative_to(base):
                    name = value.relative_to(base).as_posix()
                    if name in self.artifacts:
                        paths.add(name)
        return paths

    def stage_proof(self, stage, kind):
        details = self.record["stages"][stage]["details"]
        path = details.get("proof")
        require(path in details["evidence"], f"{stage} proof is absent from stage evidence")
        proof = self.document(path)
        self.identity(proof, kind)
        clean_states(proof, f"{stage} proof")
        return proof

    def source_paths(self, reports):
        result = set()
        for name in names(reports, "recompile source_reports"):
            require(name in self.record["stages"]["recovery"]["details"]["evidence"],
                    "source report is not recovery-stage evidence")
            report = object_value(self.document(name), "recovery source report")
            require(report.get("status") == "success" and report.get("platform") == self.case["platform"],
                    "invalid recovery source report")
            clean_states(report, "recovery source report")
            if self.case["platform"] == "android":
                require(report.get("backend") == {"name": "neverd", "version": "1", "execution": "builtin"},
                        "source report used an external recovery backend")
                paths = names(report.get("java_sources"), "Java source inventory")
            else:
                require(report.get("architecture") == self.case["architecture"], "source report architecture mismatch")
                outputs = object_value(report.get("outputs"), "source report outputs")
                paths = [outputs[key] for key in ("native_source", "objc_source", "swift_source") if key in outputs]
                names(paths, "iOS source inventory")
            for source in paths:
                relative_name(source)
                path = (PurePosixPath(name).parent / source).as_posix()
                require(PurePosixPath(path).suffix in SOURCE_EXTENSIONS[self.case["platform"]],
                        "recovery source inventory references a non-source file")
                self.artifact(path, nonempty=True)
                result.add(path)
        return result

    def recompile(self):
        proof = self.stage_proof("recompile", "independent-recompile")
        sources = set(names(proof.get("input_sources"), "recompile input_sources"))
        available = self.source_paths(proof.get("source_reports"))
        require(sources <= available, "compiler source input is not a recovered source artifact")
        if self.case["platform"] == "android":
            require(sources == available, "recompile omitted recovered Java sources")
        # iOS native C / Objective-C / Swift files can represent overlapping
        # implementations. This input binding does not prove a complete method
        # denominator or equivalence; the independent inventory/recovery stages
        # must establish that before they may become successful.
        require(proof.get("original_implementation_inputs") == [], "recompile reuses original implementation inputs")
        dependencies = self.document(proof.get("dependency_manifest"))
        exact_schema(dependencies, "dependency manifest")
        require(dependencies.get("original_implementation_inputs") == [], "dependency manifest reuses original implementation inputs")
        inputs = dependencies.get("inputs")
        require(isinstance(inputs, list) and inputs, "dependency manifest has no compiler input inventory")
        seen, recovered = set(), set()
        locks = self.app.get("acceptance_dependencies", [])
        require(isinstance(locks, list), "invalid acceptance dependency locks")
        for row in inputs:
            object_value(row, "dependency input")
            path, origin = relative_name(row.get("path")), row.get("origin")
            require(path not in seen, "duplicate compiler dependency input")
            seen.add(path)
            self.artifact(path, nonempty=True)
            if origin == "recovered-source":
                require(path in sources, "dependency manifest labels non-recovered code as recovered")
                recovered.add(path)
            else:
                require(origin in ("test-harness", "platform-sdk", "non-code-resource"),
                        "dependency manifest contains original/unknown implementation input")
                require(any(isinstance(lock, dict) and lock.get("origin") == origin
                            and lock.get("sha256") == self.artifacts[path]["sha256"] for lock in locks),
                        "compiler dependency does not match the manifest's independent lock")
        require(recovered == sources, "dependency manifest omits recovered compiler inputs")
        command_paths = self.command_paths(proof.get("command_ids"))
        require(seen <= command_paths, "compiler command did not consume its declared source/dependency inputs")
        compiler_ids = []
        for key in proof["command_ids"]:
            argv = self.commands[key]["argv"]
            tool = PurePosixPath(argv[0]).name
            if tool in ("javac", "clang", "clang++", "swiftc") or (
                    tool == "xcrun" and any(arg in ("clang", "clang++", "swiftc") for arg in argv[1:])):
                compiler_ids.append(key)
        require(compiler_ids and sources <= self.command_paths(compiler_ids),
                "recovered sources were not inputs to an independent source compiler")
        outputs = set(names(proof.get("outputs"), "recompile outputs"))
        require(not outputs & seen, "compiler output aliases a compiler input")
        for path in outputs:
            self.artifact(path, nonempty=True)
        require(outputs <= command_paths, "compiler output is not bound to a successful compiler command")
        require(command_paths <= seen | outputs, "compiler command references an undeclared artifact input/output")
        # Original implementation bytes are not permitted compiler inputs. A
        # genuinely independent rebuild may produce byte-identical outputs.
        original_artifacts = names(dependencies.get("original_artifacts"), "original implementation artifact inventory")
        original_hashes = set()
        for path in original_artifacts:
            self.artifact(path, nonempty=True)
            require(path in self.record["stages"]["original_build"]["details"]["evidence"],
                    "original implementation artifact is not original-build evidence")
            original_hashes.add(self.artifacts[path]["sha256"])
        require(not any(self.artifacts[path]["sha256"] in original_hashes for path in seen),
                "compiler inputs reuse original implementation bytes")
        return outputs, original_artifacts

    def assertions(self, name, label):
        document = self.document(name)
        exact_schema(document, label)
        rows = document.get("assertions")
        require(isinstance(rows, list) and rows, f"{label} has no meaningful assertions")
        result = {}
        for row in rows:
            object_value(row, label)
            key = row.get("id")
            require(isinstance(key, str) and key and key not in result, f"{label} has missing/duplicate assertion IDs")
            require(row.get("status") == "pass" and "value" in row
                    and isinstance(row.get("obligation"), str) and row["obligation"], f"{label} has a failed/incomplete assertion")
            result[key] = (row["obligation"], json.dumps(row["value"], sort_keys=True, ensure_ascii=True, allow_nan=False))
        return result

    def behavior(self, outputs, originals):
        proof = self.stage_proof("behavior", "behavior-comparison")
        original, rebuilt = proof.get("original_artifact"), proof.get("rebuilt_artifact")
        require(original in originals and rebuilt in outputs and original != rebuilt,
                "behavior execution artifacts are not bound to original and independently compiled outputs")
        require(original in self.command_paths(proof.get("original_command_ids")),
                "original behavior command did not execute/install its artifact")
        require(rebuilt in self.command_paths(proof.get("rebuilt_command_ids")),
                "rebuilt behavior command did not execute/install the compiler output")
        if self.case["profile"] == "official-release":
            official = object_value(self.app.get("official_apk"), "pinned official APK")
            require(self.artifacts[original]["sha256"] == official.get("sha256"),
                    "original behavior artifact differs from the pinned official APK")
        paths = [proof.get(key) for key in ("expected_assertions", "original_results", "rebuilt_results")]
        require(len(set(paths)) == 3, "behavior evidence aliases expected/original/rebuilt results")
        for result, ids in ((paths[1], proof["original_command_ids"]), (paths[2], proof["rebuilt_command_ids"])):
            require(result in {self.commands[key]["stdout"] for key in ids},
                    "behavior results are not the recorded stdout of the corresponding execution commands")
        expected, before, after = [self.assertions(path, label)
                                   for path, label in zip(paths, ("expected oracle", "original behavior", "rebuilt behavior"))]
        require(expected == before == after, "independent original/rebuilt assertion identities, obligations or values differ")
        config = object_value(self.app.get(self.case["platform"]), "application behavior configuration")
        obligations = set(names(config.get("behavior_obligations"), "manifest behavior obligations"))
        require({row[0] for row in expected.values()} == obligations, "behavior oracle omits or substitutes manifest obligations")
        runtime = object_value(proof.get("runtime"), "behavior runtime")
        require(runtime.get("sdk") == self.case["sdk"], "behavior SDK differs from the required case")
        if self.case["platform"] == "android":
            require(runtime.get("kind") == "art" and runtime.get("architecture") in ("arm64", "x86_64"),
                    "Android behavior requires actual ART execution")
        else:
            kind = "ios-device" if self.case["sdk"] == "iphoneos" else "ios-simulator"
            require(runtime.get("kind") == kind and runtime.get("architecture") == self.case["architecture"],
                    "iOS behavior platform/architecture differs from the required case")

    def validate(self):
        record = self.record
        exact_schema(record, "case result")
        for key, value in (("case_id", self.case["id"]), ("app", self.case["app"]),
                           ("consumer_commit", self.consumer), ("manifest_sha256", self.manifest_sha),
                           ("source_commit", self.app["source_commit"]), ("variant", self.case)):
            require(record.get(key) == value, f"case {key} differs from the pinned manifest/source")
        require(record.get("status") == "success" and record.get("failures") == [], "case is failed/incomplete or retains failures")
        stages = object_value(record.get("stages"), "case stages")
        require(set(stages) == set(STAGES), "case omitted or invented required stages")
        self.check_artifacts()
        self.check_commands()
        for name in STAGES:
            stage = object_value(stages[name], name)
            require(stage.get("status") == "success", f"{name} is not successful")
            details = object_value(stage.get("details"), f"{name} details")
            clean_states(details, name)
            for path in names(details.get("evidence"), f"{name} evidence"):
                self.artifact(path, nonempty=True)
        outputs, originals = self.recompile()
        self.behavior(outputs, originals)


def audit(manifest_path, results, consumer_commit, needs_path):
    require(re.fullmatch(r"[0-9a-f]{40}", consumer_commit), "consumer commit must be an exact lowercase SHA")
    manifest = load_json(manifest_path)
    exact_schema(manifest, "manifest")
    manifest_sha, _ = digest_file(manifest_path)
    apps = object_value(manifest.get("apps"), "manifest apps")
    for app in apps.values():
        object_value(app, "manifest app")
    required = manifest.get("required_cases")
    require(isinstance(required, list) and required, "manifest has no full required matrix")
    cases = {}
    for row in required:
        object_value(row, "required case")
        require(all(isinstance(row.get(key), str) and row[key] for key in ("id", "app", "platform", "profile", "architecture", "sdk")),
                "required case is missing its exact variant identity")
        require(row["id"] not in cases and row["app"] in apps and row["platform"] in SOURCE_EXTENSIONS,
                "duplicate case, unknown app or unsupported platform in manifest")
        app = object_value(apps[row["app"]], "manifest app")
        require(isinstance(app.get("source_commit"), str) and re.fullmatch(r"[0-9a-f]{40}", app["source_commit"]),
                "app lacks an immutable source commit")
        cases[row["id"]] = row
    require({row["app"] for row in cases.values()} == set(apps), "manifest app has no required case")
    baseline = set(names(manifest.get("baseline_cases"), "baseline cases", empty=True))
    require(baseline <= set(cases), "baseline case is outside the full required matrix")
    jobs = set(names(manifest.get("required_jobs"), "required jobs"))
    needs = object_value(load_json(needs_path), "GitHub needs")
    errors = []
    if set(needs) != jobs:
        errors.append("GitHub needs omitted or substituted required jobs")
    for name, row in needs.items():
        if not isinstance(row, dict) or row.get("result") != "success":
            errors.append(f"GitHub job {name} failed, was skipped/cancelled, or has no result")
    require(results.is_dir() and not results.is_symlink(), "result artifact directory is missing or a symlink")
    found, case_errors, passed = {}, {}, {}
    for path in sorted(results.rglob("result.json")):
        try:
            current = results
            for part in path.relative_to(results).parts:
                current = current / part
                require(not current.is_symlink(), "symlink result path")
            record = object_value(load_json(path), "case result")
            key = record.get("case_id")
            require(isinstance(key, str) and key in cases, "unexpected or missing case_id")
            require(key not in found, f"duplicate case result: {key}")
            found[key] = path
            evidence = CaseEvidence(path, record, cases[key], apps[cases[key]["app"]], consumer_commit, manifest_sha)
            try:
                evidence.validate()
                passed[key] = evidence
            except (GateError, OSError, ValueError, TypeError, KeyError) as error:
                case_errors[key] = str(error)
        except (GateError, OSError, ValueError, TypeError, KeyError) as error:
            errors.append(f"{path.relative_to(results)}: {error}")
    missing = sorted(set(cases) - set(found))
    if missing:
        errors.append("full release matrix is missing cases: " + ", ".join(missing))
    requirements = object_value(manifest.get("qualification_requirements"), "qualification requirements")
    for name, requirement in requirements.items():
        try:
            object_value(requirement, f"qualification {name}")
            require(requirement.get("status") == "success", f"qualification {name} remains incomplete")
            refs = requirement.get("evidence")
            require(isinstance(refs, list) and refs, f"qualification {name} lacks evidence")
            evidenced_cases = set()
            for ref in refs:
                object_value(ref, "qualification evidence reference")
                key = ref.get("case_id")
                require(key in passed, f"qualification {name} references an unqualified case")
                proof = passed[key].document(ref.get("path"))
                passed[key].identity(proof, "qualification")
                require(proof.get("requirement") == name and proof.get("status") == "success",
                        f"qualification {name} evidence is unrelated or incomplete")
                proof_cases = set(names(proof.get("case_ids"), f"qualification {name} cases"))
                require(key in proof_cases and proof_cases <= set(passed), f"qualification {name} evidence omits or invents passing cases")
                evidenced_cases.update(proof_cases)
                clean_states(proof, f"qualification {name}")
            if name == "independent_holdouts":
                holdouts = {key for key, row in cases.items() if apps[row["app"]].get("cohort") == "holdout"}
                require(holdouts and holdouts <= evidenced_cases, "holdout qualification omits the independent holdout matrix")
                require({cases[key]["platform"] for key in holdouts} == {row["platform"] for row in cases.values()},
                        "holdout qualification does not cover every required platform")
                development_repos = {app.get("repository") for app in apps.values() if app.get("cohort") != "holdout"}
                require(all(apps[cases[key]["app"]].get("repository")
                            and apps[cases[key]["app"]]["repository"] not in development_repos for key in holdouts),
                        "holdout qualification reuses a development application")
            elif name == "toolchain_variation":
                require(set(cases) <= evidenced_cases, "toolchain qualification omits required variants")
                groups = {}
                for row in cases.values():
                    toolchain = row.get("toolchain")
                    require(isinstance(toolchain, str) and toolchain, "toolchain qualification lacks pinned variant identities")
                    group = (row["app"], row["profile"], row["architecture"])
                    groups.setdefault(group, set()).add((row["sdk"], toolchain))
                require(all(len(variants) >= 2 for variants in groups.values()),
                        "toolchain qualification does not repeat the complete matrix")
        except (GateError, OSError, ValueError, TypeError, KeyError) as error:
            errors.append(str(error))
    qualified = not errors and not case_errors and set(passed) == set(cases)
    return {"schema_version": 1, "status": "success" if qualified else "failed", "qualified": qualified,
            "scope": "full-release", "consumer_commit": consumer_commit, "manifest_sha256": manifest_sha,
            "required_cases": sorted(cases), "received_cases": sorted(found), "qualified_cases": sorted(passed),
            "missing_cases": missing, "baseline_cases": sorted(baseline),
            "baseline_qualified_cases": sorted(baseline & set(passed)), "case_errors": case_errors, "errors": errors}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("manifest", "results", "needs-json", "output"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--consumer-commit", required=True)
    args = parser.parse_args(argv)
    try:
        report = audit(args.manifest, args.results, args.consumer_commit, args.needs_json)
    except (GateError, OSError, ValueError, TypeError, KeyError) as error:
        report = {"schema_version": 1, "status": "failed", "qualified": False, "scope": "full-release",
                  "consumer_commit": args.consumer_commit, "errors": [str(error)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
