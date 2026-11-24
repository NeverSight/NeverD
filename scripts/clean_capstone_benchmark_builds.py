#!/usr/bin/env python3
"""Clean compiled targets in explicitly selected Capstone or NeverD benchmark builds.

Pass out-of-source CMake build directories after recording benchmark results.
The native clean target knows which files it generated. The build directory
can be rebuilt with its original configuration after cleaning.
"""

import argparse
from pathlib import Path
import subprocess
import sys


def clean(build: Path) -> None:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith(("#", "//")):
            cache[key.partition(":")[0]] = value

    if cache.get("CMAKE_PROJECT_NAME") not in {"capstone", "NeverD"}:
        raise ValueError(f"{build} is not a Capstone or NeverD build")
    source = cache.get("CMAKE_HOME_DIRECTORY")
    if not source or Path(source).resolve() == build.resolve():
        raise ValueError(f"{build} must be an out-of-source build")

    configurations = cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
    for config in configurations:
        command = ["cmake", "--build", str(build), "--target", "clean"]
        if config:
            command.extend(["--config", config])
        subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, nargs="+", help="completed benchmark build directory")
    args = parser.parse_args()
    try:
        for build in args.build:
            clean(build)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
