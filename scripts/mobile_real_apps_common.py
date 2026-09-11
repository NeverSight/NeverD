#!/usr/bin/env python3
"""Evidence and bounded command execution for the Actions-only app corpus.

This module orchestrates public test inputs. NeverD's recovery implementation
remains the native CLI; no decompiler is implemented or substituted here.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import subprocess
import time
from datetime import datetime, timezone

STAGES = ("provenance", "original_build", "inventory", "recovery", "recompile", "behavior")
MAX_COMMAND_OUTPUT = 256 * 1024 * 1024


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def load_json(path: Path):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON field: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_pairs)


def safe_relative(name: str) -> Path:
    parts = PurePosixPath(name)
    if not name or parts.is_absolute() or ".." in parts.parts or "\\" in name:
        raise ValueError(f"Unsafe evidence path: {name!r}")
    return Path(*parts.parts)


class CaseContext:
    def __init__(self, *, app: dict, variant: dict, source: Path, work: Path,
                 neverd: Path, timeout: int, consumer_commit: str,
                 manifest_sha256: str):
        if timeout <= 0:
            raise ValueError("Case timeout must be positive")
        self.app, self.variant = app, variant
        self.source, self.work, self.neverd = source.resolve(), work.resolve(), neverd.resolve()
        self.timeout = timeout
        self.deadline = time.monotonic() + timeout
        self.work.mkdir(parents=True, exist_ok=True)
        if any(self.work.iterdir()):
            raise ValueError("Evidence directory must be empty to exclude stale results")
        self.result = {
            "schema_version": 1,
            "case_id": variant["id"],
            "consumer_commit": consumer_commit,
            "manifest_sha256": manifest_sha256,
            "source_commit": app["source_commit"],
            "app": variant["app"],
            "variant": variant,
            "work_directory": str(self.work),
            "started_at": utc_now(),
            "status": "incomplete",
            "stages": {}, "failures": [], "commands": [], "artifacts": [],
        }
        self.write_json("result.json", self.result)

    def write_json(self, name: str, data) -> None:
        destination = self.work / safe_relative(name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink() or not destination.resolve().is_relative_to(self.work):
            raise ValueError(f"Evidence path escapes work directory: {name}")
        temporary = destination.with_name(destination.name + ".tmp")
        if temporary.is_symlink():
            raise ValueError(f"Symlinked evidence temporary file: {temporary}")
        temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        temporary.replace(destination)

    def stage(self, name: str, status: str, **details) -> None:
        if name not in STAGES or status not in {"success", "failed", "incomplete"}:
            raise ValueError(f"Invalid stage: {name}/{status}")
        self.result["stages"][name] = {"status": status, "details": details}
        self.write_json("result.json", self.result)

    def fail(self, reason: str) -> None:
        self.result["failures"].append(str(reason))
        self.result["status"] = "failed"
        self.write_json("result.json", self.result)

    def command(self, name: str, argv, cwd=None, env=None, timeout=None,
                allow_failure=False) -> subprocess.CompletedProcess[str]:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise ValueError(f"Invalid command evidence name: {name}")
        started = time.monotonic()
        remaining = self.deadline - started
        limit = min(900 if timeout is None else float(timeout), remaining)
        if limit <= 0:
            raise TimeoutError("Total application case deadline exhausted")
        command_deadline = started + limit
        command = [os.fspath(item) for item in argv]
        if not command:
            raise ValueError("Empty command")
        command_dir = self.work / "commands"
        command_dir.mkdir(exist_ok=True)
        stem = f"{len(self.result['commands']):04d}-{name}"
        stdout_path, stderr_path = command_dir / (stem + ".stdout"), command_dir / (stem + ".stderr")
        effective_env = dict(os.environ)
        if env:
            effective_env.update({key: str(value) for key, value in env.items()})
        # Upstream builds never need the workflow's GitHub/API credentials.
        # Keep SDK, compiler and CI environment settings, but omit credentials
        # from child processes and never serialize the full process environment.
        for key in list(effective_env):
            if any(part in key.upper() for part in ("TOKEN", "PASSWORD", "SECRET", "API_KEY", "PRIVATE_KEY")):
                effective_env.pop(key)
        record = {
            "id": stem, "name": name, "argv": command, "cwd": str(Path(cwd or self.source).resolve()),
            "started_at": utc_now(), "timeout_seconds": limit,
            "stdout": stdout_path.relative_to(self.work).as_posix(),
            "stderr": stderr_path.relative_to(self.work).as_posix(),
            "status": "running", "exitcode": None,
        }
        if self.variant.get("platform") == "ios" \
                and command[:2] == [str(self.neverd), "mobile"] \
                and effective_env.get("NEVERD_NATIVE_PHASES") == "1":
            # Record only this fixed, nonsecret setting, not arbitrary values.
            record["diagnostic_environment"] = {"NEVERD_NATIVE_PHASES": "1"}
        self.result["commands"].append(record)
        self.write_json("result.json", self.result)
        failure = None
        process = None
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open("w", encoding="utf-8") as stderr:
            def check_output_limit():
                nonlocal failure
                if any(os.fstat(stream.fileno()).st_size > MAX_COMMAND_OUTPUT for stream in (stdout, stderr)):
                    record["status"] = "output-limit"
                    record["error"] = "Command output exceeds parser budget; complete logs retained"
                    failure = RuntimeError(record["error"])
                    return True
                return False

            try:
                process = subprocess.Popen(command, cwd=record["cwd"], env=effective_env,
                                           stdout=stdout, stderr=stderr, text=True,
                                           start_new_session=(os.name == "posix"))
                while True:
                    if check_output_limit():
                        break
                    remaining = command_deadline - time.monotonic()
                    if remaining <= 0:
                        record["status"] = "timeout"
                        failure = subprocess.TimeoutExpired(command, limit)
                        break
                    try:
                        record["exitcode"] = process.wait(timeout=min(1, remaining))
                    except subprocess.TimeoutExpired:
                        continue
                    if not check_output_limit():
                        record["status"] = "success" if process.returncode == 0 else "failed"
                    break
            except OSError as error:
                record["status"] = "failed"
                record["error"] = str(error)
                failure = error
            finally:
                if process is not None:
                    leader_exited = process.returncode is not None
                    try:
                        if os.name == "posix":
                            # start_new_session gives this command its own
                            # group. A group surviving a reaped leader is an
                            # unexpected background writer, even on exit 0.
                            os.killpg(process.pid, signal.SIGKILL)
                            if leader_exited:
                                record["leftover_process_group"] = True
                                if failure is None:
                                    record["status"] = "failed"
                                    record["error"] = "Command left a running background process group"
                                    failure = RuntimeError(record["error"])
                        elif not leader_exited:
                            process.kill()
                    except ProcessLookupError:
                        pass
                    except OSError as cleanup_error:
                        record["cleanup_error"] = str(cleanup_error)
                        if failure is None:
                            record["status"] = "failed"
                            failure = cleanup_error
                        try:
                            process.kill()
                        except OSError as fallback_error:
                            record["cleanup_fallback_error"] = str(fallback_error)
                    if not leader_exited:
                        try:
                            record["exitcode"] = process.wait(timeout=10)
                        except (OSError, subprocess.TimeoutExpired) as cleanup_error:
                            record["reap_error"] = str(cleanup_error)
                            if failure is None:
                                record["status"] = "failed"
                                failure = cleanup_error
                    # Include bytes emitted while the command was being
                    # stopped; never decode a log beyond the parser budget.
                    try:
                        check_output_limit()
                    except OSError as log_error:
                        record["log_error"] = str(log_error)
                        if failure is None:
                            record["status"] = "failed"
                            failure = log_error
        record["ended_at"] = utc_now()
        self.write_json(f"commands/{stem}.json", record)
        self.write_json("result.json", self.result)
        if failure:
            raise RuntimeError(f"{name}: {record['status']}: {failure}") from failure
        completed = subprocess.CompletedProcess(command, record["exitcode"],
            stdout_path.read_text(encoding="utf-8", errors="replace"),
            stderr_path.read_text(encoding="utf-8", errors="replace"))
        if completed.returncode and not allow_failure:
            raise RuntimeError(f"{name} exited {completed.returncode}; see {record['stderr']}")
        return completed

    def finish(self) -> dict:
        for name in STAGES:
            if name not in self.result["stages"]:
                self.stage(name, "incomplete", reason="Stage did not complete")
        artifacts = []
        # os.walk is explicit about unreadable directories; a missing subtree
        # must not silently become an apparently complete evidence manifest.
        def walk_error(error):
            self.result["failures"].append(f"Evidence enumeration failed: {error}")
        for directory, dirs, files in os.walk(self.work, followlinks=False, onerror=walk_error):
            for name in dirs + files:
                path = Path(directory) / name
                if path.is_symlink():
                    self.fail(f"Symlink is not a standalone evidence artifact: {path.relative_to(self.work)}")
            for name in sorted(files):
                path = Path(directory) / name
                if path == self.work / "result.json" or path.is_symlink():
                    continue
                try:
                    artifacts.append({"path": path.relative_to(self.work).as_posix(),
                                      "size": path.stat().st_size, "sha256": digest(path)})
                except OSError as error:
                    self.result["failures"].append(f"Cannot hash evidence {path.relative_to(self.work)}: {error}")
        self.result["artifacts"] = sorted(artifacts, key=lambda row: row["path"])
        if self.result["failures"] or any(row["status"] == "failed" for row in self.result["stages"].values()):
            self.result["status"] = "failed"
        elif any(row["status"] != "success" for row in self.result["stages"].values()):
            self.result["status"] = "incomplete"
        elif not self.result["commands"] or any(row["status"] != "success" for row in self.result["commands"]):
            self.result["status"] = "failed"
        else:
            self.result["status"] = "success"
        self.result["ended_at"] = utc_now()
        self.write_json("result.json", self.result)
        return self.result
