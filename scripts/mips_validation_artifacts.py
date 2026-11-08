"""Owned scratch and bounded receipts for the two MIPS validation drivers.

This never owns an SDK, build tree, CTest log, firmware or comparison baseline.
An output directory is reusable only after this producer has claimed it empty.
Children inherit its lock, so recovery cannot race a surviving compiler/CLI.
"""

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


class MIPSValidationArtifacts:
    def __init__(self, output, producer, inputs=()):
        if Path(output).is_symlink():
            raise ValueError("MIPS output must not be a symlink")
        self.output = Path(output).resolve()
        self.work = self.output / "work"
        if any(Path(p).resolve().is_relative_to(self.output) for p in inputs):
            raise ValueError("MIPS inputs and baselines must stay outside the owned output")
        self.producer = producer
        self.commands = []
        self.evidence = {}
        self.pending = {}
        self.processes = set()
        self.guard = threading.RLock()
        self.stopping = False
        self.deferred = False
        self.removed_bytes = 0
        self.recovered_bytes = 0

    def write(self, name, value):
        path = self.output / name
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(value, indent=2) + "\n")
        temporary.replace(path)

    def _remove_tree(self, path):
        if path.is_symlink():
            raise ValueError(f"owned artifact directory became a symlink: {path}")
        if path.exists():
            size = sum(p.stat().st_size for p in path.rglob("*")
                       if p.is_file() and not p.is_symlink())
            shutil.rmtree(path)
            self.removed_bytes += size
            return size
        return 0

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=True)
        self.lock = (self.output / ".mips-validation.lock").open("a+b")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            marker = self.output / "owner.json"
            identity = {"schema": 1, "producer": self.producer}
            if marker.exists():
                if json.loads(marker.read_text()) != identity:
                    raise ValueError("output belongs to another producer")
            elif any(p.name != ".mips-validation.lock" for p in self.output.iterdir()):
                raise ValueError("unowned nonempty output: retain it and choose a fresh output once")
            else:
                self.write("owner.json", identity)
            pending_path = self.output / "pending.json"
            old_pending = json.loads(pending_path.read_text()) if pending_path.exists() else []
            for entry in old_pending:
                group = entry.get("group")
                if group is None:
                    raise RuntimeError("interrupted process startup needs a reader audit before recovery")
                if self._group_alive(group):
                    raise BlockingIOError("a previous validation process group still owns the scratch")
            # Only the latest and immediately preceding failed sources are
            # useful to this repeatable check. No accumulating archive series.
            self._remove_tree(self.output / "previous-evidence")
            evidence = self.output / "evidence"
            if evidence.exists():
                if evidence.is_symlink():
                    raise ValueError("owned evidence became a symlink")
                if any(evidence.iterdir()):
                    evidence.rename(self.output / "previous-evidence")
                else:
                    evidence.rmdir()
            evidence.mkdir()
            if self.work.exists():
                if self.work.is_symlink():
                    raise ValueError("owned scratch became a symlink")
                pending = self.output / "pending.json"
                logs = []
                for path in self.work.rglob("*"):
                    if not path.is_file() or path.is_symlink():
                        continue
                    if path.suffix in (".c", ".ll", ".S"):
                        self.keep_source(path)
                    elif path.suffix in (".log", ".stdout", ".stderr"):
                        logs.append(self._log_record(path, failed=True))
                self.write("recovered.json", {
                    "commands": json.loads(pending.read_text()) if pending.exists() else [],
                    "logs": logs,
                })
            self.recovered_bytes = self._remove_tree(self.work)
            self.work.mkdir()
            self.old_term = signal.getsignal(signal.SIGTERM)
            signal.signal(signal.SIGTERM, self._interrupt)
            self.write("lifecycle.json", {"status": "running", "recovered_bytes": self.recovered_bytes})
            self.write("commands.json", [])
            self.write("pending.json", [])
            return self
        except BaseException:
            self.lock.close()
            raise

    def _interrupt(self, signum, frame):
        raise KeyboardInterrupt(f"MIPS validation interrupted by signal {signum}")

    def discard(self, *paths):
        with self.guard:
            if self.deferred:
                return
            for path in paths:
                path = Path(path)
                if not path.is_relative_to(self.work) or path.is_symlink():
                    raise ValueError(f"not an owned temporary file: {path}")
                if path.exists():
                    self.removed_bytes += path.stat().st_size
                    path.unlink()

    def keep_source(self, path):
        path = Path(path)
        with self.guard:
            if self.deferred:
                return
            if path.is_file() and path.is_relative_to(self.work) and not path.is_symlink():
                target = self.output / "evidence" / (digest(path) + path.suffix)
                if not target.exists():
                    shutil.copyfile(path, target)
                    if digest(target) != digest(path):
                        raise RuntimeError("failed-source evidence copy changed")
                source = str(path.relative_to(self.work))
                checksum = digest(path)
                self.evidence[source, checksum] = {"source": source, "sha256": checksum, "file": target.name}
                self.write("evidence/index.json", list(self.evidence.values()))

    @staticmethod
    def _group_alive(group):
        try:
            os.killpg(group, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            # Darwin can report EPERM while an orphaned group is retiring.
            # It does not prove that all file readers have exited.
            return True

    def cancel(self):
        with self.guard:
            self.stopping = True
            groups = {process.pid for process in self.processes}
            groups.update(entry["group"] for entry in self.pending.values() if entry["group"] is not None)
            for group in groups:
                try:
                    os.killpg(group, signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def _log_record(self, path, failed):
        with path.open("rb") as stream:
            stream.seek(max(0, path.stat().st_size - 8192))
            tail = stream.read().decode(errors="replace")
        return {"name": path.name, "sha256": digest(path), "bytes": path.stat().st_size,
                "tail": tail if failed else ""}

    @contextmanager
    def process(self, command, stdout, stderr=None, env=None):
        """One owned subprocess; callers still own timing and result assertions."""
        command = [str(x) for x in command]
        process = None
        try:
            with stdout.open("wb") as out:
                err = stderr.open("wb") if stderr else None
                try:
                    with self.guard:
                        if self.stopping:
                            raise RuntimeError("MIPS validation cancelled")
                        self.pending[str(stdout)] = {"argv": command, "group": None, "stdout": stdout.name,
                                                     "stderr": stderr.name if stderr else None}
                        self.write("pending.json", list(self.pending.values()))
                        started = time.monotonic()
                        process = subprocess.Popen(command, stdout=out,
                                                   stderr=err or subprocess.STDOUT, env=env,
                                                   start_new_session=True, pass_fds=(self.lock.fileno(),))
                        self.processes.add(process)
                        self.pending[str(stdout)]["group"] = process.pid
                        self.write("pending.json", list(self.pending.values()))
                    yield process, started
                finally:
                    if process is not None:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGTERM)
                            try:
                                process.wait(timeout=2)
                            except subprocess.TimeoutExpired:
                                os.killpg(process.pid, signal.SIGKILL)
                                process.wait()
                        with self.guard:
                            self.processes.discard(process)
                    if err:
                        err.close()
        finally:
            if process is not None:
                # Never retain a duplicate full log for every successful sample.
                descendants = self._group_alive(process.pid)
                if descendants:
                    with self.guard:
                        self.deferred = self.stopping = True
                logs = []
                for path in (stdout, stderr):
                    if path is not None:
                        logs.append({"name": path.name, "deferred": True} if descendants
                                    else self._log_record(path, failed=bool(process.returncode)))
                with self.guard:
                    self.commands.append({"argv": command, "exit": process.returncode, "logs": logs})
                    self.write("commands.json", self.commands)
                    if not descendants:
                        self.pending.pop(str(stdout))
                    self.write("pending.json", list(self.pending.values()))
                if descendants:
                    raise RuntimeError("command leader exited with live descendants; scratch retained")
            else:
                # Popen reaps failed execs before raising OSError. A signal
                # during startup is not that guarantee: retain unknown owner
                # evidence rather than treating a missing Python handle as
                # proof that no child started.
                if isinstance(sys.exc_info()[1], OSError):
                    with self.guard:
                        self.pending.pop(str(stdout), None)
                        self.write("pending.json", list(self.pending.values()))

    def __exit__(self, kind, error, traceback):
        try:
            self.cancel()
            if any(entry["group"] is None or self._group_alive(entry["group"])
                   for entry in self.pending.values()):
                self.write("lifecycle.json", {"status": "deferred-process-readers",
                                              "error": str(error) if error else None})
                return False
            self.deferred = False
            if error:
                for path in self.work.rglob("*"):
                    if path.suffix in (".c", ".ll", ".S"):
                        self.keep_source(path)
            self._remove_tree(self.work)
            self.pending.clear()
            self.write("pending.json", [])
            self.write("lifecycle.json", {"status": "failed" if error else "finished",
                                          "error": str(error) if error else None,
                                          "removed_bytes": self.removed_bytes,
                                          "recovered_bytes": self.recovered_bytes,
                                          "commands": len(self.commands)})
        finally:
            signal.signal(signal.SIGTERM, self.old_term)
            self.lock.close()
