"""Build NeverD with the latest default branches of its NeverSight dependencies.

Configure once with CMake, then use: python scripts/build.py BUILD [cmake build options].
The directory is locked across refresh, configure and build. A build uses one
recorded source snapshot even if a fork advances while compilation is running.
Build compilation targets here; run tests and cmake --install after success.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import fork_dependencies as forks


def cache_values(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def configuration(cache, options):
    if not cache.get("CMAKE_CONFIGURATION_TYPES"):
        return cache.get("CMAKE_BUILD_TYPE", "")
    if "--config" in options:
        return options[options.index("--config") + 1]
    for value in options:
        if value.startswith("--config="):
            return value.split("=", 1)[1]
    raise ValueError("Multi-config builds require an explicit --config Release or --config Debug.")


def build(directory, options):
    directory = Path(directory).resolve()
    with forks.build_lock(directory):
        receipt_path = directory / "neverd-build-receipt.json"
        receipt = {"status": "in_progress", "started_at": forks.timestamp(), "options": options,
                   "scope": "Only the requested build targets and configuration; not all features or tests."}
        forks.write_json(receipt_path, receipt)
        try:
            if any(value in ("install", "install/strip", "--target=install", "--target=install/strip")
                   for value in options):
                raise ValueError("Run scripts/build.py for compilation first, then cmake --install "
                                 "after a successful build; install is a separate step.")
            cache = cache_values(directory)
            repositories = json.loads(forks.REPOSITORIES.read_text())
            for name in repositories:
                if cache.get("FETCHCONTENT_SOURCE_DIR_" + name.upper()):
                    raise RuntimeError(f"Clear FETCHCONTENT_SOURCE_DIR_{name.upper()} before "
                                       "a mainline build; local source overrides cannot follow the fork.")
            configuration(cache, options)
            snapshot = forks.refresh(directory)
            receipt["snapshot"] = json.loads(snapshot.read_text())
            receipt["snapshot_sha256"] = forks.digest(snapshot)
            env = {**os.environ, "NEVERD_DEPENDENCY_SNAPSHOT": forks.digest(snapshot)}
            cmake = cache.get("CMAKE_COMMAND", "cmake")
            subprocess.run([cmake, "-S", cache["CMAKE_HOME_DIRECTORY"], "-B", str(directory)],
                           env=env, check=True)
            receipt["configuration"] = configuration(cache_values(directory), options)
            subprocess.run([cmake, "--build", str(directory), *options], env=env, check=True)
            effective = directory / "neverd-dependency-inputs.json"
            if effective.exists():
                receipt["configure_inputs"] = json.loads(effective.read_text())
                receipt["inputs_sha256"] = forks.digest(effective)
            receipt["cache_sha256"] = forks.digest(directory / "CMakeCache.txt")
            windows_types = directory / "neverd-windows-types-inputs.json"
            if windows_types.exists():
                receipt["windows_types_sha256"] = forks.digest(windows_types)
            receipt["status"] = "success"
        except BaseException:
            receipt["status"] = "failed"
            raise
        finally:
            receipt["finished_at"] = forks.timestamp()
            forks.write_json(receipt_path, receipt)


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 2)
    build(sys.argv[1], sys.argv[2:])
