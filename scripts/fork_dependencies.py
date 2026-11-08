"""Resolve NeverSight default branches before a build; never mutate compiling sources."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from fork_exports import Exports, write_json


ROOT = Path(__file__).resolve().parents[1]
REPOSITORIES = ROOT / "third_party/forks.json"


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


@contextmanager
def build_lock(build):
    """OS-owned lock: a killed process cannot leave a stale lock owner."""
    path = Path(build) / ".neverd-deps/build.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if os.name == "nt":
            import msvcrt
            stream.seek(0, os.SEEK_END)
            if not stream.tell():
                stream.write(b"\0")
                stream.flush()
            while True:
                stream.seek(0)
                try:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield stream.fileno()
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def resolve_head(repository):
    result = subprocess.run(
        ["git", "-c", "http.version=HTTP/1.1", "ls-remote", "--symref", repository, "HEAD"],
        check=True, capture_output=True, text=True, timeout=60,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    branch = re.search(r"^ref: refs/heads/(.+)\tHEAD$", result.stdout, re.M)
    commit = re.search(r"^([0-9a-f]{40,64})\tHEAD$", result.stdout, re.M)
    if not branch or not commit:
        raise RuntimeError(f"Cannot resolve the default branch of {repository}")
    return {"repository": repository, "branch": branch[1], "commit": commit[1]}


def library_name(name):
    name = name.removeprefix("neverd_")
    return "oneTBB" if name == "tbb" else name


def git(*args, cwd=None):
    return subprocess.run(["git", "-c", "http.version=HTTP/1.1", *args], cwd=cwd or ROOT,
                          check=True, capture_output=True, text=True, timeout=180,
                          env={**os.environ, "GIT_TERMINAL_PROMPT": "0"}).stdout.strip()


def update_submodules(heads):
    """Caller holds the shared source lock; preserve local work in each fork."""
    sources = {}
    for name, head in heads.items():
        relative = "third_party/" + library_name(name)
        configured = git("config", "-f", ".gitmodules", "--get",
                         f"submodule.{relative}.url")
        if configured != head["repository"]:
            raise RuntimeError(f"Fork URL differs between .gitmodules and forks.json: {name}")
        source = ROOT / relative
        if not (source / ".git").exists():
            git("submodule", "update", "--init", "--", relative)
        if git("status", "--porcelain", cwd=source):
            raise RuntimeError(f"Preserving local submodule changes in {relative}; "
                               "commit and push them to the fork before a mainline build.")
        if git("remote", "get-url", "origin", cwd=source) != head["repository"]:
            raise RuntimeError(f"Submodule origin is not the configured NeverSight fork: {relative}")
        sources[name] = source
    for name, source in sources.items():
        head = heads[name]
        # Fetch the resolved commit, so a branch moving during the fetch cannot
        # change this build's inputs. Never reset or clean a caller's checkout.
        git("fetch", "--no-tags", "origin", head["commit"], cwd=source)
        ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", head["commit"]],
                                  cwd=source, check=False, capture_output=True)
        if ancestor.returncode and git("rev-parse", "--is-shallow-repository", cwd=source) == "true":
            # A runner may have separate shallow roots for its gitlink and the
            # live tip. Missing ancestry is not evidence of unpublished work.
            git("fetch", "--unshallow", "--no-tags", "origin", head["commit"], cwd=source)
            ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", head["commit"]],
                                      cwd=source, check=False, capture_output=True)
        if ancestor.returncode:
            raise RuntimeError(f"Preserving divergent submodule commits in {source}; "
                               "reconcile them with the fork default branch first.")
        git("checkout", "--no-overwrite-ignore", "--detach", head["commit"], cwd=source)
    return sources


def create_archive(source, commit, destination):
    with destination.open("wb") as stream:
        subprocess.run(["git", "-C", str(source), "archive", "--format=tar.gz",
                        "--prefix=source/", commit], stdout=stream, check=True, timeout=60)


def refresh(build, repositories=None):
    """Caller holds build_lock. Publish the snapshot only after every fetch succeeds."""
    build = Path(build).resolve()
    repositories = repositories or json.loads(REPOSITORIES.read_text())
    exports = Exports(build)
    exports.require_refreshable()
    exports.collect()
    previous_path = build / ".neverd-deps/snapshot.json"
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}
    try:
        # Separate build directories share the checkout but compile immutable
        # exports. Updating one submodule cannot modify another build's sources.
        with build_lock(ROOT / "build-fork-cache"):
            with ThreadPoolExecutor(max_workers=5) as executor:
                heads = dict(zip(repositories, executor.map(resolve_head, repositories.values())))
            working_sources = update_submodules(heads)
            for name, source in heads.items():
                archive = build / "third_party" / library_name(name) / source["commit"] / "source.tar.gz"
                exports.create(archive.parent)
                old = previous.get("sources", {}).get(name, {})
                if archive.exists():
                    checksum = digest(archive)
                    if old.get("commit") == source["commit"] and checksum != old.get("sha256"):
                        raise RuntimeError(f"Cached source archive changed: {archive}")
                else:
                    temporary = archive.with_suffix(".download")
                    try:
                        create_archive(working_sources[name], source["commit"], temporary)
                        checksum = digest(temporary)
                        os.replace(temporary, archive)
                    finally:
                        temporary.unlink(missing_ok=True)
                source.update(archive=archive.as_posix(), sha256=checksum)
                print(f"{name}: {source['branch']} @ {source['commit']}", flush=True)
        snapshot = {"schema": 1, "resolved_at": timestamp(), "sources": heads}
        write_json(previous_path, snapshot)
        return previous_path
    finally:
        exports.collect()


def check(build, expected=None):
    path = Path(build) / ".neverd-deps/snapshot.json"
    snapshot = json.loads(path.read_text())
    actual = digest(path)
    if expected is not None and expected != actual:
        raise RuntimeError("Fork snapshot differs from the configured build graph. "
                           f"Run python scripts/build.py {Path(build)} to reconfigure and build.")
    # The wrapper freezes one snapshot throughout configure, auto-regeneration
    # and compilation. A later remote update belongs to the next invocation.
    if os.environ.get("NEVERD_DEPENDENCY_SNAPSHOT") == actual:
        return
    sources = snapshot["sources"]
    with ThreadPoolExecutor(max_workers=5) as executor:
        current = list(executor.map(resolve_head, (s["repository"] for s in sources.values())))
    changed = [name for (name, old), new in zip(sources.items(), current)
               if (old["branch"], old["commit"]) != (new["branch"], new["commit"])]
    if changed:
        raise RuntimeError("Fork branches advanced: " + ", ".join(changed) +
                           f". Run python scripts/build.py {Path(build)} to refresh and build.")


def check_install(build, configuration):
    """Input receipts never certify unbuilt targets; reject known stale installs."""
    build = Path(build)
    receipt = json.loads((build / "neverd-build-receipt.json").read_text())
    inputs = build / "neverd-dependency-inputs.json"
    if (receipt.get("status") != "success" or
            (receipt.get("configuration") or "Release") != configuration or
            receipt.get("inputs_sha256") != digest(inputs) or
            receipt.get("cache_sha256") != digest(build / "CMakeCache.txt") or
            receipt.get("snapshot_sha256") != digest(build / ".neverd-deps/snapshot.json")):
        raise RuntimeError("Installation requires a successful build for the current "
                           "inputs and configuration. Run python scripts/build.py first.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("refresh", "check", "check-install", "begin-configure", "retain", "release"))
    parser.add_argument("build", type=Path)
    parser.add_argument("--expected")
    parser.add_argument("--config", default="")
    parser.add_argument("--reader")
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--configure", action="store_true")
    args = parser.parse_args()
    if args.mode == "refresh":
        with build_lock(args.build):
            refresh(args.build)
            if args.configure:
                Exports(args.build).begin_configuration()
    elif args.mode == "begin-configure":
        # The supported wrapper already holds the inherited build lock.
        snapshot = args.build / ".neverd-deps/snapshot.json"
        if os.environ.get("NEVERD_DEPENDENCY_SNAPSHOT") == digest(snapshot):
            Exports(args.build).begin_configuration()
        else:
            with build_lock(args.build):
                Exports(args.build).begin_configuration()
    elif args.mode in ("retain", "release"):
        with build_lock(args.build):
            exports = Exports(args.build)
            if args.mode == "retain":
                source = json.loads(args.snapshot.read_text()) if args.snapshot else exports.snapshot()
                exports.retain(args.reader, source)
            else:
                exports.release(args.reader)
    elif args.mode == "check":
        check(args.build, args.expected)
    else:
        check_install(args.build, args.config)


if __name__ == "__main__":
    main()
