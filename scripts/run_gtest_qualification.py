#!/usr/bin/env python3
"""Require every discovered GoogleTest identity to execute and pass.

Discovery uses the execution filter, retains parameterized identities, and
requires a minimum inventory so a missing suite cannot pass vacuously.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .run_lift_qualification import audit, sha256
else:
    from run_lift_qualification import audit, sha256


def identities(listing: str, minimum: int) -> tuple[str, ...]:
    if minimum < 1:
        raise ValueError("qualification requires a positive minimum inventory")
    names, suite = [], None
    for raw in listing.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line or line.startswith(("Running main() from ",
                                        "Note: Google Test filter = ")):
            continue
        if re.fullmatch(r"[A-Za-z0-9_/]+\.", line):
            suite = line
        elif re.fullmatch(r"  [A-Za-z0-9_/]+", line) and suite:
            names.append(suite + line.strip())
        else:
            raise ValueError(f"unrecognized GoogleTest discovery line: {raw!r}")
    if len(names) < minimum or len(set(names)) != len(names):
        raise ValueError("discovered inventory is too small or has duplicate identities")
    if any(part.startswith("DISABLED_") for name in names
           for part in re.split(r"[./]", name)):
        raise ValueError("qualification inventory contains disabled tests")
    return tuple(names)


def run(binary: Path, output: Path, test_filter: str, minimum: int,
        timeout: int) -> dict:
    binary, output = binary.resolve(), output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    receipt = output.with_suffix(".qualification.json")
    listing_path = output.with_suffix(".discovered.txt")
    if binary in (output, receipt, listing_path):
        raise ValueError("qualification output must not overwrite its binary")
    # No failed discovery or execution may leave an old successful receipt.
    for path in (output, receipt, listing_path):
        path.unlink(missing_ok=True)
    args = [str(binary), "--gtest_filter=" + test_filter]
    digest = sha256(binary)
    discovery = subprocess.run(args + ["--gtest_list_tests"], check=True,
                               text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
    listing_path.write_text(discovery.stdout, encoding="utf-8")
    names = identities(discovery.stdout, minimum)
    result = subprocess.run(args + ["--gtest_output=xml:" + str(output)],
                            timeout=timeout)
    outcome = audit(ET.parse(output).getroot(), names, result.returncode)
    if sha256(binary) != digest:
        raise ValueError("qualification binary changed during execution")
    report = {"binary_sha256": digest, "filter": test_filter,
              "minimum_tests": minimum, "outcome": outcome}
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{binary.name}: {len(names)} passed, zero failed or skipped", flush=True)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--filter", default="*")
    parser.add_argument("--min-tests", type=int, required=True)
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()
    run(args.binary, args.output, args.filter, args.min_tests, args.timeout)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, ET.ParseError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
