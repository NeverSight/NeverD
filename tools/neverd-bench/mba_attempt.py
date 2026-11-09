#!/usr/bin/env python3
"""Own the scratch and bounded evidence of a local MBA research attempt.

The SDK, corpus and reusable build directories remain outside this directory.
Only directories created by this runner are rotated. Keep the current and
previous attempt for diagnosis; temporary compiler/fixture output is disposable.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

OWNER = "neverd-mba-attempt-v1"


def write_record(path, value):
    temporary = path.with_suffix(".tmp")
    with temporary.open("w") as output:
        json.dump(value, output, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def owned_record(directory):
    if directory.is_symlink():
        raise ValueError(f"Attempt directory is a symlink: {directory}")
    # The parent marker already owns these two slots. A kill during setup can
    # leave a slot before its first atomic record has been published.
    if not (directory / "attempt.json").exists():
        return {"owner": OWNER, "status": "running", "command": [],
                "setupIncomplete": True}
    record = json.loads((directory / "attempt.json").read_text())
    if record.get("owner") != OWNER:
        raise ValueError(f"Unowned attempt directory: {directory}")
    return record


def remove_scratch(directory):
    scratch = directory / "tmp"
    if scratch.is_symlink():
        scratch.unlink()
    elif scratch.exists():
        shutil.rmtree(scratch)


def stop_group(pid):
    # This is the session started for this command, never a saved old PID.
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


def run_attempt(directory, command):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "lease").open("a+") as lease:
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another MBA attempt or its child still holds the lease")
        marker = directory / "owner.json"
        if not marker.exists():
            if any(path.name != "lease" for path in directory.iterdir()):
                raise ValueError("Refusing to adopt a nonempty artifact directory")
            write_record(marker, {"owner": OWNER})
        elif json.loads(marker.read_text()) != {"owner": OWNER}:
            raise ValueError("Artifact directory belongs to another producer")

        current, previous = directory / "current", directory / "previous"
        if previous.exists():
            owned_record(previous)
            shutil.rmtree(previous)
        if current.exists():
            record = owned_record(current)
            remove_scratch(current)
            if record["status"] == "running":
                record["status"] = "interrupted"
                write_record(current / "attempt.json", record)
            current.rename(previous)
        current.mkdir()
        scratch = current / "tmp"
        scratch.mkdir()
        record = {"owner": OWNER, "status": "running", "command": command}
        write_record(current / "attempt.json", record)
        environment = {
            **os.environ,
            "NEVERD_MBA_ATTEMPT": str(current),
            "NEVERD_MBA_LEASE_FD": str(lease.fileno()),
            "TMPDIR": str(scratch),
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        process = None

        def interrupt(signum, _frame):
            raise KeyboardInterrupt(signum)

        old_handlers = {sig: signal.signal(sig, interrupt)
                        for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            process = subprocess.Popen(command, env=environment,
                                       pass_fds=(lease.fileno(),),
                                       start_new_session=True)
            result = process.wait()
            record.update(status="passed" if result == 0 else "failed",
                          returncode=result)
            return result
        except KeyboardInterrupt:
            record.update(status="interrupted", returncode=130)
            return 130
        except BaseException:
            record.update(status="failed", returncode=1)
            raise
        finally:
            if process is not None:
                stop_group(process.pid)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                # A failed driver may have exited before one of its children.
                # No background work outlives this owned attempt.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            remove_scratch(current)
            write_record(current / "attempt.json", record)
            for sig, handler in old_handlers.items():
                signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a producer command is required")
    try:
        return run_attempt(args.directory, command)
    except (ValueError, RuntimeError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    sys.exit(main())
