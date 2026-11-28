import unittest

from scripts.check_windows_xcpt4_source import check_runtime, check_source


class XCPT4SourceTests(unittest.TestCase):
    def test_pe32_requires_the_cases_compiled_for_x86(self):
        skipped = [29, 58, 61, 87, 89]
        lines = [f"test{i}...{'skipped' if i in skipped else 'passed'}"
                 for i in range(1, 91)]
        output = "\n".join(["Start of exception test", *lines, "End of exception test"])
        self.assertEqual(check_runtime({"exit_code": 0, "stdout": output}, pe32=True),
                         {"passed": 85, "skipped": skipped})
        with self.assertRaises(ValueError):
            check_runtime({"exit_code": 0, "stdout": output})
        for changed in (output.replace("test58...skipped", "test58...passed"),
                        output.replace("test57...passed", "test57...skipped"),
                        output.replace("test61...skipped\n", "")):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                check_runtime({"exit_code": 0, "stdout": changed}, pe32=True)

    def test_runtime_requires_all_results_even_after_a_zero_exit(self):
        output = ("Start of exception test\n"
                  + "".join(f"    test{i}...succeeded\n" for i in range(1, 91))
                  + "End of exception test\n")
        self.assertEqual(check_runtime({"exit_code": 0, "stdout": output}),
                         {"passed": 90, "skipped": []})
        x64 = output.replace("test29...succeeded", "test29...skipped")
        self.assertEqual(check_runtime({"exit_code": 0, "stdout": x64}),
                         {"passed": 89, "skipped": [29]})
        for changed in (output.replace("test12...succeeded", "test12...failed"),
                        output.replace("    test90...succeeded\n", ""),
                        output.replace("test90", "test89"),
                        output.replace("test30...succeeded", "test30...skipped"),
                        output.replace("End of exception test", "")):
            with self.subTest(changed=changed[-100:]), self.assertRaises(ValueError):
                check_runtime({"exit_code": 0, "stdout": changed})
        with self.assertRaises(ValueError):
            check_runtime({"exit_code": 1, "stdout": output})

    def test_runtime_rejects_wine_crashes_even_after_all_cases_pass(self):
        output = ("Start of exception test\n"
                  + "".join(f"    test{i}...succeeded\n" for i in range(1, 91))
                  + "End of exception test\n")
        for stream in ("stdout", "stderr"):
            for diagnostic in (
                    "wine: Unhandled exception 0xc0000091, starting debugger...",
                    "Unhandled exception: floating point overflow in 32-bit code",
                    "Unhandled page fault on read access at address 00000000"):
                result = {"exit_code": 0, "stdout": output, "stderr": ""}
                result[stream] += diagnostic + "\n"
                with self.subTest(stream=stream, diagnostic=diagnostic), \
                        self.assertRaises(ValueError):
                    check_runtime(result)
        self.assertEqual(check_runtime({
            "exit_code": 0, "stdout": output,
            "stderr": "wine: configuration has been updated.\n"}),
            {"passed": 90, "skipped": []})

    def test_source_needs_the_main_body_with_its_finally_clauses_filled(self):
        source = """/* neverd.analysis-only: recovered Windows SEH/C++ */
/* code=[0x140001500, 0x14000392D), seh.scope[0], gs.cookie_offset=8
 * highir.structured_regions=114 */
int sub_140001500(void) {
  __try { goto done; } __finally { if (AbnormalTermination()) { } }
done:
  return 0;
}
"""
        check_source(source)
        for changed in (source.replace("sub_140001500", "sub_140001000"),
                        source.replace("goto done;", "_local_unwind(frame, target);"),
                        source.replace("structured_regions=114", "structured_regions=114; unresolved runtime stack"),
                        source.replace("__finally", ""),
                        source.replace("__try", ""),
                        source.replace("(void) {", "(void); {")):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                check_source(changed)


if __name__ == "__main__":
    unittest.main()
