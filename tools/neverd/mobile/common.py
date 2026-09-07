"""Bounded filesystem and process operations shared by mobile importers."""

from __future__ import annotations

import os
import re
import shutil
import signal
import stat
import subprocess
import time
import zipfile
from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


class MobileError(Exception):
    """An input, dependency, or recovery failure suitable for a CLI diagnostic."""


@dataclass(frozen=True)
class Limits:
    timeout: int = 300
    max_files: int = 20000
    max_bytes: int = 2 * 1024 * 1024 * 1024

    def __post_init__(self):
        if min(self.timeout, self.max_files, self.max_bytes) <= 0:
            raise MobileError("timeout and input limits must be positive")


_workspace_budget: ContextVar[tuple[Path, Limits] | None] = ContextVar("mobile_budget", default=None)


def walk_error(error: OSError) -> None:
    raise MobileError(f"cannot enumerate input/output directory: {error}") from error


def validate_tree(root: Path, limits: Limits, *, live: bool = False) -> None:
    """Check generated files too: a backend must not publish links or devices."""
    if not stat.S_ISDIR(root.lstat().st_mode):
        raise MobileError("output root must remain a directory")

    def scan_error(error):
        if live and isinstance(error, FileNotFoundError):
            return
        walk_error(error)

    count = size = 0
    for current, dirs, files in os.walk(root, followlinks=False, onerror=scan_error):
        for name in dirs + files:
            path = Path(current) / name
            try:
                mode = path.lstat()
            except FileNotFoundError:
                if live:
                    continue  # Backends can retire temporary files mid-scan.
                raise
            if not (stat.S_ISREG(mode.st_mode) or stat.S_ISDIR(mode.st_mode)):
                raise MobileError("generated output contains a link or special file")
            count += 1
            size += mode.st_size if stat.S_ISREG(mode.st_mode) else 0
            if count > limits.max_files or size > limits.max_bytes:
                raise MobileError("generated output exceeds the configured limits")


@contextmanager
def workspace_budget(root: Path, limits: Limits):
    # Importers can temporarily retain both packaged and unpackaged input next
    # to generated output. Each individual input and final output is bounded;
    # the complete work area is additionally limited to three such sets.
    token = _workspace_budget.set((root, Limits(limits.timeout, limits.max_files * 3, limits.max_bytes * 3)))
    try:
        yield
    finally:
        _workspace_budget.reset(token)


def relative_member(name: str) -> Path:
    """Accept a portable relative path, with identical meaning on every host."""
    pieces = name.rstrip("/").split("/")
    if not name or "\\" in name or "\x00" in name or any(
        part in ("", ".", "..")
        or ":" in part
        or part.endswith((".", " "))
        or re.fullmatch(r"(?i)(con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\..*)?", part)
        for part in pieces
    ):
        raise MobileError(f"unsafe input path: {name!r}")
    return Path(*PurePosixPath(name).parts)


def extract_zip(source: Path, dest: Path, limits: Limits) -> list[Path]:
    """Validate the entire directory before writing any archive member."""
    try:
        with zipfile.ZipFile(source) as archive:
            entries = archive.infolist()
            if len(entries) > limits.max_files:
                raise MobileError("archive exceeds the file-count limit")
            seen: dict[str, bool] = {}
            spelling: dict[str, str] = {}
            total = 0
            members = []
            for entry in entries:
                if entry.orig_filename != entry.filename:
                    raise MobileError("archive path contains NUL or a nonportable separator")
                path = relative_member(entry.filename)
                for part in (path, *path.parents):
                    key_part = part.as_posix().casefold()
                    if key_part in spelling and spelling[key_part] != part.as_posix():
                        raise MobileError("archive paths have conflicting case")
                    spelling[key_part] = part.as_posix()
                if len(spelling) - 1 > limits.max_files:
                    raise MobileError("archive exceeds the file-count limit including directories")
                key = path.as_posix().casefold()
                if key in seen:
                    raise MobileError(f"duplicate archive path: {entry.filename!r}")
                directory = entry.is_dir()
                seen[key] = directory
                mode = entry.external_attr >> 16
                kind = stat.S_IFMT(mode)
                if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
                    raise MobileError(f"archive contains a link or special file: {entry.filename!r}")
                if (kind == stat.S_IFDIR and not directory) or (kind == stat.S_IFREG and directory):
                    raise MobileError("archive member type disagrees with its path")
                if entry.flag_bits & 1:
                    raise MobileError("encrypted ZIP entries are not supported")
                total += entry.file_size
                if total > limits.max_bytes:
                    raise MobileError("archive exceeds the uncompressed size limit")
                members.append((entry, path))
            for _, path in members:
                for parent in path.parents:
                    if seen.get(parent.as_posix().casefold()) is False:
                        raise MobileError("archive file conflicts with a directory")
            dest.mkdir(parents=True, exist_ok=True)
            written = []
            remaining = limits.max_bytes
            for entry, relative in members:
                target = dest / relative
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as src, target.open("xb") as dst:
                    count = 0
                    while chunk := src.read(min(1024 * 1024, remaining + 1)):
                        remaining -= len(chunk)
                        count += len(chunk)
                        if remaining < 0 or count > entry.file_size:
                            raise MobileError("archive exceeds its declared size")
                        dst.write(chunk)
                    if count != entry.file_size:
                        raise MobileError("truncated archive entry")
                written.append(target)
            return written
    except (zipfile.BadZipFile, NotImplementedError, RuntimeError, EOFError) as exc:
        raise MobileError(f"cannot read archive: {exc}") from exc


def safe_copy_tree(source: Path, dest: Path, limits: Limits) -> None:
    """Copy an input tree without following links or accepting special files."""
    if not stat.S_ISDIR(source.lstat().st_mode):
        raise MobileError("input tree must be a directory, not a symbolic link")
    entries = []
    size = 0
    for root, dirs, files in os.walk(source, followlinks=False, onerror=walk_error):
        dirs.sort()
        files.sort()
        for name in dirs + files:
            path = Path(root) / name
            relative = relative_member(path.relative_to(source).as_posix())
            mode = path.lstat().st_mode
            if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
                raise MobileError(f"input contains a link or special file: {relative}")
            size += path.stat().st_size if stat.S_ISREG(mode) else 0
            entries.append((path, relative, stat.S_ISDIR(mode)))
            if len(entries) > limits.max_files or size > limits.max_bytes:
                raise MobileError("input directory exceeds the configured limits")
    dest.mkdir(parents=True, exist_ok=True)
    for path, relative, directory in entries:
        target = dest / relative
        if directory:
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target, follow_symlinks=False)


def _stop_process(process: subprocess.Popen) -> None:
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            # Some restricted hosts report EPERM instead of ESRCH when the
            # complete group has already exited. A live leader must still be
            # stopped (and a real permission failure must remain an error).
            if process.poll() is None:
                process.kill()
                process.wait()
                raise
    else:
        process.kill()
    process.wait()


def run_tool(argv: list[str], log: Path, timeout: int, *, env: dict[str, str] | None = None) -> None:
    """Execute an argument vector with bounded diagnostics and runtime."""
    if not argv or timeout <= 0:
        raise MobileError("invalid backend command or timeout")
    if os.name == "nt" and Path(argv[0]).suffix.lower() in (".bat", ".cmd"):
        raise MobileError("batch launchers are unsupported; configure a native executable")
    log.parent.mkdir(parents=True, exist_ok=True)
    log_limit = 16 * 1024 * 1024
    job = None
    try:
        if os.name == "nt":
            from .windows_job import WindowsJob
            job = WindowsJob()
        with log.open("wb") as stream:
            process = subprocess.Popen(
                argv, stdin=subprocess.DEVNULL, stdout=stream,
                stderr=subprocess.STDOUT, shell=False,
                start_new_session=os.name == "posix",
                env=env,
            )
            deadline = time.monotonic() + timeout
            budget = _workspace_budget.get()
            next_budget_check = 0.0
            try:
                if job:
                    job.assign(process)
                while True:
                    if budget and time.monotonic() >= next_budget_check:
                        validate_tree(*budget, live=True)
                        next_budget_check = time.monotonic() + 1.0
                    if log.stat().st_size > log_limit:
                        _stop_process(process)
                        stream.truncate(log_limit)
                        raise MobileError("backend diagnostic output exceeded 16 MiB")
                    if process.poll() is not None:
                        # A successful wrapper must not leave writers running
                        # while its output is validated and published.
                        if os.name == "posix":
                            _stop_process(process)
                        if job:
                            job.close()
                        break
                    if time.monotonic() >= deadline:
                        _stop_process(process)
                        raise MobileError(f"backend timed out after {timeout} seconds")
                    time.sleep(0.05)
            except BaseException:
                if process.poll() is None:
                    _stop_process(process)
                raise
        if process.returncode != 0:
            with log.open("rb") as stream:
                stream.seek(max(0, log.stat().st_size - 4096))
                detail = stream.read().decode("utf-8", errors="replace").strip()
            raise MobileError(f"backend exited with status {process.returncode}: {detail}")
    except OSError as exc:
        raise MobileError(f"cannot execute backend {Path(argv[0]).name}: {exc}") from exc
    finally:
        if job:
            job.close()
