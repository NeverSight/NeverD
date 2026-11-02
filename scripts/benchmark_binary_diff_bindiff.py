#!/usr/bin/env python3
"""Run licensed IDA/BinExport and BinDiff on owned native diagnostic inputs.

Tools and configuration must already be installed and explicitly selected.
Inputs are analyzed, never executed. This is an end-to-end diagnostic track.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sqlite3
import sys

if __package__:
    from . import benchmark_binary_diff as bench
    from .benchmark_binary_diff_external import DEFAULT_STAGE_TIMEOUT_SECONDS, archive_tools, stage
else:
    import benchmark_binary_diff as bench
    from benchmark_binary_diff_external import DEFAULT_STAGE_TIMEOUT_SECONDS, archive_tools, stage


EXPORT_SCHEMA = "neverd.benchmark.binexport.v1"


def checked_export(receipt: Path, exported: Path, source: Path) -> dict:
    value = bench.load(receipt)
    bench.require(value["schema"] == EXPORT_SCHEMA and value["complete"] is True,
                  "BinExport did not complete")
    bench.require(value["input_sha256"] == bench.digest(source),
                  "IDA analyzed a different input")
    bench.require(bool(value["ida_version"]), "IDA version is missing")
    bench.require(exported.stat().st_size > 0 and
                  value["export_sha256"] == bench.digest(exported),
                  "BinExport bytes do not match their receipt")
    return value


def run(args: argparse.Namespace) -> int:
    bench.require(sys.platform.startswith("linux"), "this runner requires Linux")
    corpus_path = args.corpus.resolve()
    cases = bench.corpus_cases(bench.load(corpus_path))
    executable, ida = args.bindiff.absolute(), args.ida.absolute()
    plugin, config = args.binexport_plugin.absolute(), args.config.absolute()
    script = Path(__file__).resolve().parents[1] / "tools/neverd-bench/export_ida_bindiff.py"
    output = args.output_dir.resolve()
    tools = [executable, ida, plugin, config, script, Path(__file__).resolve(),
             Path(bench.__file__).resolve(), Path(bench.binexport.__file__).resolve(),
             Path(__file__).with_name("benchmark_binary_diff_external.py").resolve()]
    identities = {path: bench.digest(path) for path in tools}
    # Validate every input before starting an external process or creating results.
    inputs = [[bench.checked_artifact(case[side], corpus_path.parent)
               for side in ("primary", "secondary")] for case in cases]
    output.mkdir(parents=True, exist_ok=False)
    archived = archive_tools(tools, output / "runtime")
    script = archived[tools.index(script)]
    bench.save(output / "runtime.json", {
        "files": [{"original_path": str(source), **bench.artifact(copy, output)}
                  for source, copy in zip(tools, archived)]})
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    version_run = stage(output, "version", [str(executable), "--version"], env, args.timeout)
    bench.require(version_run["exit"] == 0, "BinDiff version query failed")
    version = (output / "version.log").read_text().strip()
    bench.require(bool(version), "BinDiff version output is empty")
    results = []
    for case, pair in zip(cases, inputs):
        folder = output / case["id"]
        folder.mkdir()
        run_record = {"schema": bench.RUN_SCHEMA, "case": case["id"],
                      "format": "bindiff", "track": "end-to-end", "completed": False,
                      "tool": {"name": "BinDiff", "version": version},
                      "input_sha256": [bench.digest(path) for path in pair],
                      "configuration": {"stage_timeout_seconds": args.timeout},
                      "result": None, "stages": [], "error": None}
        try:
            exports = []
            run_record["analyses"] = []
            for side, source in zip(("before", "after"), pair):
                exported, receipt = folder / (side + ".BinExport"), folder / (side + "-export.json")
                export_env = dict(env, NEVERD_BENCH_BINEXPORT_OUTPUT=str(exported),
                                  NEVERD_BENCH_BINEXPORT_RECEIPT=str(receipt),
                                  NEVERD_BENCH_BINEXPORT_PLUGIN=str(plugin))
                command = [str(ida), "-A", "-L" + str(folder / (side + "-ida.log")),
                           "-S" + json.dumps(str(script), ensure_ascii=False),
                           "-o" + str(folder / (side + ".i64")), str(source)]
                outcome = stage(folder, "export-" + side, command, export_env, args.timeout)
                run_record["stages"].append(outcome)
                bench.require(outcome["exit"] == 0 and outcome["error"] is None,
                              "IDA export failed: " + side)
                exports.append(checked_export(receipt, exported, source))
                run_record["analyses"].append({"receipt": bench.artifact(receipt, folder),
                                               "export": bench.artifact(exported, folder)})
            run_record["exporters"] = exports
            command = [str(executable), "--config=" + str(config),
                       "--primary=" + str(folder / "before.BinExport"),
                       "--secondary=" + str(folder / "after.BinExport"),
                       "--output_dir=" + str(folder), "--output_format=bin,log"]
            outcome = stage(folder, "compare", command, env, args.timeout)
            run_record["stages"].append(outcome)
            bench.require(outcome["exit"] == 0 and outcome["error"] is None, "BinDiff comparison failed")
            matches = list(folder.glob("*.BinDiff"))
            bench.require(len(matches) == 1, "BinDiff must produce exactly one result database")
            # Native metadata must bind the expected input order before admission.
            bench.read_pairs("bindiff", matches[0], pair)
            run_record["result"] = bench.artifact(matches[0], folder)
            run_record["completed"] = True
        except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
            run_record["error"] = str(error)
        run_record["artifacts"] = [bench.artifact(path, folder) for path in
                                   [*archived, output / "runtime.json",
                                    output / "version.log", output / "version.json",
                                    *sorted(path for path in folder.iterdir() if path.is_file())]]
        run_path = folder / "run.json"
        bench.save(run_path, run_record)
        try:
            result = bench.evaluate(corpus_path, run_path)
        except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
            result = {"case": case["id"], "error": str(error), "run_sha256": bench.digest(run_path)}
        bench.save(folder / "evaluation.json", result)
        results.append(result)
        print(case["id"], bench.metric_summary(result), flush=True)
    bench.require(all(bench.digest(path) == sha for path, sha in identities.items()),
                  "a tool or configuration changed during the run")
    bench.save(output / "results.json", {"schema": bench.RESULT_SCHEMA, "cases": results,
                                       "track": "end-to-end", "runtime": [bench.artifact(p, output) for p in archived]})
    if any("metrics" not in result for result in results):
        return 2
    return 0 if all(bench.mapping_passed(result) for result in results) else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("corpus", "ida", "binexport-plugin", "bindiff", "config", "output-dir"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=DEFAULT_STAGE_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    try:
        bench.require(math.isfinite(args.timeout) and args.timeout > 0,
                      "stage timeout must be finite and positive")
        return run(args)
    except KeyboardInterrupt:
        print("BinDiff benchmark: cancelled", file=sys.stderr)
        return 130
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
        print(f"BinDiff benchmark: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
