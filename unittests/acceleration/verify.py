"""Run the complete acceleration inventory and reject failures or skips."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.run_lift_qualification import audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--minimum", type=int, default=19)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--tbb-runtimes", type=int, choices=(0, 1))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    listed = subprocess.check_output([str(binary), "--gtest_list_tests"], text=True)
    (args.output / "inventory.txt").write_text(listed, encoding="utf-8")
    names, suite = [], ""
    for line in listed.splitlines():
        value = line.split(" #", 1)[0].strip()
        if line and not line.startswith(" ") and value.endswith("."):
            suite = value
        elif line.startswith("  ") and suite:
            names.append(suite + value.split()[0])
    if args.minimum < 1 or len(names) < args.minimum or len(set(names)) != len(names):
        raise RuntimeError(f"incomplete or duplicate inventory: {names}")
    xml = (args.output / "results.xml").resolve()
    xml.unlink(missing_ok=True)
    env = os.environ.copy()
    if args.tbb_runtimes is not None:
        env["TBB_VERSION"] = "1"
    with (args.output / "run.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(
            [str(binary), "--gtest_output=xml:" + str(xml)],
            stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout, env=env,
        )
    try:
        report = audit(ET.parse(xml).getroot(), names, result.returncode)
    except (OSError, ET.ParseError, ValueError) as error:
        report = {"error": str(error), "exit_code": result.returncode}
        (args.output / "audit.json").write_text(json.dumps(report, indent=2) + "\n")
        raise RuntimeError(f"verification failed; see {args.output / 'run.log'}") from error
    if args.tbb_runtimes is not None:
        log = (args.output / "run.log").read_text(encoding="utf-8")
        count = len(re.findall(r"^oneTBB: VERSION\s", log, re.MULTILINE))
        report["tbb_runtimes"] = count
        report["expected_tbb_runtimes"] = args.tbb_runtimes
        report["passed"] = report["passed"] and count == args.tbb_runtimes
    (args.output / "audit.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise RuntimeError(json.dumps(report))
    print(f"{len(names)} cases passed, zero failed, zero skipped: {binary}")


if __name__ == "__main__":
    main()
