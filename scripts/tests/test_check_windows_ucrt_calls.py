"""The SDK comparison must reject changed enum values and call types."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "check_windows_ucrt_calls.py"
SPEC = importlib.util.spec_from_file_location("check_windows_ucrt_calls", SCRIPT)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)

SOURCE = """
#define static_assert _Static_assert
#ifndef NEVERD_TYPE_Operation
#define NEVERD_TYPE_Operation
// Operation: 4 bytes
enum Operation
{
    Add = 1,
    Subtract = 2,
};
static_assert(sizeof(enum Operation) == 4, "Operation");
#endif
typedef enum Operation Operation;
extern double calculate(Operation op, double value);
double wrapper(double value) { return calculate(Add, value); }
"""
SDK = """
typedef enum { Add = 1, Subtract = 2 } Operation;
extern double calculate(Operation op, double value);
"""


class SDKEnums(unittest.TestCase):
    def compile(self, text):
        clang = shutil.which("clang")
        if not clang:
            self.skipTest("Clang is unavailable")
        result = subprocess.run([clang, "-x", "c", "-fsyntax-only", "-"],
                                input=text, text=True, capture_output=True)
        return result.returncode, result.stderr

    def test_keeps_calls_and_checks_the_original_anonymous_enum(self):
        checked = CHECK.sdk_checked_source(SOURCE)
        self.assertIn("return calculate(Add, value);", checked)
        self.assertEqual(self.compile(SOURCE)[0], 0)
        self.assertEqual(self.compile(SDK + checked)[0], 0)
        self.assertNotEqual(self.compile(SDK + SOURCE)[0], 0)

    def test_changed_value_and_size_fail(self):
        for changed in (SOURCE.replace("Subtract = 2", "Subtract = 3"),
                        SOURCE.replace("== 4", "== 8")):
            with self.subTest(changed=changed):
                self.assertNotEqual(self.compile(SDK + CHECK.sdk_checked_source(changed))[0], 0)

    def test_enum_constants_do_not_require_an_emitted_typedef(self):
        changed = SOURCE.replace("typedef enum Operation Operation;", "")
        checked = CHECK.sdk_checked_source(changed)
        self.assertEqual(self.compile(SDK + checked)[0], 0)

    def test_tagged_uses_match_the_independently_checked_sdk_typedef(self):
        changed = SOURCE.replace("calculate(Operation op", "calculate(enum Operation op")
        changed = changed.replace("calculate(Add, value)", "calculate((enum Operation)Add, value)")
        self.assertEqual(self.compile(changed)[0], 0)
        self.assertEqual(self.compile(SDK + CHECK.sdk_checked_source(changed))[0], 0)
        bad = changed.replace("Subtract = 2", "Subtract = 9")
        self.assertNotEqual(self.compile(SDK + CHECK.sdk_checked_source(bad))[0], 0)

    def test_wrong_prototype_and_missing_argument_still_fail(self):
        for changed in (SOURCE.replace("extern double calculate", "extern int calculate"),
                        SOURCE.replace("calculate(Add, value)", "calculate(Add)")):
            with self.subTest(changed=changed):
                self.assertNotEqual(self.compile(SDK + CHECK.sdk_checked_source(changed))[0], 0)

    def test_unsigned_value_is_checked_against_sdk_representation(self):
        changed = SOURCE.replace("Subtract = 2", "Subtract = -2")
        self.assertNotEqual(self.compile(SDK + CHECK.sdk_checked_source(changed))[0], 0)

    def test_unrecognized_initializer_fails_closed(self):
        with self.assertRaises(ValueError):
            CHECK.sdk_checked_source(SOURCE.replace("Subtract = 2", "Subtract = Add + 1"))


if __name__ == "__main__":
    unittest.main()
