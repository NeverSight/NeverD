"""Process and retained-evidence contracts for external DIFF diagnostics."""
from __future__ import annotations

import os
from pathlib import Path
import signal
import shutil
import sqlite3
import subprocess
import sys
import time

if __package__:
    from . import benchmark_binary_diff as bench
else:
    import benchmark_binary_diff as bench

DEFAULT_STAGE_TIMEOUT_SECONDS = 600.0


def stage(folder: Path, name: str, command: list[str], env: dict,
          timeout: float) -> dict:
    start = time.monotonic()
    record = {"command": command, "exit": None, "error": None, "resources": None,
              "timeout_seconds": timeout}
    timer = Path("/usr/bin/time")
    resource_path = folder / (name + ".resources")
    measured_command = command
    executable = command[0]
    if os.sep in executable and not Path(executable).is_absolute():
        executable = str(folder / executable)
    # The retained resource receipt uses GNU time's Linux options and units.
    # macOS has a different /usr/bin/time; it must not replace the tool's exit.
    if sys.platform.startswith("linux") and timer.is_file() and shutil.which(executable, path=env.get("PATH")):
        record["measurement_tool"] = {"path": str(timer), "sha256": bench.digest(timer)}
        measured_command = [str(timer), "--quiet", "--format=%U %S %M %e", "--output=" + str(resource_path), *command]
    cancelled = False
    with (folder / (name + ".log")).open("x") as log:
        try:
            process = subprocess.Popen(measured_command, cwd=folder, env=dict(env, DEBUGINFOD_URLS=""), stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                record["exit"] = process.wait(timeout=timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                # IDA and launcher descendants belong to this newly created group.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                cancelled = isinstance(error, KeyboardInterrupt)
                record["error"] = "cancelled" if cancelled else "timeout"
        except OSError as error:
            record["error"] = str(error)
    record["wall_seconds"] = time.monotonic() - start
    if record["error"] is None and resource_path.is_file():
        values = resource_path.read_text().splitlines()[-1].split()
        bench.require(len(values) == 4, "invalid GNU time resource receipt")
        record["resources"] = {"profile": "gnu-time-child-maxrss-v1",
                               "user_seconds": float(values[0]), "system_seconds": float(values[1]),
                               "max_rss_kib": int(values[2]), "elapsed_seconds": float(values[3])}
    bench.save(folder / (name + ".json"), record)
    if cancelled:
        raise KeyboardInterrupt
    return record


def archive_tools(paths: list[Path], folder: Path) -> list[Path]:
    folder.mkdir(exist_ok=False)
    copies = []
    for index, source in enumerate(paths):
        copy = folder / f"{index:02d}-{source.name}"
        shutil.copy2(source, copy)
        bench.require(bench.digest(copy) == bench.digest(source),
                      "tool archive does not match its source")
        copies.append(copy)
    return copies


def files(folder: Path) -> list[Path]:
    return sorted(path for path in folder.rglob("*") if path.is_file() and
                  "__pycache__" not in path.parts and path.suffix != ".pyc")


def complete_case(corpus: Path, folder: Path, record: dict, runtime: list[Path]) -> dict:
    record["artifacts"] = [bench.artifact(path, folder) for path in
                           [*runtime, *files(folder)]]
    run_path = folder / "run.json"
    bench.save(run_path, record)
    try:
        result = bench.evaluate(corpus, run_path)
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error) as error:
        result = {"case": record["case"], "error": str(error),
                  "run_sha256": bench.digest(run_path)}
    bench.save(folder / "evaluation.json", result)
    print(record["case"], bench.metric_summary(result) if "metrics" in result else record.get("error") or result["error"], flush=True)
    return result


def finish(output: Path, results: list[dict]) -> int:
    bench.save(output / "results.json", {"schema": bench.RESULT_SCHEMA,
                                       "track": "end-to-end", "cases": results})
    if any("metrics" not in result for result in results):
        return 2
    return 0 if all(bench.mapping_passed(result) for result in results) else 1
