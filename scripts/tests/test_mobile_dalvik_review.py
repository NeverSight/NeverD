"""Independent source binding regressions for the built-in Android emitter."""
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


def recover(*sources):
    budget = Budget(Limits(timeout=30))
    classes = [parse_smali(textwrap.dedent(source), input_id=f"owned-review-{i}.smali", budget=budget)
               for i, source in enumerate(sources)]
    return recover_java(link_classes(classes, budget), budget=budget)


BASE = """
.class public LBase;
.super Ljava/lang/Object;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method
.method public get()I
    .registers 2
    const/4 v0, 7
    return v0
.end method
"""

CHILD = """
.class public LChild;
.super LBase;
.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, LBase;-><init>()V
    return-void
.end method
"""


class DalvikProjectionReviewTests(unittest.TestCase):
    def test_class_named_java_cannot_rebind_runtime_types(self):
        with self.assertRaisesRegex(MobileError, "runtime package"):
            recover("""
                .class public Ljava;
                .super Ljava/lang/Object;
                .method public static value()I
                    .registers 1
                    const/4 v0, 7
                    return v0
                .end method
            """)

    def execute(self, report, harness):
        suffix = ".exe" if os.name == "nt" else ""
        configured = os.environ.get("JAVA_HOME")
        if configured:
            home = Path(configured)
            javac, java = home / "bin" / ("javac" + suffix), home / "bin" / ("java" + suffix)
            self.assertTrue(javac.is_file() and java.is_file(), "JAVA_HOME must provide java and javac")
            javac, java = str(javac), str(java)
        else:
            javac, java = shutil.which("javac"), shutil.which("java")
            if not javac or not java:
                self.skipTest("JDK required only to compile and execute the source oracle")
        version = subprocess.run([java, "-version"], capture_output=True, text=True, timeout=10)
        if not configured and sys.platform == "darwin" and "Unable to locate a Java Runtime" in version.stderr:
            self.skipTest("macOS Java launcher is present but no JDK is installed")
        self.assertEqual(version.returncode, 0, version.stdout + version.stderr)
        with tempfile.TemporaryDirectory(prefix="neverd-dalvik-review-") as directory:
            root = Path(directory)
            sources = []
            for unit in report["source_units"]:
                path = root / unit["path"]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(unit["source"], encoding="utf-8")
                sources.append(str(path))
            path = root / "Verify.java"
            path.write_text(textwrap.dedent(harness), encoding="utf-8")
            for command in ([javac, "-d", str(root), *sources, str(path)], [java, "-cp", str(root), "Verify"]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_java_field_cannot_shadow_generated_float_type_qualifiers(self):
        report = recover("""
            .class public Lfixture/Shadow;
            .super Ljava/lang/Object;
            .field public static java:I
            .method public static echo(F)F
                .registers 1
                return p0
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 1)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) {
                    fixture.Shadow.java = 19;
                    for (int bits : new int[]{0, 0x80000000, 0x3fc00000, 0x7fc12345}) {
                        float actual = fixture.Shadow.echo(Float.intBitsToFloat(bits));
                        if (Float.floatToRawIntBits(actual) != bits)
                            throw new AssertionError("float carrier changed under field shadow");
                    }
                    if (fixture.Shadow.java != 19) throw new AssertionError("input field changed");
                }
            }
        """)

    def test_super_receiver_replaced_by_null_is_rejected(self):
        # The register still has p0's number, but its value is no longer this.
        with self.assertRaisesRegex(MobileError, "receiver|self"):
            recover(BASE, CHILD + """
                .method public getNull()I
                    .registers 2
                    const/4 p0, 0
                    invoke-super {p0}, LBase;->get()I
                    move-result v0
                    return v0
                .end method
            """)

    def test_super_receiver_join_requires_original_self_on_every_path(self):
        with self.assertRaisesRegex(MobileError, "receiver|self"):
            recover(BASE, CHILD + """
                .method public choose(LChild;I)I
                    .registers 4
                    if-eqz p2, :keep
                    move-object p0, p1
                :keep
                    invoke-super {p0}, LBase;->get()I
                    move-result v0
                    return v0
                .end method
            """)

    def test_copied_original_self_keeps_super_binding_after_p0_changes(self):
        report = recover(BASE, CHILD + """
            .method public original()I
                .registers 2
                invoke-super {p0}, LBase;->get()I
                move-result v0
                return v0
            .end method
            .method public copied()I
                .registers 2
                move-object v0, p0
                const/4 p0, 0
                invoke-super {v0}, LBase;->get()I
                move-result v0
                return v0
            .end method
        """)
        self.assertEqual(report["recovered_method_count"], 5)
        self.execute(report, """
            public class Verify {
                public static void main(String[] args) {
                    Child value = new Child();
                    if (value.original() != 7 || value.copied() != 7)
                        throw new AssertionError("proven original receiver lost its super binding");
                }
            }
        """)


if __name__ == "__main__":
    unittest.main()
