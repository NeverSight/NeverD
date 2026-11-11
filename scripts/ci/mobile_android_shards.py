#!/usr/bin/env python3
"""Enumerate and execute the owned Android profiles in bounded CI shards.

The shell catalog retains its readable loops and argument arrays. During
expansion `python` is a shell function that only writes NUL-delimited argv;
fixture runners are invoked later, individually, with the selected JDK.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "scripts/ci/mobile_android_profiles.sh"
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.mobile_android_artifacts import ArtifactStore, read_json
EXPAND = r'''
set -eo pipefail
profile_group() {
  group="$1"; jdk="$2"; deadline="$3"
  case "$jdk" in
    21) JAVA_HOME="$NEVERD_JAVA21_HOME" ;;
    25) JAVA_HOME="$NEVERD_JAVA25_HOME" ;;
    *) JAVA_HOME=invalid ;;
  esac
}
python() {
  printf '%s\0' "$group" "$jdk" "$deadline" "$@"
  printf '\0'
}
source "$1"
'''


def profiles(env: dict[str, str], catalog: Path = CATALOG) -> list[dict]:
    for key in ("NEVERD_JAVA21_HOME", "NEVERD_JAVA25_HOME", "RUNNER_TEMP",
                "NEVERD_KOTLIN_JAVA_HOME", "NEVERD_D8", "ANDROID_HOME"):
        if not env.get(key):
            raise ValueError(f"missing catalog environment: {key}")
    output = subprocess.run(["bash", "-c", EXPAND, "catalog", str(catalog)],
                            env=env, cwd=ROOT, check=True, capture_output=True).stdout
    if not output.endswith(b"\0\0"):
        raise ValueError("catalog emitted no complete profiles")
    result = []
    work_dirs = set()
    for entry in output[:-2].split(b"\0\0"):
        group, jdk, deadline, *argv = entry.decode().split("\0")
        if jdk not in ("21", "25") or not 1 <= int(deadline) <= 1200:
            raise ValueError("invalid JDK or profile deadline")
        if not argv or not argv[0].startswith("scripts/test_mobile_android_"):
            raise ValueError("catalog entry is not an Android fixture runner")
        if not (ROOT / argv[0]).is_file():
            raise ValueError(f"missing fixture runner: {argv[0]}")
        if argv.count("--work-dir") != 1:
            raise ValueError("each profile must have one explicit --work-dir")
        work_dir = Path(argv[argv.index("--work-dir") + 1])
        if (work_dir.parent != Path(env["RUNNER_TEMP"]) or work_dir in work_dirs
                or not work_dir.name.startswith("android-")):
            raise ValueError(f"duplicate or non-isolated profile directory: {work_dir}")
        work_dirs.add(work_dir)
        result.append({"id": work_dir.name, "group": group, "jdk": int(jdk),
                       "timeout_seconds": int(deadline), "argv": argv})
    return result


def shard_profiles(all_profiles: list[dict], shard: int, count: int) -> list[dict]:
    if not 1 <= count <= 64 or not 0 <= shard < count:
        raise ValueError("invalid shard index or count")
    if count > len(all_profiles):
        raise ValueError("shards must not be empty")
    # Interleave adjacent source/SDK variants and expensive compiler families.
    return all_profiles[shard::count]


def run_profile(profile: dict, env: dict[str, str], log: Path) -> dict:
    current_env = dict(env)
    java_home = env[f"NEVERD_JAVA{profile['jdk']}_HOME"]
    current_env["JAVA_HOME"] = java_home
    current_env["PATH"] = str(Path(java_home) / "bin") + os.pathsep + env["PATH"]
    if "--work-dir" in profile["argv"]:
        # The catalog owns these fixture outputs. Installed SDKs and the build
        # remain external inputs; only this store's per-profile work is retired.
        store = ArtifactStore(Path(env["RUNNER_TEMP"]) / "android-profile-artifacts")
        job = store.begin(profile["id"])
        argv = [sys.executable, *profile["argv"]]
        argv[argv.index("--work-dir") + 1] = str(job.work / "output")
        result = job.run(argv, cwd=ROOT, env=current_env, timeout=profile["timeout_seconds"])
        evidence = read_json(Path(result["evidence"]))
        log.write_text(evidence["diagnostic_tail"])
        return {**profile, **result, "log": str(log)}
    start = time.monotonic()
    timed_out = False
    with log.open("xb") as output:
        process = subprocess.Popen([sys.executable, *profile["argv"]], cwd=ROOT,
                                   env=current_env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=profile["timeout_seconds"])
        except subprocess.TimeoutExpired:
            timed_out = True
            grace_deadline = time.monotonic() + 5
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            # The leader can exit while a compiler or JVM child ignores TERM.
            # Its exit does not prove the process group has finished cleanup.
            remaining = grace_deadline - time.monotonic()
            if remaining > 0:
                try:
                    os.killpg(process.pid, 0)
                except ProcessLookupError:
                    pass
                else:
                    time.sleep(remaining)
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            code = 124
    return {**profile, "returncode": code, "timed_out": timed_out,
            "elapsed_seconds": round(time.monotonic() - start, 3), "log": str(log)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shard", type=int, required=True)
    parser.add_argument("--shard-count", type=int, default=32)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--list-only", action="store_true")
    args = parser.parse_args()
    all_profiles = profiles(dict(os.environ))
    selected = shard_profiles(all_profiles, args.shard, args.shard_count)
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": 1, "catalog_sha256": hashlib.sha256(CATALOG.read_bytes()).hexdigest(),
                "total_profiles": len(all_profiles), "shard": args.shard,
                "shard_count": args.shard_count, "profiles": selected}
    (args.output / "plan.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.list_only:
        return 0
    failed = 0
    with (args.output / "results.jsonl").open("x") as results:
        for index, profile in enumerate(selected):
            print(f"[{index + 1}/{len(selected)}] {profile['id']} (JDK {profile['jdk']})", flush=True)
            result = run_profile(profile, dict(os.environ), args.output / f"{profile['id']}.log")
            results.write(json.dumps(result) + "\n")
            results.flush()
            if result["returncode"]:
                failed += 1
                print(f"::error::{profile['id']} failed with {result['returncode']}", flush=True)
                print(Path(result["log"]).read_text(errors="replace")[-16000:], flush=True)
            if result["returncode"] in (130, 143):
                summary = {"planned": len(selected), "completed": index + 1,
                           "failed": failed, "interrupted": True}
                (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
                return result["returncode"]
    summary = {"planned": len(selected), "completed": len(selected), "failed": failed}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
