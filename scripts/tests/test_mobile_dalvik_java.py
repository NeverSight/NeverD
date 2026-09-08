"""Executable Java and negative dataflow checks for the internal Android engine."""
from __future__ import annotations

from pathlib import Path
import shutil
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))
from mobile.common import Limits, MobileError
from mobile.dalvik_model import Budget, Class, Instruction as Op, Method, MethodRef, TryRegion, link_classes
from mobile.dalvik_java import recover_java


def method(name, params, returns, registers, code, tries=()):
    return Method(MethodRef("Lfixture/Core;", name, tuple(params), returns), frozenset({"public", "static"}),
                  registers, code, list(tries), max(op.pc for op in code) + 1)


def recover(methods):
    cls = Class("Lfixture/Core;", "Ljava/lang/Object;", frozenset({"public"}), "owned-fixture", methods=methods)
    budget = Budget(Limits(timeout=30))
    return recover_java(link_classes([cls], budget), budget=budget)


class DalvikJavaTests(unittest.TestCase):
    def test_undefined_register_is_rejected_before_source_publication(self):
        with self.assertRaisesRegex(MobileError, "undefined"):
            recover([method("bad", (), "I", 1, [Op(0, "return", (0,))])])

    def test_conditional_definition_cannot_supply_missing_path(self):
        code = [Op(0, "if-eqz", (1,), target=2), Op(1, "const/4", (0,), literal=7), Op(2, "return", (0,))]
        with self.assertRaisesRegex(MobileError, "undefined"):
            recover([method("bad", ("I",), "I", 2, code)])

    def test_wide_result_cannot_borrow_overwritten_high_word(self):
        code = [Op(0, "const-wide/16", (0,), literal=9), Op(1, "const/4", (1,), literal=0),
                Op(2, "return-wide", (0,))]
        with self.assertRaisesRegex(MobileError, "undefined"):
            recover([method("bad", (), "J", 2, code)])

    def test_branch_cannot_bypass_move_result_producer(self):
        target = MethodRef("Ljava/lang/Math;", "abs", ("I",), "I")
        code = [Op(0, "if-eqz", (1,), target=2), Op(1, "invoke-static", (1,), reference=target),
                Op(2, "move-result", (0,)), Op(3, "return", (0,))]
        with self.assertRaisesRegex(MobileError, "bypasses"):
            recover([method("bad", ("I",), "I", 2, code)])

    def test_handler_cannot_read_destination_of_throwing_instruction(self):
        code = [Op(0, "div-int", (0, 2, 3)), Op(1, "return", (0,)),
                Op(2, "move-exception", (1,)), Op(3, "return", (0,))]
        with self.assertRaisesRegex(MobileError, "undefined"):
            recover([method("bad", ("I", "I"), "I", 4, code,
                            [TryRegion(0, 1, (("Ljava/lang/ArithmeticException;", 2),))])])

    def test_unknown_instruction_is_not_an_empty_body(self):
        with self.assertRaisesRegex(MobileError, "unsupported instruction"):
            recover([method("bad", (), "V", 0, [Op(0, "unrecognized"), Op(1, "return-void")])])

    def test_independent_java_execution_covers_loops_exceptions_and_bits(self):
        javac, java = shutil.which("javac"), shutil.which("java")
        jdk = Path(os.environ.get("JAVA_HOME", ""))
        if (jdk / "bin/javac").is_file() and (jdk / "bin/java").is_file():
            javac, java = str(jdk / "bin/javac"), str(jdk / "bin/java")
        if not javac or not java:
            self.skipTest("JDK required only to compile and execute the source oracle")
        version = subprocess.run([java, "-version"], text=True, capture_output=True, timeout=10)
        if sys.platform == "darwin" and "Unable to locate a Java Runtime" in version.stderr:
            self.skipTest("macOS Java launcher is present but no JDK is installed")
        self.assertEqual(version.returncode, 0, version.stdout + version.stderr)
        # This model is independent of both input readers.
        methods = [
            method("twice", ("I",), "I", 2, [Op(0, "mul-int/lit8", (0, 1), literal=2), Op(1, "return", (0,))]),
            method("divide", ("I", "I"), "I", 3,
                   [Op(0, "div-int", (0, 1, 2)), Op(1, "return", (0,)), Op(2, "move-exception", (0,)),
                    Op(3, "const/4", (0,), literal=-1), Op(4, "return", (0,))],
                   [TryRegion(0, 1, (("Ljava/lang/ArithmeticException;", 2),))]),
            method("sum", ("[I",), "I", 5,
                   [Op(0, "const/4", (0,), literal=0), Op(1, "const/4", (1,), literal=0),
                    Op(2, "array-length", (2, 4)), Op(3, "if-ge", (1, 2), target=10),
                    Op(4, "aget", (3, 4, 1)), Op(5, "if-gez", (3,), target=7), Op(6, "neg-int", (3, 3)),
                    Op(7, "add-int/2addr", (0, 3)), Op(8, "add-int/lit8", (1, 1), literal=1),
                    Op(9, "goto", target=3), Op(10, "return", (0,))]),
            method("floatBits", ("F",), "F", 1, [Op(0, "return", (0,))]),
            method("doubleBits", ("D",), "D", 2, [Op(0, "return-wide", (0,))]),
            method("wide", ("J",), "J", 4,
                   [Op(0, "const-wide/16", (0,), literal=3), Op(1, "mul-long", (0, 2, 0)), Op(2, "return-wide", (0,))]),
        ]
        report = recover(methods)
        self.assertEqual((report["method_count"], report["recovered_method_count"]), (6, 6))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = []
            for unit in report["source_units"]:
                target = root / unit["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(unit["source"])
                sources.append(str(target))
            harness = root / "Verify.java"
            harness.write_text("""
import fixture.Core;
public class Verify {
 public static void main(String[] args) {
   int[] values = {Integer.MIN_VALUE, -65537, -1, 0, 1, 65537, Integer.MAX_VALUE};
   for (int x : values) {
     if (Core.twice(x) != x * 2 || Core.divide(x, 0) != -1 || Core.divide(x, -1) != x / -1)
       throw new AssertionError("integer semantics");
   }
   if (Core.sum(new int[]{-3,2,-9,0}) != 14 || Core.sum(new int[0]) != 0)
     throw new AssertionError("loop semantics");
   for (int bits : new int[]{0, 0x80000000, 0x7f800000, 0x7fc12345, 0x3fc00000})
     if (Float.floatToRawIntBits(Core.floatBits(Float.intBitsToFloat(bits))) != bits)
       throw new AssertionError("float bits");
   for (long bits : new long[]{0L, Long.MIN_VALUE, 0x7ff0000000000000L, 0x7ff8123456789abCL})
     if (Double.doubleToRawLongBits(Core.doubleBits(Double.longBitsToDouble(bits))) != bits)
       throw new AssertionError("double bits");
   for (long x : new long[]{Long.MIN_VALUE, -1L, 0L, 1L, Long.MAX_VALUE})
     if (Core.wide(x) != x * 3L) throw new AssertionError("wide semantics");
 }
}
""")
            for argv in ([javac, "-d", str(root), *sources, str(harness)], [java, "-cp", str(root), "Verify"]):
                result = subprocess.run(argv, text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
