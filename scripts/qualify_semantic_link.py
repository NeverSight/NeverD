"""Compare the exact shared-LLVM flag fixture linkage on native hosts."""
from pathlib import Path
import datetime
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

source = Path(sys.argv[1]).resolve()
evidence = Path(sys.argv[2]).resolve()
evidence.mkdir(exist_ok=False)
build = source / "build-semantic-link"
candidate, baseline = os.environ["SOURCE_SHA"], os.environ["BASELINE_SHA"]
assert all(re.fullmatch(r"[0-9a-f]{40}", x) for x in (candidate, baseline))
runner = os.environ["RUNNER_OS"]
report = dict(candidate=candidate, baseline=baseline, runner=runner,
              platform=platform.platform(), workflow_source=os.environ["GITHUB_SHA"],
              started=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              commands=[], roles={})

def save():
    (evidence / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(name, command, timeout=3600):
    path = evidence / (name + ".log")
    expired = None
    with path.open("wb") as log:
        try:
            result = subprocess.run(list(map(str, command)), cwd=source,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            code = result.returncode
        except subprocess.TimeoutExpired as error:
            expired, code = error, 124
    data = path.read_bytes()
    archive = path.with_suffix(".log.gz")
    archive.write_bytes(gzip.compress(data, mtime=0))
    assert gzip.decompress(archive.read_bytes()) == data
    path.unlink()
    report["commands"].append(dict(name=name, command=list(map(str, command)),
                                   exit=code, timeout=timeout, expired=expired is not None,
                                   log=archive.name,
                                   decompressed_sha256=hashlib.sha256(data).hexdigest()))
    save()
    print(name, code, flush=True)
    if expired:
        raise expired
    return code

def configure(role, revision):
    assert not subprocess.check_output(["git", "status", "--porcelain"], cwd=source)
    subprocess.run(["git", "checkout", "--detach", revision], cwd=source, check=True)
    assert subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip() == revision
    subprocess.run(["git", "submodule", "update", "--init", "--depth", "1",
                    "third_party/capstone", "third_party/unicorn", "signatures"],
                   cwd=source, check=True)
    command = ["cmake", "-S", source, "-B", build, "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
               "-DCMAKE_C_COMPILER=" + os.environ["PROBE_CC"],
               "-DCMAKE_CXX_COMPILER=" + os.environ["PROBE_CXX"],
               "-DNEVERD_LLVM_PREBUILT=ON", "-DNEVERD_BUILD_SHARED=ON",
               "-DNEVERD_ENABLE_SEMANTIC_TESTS=ON", "-DNEVERD_ENABLE_CPU_EMULATION=OFF",
               "-DNEVERD_ENABLE_WEB_ANALYSIS=OFF"]
    if runner == "Linux":
        command.append("-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14")
    assert run(role + "-configure", command, 900) == 0
    code = run(role + "-build", [sys.executable, "scripts/build.py", build, "--config",
               "Release", "--target", "NeverDSelfCompareFlagTests", "--parallel", "3"], 7200)
    row = report["roles"][role] = dict(source=revision, build_exit=code, binaries={})
    for name in ["neverd-build-receipt.json", "neverd-dependency-inputs.json", "CMakeCache.txt"]:
        if (build / name).exists():
            shutil.copy2(build / name, evidence / (role + "-" + name))
    for directory in ("bin", "lib"):
        root = build / directory
        if root.exists():
            for path in root.iterdir():
                if path.is_file() and (path.name in ("neverd", "neverd.exe", "NeverDSelfCompareFlagTests",
                                                     "NeverDSelfCompareFlagTests.exe", "neverd.dll") or
                                      path.name.startswith("libneverd.")):
                    row["binaries"][path.name] = digest(path)
    save()
    return code

exe = build / "bin" / ("NeverDSelfCompareFlagTests.exe" if runner == "Windows"
                       else "NeverDSelfCompareFlagTests")
filter_arg = "--gtest_filter=SelfCompareFlags/X86SelfCompareFlagsRT.Verify/cmp_eq_32_0"
if runner in ("macOS", "Windows"):
    if configure("baseline", baseline) == 0:
        report["roles"]["baseline"]["probe_exit"] = run("baseline-probe", [exe, filter_arg], 120)
        if runner == "macOS":
            run("baseline-backtrace", ["lldb", "--batch", "-o", "run", "-o", "bt",
                                      "-o", "thread backtrace all", "--", exe, filter_arg], 120)
    save()

assert configure("candidate", candidate) == 0
inventory_command = ["ctest", "--test-dir", str(build), "-C", "Release", "-L",
                     "^NeverDSelfCompareFlagTests$", "--show-only=json-v1"]
inventory = json.loads(subprocess.check_output(inventory_command, cwd=source, text=True))
(evidence / "candidate-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
expected = {t["name"] for t in inventory["tests"]}
assert expected, "empty owning test selection"
xml = evidence / "candidate-results.xml"
code = run("candidate-tests", ["ctest", "--test-dir", build, "-C", "Release", "-L",
           "^NeverDSelfCompareFlagTests$", "--no-tests=error", "--output-on-failure",
           "--parallel", "3", "--output-junit", xml], 1800)
tests = list(ET.parse(xml).getroot().iter("testcase"))
actual = {t.attrib["name"] for t in tests}
bad = [t.attrib["name"] for t in tests if t.find("failure") is not None or
       t.find("error") is not None or t.find("skipped") is not None or
       t.attrib.get("status") in ("notrun", "disabled", "fail")]
report["candidate_tests"] = dict(exit=code, expected=len(expected), actual=len(actual),
                                bad=bad, missing=sorted(expected-actual),
                                unexpected=sorted(actual-expected))
report["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
save()
assert code == 0 and actual == expected and not bad, report["candidate_tests"]
