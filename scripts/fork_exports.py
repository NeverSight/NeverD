"""Own immutable fork exports and their graph/frozen-reader lifetimes.

All mutations run under fork_dependencies.build_lock. Existing, unmarked
exports are caller-owned and are never adopted for automatic deletion.
"""

import json
import os
from pathlib import Path
import re
import shutil
import uuid


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Callers serialize this fixed journal slot; SIGKILL cannot accumulate
    # one anonymous temporary file per attempted publication.
    temporary = path.with_name(path.name + ".pending")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
    temporary.replace(path)


class Exports:
    def __init__(self, build):
        self.build = Path(build).resolve()
        self.directory = self.build / ".neverd-deps"
        self.path = self.directory / "exports.json"
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {
            "schema": 1, "owned": {}, "graph": [], "pending": None, "readers": {}
        }
        if self.state.get("schema") != 1:
            raise ValueError("Unsupported fork export ownership schema")
        for key in self.state["owned"]:
            self.export_path(key)
        for keys in [self.state["graph"], *self.state["readers"].values()]:
            for key in keys:
                self.export_path(key)
        if self.state["pending"]:
            self.snapshot_paths(self.state["pending"])

    def export_path(self, key):
        if not re.fullmatch(r"third_party/[A-Za-z0-9_.-]+/[0-9a-f]{40,64}", key):
            raise ValueError(f"Invalid owned fork export path: {key}")
        path = self.build / key
        if path.resolve() != path or path.is_symlink():
            raise ValueError(f"Fork export is redirected: {path}")
        return path

    def snapshot_paths(self, snapshot):
        result = []
        for source in snapshot.get("sources", {}).values():
            path = Path(source["archive"]).parent
            key = path.relative_to(self.build).as_posix()
            if self.export_path(key) != path or Path(source["archive"]).name != "source.tar.gz":
                raise ValueError(f"Invalid fork export archive: {source['archive']}")
            result.append(key)
        return result

    def save(self):
        write_json(self.path, self.state)

    def snapshot(self):
        path = self.directory / "snapshot.json"
        return json.loads(path.read_text()) if path.exists() else {"sources": {}}

    def create(self, directory):
        key = Path(directory).relative_to(self.build).as_posix()
        path = self.export_path(key)
        if key in self.state["owned"]:
            self.verify(key)
            return
        if path.exists():
            return  # Legacy or caller-owned: preserve, even after graph changes.
        record = {"token": uuid.uuid4().hex, "phase": "creating"}
        self.state["owned"][key] = record
        self.save()  # Ownership precedes directory or archive creation.
        path.mkdir(parents=True)
        write_json(path / ".neverd-export.json", {"token": record["token"]})
        record["phase"] = "ready"
        self.save()

    def verify(self, key):
        path = self.export_path(key)
        record = self.state["owned"][key]
        marker = path / ".neverd-export.json"
        if marker.exists():
            if json.loads(marker.read_text()) != {"token": record["token"]}:
                raise ValueError(f"Fork export ownership changed: {path}")
        elif path.exists():
            if record["phase"] not in ("creating", "retiring"):
                raise ValueError(f"Fork export ownership marker missing: {path}")
            if any(p.name != ".neverd-export.json.pending" for p in path.iterdir()):
                raise ValueError(f"Unmarked fork export has unknown contents: {path}")

    def require_refreshable(self):
        if self.state["pending"]:
            raise RuntimeError("An unfinished CMake configuration still owns fork exports. "
                               "Run scripts/build.py to recover that snapshot before refreshing.")

    def begin_configuration(self):
        snapshot = self.snapshot()
        self.snapshot_paths(snapshot)
        pending = self.state["pending"]
        if pending and pending != snapshot:
            raise RuntimeError("Pending CMake configuration has a different fork snapshot")
        self.state["pending"] = snapshot
        self.save()

    def configured(self):
        pending = self.state["pending"]
        if pending is None or pending != self.snapshot():
            raise RuntimeError("No matching pending fork configuration")
        self.state["graph"] = self.snapshot_paths(pending)
        self.state["pending"] = None
        self.save()
        self.collect()

    def retain(self, reader, snapshot):
        if not reader or reader in self.state["readers"]:
            raise ValueError("A frozen export reader must have a new nonempty name")
        paths = self.snapshot_paths(snapshot)
        for key in paths:
            if not (self.export_path(key) / "source.tar.gz").is_file():
                raise ValueError(f"Cannot retain an unavailable export: {key}")
        self.state["readers"][reader] = paths
        self.save()

    def release(self, reader):
        if reader not in self.state["readers"]:
            raise ValueError(f"Unknown frozen export reader: {reader}")
        del self.state["readers"][reader]
        self.save()
        self.collect()

    def collect(self):
        # The Windows CRT byte lock has not been qualified for orphaned CMake
        # readers. Keep its prior retention policy until that boundary is owned.
        if os.name == "nt":
            return
        live = set(self.snapshot_paths(self.snapshot())) | set(self.state["graph"])
        for paths in self.state["readers"].values():
            live.update(paths)
        if self.state["pending"]:
            live.update(self.snapshot_paths(self.state["pending"]))
        retired = []
        for key in list(self.state["owned"]):
            if key in live:
                continue
            self.verify(key)
            self.state["owned"][key]["phase"] = "retiring"
            self.save()
            path = self.export_path(key)
            count = size = 0
            if path.exists():
                for f in path.rglob("*"):
                    if f.is_file() and not f.is_symlink():
                        count += 1
                        size += f.stat().st_size
                # Leave the ownership marker until every other child is gone,
                # so interrupted retirement can resume without guessing.
                for child in path.iterdir():
                    if child.name == ".neverd-export.json":
                        continue
                    if child.is_dir() and not child.is_symlink():
                        shutil.rmtree(child)
                    else:
                        child.unlink()
                (path / ".neverd-export.json").unlink(missing_ok=True)
                path.rmdir()
            del self.state["owned"][key]
            self.save()
            retired.append({"path": key, "files": count, "logical_bytes": size})
        if retired:
            write_json(self.directory / "last-retired-exports.json", {"exports": retired})
