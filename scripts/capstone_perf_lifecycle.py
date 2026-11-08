#!/usr/bin/env python3
"""Own the disposable workspace of a local Capstone performance experiment.

Recipes and frozen inputs stay outside this workspace. A completed experiment
can retain its workspace for named readers; release those readers after their
last use. Canonical build caches and input libraries are never cleanup targets.
"""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
import uuid


class Interrupted(RuntimeError):
    def __init__(self, signum):
        self.signum = signum
        super().__init__(f"Interrupted by signal {signum}")


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


class Store:
    """One exclusive owner and one current experiment per benchmark root."""

    def __init__(self, root):
        self.directory = Path(root).resolve() / ".capstone-perf"
        self.workspace = self.directory / "current"
        self.state_path = self.directory / "state.json"
        self.lock = None

    def __enter__(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = (self.directory / "lock").open("a+")
        try:
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            raise RuntimeError(
                "A Capstone experiment or its child still owns the workspace"
            ) from None
        return self

    def __exit__(self, *_):
        self.lock.close()

    def state(self):
        return (
            json.loads(self.state_path.read_text())
            if self.state_path.exists()
            else None
        )

    def diagnostics(self):
        # Keep one bounded diagnostic, not a new report directory on every retry.
        records = []
        remaining = 16384
        for path in sorted((self.workspace / "logs").glob("*.log"), reverse=True):
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
                size = stream.tell()
                length = min(size, remaining)
                stream.seek(size - length)
                tail = stream.read().decode("utf-8", errors="replace")
            records.append(
                {
                    "name": path.name,
                    "bytes": size,
                    "sha256": digest.hexdigest(),
                    "tail": tail,
                }
            )
            remaining -= length
        return records

    def remove_workspace(self, state):
        if not self.workspace.exists():
            return
        if self.workspace.is_symlink():
            raise ValueError("Refusing a symlink as the owned workspace")
        marker = self.workspace / ".owner.json"
        if (
            state["status"] == "retiring"
            and not marker.exists()
            and not any(self.workspace.iterdir())
        ):
            self.workspace.rmdir()
            return
        if state["status"] == "creating" and not marker.exists():
            members = list(self.workspace.iterdir())
            if any(path.name != ".owner.json.tmp" for path in members):
                raise ValueError("Unexpected content in an unfinished workspace")
            for path in members:
                path.unlink()
            self.workspace.rmdir()
            return
        if not marker.is_file() or json.loads(marker.read_text()) != state["owner"]:
            raise ValueError("Workspace ownership marker does not match the journal")
        # Keep the marker until the last child is removed so recovery can finish
        # a partially completed retirement. Never follow an input symlink.
        for child in self.workspace.iterdir():
            if child == marker:
                continue
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child)
            else:
                child.unlink()
        marker.unlink()
        self.workspace.rmdir()

    def retire(self, state, reason, summary=None):
        creating = state["status"] == "creating"
        record = {
            "experiment": state["owner"]["experiment"],
            "reason": reason,
            "steps": state.get("steps", []),
            "summary": summary or {},
            "diagnostics": self.diagnostics(),
        }
        write_json(self.directory / "last-retired.json", record)
        if reason != "released":
            write_json(self.directory / "last-failure.json", record)
        state.update(status="creating" if creating else "retiring", readers=[])
        write_json(self.state_path, state)
        self.remove_workspace(state)
        state.update(status="retired")
        write_json(self.state_path, state)

    def recover(self):
        state = self.state()
        if state and state["status"] == "retiring":
            # The compact diagnostic was committed before retirement began.
            self.remove_workspace(state)
            state.update(status="retired")
            write_json(self.state_path, state)
        elif state and state["status"] in ("creating", "running"):
            # The inherited lock prevents recovery while an orphaned child is
            # still building, measuring, or reading an input in this workspace.
            self.retire(state, "recovered-interrupted-run")
        return self.state()

    def start(self, experiment, inputs):
        state = self.recover()
        if state and state["status"] == "ready":
            raise RuntimeError(f"Workspace still has readers: {state['readers']}")
        if self.workspace.exists():
            raise ValueError("Unregistered workspace; refusing to remove it")
        paths = [Path(path).resolve() for path in inputs]
        for path in paths:
            if not path.exists() or path.is_relative_to(self.workspace):
                raise ValueError(
                    f"Input must exist outside the disposable workspace: {path}"
                )
        owner = {"experiment": experiment, "token": uuid.uuid4().hex}
        state = {
            "owner": owner,
            "status": "creating",
            "readers": [],
            "inputs": list(map(str, paths)),
            "steps": [],
        }
        # Journal before creation. Recovery also handles interruption between
        # these writes without treating an unrelated pre-existing tree as ours.
        write_json(self.state_path, state)
        self.workspace.mkdir()
        write_json(self.workspace / ".owner.json", owner)
        (self.workspace / "logs").mkdir()
        state.update(status="running")
        write_json(self.state_path, state)
        return state

    def release(self, reader, summary):
        state = self.recover()
        if not state or state["status"] != "ready" or reader not in state["readers"]:
            raise ValueError(f"No current reader named {reader}")
        state["readers"].remove(reader)
        if state["readers"]:
            write_json(self.state_path, state)
        else:
            self.retire(state, "released", summary)


class Experiment:
    def __init__(self, root, name, inputs=(), readers=()):
        self.store = Store(root)
        self.name = name
        self.inputs = inputs
        self.readers = list(readers)
        self.child = None
        self.handlers = {}

    def __enter__(self):
        self.store.__enter__()
        try:
            self.state = self.store.start(self.name, self.inputs)
            self.workspace = self.store.workspace
            for signum in (signal.SIGINT, signal.SIGTERM):
                self.handlers[signum] = signal.getsignal(signum)
                signal.signal(signum, self.interrupt)
        except BaseException:
            self.store.__exit__()
            raise
        return self

    @staticmethod
    def interrupt(signum, _frame):
        raise Interrupted(signum)

    def environment(self):
        env = os.environ.copy()
        env.update(
            CAPSTONE_PERF_WORKSPACE=str(self.workspace),
            CAPSTONE_PERF_TOKEN=self.state["owner"]["token"],
            CAPSTONE_PERF_LOCK_FD=str(self.store.lock.fileno()),
        )
        return env

    def run(self, name, command, *, env=None, cwd=None):
        if not re.fullmatch(r"[a-zA-Z0-9_-]+", name):
            raise ValueError("Step name must be a stable filename")
        if any(step["name"] == name for step in self.state["steps"]):
            raise ValueError(f"Duplicate step: {name}")
        command = list(map(str, command))
        step = {"name": name, "command": command, "returncode": None}
        self.state["steps"].append(step)
        write_json(self.store.state_path, self.state)
        environment = self.environment()
        if env:
            environment.update(env)
        started = time.monotonic()
        try:
            with (self.workspace / "logs" / (name + ".log")).open("w") as stream:
                self.child = subprocess.Popen(
                    command,
                    cwd=cwd,
                    env=environment,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                    pass_fds=(self.store.lock.fileno(),),
                )
                status = self.child.wait()
                self.child = None
            step.update(returncode=status, seconds=time.monotonic() - started)
            write_json(self.store.state_path, self.state)
            if status:
                raise subprocess.CalledProcessError(status, command)
        except BaseException:
            self.stop_child()
            raise

    def stop_child(self):
        if self.child is None:
            return
        try:
            os.killpg(self.child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(self.child.pid, signal.SIGKILL)
            self.child.wait()
        self.child = None

    def __exit__(self, error_type, error, _traceback):
        try:
            self.stop_child()
            if error_type is not None:
                self.store.retire(
                    self.state,
                    "interrupted" if isinstance(error, Interrupted) else "failed",
                    {"error": str(error)[:2048]},
                )
            elif self.readers:
                self.state.update(status="ready", readers=self.readers)
                write_json(self.store.state_path, self.state)
            else:
                self.store.retire(self.state, "released")
        finally:
            for signum, handler in self.handlers.items():
                signal.signal(signum, handler)
            self.store.__exit__()


def managed_workspace():
    """Reject direct producer calls that bypass the owning driver."""
    try:
        workspace = Path(os.environ["CAPSTONE_PERF_WORKSPACE"])
        token = os.environ["CAPSTONE_PERF_TOKEN"]
        descriptor = int(os.environ["CAPSTONE_PERF_LOCK_FD"])
    except (KeyError, ValueError):
        raise RuntimeError(
            "Run this producer through its Capstone experiment driver"
        ) from None
    lock = workspace.parent / "lock"
    actual, expected = os.fstat(descriptor), lock.stat()
    if (actual.st_dev, actual.st_ino) != (expected.st_dev, expected.st_ino):
        raise ValueError("Experiment lock descriptor does not match")
    state = json.loads((workspace.parent / "state.json").read_text())
    if state["status"] != "running" or state["owner"]["token"] != token:
        raise ValueError("Experiment ownership does not match")
    if (
        workspace.name != "current"
        or json.loads((workspace / ".owner.json").read_text()) != state["owner"]
    ):
        raise ValueError("Unexpected experiment workspace")
    return workspace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("recover")
    release = commands.add_parser("release")
    release.add_argument("--reader", required=True)
    release.add_argument("--summary", required=True, type=Path)
    args = parser.parse_args()
    with Store(args.root) as store:
        if args.command == "recover":
            store.recover()
        elif args.command == "release":
            store.release(args.reader, json.loads(args.summary.read_text()))
        print(json.dumps(store.state(), indent=2))


if __name__ == "__main__":
    main()
