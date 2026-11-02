#!/usr/bin/env python3
"""Run fresh licensed IDA/Diaphora analysis and matching on owned DIFF corpora."""
from __future__ import annotations

import argparse
import json
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


DEFAULT_CPUS = 2


def environment(python_paths: list[Path], cpus: int) -> dict:
    # Inherited DIAPHORA_* options can silently change the selected profile.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("DIAPHORA_") and key not in ("PYTHONPATH", "PYTHONHOME")}
    env.update(PYTHONPATH=os.pathsep.join(str(path) for path in python_paths),
               PYTHONDONTWRITEBYTECODE="1", QT_QPA_PLATFORM="offscreen",
               DIAPHORA_CPU_COUNT=str(cpus), OMP_NUM_THREADS=str(cpus), OPENBLAS_NUM_THREADS=str(cpus))
    return env


def run(args: argparse.Namespace) -> int:
    bench.require(sys.platform.startswith("linux"), "this runner requires Linux")
    corpus = args.corpus.resolve()
    cases = bench.corpus_cases(bench.load(corpus))
    pairs = [[bench.checked_artifact(case[side], corpus.parent)
              for side in ("primary", "secondary")] for case in cases]
    source = args.diaphora.resolve()
    python_paths = [path.resolve() for path in args.python_path]
    bench.require(all(path.is_dir() for path in python_paths), "Python dependency path does not exist")
    bench.require(all((source / name).is_file() for name in
                      ("diaphora.py", "diaphora_ida.py", "diaphora_config.py")), "Diaphora source directory is incomplete")
    script = Path(__file__).resolve().parents[1] / "tools/neverd-bench/run_diaphora.py"
    tools = [args.ida.absolute(), args.python.absolute(), script, Path(__file__).resolve(),
             Path(bench.__file__).resolve(), Path(bench.binexport.__file__).resolve(),
             Path(external.__file__).resolve()]
    identities = {path: bench.digest(path) for path in tools}
    output = args.output_dir.resolve()
    bench.require(not output.is_relative_to(source), "output must be outside the Diaphora source directory")
    output.mkdir(parents=True, exist_ok=False)
    archived = external.archive_tools(tools, output / "runtime")
    frozen_source = output / "runtime/diaphora"
    shutil.copytree(source, frozen_source, ignore=shutil.ignore_patterns(".git", "__pycache__", "*.pyc"))
    source_files = external.files(frozen_source)
    for path in source_files:
        original = source / path.relative_to(frozen_source)
        identities[original] = bench.digest(path)
        bench.require(bench.digest(original) == identities[original], "Diaphora source changed while archiving")
    bench.save(output / "runtime.json", {"files": [
        {"original_path": str(src), **bench.artifact(copy, output)} for src, copy in zip(tools, archived)],
        "diaphora_source": str(source), "source_files": [bench.artifact(path, output) for path in source_files],
        "python_dependency_paths": [str(path) for path in python_paths]})
    runtime = [*archived, *source_files, output / "runtime.json"]
    script = archived[tools.index(script)]
    profile = {"decompiler": args.decompiler, "microcode": args.microcode,
               "include_library_thunks": args.include_library_thunks}
    env = environment(python_paths, args.cpus)
    env.update(NEVERD_BENCH_DIAPHORA_SOURCE=str(frozen_source),
               NEVERD_BENCH_DIAPHORA_PROFILE=json.dumps(profile))
    results = []
    for case, pair in zip(cases, pairs):
        folder = output / case["id"]
        folder.mkdir()
        record = {"schema": bench.RUN_SCHEMA, "case": case["id"], "format": "diaphora-native",
                  "track": "end-to-end", "completed": False, "result": None, "error": None,
                  "input_sha256": [bench.digest(path) for path in pair], "stages": [],
                  "tool": {"name": "Diaphora", "version": "unavailable"},
                  "configuration": {**profile, "cpus": args.cpus, "stage_timeout_seconds": args.timeout}}
        cancelled = False
        try:
            exports = []
            for side, path in zip(("before", "after"), pair):
                exported = folder / (side + ".sqlite")
                export_env = dict(env, NEVERD_BENCH_DIAPHORA_MODE="export", NEVERD_BENCH_DIAPHORA_OUTPUT=str(exported))
                command = [str(args.ida.absolute()), "-A", "-L" + str(folder / (side + "-ida.log")),
                           "-S" + json.dumps(str(script), ensure_ascii=False), "-o" + str(folder / (side + ".i64")), str(path)]
                outcome = external.stage(folder, "export-" + side, command, export_env, args.timeout)
                record["stages"].append(outcome)
                bench.require(outcome["exit"] == 0 and outcome["error"] is None, "Diaphora export failed")
                exports.append(bench.checked_diaphora_export(Path(str(exported) + ".json"), exported, path))
            version = exports[0]["diaphora_version"]
            bench.require(exports[1]["diaphora_version"] == version, "Diaphora export versions differ")
            record["tool"]["version"] = "format " + version + "; source SHA-256 " + bench.digest(frozen_source / "diaphora.py")
            result = folder / "matches.diaphora"
            compare_env = dict(env, NEVERD_BENCH_DIAPHORA_MODE="match", NEVERD_BENCH_DIAPHORA_OUTPUT=str(result),
                               NEVERD_BENCH_DIAPHORA_PRIMARY=str(folder / "before.sqlite"),
                               NEVERD_BENCH_DIAPHORA_SECONDARY=str(folder / "after.sqlite"))
            outcome = external.stage(folder, "compare", [str(args.python.absolute()), str(script)], compare_env, args.timeout)
            record["stages"].append(outcome)
            bench.require(outcome["exit"] == 0 and outcome["error"] is None, "Diaphora comparison failed")
            receipt = Path(str(result) + ".json")
            bench.read_pairs("diaphora-native", receipt, pair)
            record["result"] = bench.artifact(receipt, folder)
            record["completed"] = True
        except KeyboardInterrupt:
            cancelled, record["error"] = True, "cancelled"
        except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
            record["error"] = str(error)
        results.append(external.complete_case(corpus, folder, record, runtime))
        if cancelled:
            external.finish(output, results)
            raise KeyboardInterrupt
    bench.require(all(bench.digest(p) == sha for p, sha in identities.items()), "Diaphora runtime changed")
    return external.finish(output, results)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("corpus", "ida", "diaphora", "python", "output-dir"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--python-path", type=Path, action="append", default=[])
    parser.add_argument("--timeout", type=float, default=external.DEFAULT_STAGE_TIMEOUT_SECONDS)
    parser.add_argument("--cpus", type=int, default=DEFAULT_CPUS)
    for flag in ("decompiler", "microcode", "include-library-thunks"):
        parser.add_argument("--" + flag, action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args(argv)
    try:
        bench.require(math.isfinite(args.timeout) and args.timeout > 0 and args.cpus > 0,
                      "deadline and CPU count must be positive")
        bench.require(args.decompiler or not args.microcode, "microcode requires the decompiler profile")
        return run(args)
    except KeyboardInterrupt:
        print("Diaphora benchmark: cancelled", file=sys.stderr)
        return 130
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
        print(f"Diaphora benchmark: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
