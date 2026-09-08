"""Dalvik source regressions for initialization, register overlap and linking."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))

from mobile.common import Limits, MobileError
from mobile.dalvik_java import recover_java
from mobile.dalvik_model import Budget, link_classes
from mobile.dalvik_smali import parse_smali


def read_classes(*sources):
    budget = Budget(Limits(timeout=30))
    classes = [parse_smali(textwrap.dedent(source), input_id=f"owned-{index}.smali", budget=budget)
               for index, source in enumerate(sources)]
    return link_classes(classes, budget), budget


def recover(*sources):
    classes, budget = read_classes(*sources)
    return recover_java(classes, budget=budget)


class DalvikSemanticsTests(unittest.TestCase):
    def java_tools(self):
        suffix = ".exe" if os.name == "nt" else ""
        configured = os.environ.get("JAVA_HOME")
        if configured:
            home = Path(configured)
            javac, java = home / "bin" / ("javac" + suffix), home / "bin" / ("java" + suffix)
            self.assertTrue(javac.is_file() and java.is_file(), "JAVA_HOME must name a JDK with java and javac")
            javac, java = str(javac), str(java)
        else:
            javac, java = shutil.which("javac"), shutil.which("java")
            if not javac or not java:
                self.skipTest("JDK required only to compile and execute the source oracle")
        version = subprocess.run([java, "-version"], text=True, capture_output=True, timeout=10)
        if not configured and sys.platform == "darwin" and "Unable to locate a Java Runtime" in version.stderr:
            self.skipTest("macOS Java launcher is present but no JDK is installed")
        self.assertEqual(version.returncode, 0, version.stdout + version.stderr)
        return javac, java

    def execute(self, report, harness, *, file_input=False):
        javac, java = self.java_tools()
        with tempfile.TemporaryDirectory(prefix="neverd-dalvik-semantics-") as directory:
            root = Path(directory)
            sources = []
            for unit in report["source_units"]:
                target = root / unit["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(unit["source"], encoding="utf-8")
                sources.append(str(target))
            verify = root / "Verify.java"
            verify.write_text(textwrap.dedent(harness), encoding="utf-8")
            sources.append(str(verify))
            args = []
            if file_input:
                source = root / "input.bin"
                source.write_bytes(b"\x5a")
                args = [str(source), str(root / "does-not-exist.bin")]
            for command in ([javac, "-d", str(root), *sources], [java, "-cp", str(root), "Verify", *args]):
                result = subprocess.run(command, text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, " ".join(command) + "\n" + result.stdout + result.stderr)

    def test_sget_of_encoded_final_constant_preserves_class_initialization(self):
        report = recover("""
            .class public LCounter;
            .super Ljava/lang/Object;
            .field public static flag:I
        """, """
            .class public LTarget;
            .super Ljava/lang/Object;
            .field public static final magic:I = 7
            .method static constructor <clinit>()V
                .registers 1
                sget v0, LCounter;->flag:I
                add-int/lit8 v0, v0, 1
                sput v0, LCounter;->flag:I
                return-void
            .end method
        """, """
            .class public LCaller;
            .super Ljava/lang/Object;
            .method public static run()I
                .registers 1
                sget v0, LTarget;->magic:I
                return v0
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 2)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) {
                    if (Counter.flag != 0) throw new AssertionError("premature target initialization");
                    if (Caller.run() != 7 || Counter.flag != 1)
                        throw new AssertionError("sget must initialize Target exactly once: " + Counter.flag);
                    if (Caller.run() != 7 || Counter.flag != 1)
                        throw new AssertionError("sget repeated Target initialization");
                }
            }
        """)

    def test_overlapping_wide_write_retires_the_old_high_word(self):
        report = recover("""
            .class public LOverlap;
            .super Ljava/lang/Object;
            .method public static run()J
                .registers 4
                const-wide/16 v2, 9
                const-wide/16 v1, 7
                const/4 v3, 0
                return-wide v1
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 1)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) {
                    if (Overlap.run() != 7L)
                        throw new AssertionError("writing retired v3 must preserve the current v1/v2 pair");
                }
            }
        """)

    def test_local_callee_identity_includes_its_return_type(self):
        classes, budget = read_classes("""
            .class public LIdentity;
            .super Ljava/lang/Object;
            .method public static target()I
                .registers 1
                const/4 v0, 7
                return v0
            .end method
            .method public static run()J
                .registers 2
                invoke-static {}, LIdentity;->target()J
                move-result-wide v0
                return-wide v0
            .end method
        """)
        with self.assertRaises(MobileError):
            recover_java(classes, budget=budget)

    def test_virtual_invocation_cannot_resolve_to_a_local_static_method(self):
        classes, budget = read_classes("""
            .class public LDispatch;
            .super Ljava/lang/Object;
            .method public static target(I)I
                .registers 1
                return p0
            .end method
            .method public static run(LDispatch;I)I
                .registers 3
                invoke-virtual {p0, p1}, LDispatch;->target(I)I
                move-result v0
                return v0
            .end method
        """)
        with self.assertRaises(MobileError):
            recover_java(classes, budget=budget)

    def test_matching_local_static_callee_keeps_its_value_flow(self):
        report = recover("""
            .class public LMatching;
            .super Ljava/lang/Object;
            .method public static target(I)I
                .registers 1
                return p0
            .end method
            .method public static run(I)I
                .registers 2
                invoke-static {p0}, LMatching;->target(I)I
                move-result v0
                return v0
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 2)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) {
                    for (int value : new int[]{Integer.MIN_VALUE, -1, 0, 7, Integer.MAX_VALUE})
                        if (Matching.run(value) != value) throw new AssertionError("matching callee value");
                }
            }
        """)

    def test_super_constructor_checked_exception_compiles_and_propagates(self):
        report = recover("""
            .class public LRecoveredStream;
            .super Ljava/io/FileInputStream;
            .method public constructor <init>(Ljava/lang/String;)V
                .registers 2
                invoke-direct {p0, p1}, Ljava/io/FileInputStream;-><init>(Ljava/lang/String;)V
                return-void
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 1)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) throws Throwable {
                    try (RecoveredStream stream = new RecoveredStream(args[0])) {
                        if (stream.read() != 0x5a || stream.read() != -1)
                            throw new AssertionError("super constructor did not open the input");
                    }
                    try {
                        new RecoveredStream(args[1]).close();
                        throw new AssertionError("super constructor swallowed FileNotFoundException");
                    } catch (java.io.FileNotFoundException expected) {
                    }
                }
            }
        """, file_input=True)


if __name__ == "__main__":
    unittest.main()
