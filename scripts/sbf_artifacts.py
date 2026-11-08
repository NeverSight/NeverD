"""Explicit ownership and reader pins for disposable SBF experiment products.

Only registered products below one workspace are collected. Existing inputs,
build caches and recovery archives are never discovered by name or age.
"""

from contextlib import AbstractContextManager, contextmanager
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import threading
import uuid

try:
    import fcntl
except ImportError:
    fcntl = None

try:
    from .sbf_corpus_common import LIMITS, sha256
except ImportError:
    from sbf_corpus_common import LIMITS, sha256


_active = None
_OWNER = "NeverD SBF artifact workspace v1"


def child_options():
    """Keep the workspace lease alive while an owned child is still using it."""
    return {"pass_fds": (_active.lock.fileno(),)} if _active is not None else {}


def run_owned(command, *, check=False, timeout=None, **kwargs):
    """Run a build/consumer under the same lease and reap its group on interruption."""
    if kwargs.pop("capture_output", False):
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    process = subprocess.Popen(command, start_new_session=True,
                               **child_options(), **kwargs)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        if os.name == "posix":
            try:
                # A managed child (such as the corpus CLI) first cancels and
                # reaps its own separately isolated request groups.
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        elif process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()
        raise
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if check:
        result.check_returncode()
    return result


class ArtifactWorkspace(AbstractContextManager):
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.control = self.root / ".sbf-artifacts"
        self.lock = None
        self.state = None
        self.run_id = None
        self.role = None
        self.previous_term = None

    def __enter__(self):
        global _active
        if fcntl is None:
            raise RuntimeError("managed SBF artifact workspaces require POSIX file locks")
        if _active is not None:
            raise RuntimeError("an SBF artifact workspace is already active in this process")
        self.control.mkdir(parents=True, exist_ok=True)
        if self.control.is_symlink():
            raise ValueError("artifact control directory must not be a symlink")
        self.lock = (self.control / "lock").open("a+b")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            raise RuntimeError("the SBF artifact workspace has an active producer or reader") from None
        try:
            path = self.control / "state.json"
            self.state = json.loads(path.read_text()) if path.exists() else {
                "owner": _OWNER, "entries": {}, "pins": {}, "active": None}
            if self.state.get("owner") != _OWNER:
                raise ValueError("unrecognized artifact workspace ownership")
            self.restore_source()
            self.retire_build_products()
            # Acquiring the same lease proves that no registered child still
            # uses a producer interrupted by SIGKILL. finally is not assumed.
            interrupted = self.state["active"]
            if interrupted:
                entry = self.state["entries"][interrupted]
                self.summary(entry["role"], self.describe(interrupted, "interrupted producer recovered"))
                self.state["active"] = None
            (self.control / "state.tmp").unlink(missing_ok=True)
            self.collect()
            self.save()
            _active = self
            if threading.current_thread() is threading.main_thread():
                def terminate(signum, frame):
                    raise KeyboardInterrupt
                self.previous_term = signal.signal(signal.SIGTERM, terminate)
            return self
        except BaseException:
            self.lock.close()
            raise

    def __exit__(self, kind, error, traceback):
        global _active
        try:
            self.finish("completed" if kind is None else f"{kind.__name__}: {error}")
        finally:
            _active = None
            if self.previous_term is not None:
                signal.signal(signal.SIGTERM, self.previous_term)
            self.lock.close()
        return False

    def finish(self, disposition="completed"):
        self.restore_source()
        self.retire_build_products()
        if self.run_id:
            self.summary(self.role, self.describe(self.run_id, disposition))
            self.state["active"] = None
        self.collect()
        self.save()
        self.run_id = self.role = None

    def save(self):
        temporary = self.control / "state.tmp"
        with temporary.open("w") as stream:
            json.dump(self.state, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(self.control / "state.json")

    @staticmethod
    def name(value):
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,79}", value):
            raise ValueError("artifact role/pin must be a short stable identifier")
        return value

    def relative(self, path):
        path = Path(path).absolute()
        if path.is_symlink() or not path.resolve().is_relative_to(self.root):
            raise ValueError(f"artifact escapes workspace: {path}")
        for parent in path.parents:
            # Resolve aliases above the workspace (macOS /var -> /private/var),
            # but never follow a symlink within a registered product.
            if parent.is_symlink():
                raise ValueError(f"artifact parent is a symlink: {parent}")
            if parent.resolve() == self.root:
                break
        relative = path.resolve().relative_to(self.root)
        if not relative.parts or relative.parts == (".sbf-artifacts",):
            raise ValueError("cannot own the workspace or its control directory")
        return str(relative)

    def begin(self, role, metadata=None):
        if self.run_id:
            raise RuntimeError("one producer bundle per workspace lease")
        self.role = self.name(role)
        self.run_id = uuid.uuid4().hex
        self.state["entries"][self.run_id] = {
            "role": self.role, "paths": [], "metadata": metadata or {}}
        self.state["active"] = self.run_id
        self.save()
        return self

    def own(self, path, *, directory=False):
        """Reserve a new generated path before the producer can create it."""
        if not self.run_id:
            raise RuntimeError("begin a producer before registering output")
        path = Path(path).absolute()
        relative = self.relative(path)
        if Path(relative).parts[0] == ".sbf-artifacts":
            raise ValueError("producer output cannot replace lifecycle metadata")
        if path.exists():
            raise FileExistsError(path)
        self.state["entries"][self.run_id]["paths"].append({
            "path": relative, "directory": directory})
        self.save()
        path.parent.mkdir(parents=True, exist_ok=True)
        if directory:
            path.mkdir()
            (path / ".sbf-owner").write_text(self.run_id)
        return path

    def keep(self, reader):
        """Keep this bundle for a concrete reader after the producer returns."""
        if not self.run_id:
            raise RuntimeError("no producer bundle to retain")
        self.state["pins"][self.name(reader)] = self.run_id
        self.save()

    def pin(self, reader, existing_reader):
        self.state["pins"][self.name(reader)] = self.state["pins"][existing_reader]
        self.save()

    def retained_path(self, reader):
        entry_id = self.state["pins"].get(reader)
        if entry_id is None:
            return None
        paths = self.state["entries"][entry_id]["paths"]
        if len(paths) != 1:
            raise ValueError("retained_path requires a single owned product")
        path = self.root / paths[0]["path"]
        self.relative(path)
        if paths[0]["directory"]:
            if (path / ".sbf-owner").read_text() != entry_id:
                raise ValueError(f"directory ownership changed: {path}")
        elif paths[0].get("sha256") and sha256(path) != paths[0]["sha256"]:
            raise ValueError(f"retained artifact changed: {path}")
        return path

    def release(self, reader):
        self.state["pins"].pop(self.name(reader), None)
        self.collect()
        self.save()

    def temporary_build_product(self, path):
        """Invalidate an explicitly owned diagnostic executable after readers exit.

        Record the prior digest before a build can replace it. An unchanged
        reusable binary survives a failed build; a changed/partial diagnostic
        binary must not later masquerade as a canonical-source build.
        """
        if not self.run_id:
            raise RuntimeError("begin a diagnostic producer first")
        path = Path(path).absolute()
        if path.is_symlink():
            raise ValueError("temporary build product must not be a symlink")
        self.state.setdefault("build_products", []).append({
            "path": str(path.resolve()), "producer": self.run_id,
            "before_sha256": sha256(path) if path.exists() else None})
        self.save()

    def retire_build_products(self):
        if not self.state.get("build_products"):
            return
        for item in self.state.get("build_products", []):
            path = Path(item["path"])
            if path.is_symlink():
                raise ValueError("diagnostic build product ownership changed")
            if path.exists():
                digest = sha256(path)
                if digest != item["before_sha256"]:
                    entry = self.state["entries"][item["producer"]]
                    entry["metadata"].setdefault("retired_build_products", []).append({
                        "path": str(path), "sha256": digest, "bytes": path.stat().st_size})
                    path.unlink()
        self.state.pop("build_products", None)
        self.save()

    @contextmanager
    def source_overlay(self, source, replacement):
        """Restore this exact diagnostic edit, including at the next startup.

        An intervening edit fails closed with both copies preserved. The
        original may already contain uncommitted research changes.
        """
        if self.state.get("source_edit"):
            raise RuntimeError("one diagnostic source overlay per workspace")
        source = Path(source).resolve()
        original = source.read_bytes()
        backup = self.control / "source-original"
        staged = source.with_name(source.name + ".sbf-overlay.tmp")
        if staged.exists() or staged.is_symlink():
            raise FileExistsError(staged)
        backup.write_bytes(original)
        self.state["source_edit"] = {
            "path": str(source), "original_sha256": sha256(source),
            "overlay_sha256": hashlib.sha256(replacement).hexdigest(),
            "mode": source.stat().st_mode & 0o777}
        self.save()
        try:
            staged.write_bytes(replacement)
            staged.chmod(self.state["source_edit"]["mode"])
            staged.replace(source)
            yield
        finally:
            self.restore_source()

    def restore_source(self):
        edit = self.state.get("source_edit")
        if edit is None:
            return
        source = Path(edit["path"])
        backup = self.control / "source-original"
        if source.is_symlink() or sha256(backup) != edit["original_sha256"]:
            raise ValueError("diagnostic source ownership changed; preserve recovery copies")
        if sha256(source) not in {edit["original_sha256"], edit["overlay_sha256"]}:
            raise ValueError("diagnostic source changed; preserve recovery copies")
        staged = source.with_name(source.name + ".sbf-overlay.tmp")
        staged.write_bytes(backup.read_bytes())
        staged.chmod(edit["mode"])
        staged.replace(source)
        self.state.pop("source_edit")
        self.save()
        backup.unlink()

    def summary(self, role, value):
        # One compact current receipt per stable role, not a growing archive.
        payload = json.dumps(value, indent=2) + "\n"
        if len(payload.encode()) > LIMITS.max_report_bytes:
            raise ValueError("artifact summary exceeds the existing report budget")
        target = self.control / "summaries" / (self.name(role) + ".json")
        target.parent.mkdir(exist_ok=True)
        temporary = target.with_suffix(".tmp")
        temporary.write_text(payload)
        temporary.replace(target)
        return target

    def describe(self, entry_id, disposition):
        entry = self.state["entries"][entry_id]
        result = {"role": entry["role"], "disposition": disposition,
                  "metadata": entry["metadata"], "files": 0, "bytes": 0,
                  "diagnostic_tails": {}, "tails_truncated": False}
        remaining = 65536
        for item in entry["paths"]:
            path = self.root / item["path"]
            files = path.rglob("*") if item["directory"] else [path]
            for file in files:
                if file.is_symlink() or not file.is_file():
                    continue
                size = file.stat().st_size
                result["files"] += 1
                result["bytes"] += size
                if file.name in {"stderr.txt", "events.jsonl"} or file.suffix == ".log":
                    length = min(size, 4096, remaining)
                    if length:
                        with file.open("rb") as stream:
                            stream.seek(size - length)
                            result["diagnostic_tails"][str(file.relative_to(self.root))] = stream.read().decode(errors="replace")
                        remaining -= length
                    result["tails_truncated"] |= length != size
        return result

    def collect(self):
        live = set(self.state["pins"].values()) | {self.state["active"]}
        for entry_id, entry in list(self.state["entries"].items()):
            if entry_id in live:
                continue
            for item in entry["paths"]:
                path = self.root / item["path"]
                self.relative(path)
                if not path.exists():
                    continue
                if item["directory"]:
                    marker = path / ".sbf-owner"
                    if marker.is_file() and marker.read_text() == entry_id:
                        shutil.rmtree(path)
                    elif not any(path.iterdir()):
                        path.rmdir()  # killed between mkdir and ownership marker
                    else:
                        raise ValueError(f"directory ownership changed: {path}")
                else:
                    if item.get("sha256") and sha256(path) != item["sha256"]:
                        raise ValueError(f"retained artifact changed: {path}")
                    path.unlink()
            del self.state["entries"][entry_id]

    def snapshot_worker(self, source, reader):
        """Share identical immutable workers; keep only named pending readers."""
        source = Path(source)
        reader = self.name(reader)
        digest = sha256(source)
        entry_id = "worker-" + digest
        path = self.control / "workers" / digest
        if entry_id in self.state["entries"]:
            if sha256(path) != digest:
                raise ValueError("frozen worker changed")
        else:
            self.state["entries"][entry_id] = {
                "role": reader, "metadata": {"worker_sha256": digest},
                "paths": [{"path": str(path.relative_to(self.root)), "directory": False}]}
            self.save()
            path.parent.mkdir(exist_ok=True)
            with source.open("rb") as src, path.open("xb") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
            if sha256(path) != digest:
                raise ValueError("frozen worker copy differs from its input")
            path.chmod(0o555)
            self.state["entries"][entry_id]["paths"][0]["sha256"] = digest
        self.state["pins"][reader] = entry_id
        self.collect()
        self.save()
        return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    pin = commands.add_parser("pin", help="keep an existing result for another reader")
    pin.add_argument("reader")
    pin.add_argument("existing_reader")
    release = commands.add_parser("release", help="finish or abandon a named reader")
    release.add_argument("reader")
    commands.add_parser("recover", help="recover interrupted owned producers when no reader is live")
    commands.add_parser("status", help="show the current registered products and readers")
    args = parser.parse_args()
    with ArtifactWorkspace(args.workspace) as artifacts:
        if args.command == "pin":
            artifacts.pin(args.reader, args.existing_reader)
        elif args.command == "release":
            artifacts.release(args.reader)
        print(json.dumps(artifacts.state, indent=2))


if __name__ == "__main__":
    main()
