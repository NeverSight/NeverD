#!/usr/bin/env python3
"""Recompile array and producer transfers into a contravariantly bounded destination."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts.test_mobile_android_empty_original_bridge_throws import exception_inventory
from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity


def verify_layout(args, work):
    verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
    originals = work / "originals"
    observers = work / "observers"
    verify.compile_local(sorted((FIXTURES / "destination_bounds/java").rglob("*.java")), originals)
    verify.compile_local([FIXTURES / "harness/DestinationBoundsHarness.java"], observers, classpath=originals)

    def run_observer(paths):
        return verify.run([verify.java, "-cp", os.pathsep.join(map(str, [*paths, observers])),
                           "DestinationBoundsHarness"], "observe")

    baseline = run_observer([originals])
    entries = sorted(originals.rglob("*.class"))
    dependency = work / "provider.jar"
    flags = ["--classpath", args.android_jar.resolve()]
    if args.dependency_provider:
        provider = originals / "fixture/Transfers.class"
        entries.remove(provider)
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.write(provider, "fixture/Transfers.class")
        flags.extend(["--classpath", dependency])
    dex = work / "dex"
    dex.mkdir()
    verify.run([*verify.d8, "--debug", "--min-api", "24", *flags, "--output", dex, *entries], "encode")
    output = work / "recovered"
    verify.run([args.neverd.resolve(), "mobile", dex / "classes.dex", *flags, "-o", output], "recover")
    recovery = json.loads((output / "report.json").read_text())["android_method_recovery"]
    rebuilt = work / "rebuilt"
    verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                         classpath=dependency if args.dependency_provider else None,
                         release=recovery.get("minimum_java_release", 8))

    before = class_identity.compiler_classes(originals)
    if args.dependency_provider:
        del before["Lfixture/Transfers;"]
    after = class_identity.compiler_classes(rebuilt)
    old_throws, new_throws = exception_inventory(originals), exception_inventory(rebuilt)
    helpers = {h["class"] + "->" + h["name"] + h["prototype"] for h in recovery.get("generated_source_helpers", [])}
    original_methods = {m for c in before.values() for m in c["methods"]}
    rebuilt_methods = {m for c in after.values() for m in c["methods"]}
    if (recovery["unrecovered_method_count"] or recovery.get("projected_method_count", 0)
            or set(before) != set(after) or rebuilt_methods != original_methods | helpers
            or {row["identity"] for row in recovery["methods"]} != original_methods):
        raise RuntimeError("Destination transfer lost an original definition")
    for owner, cls in before.items():
        for method, metadata in cls["methods"].items():
            if after[owner]["methods"][method] != metadata or old_throws[method] != new_throws[method]:
                raise RuntimeError("Changed method contract: " + method)
    if run_observer([rebuilt, dependency] if args.dependency_provider else [rebuilt]) != baseline:
        raise RuntimeError("Changed destination contents, runtime result type or exception identity")
    print("All destination transfer sources compiled and matched the original program.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("neverd", "android-jar"):
        parser.add_argument("--" + flag, type=Path, required=True)
    for flag in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + flag, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True)
        verify_layout(args, args.work_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="neverd-destination-") as work:
            verify_layout(args, Path(work))


if __name__ == "__main__":
    main()
