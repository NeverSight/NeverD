#!/usr/bin/env python3
"""Own Android validation work, with explicit readers and bounded retry evidence.

Only directories created by this store are retired. Installed SDKs, input APKs,
source trees and existing verification archives are references, never outputs.
The work root may be on scratch storage; state and compact evidence can live on
a separate persistent volume. POSIX locks also protect surviving child jobs.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
try:
    import fcntl
except ImportError:
    fcntl = None
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
import urllib.request


SCHEMA = "neverd-android-artifacts-1"
DIAGNOSTIC_BYTES = 16384  # The same failure tail shown by the CI shard runner.


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def read_json(path: Path):
    return json.loads(path.read_text())


def write_json(path: Path, value) -> None:
    temporary = path.with_suffix(path.suffix + ".part")
    with temporary.open("w") as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value):
        raise ValueError("artifact slot/reader names must be simple identifiers")
    return value


def group_alive(pid: int | None) -> bool:
    if pid is None:
        return False
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_group(process: subprocess.Popen, grace: float = 5) -> None:
    deadline = time.monotonic() + grace
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass
    # A compiler/JVM can survive the leader. Reap the whole group before cleanup.
    remaining = deadline - time.monotonic()
    if remaining > 0 and group_alive(process.pid):
        time.sleep(remaining)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


class Interrupted(Exception):
    def __init__(self, signum: int):
        self.signum = signum


class ArtifactStore:
    def __init__(self, root: Path, work_root: Path | None = None):
        if fcntl is None:
            raise RuntimeError("Android artifact jobs require POSIX process groups and locks")
        if root.is_symlink() or (work_root is not None and work_root.is_symlink()):
            raise ValueError("artifact roots cannot be symlinks")
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.work_root = (work_root or self.root / "work").resolve()
        with self.lock():
            marker = self.root / "owner.json"
            wanted = {"schema": SCHEMA, "work_root": str(self.work_root)}
            if marker.exists():
                if read_json(marker) != wanted:
                    raise ValueError("artifact store owner/work root changed")
            else:
                if set(p.name for p in self.root.iterdir()) - {".lock"}:
                    raise ValueError("refusing to adopt a nonempty artifact root")
                write_json(marker, wanted)
            self.work_root.mkdir(parents=True, exist_ok=True)
            work_marker = self.work_root / ".owner.json"
            work_owner = {"schema": SCHEMA, "state_root": str(self.root)}
            if work_marker.exists():
                if read_json(work_marker) != work_owner:
                    raise ValueError("work root belongs to another artifact store")
            else:
                if any(self.work_root.iterdir()):
                    raise ValueError("refusing to adopt a nonempty work root")
                write_json(work_marker, work_owner)
            for part in ("jobs", "locks", "evidence", "cache"):
                (self.root / part).mkdir(exist_ok=True)
            if not (self.root / "cache.json").exists():
                write_json(self.root / "cache.json", {})
        self.recover()

    @contextmanager
    def lock(self):
        with (self.root / ".lock").open("a+b") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            yield

    def _lease(self, slot: str, shared: bool = False):
        stream = (self.root / "locks" / (name(slot) + ".lock")).open("a+b")
        try:
            fcntl.flock(stream, (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | fcntl.LOCK_NB)
        except BaseException:
            stream.close()
            raise
        return stream

    def _record(self, slot: str) -> Path:
        return self.root / "jobs" / (name(slot) + ".json")

    def _work(self, state: dict) -> Path:
        path = self.work_root / name(state["slot"])
        if path.is_symlink():
            raise ValueError("owned work directory was replaced with a symlink")
        if path.exists():
            marker = path / ".owner.json"
            if marker.exists():
                if read_json(marker) != {"schema": SCHEMA, "token": state["token"],
                                         "slot": state["slot"]}:
                    raise ValueError("owned work directory identity changed")
            elif set(p.name for p in path.iterdir()) - {".owner.json.part"}:
                raise ValueError("incomplete workspace contains unregistered files")
        return path

    def _compact(self, state: dict) -> None:
        work = self._work(state)
        if not work.exists():
            return
        files = []
        roots = [("", work), *((f"attached-{i}/", path)
                                for i, path in enumerate(self._outputs(state)))]
        for prefix, root in roots:
            for path in sorted(root.rglob("*")):
                relative = prefix + path.relative_to(root).as_posix()
                if path.is_symlink():
                    files.append({"path": relative, "symlink": os.readlink(path)})
                elif path.is_file():
                    files.append({"path": relative, "bytes": path.stat().st_size,
                                  "sha256": digest(path)})
        evidence = self.root / "evidence"
        index = evidence / (state["slot"] + ".files.json.gz")
        payload = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
        temporary = index.with_suffix(".part")
        temporary.write_bytes(gzip.compress(payload, mtime=0))
        if gzip.decompress(temporary.read_bytes()) != payload:
            raise RuntimeError("compact inventory round-trip failed")
        temporary.replace(index)
        log = work / "driver.log"
        tail = ""
        if log.is_file():
            with log.open("rb") as stream:
                stream.seek(max(0, log.stat().st_size - DIAGNOSTIC_BYTES))
                tail = stream.read().decode(errors="replace")
        summary = {**state, "files": len(files),
                   "apparent_work_bytes": sum(row.get("bytes", 0) for row in files),
                   "inventory_sha256": digest(index), "diagnostic_tail": tail}
        # These are fixture verdicts, not the potentially huge decompiler report.
        for filename in ("summary.json", "evidence.json"):
            path = work / "output" / filename
            if path.is_file() and path.stat().st_size <= DIAGNOSTIC_BYTES:
                try:
                    summary[filename] = read_json(path)
                except (json.JSONDecodeError, UnicodeDecodeError) as error:
                    summary[filename] = {"parse_error": type(error).__name__,
                                         "sha256": digest(path)}
        write_json(evidence / (state["slot"] + ".json"), summary)

    def _outputs(self, state: dict) -> list[Path]:
        result = []
        for row in state.get("outputs", []):
            path = Path(row["path"])
            parent = path.parent.stat()
            if [parent.st_dev, parent.st_ino] != row["parent_identity"]:
                raise ValueError("declared output parent identity changed")
            if path.is_symlink() or (path.exists() and not path.is_dir()):
                raise ValueError("declared output is not an owned directory")
            result.append(path)
        return result

    def _retire(self, state: dict) -> None:
        if state["readers"] or group_alive(state.get("pgid")):
            return
        self._compact(state)
        for output in self._outputs(state):
            if output.exists():
                shutil.rmtree(output)
        work = self._work(state)
        if work.exists():
            shutil.rmtree(work)
        state["retired"] = True
        write_json(self._record(state["slot"]), state)
        evidence = self.root / "evidence" / (state["slot"] + ".json")
        if evidence.exists():
            summary = read_json(evidence)
            summary["retired"] = True
            write_json(evidence, summary)

    def _gc_cache(self) -> None:
        aliases = read_json(self.root / "cache.json")
        keep = {row["sha256"] for row in aliases.values()}
        for record in (self.root / "jobs").glob("*.json"):
            state = read_json(record)
            if state["phase"] == "running" or state["readers"] or not state.get("retired"):
                keep.update(row["sha256"] for row in state["inputs"]
                            if Path(row["path"]).parent == self.root / "cache")
        for path in (self.root / "cache").iterdir():
            if path.name.endswith(".part"):
                path.unlink()
            elif re.fullmatch(r"[0-9a-f]{64}\.jar", path.name) and path.stem not in keep:
                if path.is_symlink() or digest(path) != path.stem:
                    raise ValueError("owned cached input changed")
                path.unlink()

    def recover(self) -> None:
        with self.lock():
            for record in (self.root / "jobs").glob("*.json"):
                state = read_json(record)
                try:
                    lease = self._lease(state["slot"])
                except BlockingIOError:
                    continue
                with lease:
                    if group_alive(state.get("pgid")):
                        continue
                    if state["phase"] == "running":
                        state.update(phase="abandoned", returncode=None)
                        state["readers"] = [r for r in state["readers"]
                                            if r not in state.get("success_readers", [])]
                        write_json(record, state)
                    if not state.get("retired"):
                        self._retire(state)
            self._gc_cache()

    def begin(self, slot: str, inputs=(), readers=(), success_readers=(), outputs=()) -> "Job":
        slot = name(slot)
        inputs = [{"path": str(Path(p).resolve()), "sha256": digest(Path(p))}
                  for p in inputs]
        readers = sorted({name(reader) for reader in readers})
        success_readers = sorted({name(reader) for reader in success_readers})
        if set(readers) & set(success_readers):
            raise ValueError("reader must have one retention condition")
        self.recover()
        with self.lock():
            lease = self._lease(slot)
            try:
                record = self._record(slot)
                if record.exists() and not read_json(record).get("retired"):
                    raise RuntimeError("slot still has an active or pending reader")
                path = self.work_root / slot
                if path.exists():
                    raise ValueError("unregistered work directory; refusing to replace it")
                declared = []
                for output in outputs:
                    output = Path(output).absolute()
                    if output.is_symlink() or (output.exists() and
                                               (not output.is_dir() or any(output.iterdir()))):
                        raise ValueError("declared output must be absent or an empty directory")
                    output = output.parent.resolve() / output.name
                    protected = [self.root, self.work_root, *(Path(row["path"]) for row in inputs)]
                    for other_record in (self.root / "jobs").glob("*.json"):
                        prior = read_json(other_record)
                        if not prior.get("retired"):
                            protected.extend(Path(row["path"]) for row in prior.get("outputs", []))
                    if any(output.is_relative_to(p) or p.is_relative_to(output) for p in protected):
                        raise ValueError("declared output overlaps a protected input or active output")
                    if any(output.is_relative_to(Path(row["path"])) or
                           Path(row["path"]).is_relative_to(output) for row in declared):
                        raise ValueError("declared outputs overlap")
                    parent = output.parent.stat()
                    declared.append({"path": str(output),
                                     "parent_identity": [parent.st_dev, parent.st_ino]})
                state = {"schema": SCHEMA, "slot": slot, "token": uuid.uuid4().hex,
                         "phase": "running", "readers": sorted(readers + success_readers),
                         "success_readers": success_readers, "inputs": inputs, "outputs": declared,
                         "pgid": None, "retired": False}
                write_json(record, state)
                path.mkdir()
                write_json(path / ".owner.json", {k: state[k] for k in ("schema", "slot", "token")})
                return Job(self, state, lease, path)
            except BaseException:
                lease.close()
                raise

    @contextmanager
    def reader(self, slot: str, reader: str):
        with self.lock():
            lease = self._lease(slot, shared=True)
            try:
                state = read_json(self._record(slot))
                if reader not in state["readers"] or state.get("retired"):
                    raise ValueError("reader was not registered before production")
                work = self._work(state)
            except BaseException:
                lease.close()
                raise
        try:
            yield work
        finally:
            lease.close()

    def release(self, slot: str, reader: str) -> None:
        with self.lock():
            with self._lease(slot):
                state = read_json(self._record(slot))
                if state["phase"] == "running":
                    raise RuntimeError("producer is still running")
                state["readers"].remove(reader)
                write_json(self._record(slot), state)
                self._retire(state)
            self._gc_cache()

    def cached(self, alias: str, sha256: str) -> Path | None:
        with self.lock():
            row = read_json(self.root / "cache.json").get(name(alias))
            if row is None or row["sha256"] != sha256:
                return None
            path = self.root / "cache" / (sha256 + ".jar")
            if path.is_symlink() or digest(path) != sha256:
                raise ValueError("cached declaration input changed")
            return path

    def retire_cache(self, alias: str) -> None:
        """Drop a consumed staging input; active/pending readers still keep it."""
        with self.lock():
            aliases = read_json(self.root / "cache.json")
            del aliases[name(alias)]
            write_json(self.root / "cache.json", aliases)
            self._gc_cache()

    def stage(self, alias: str, source: Path, sha256: str, provenance: dict) -> Path:
        name(alias)
        if not re.fullmatch(r"[0-9a-f]{64}", sha256) or digest(source) != sha256:
            raise ValueError("declaration input SHA-256 mismatch")
        with self.lock():
            path = self.root / "cache" / (sha256 + ".jar")
            if not path.exists():
                temporary = path.with_suffix(".part")
                try:
                    shutil.copyfile(source, temporary)
                    if digest(temporary) != sha256:
                        raise ValueError("staged declaration input changed")
                    temporary.replace(path)
                finally:
                    temporary.unlink(missing_ok=True)
            if digest(path) != sha256:
                raise ValueError("existing cached declaration input changed")
            aliases = read_json(self.root / "cache.json")
            aliases[alias] = {"sha256": sha256, "provenance": provenance}
            write_json(self.root / "cache.json", aliases)
            self._gc_cache()
            return path

    def download(self, alias: str, url: str, sha256: str) -> Path:
        """Reuse verified published bytes, retiring partial/failed downloads."""
        existing = self.cached(alias, sha256)
        if existing is not None:
            return existing
        job = self.begin("sdk-" + name(alias))
        phase, code = "failed", 1
        handlers = {}

        def interrupt(signum, frame):
            raise Interrupted(signum)

        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                handlers[sig] = signal.signal(sig, interrupt)
            target = job.work / "download.jar"
            with urllib.request.urlopen(url, timeout=45) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            result = self.stage(alias, target, sha256, {"url": url})
            phase, code = "success", 0
            return result
        except Interrupted as error:
            phase, code = "interrupted", 128 + error.signum
            raise
        except BaseException as error:
            job.state["exception"] = type(error).__name__ + ": " + str(error)
            raise
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            job.finish(phase, code)


class Job:
    def __init__(self, store, state, lease, work):
        self.store, self.state, self.lease, self.work = store, state, lease, work

    def finish(self, phase: str, returncode: int | None = None) -> None:
        if self.lease.closed:
            raise RuntimeError("job already finished")
        try:
            with self.store.lock():
                self.state.update(phase=phase, returncode=returncode)
                if phase != "success":
                    self.state["readers"] = [r for r in self.state["readers"]
                                             if r not in self.state["success_readers"]]
                write_json(self.store._record(self.state["slot"]), self.state)
                if self.state["readers"] or group_alive(self.state.get("pgid")):
                    self.store._compact(self.state)
                self.store._retire(self.state)
                self.store._gc_cache()
        finally:
            self.lease.close()

    def run(self, command, *, cwd=None, env=None, timeout=None, after_success=None) -> dict:
        command = [str(x).replace("{work}", str(self.work)) for x in command]
        self.state["command"] = command
        with self.store.lock():
            write_json(self.store._record(self.state["slot"]), self.state)
        start = time.monotonic()
        process = None
        handlers = {}
        code, phase = None, "failed"
        timed_out = False

        def interrupt(signum, frame):
            raise Interrupted(signum)

        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                handlers[sig] = signal.signal(sig, interrupt)
            with (self.work / "driver.log").open("xb") as output:
                process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True,
                                           pass_fds=(self.lease.fileno(),))
                with self.store.lock():
                    self.state["pgid"] = process.pid
                    write_json(self.store._record(self.state["slot"]), self.state)
                try:
                    code = process.wait(timeout=timeout)
                    phase = "success" if code == 0 else "failed"
                    if code == 0 and after_success is not None:
                        after_success(self.work)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    stop_group(process)
                    code, phase = 124, "timeout"
        except Interrupted as error:
            if process is not None:
                stop_group(process)
            code, phase = 128 + error.signum, "interrupted"
        except BaseException as error:
            phase, code = "failed", 1
            self.state["exception"] = type(error).__name__ + ": " + str(error)
            if process is not None:
                stop_group(process)
            raise
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            self.state["elapsed_seconds"] = round(time.monotonic() - start, 3)
            self.finish(phase, code)
        return {"returncode": code, "timed_out": timed_out,
                "elapsed_seconds": self.state["elapsed_seconds"],
                "evidence": str(self.store.root / "evidence" / (self.state["slot"] + ".json"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path)
    commands = parser.add_subparsers(dest="action", required=True)
    run = commands.add_parser("run")
    run.add_argument("--slot", required=True)
    run.add_argument("--input", action="append", type=Path, default=[])
    run.add_argument("--reader", action="append", default=[])
    run.add_argument("--success-reader", action="append", default=[])
    run.add_argument("--output-directory", action="append", type=Path, default=[],
                     help="additional absent/empty output owned by this job, e.g. a frozen audit path")
    run.add_argument("--cwd", type=Path)
    run.add_argument("--timeout", type=float)
    run.add_argument("command", nargs=argparse.REMAINDER)
    release = commands.add_parser("release")
    release.add_argument("--slot", required=True)
    release.add_argument("--reader", required=True)
    commands.add_parser("recover")
    stage = commands.add_parser("stage")
    stage.add_argument("--alias", required=True)
    stage.add_argument("--source", required=True, type=Path)
    stage.add_argument("--sha256", required=True)
    download = commands.add_parser("download")
    download.add_argument("--alias", required=True)
    download.add_argument("--url", required=True)
    download.add_argument("--sha256", required=True)
    args = parser.parse_args()
    store = ArtifactStore(args.root, args.work_root)
    if args.action == "run":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        if not command:
            parser.error("run requires a command")
        job = store.begin(args.slot, args.input, args.reader, args.success_reader, args.output_directory)
        result = job.run(command, cwd=args.cwd, timeout=args.timeout)
        print(json.dumps(result))
        return result["returncode"]
    if args.action == "release":
        store.release(args.slot, args.reader)
    if args.action == "stage":
        print(store.stage(args.alias, args.source, args.sha256, {"source": str(args.source)}))
    if args.action == "download":
        print(store.download(args.alias, args.url, args.sha256))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
