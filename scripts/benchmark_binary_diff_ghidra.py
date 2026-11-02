#!/usr/bin/env python3
"""Run fresh Ghidra analysis and Auto Version Tracking on owned DIFF corpora."""
from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import shutil
import sqlite3
import sys

if __package__:
    from . import benchmark_binary_diff as bench
    from . import benchmark_binary_diff_external as external
else:
    import benchmark_binary_diff as bench
    import benchmark_binary_diff_external as external


PROFILE = "AutoVersionTrackingScript-12.1.3-defaults"
DEFAULT_ANALYSIS_TIMEOUT_SECONDS = 300
DEFAULT_CPUS = 2


def checked_analysis(path: Path, source: Path, program: str) -> dict:
    value = bench.load(path)
    bench.require(value["schema"] == "neverd.benchmark.ghidra-analysis.v1" and
                  value["complete"] is True and value["analysis_timed_out"] is False,
                  "Ghidra analysis did not complete")
    bench.require(value["input_sha256"] == bench.digest(source) and value["program"] == program,
                  "Ghidra analyzed a different input")
    bench.require(bool(value["ghidra_version"]), "Ghidra version is missing")
    return value


def checked_task(path: Path, inputs: list[Path], version: str) -> dict:
    value = bench.load(path)
    bench.require(value["schema"] == "neverd.benchmark.ghidra-task.v1" and
                  value["complete"] is True, "Auto Version Tracking did not complete")
    bench.require(value["input_sha256"] == [bench.digest(p) for p in inputs],
                  "Auto Version Tracking input direction differs")
    bench.require(value["profile"] == PROFILE and value["ghidra_version"] == version and
                  value["session"] == "matches" and
                  isinstance(value["status"], str) and
                  value["status"].startswith("Auto Version Tracking Command completed successfully"),
                  "Auto Version Tracking profile or completion status differs")
    return value


def run(args: argparse.Namespace) -> int:
    bench.require(sys.platform.startswith("linux"), "this runner requires Linux")
    corpus, root, java = args.corpus.resolve(), args.ghidra.resolve(), args.java_home.resolve()
    cases = bench.corpus_cases(bench.load(corpus))
    pairs = [[bench.checked_artifact(case[side], corpus.parent)
              for side in ("primary", "secondary")] for case in cases]
    source = Path(__file__).resolve().parents[1] / "tools/neverd-bench"
    scripts = [source / (name + ".java") for name in
               ("RecordNeverDAnalysis", "RunNeverDVersionTracking", "ExportNeverDVTMatches")]
    launcher = root / "support/analyzeHeadless"
    tools = [launcher, java / "bin/java", java / "release", root / "Ghidra/application.properties",
             root / "Ghidra/Features/VersionTracking/lib/VersionTracking.jar",
             root / "Ghidra/Features/Base/lib/Base.jar", Path(__file__).resolve(),
             Path(bench.__file__).resolve(), Path(bench.binexport.__file__).resolve(),
             Path(external.__file__).resolve(), *scripts]
    identities = {path: bench.digest(path) for path in tools}
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archived = external.archive_tools(tools, output / "runtime")
    script_dir = output / "runtime/scripts"
    script_dir.mkdir()
    for original, copy in zip(tools, archived):
        if original in scripts:
            shutil.copy2(copy, script_dir / original.name)
    bench.save(output / "runtime.json", {"files": [
        {"original_path": str(src), **bench.artifact(copy, output)} for src, copy in zip(tools, archived)]})
    runtime = [*external.files(output / "runtime"), output / "runtime.json"]
    env = dict(os.environ, JAVA_HOME=str(java), PATH=str(java / "bin") + os.pathsep + os.environ["PATH"])
    results = []
    for case, pair in zip(cases, pairs):
        folder = output / case["id"]
        folder.mkdir()
        record = {"schema": bench.RUN_SCHEMA, "case": case["id"], "format": "ghidra-vt",
                  "track": "end-to-end", "completed": False, "result": None, "error": None,
                  "input_sha256": [bench.digest(p) for p in pair], "stages": [],
                  "tool": {"name": "Ghidra Auto Version Tracking", "version": "unavailable"},
                  "configuration": {"profile": PROFILE, "cpus": args.cpus,
                                    "stage_timeout_seconds": args.timeout,
                                    "analysis_timeout_seconds": args.analysis_timeout}}
        cancelled = False
        try:
            projects = folder / "projects"
            projects.mkdir()
            prefix = [str(launcher), str(projects), "comparison"]
            common = ["-max-cpu", str(args.cpus), "-scriptPath", str(script_dir)]
            analyses = []
            for side, input_path in zip(("before", "after"), pair):
                copied = folder / (side + ".bin")
                shutil.copyfile(input_path, copied)
                bench.require(bench.digest(copied) == bench.digest(input_path), "input copy changed")
                receipt = folder / (side + "-analysis.json")
                command = prefix + ["-import", str(copied), *common, "-analysisTimeoutPerFile",
                                    str(args.analysis_timeout), "-postScript", "RecordNeverDAnalysis.java", str(receipt)]
                outcome = external.stage(folder, "analyze-" + side, command, env, args.timeout)
                record["stages"].append(outcome)
                bench.require(outcome["exit"] == 0 and outcome["error"] is None, "Ghidra import failed")
                analyses.append(checked_analysis(receipt, input_path, copied.name))
            version = analyses[0]["ghidra_version"]
            bench.require(analyses[1]["ghidra_version"] == version, "Ghidra version changed")
            record["tool"]["version"] = version
            receipt, result = folder / "task.json", folder / "matches.json"
            command = prefix + ["-process", "after.bin", "-noanalysis", *common,
                                "-postScript", "RunNeverDVersionTracking.java", "/before.bin", "matches", str(receipt),
                                "-postScript", "ExportNeverDVTMatches.java", "/matches", str(result)]
            outcome = external.stage(folder, "compare", command, env, args.timeout)
            record["stages"].append(outcome)
            bench.require(outcome["exit"] == 0 and outcome["error"] is None, "Ghidra comparison failed")
            checked_task(receipt, pair, version)
            bench.read_pairs("ghidra-vt", result, pair)
            bench.require(bench.load(result)["ghidra_version"] == version, "Ghidra export version changed")
            record["result"] = bench.artifact(result, folder)
            record["completed"] = True
        except KeyboardInterrupt:
            cancelled, record["error"] = True, "cancelled"
        except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
            record["error"] = str(error)
        results.append(external.complete_case(corpus, folder, record, runtime))
        if cancelled:
            external.finish(output, results)
            raise KeyboardInterrupt
    bench.require(all(bench.digest(p) == sha for p, sha in identities.items()), "Ghidra runtime changed")
    return external.finish(output, results)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("corpus", "ghidra", "java-home", "output-dir"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=external.DEFAULT_STAGE_TIMEOUT_SECONDS)
    parser.add_argument("--analysis-timeout", type=int, default=DEFAULT_ANALYSIS_TIMEOUT_SECONDS)
    parser.add_argument("--cpus", type=int, default=DEFAULT_CPUS)
    args = parser.parse_args(argv)
    try:
        bench.require(math.isfinite(args.timeout) and args.timeout > 0 and
                      args.analysis_timeout > 0 and args.cpus > 0, "deadlines and CPU count must be positive")
        return run(args)
    except KeyboardInterrupt:
        print("Ghidra benchmark: cancelled", file=sys.stderr)
        return 130
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
        print(f"Ghidra benchmark: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
