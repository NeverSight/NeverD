#!/usr/bin/env python3
"""Recompile calls with method-owned witnesses using both source and SDK declarations."""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_internal import Verify, choose_d8, choose_jdk, class_identity
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_internal import Verify, choose_d8, choose_jdk, class_identity


SOURCES = {
    "Action": "public interface Action<A, B, R> {}",
    "Channel": """
        public interface Channel<V> {
            class Helper {
                public static <V, R> R apply(Channel<? extends V> input, R seed,
                                             Action<? super R, V, ? extends R> action) {
                    State.calls++;
                    return seed;
                }
            }
        }
    """,
    "Mode": """
        public enum Mode {
            ACTIVE;
            public final <A, B> void record(Action<? super A, ? super Channel<? super B>, ?> fn,
                                            A value, Channel<? super B> tail) {
                State.value = value;
                State.calls++;
            }
        }
    """,
    "State": """
        public final class State {
            public static int calls;
            public static Object value;
            static <T extends Throwable> T remember(T failure, String label) {
                calls++;
                return failure;
            }
        }
    """,
    "Calls": """
        public class Calls {
            public static Exception pass(Exception failure) {
                return State.remember(failure, "input");
            }
            public static Object apply(Channel raw, Object value, Action fn) {
                return Channel.Helper.apply(raw, value, fn);
            }
            public static void record(Mode mode, Action action, Object value, Channel tail) {
                mode.record(action, value, tail);
            }
        }
    """,
}

OBSERVER = """
import fixture.*;
public class Observer {
    public static void main(String[] args) {
        Object value = new Object();
        Exception failure = new Exception();
        if (Calls.pass(failure) != failure || Calls.pass(null) != null) throw new AssertionError();
        if (Calls.apply(null, value, null) != value) throw new AssertionError();
        Calls.record(Mode.ACTIVE, null, value, null);
        if (State.value != value) throw new AssertionError();
        try {
            Calls.record(null, null, value, null);
            throw new AssertionError();
        } catch (NullPointerException expected) {
            if (State.calls != 4) throw new AssertionError();
        }
        System.out.println(State.calls);
    }
}
"""


def exercise(verify, cli, android_jar, original, observer, sdk_layout):
    root = verify.work / ("dependency" if sdk_layout else "source")
    dex = root / "dex"
    dex.mkdir(parents=True)
    classes = sorted(original.rglob("*.class"))
    inputs = [p for p in classes if not sdk_layout or p.stem == "Calls"]
    classpath = ["--classpath", android_jar]
    dependencies = root / "provider.jar"
    if sdk_layout:
        with zipfile.ZipFile(dependencies, "x") as archive:
            for path in classes:
                if path not in inputs:
                    archive.write(path, path.relative_to(original))
        classpath.extend(["--classpath", dependencies])
    verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                "--output", dex, *inputs], "encode")
    output = root / "recovered"
    verify.run([cli, "mobile", dex / "classes.dex", *classpath, "-o", output], "recover")
    report = json.loads((output / "report.json").read_text())["android_method_recovery"]
    if report["unrecovered_method_count"]:
        raise AssertionError("Scoped invocation fixture omitted original methods")
    compiled = root / "classes"
    verify.compile_local(sorted((output / "sources").rglob("*.java")), compiled,
                         classpath=dependencies if sdk_layout else None,
                         release=report.get("minimum_java_release", 8))
    before = class_identity.compiler_classes(original)
    after = class_identity.compiler_classes(compiled)
    expected = {"L" + p.relative_to(original).with_suffix("").as_posix() + ";" for p in inputs}
    if set(after) != expected or any(set(before[c]["methods"]) - set(after[c]["methods"]) for c in after):
        raise AssertionError("Recompiled class or method identity changed")
    runtime = [compiled, observer]
    if sdk_layout:
        runtime.append(dependencies)
    return verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), "Observer"], "rebuilt")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-scoped-invocation-")
    with context as temp:
        work = Path(temp).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 900)
        files = []
        for name, text in SOURCES.items():
            path = work / (name + ".java")
            path.write_text("package fixture;\n" + text, encoding="utf-8")
            files.append(path)
        original = work / "original"
        verify.compile_local(files, original)
        harness = work / "Observer.java"
        harness.write_text(OBSERVER, encoding="utf-8")
        observer = work / "observer"
        verify.compile_local([harness], observer, classpath=original)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, [original, observer])),
                               "Observer"], "original")
        for sdk_layout in (False, True):
            actual = exercise(verify, args.neverd.resolve(), args.android_jar.resolve(),
                              original, observer, sdk_layout)
            if actual != baseline:
                raise AssertionError("Scoped dispatch, throwable identity or effects changed")
    print("Scoped invocation source and SDK layouts passed")


if __name__ == "__main__":
    main()
