from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import run_darwin_kernel_reference as reference


class DarwinKernelReferenceTests(unittest.TestCase):
    def test_formatted_inventory_is_complete_and_rejects_unparsed_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cases.def"
            valid = 'NEVERD_DARWIN_NATIVE_CASE(\n "memory", 37, "d"\n)\n'
            valid += 'NEVERD_DARWIN_NATIVE_CASE("slashes", 37, "//")\n'
            path.write_text(valid)
            self.assertEqual(reference.read_cases(path), [("memory", 37, b"d"), ("slashes", 37, b"//")])
            for invalid in ("", valid + valid, valid + "BROKEN_CASE()", valid.replace("37", "256")):
                with self.subTest(invalid=invalid):
                    path.write_text(invalid)
                    with self.assertRaises(ValueError):
                        reference.read_cases(path)

    def test_wrong_os_architecture_and_rosetta_cannot_supply_native_evidence(self):
        for system, actual, requested, translated in (
            ("Linux", "arm64", "arm64", ""),
            ("Darwin", "arm64", "x86_64", "0"),
            ("Darwin", "x86_64", "x86_64", "1"),
        ):
            with self.subTest(system=system, actual=actual, translated=translated):
                with (mock.patch.object(reference.platform, "system", return_value=system),
                      mock.patch.object(reference.platform, "machine", return_value=actual),
                      mock.patch.object(reference.subprocess, "run", return_value=
                                        subprocess.CompletedProcess([], 0, translated, ""))):
                    with self.assertRaises(ValueError):
                        reference.native_architecture(requested)

    def test_intel_without_rosetta_sysctl_and_native_arm_are_accepted(self):
        for architecture, status, translated in (("x86_64", 1, ""), ("arm64", 0, "0")):
            with self.subTest(architecture=architecture):
                with (mock.patch.object(reference.platform, "system", return_value="Darwin"),
                      mock.patch.object(reference.platform, "machine", return_value=architecture),
                      mock.patch.object(reference.subprocess, "run", return_value=
                                        subprocess.CompletedProcess([], status, translated, ""))):
                    self.assertEqual(reference.native_architecture(architecture), architecture)

    def test_status_output_stderr_and_timeouts_cannot_pass(self):
        outcomes = [
            subprocess.CompletedProcess([], 37, b"d", b""),
            subprocess.CompletedProcess([], 0, b"d", b""),
            subprocess.CompletedProcess([], 37, b"wrong", b""),
            subprocess.CompletedProcess([], 37, b"d", b"unexpected"),
            subprocess.TimeoutExpired([], 5, output=b"partial"),
        ]
        cases = [(str(index), 37, b"d") for index in range(len(outcomes))]
        with mock.patch.object(reference.subprocess, "run", side_effect=outcomes):
            results = reference.execute_cases(Path("native"), cases)
        self.assertEqual([result["passed"] for result in results], [True, False, False, False, False])
        self.assertEqual(results[-1]["error"], "timeout")
        self.assertEqual(results[-1]["stdout_hex"], b"partial".hex())

    def test_symbolic_link_case_has_its_own_catalogue_without_changing_old_modes(self):
        roots = []
        def execute(command, **kwargs):
            path = Path(command[2])
            roots.append(path.parent)
            self.assertEqual(path.read_bytes(), b"0123456789")
            self.assertEqual(kwargs, {"capture_output": True, "timeout": 5})
            if command[1] == "symbolic-links":
                self.assertEqual({item.name for item in path.parent.iterdir()},
                                 {"data", "empty", "link", "chain", "dangling", "cycle", "dirlink"})
                self.assertEqual((path.parent / "link").readlink(), Path("data"))
                self.assertTrue((path.parent / "cycle").is_symlink())
                self.assertEqual((path.parent / "dirlink").readlink(), Path("empty"))
                self.assertFalse((path.parent / "dangling").exists())
            elif command[1] in ("kernel-pathconf", "common-attributes", "extended-attributes", "attribute-names", "bulk-attributes", "xattr-mutations", "hard-links"):
                names = {"data", "empty", "alias", "dangling", "cycle"}
                if command[1] == "hard-links":
                    names.add("attributes")
                    self.assertEqual((path.parent / "attributes").read_bytes(), b"x")
                self.assertEqual({item.name for item in path.parent.iterdir()}, names)
                self.assertEqual((path.parent / "alias").readlink(), Path("data"))
                self.assertEqual((path.parent / "cycle").readlink(), Path("cycle"))
                self.assertFalse((path.parent / "dangling").exists())
            elif command[1] == "symbolic-descriptors":
                self.assertEqual({item.name for item in path.parent.iterdir()}, {"data", "fd-attrs"})
                self.assertEqual((path.parent / "fd-attrs").readlink(), Path("data"))
            elif command[1] == "nonblocking-descriptors":
                self.assertEqual({item.name for item in path.parent.iterdir()}, {"data", "fd-nonblock"})
                self.assertEqual((path.parent / "fd-nonblock").readlink(), Path("data"))
            elif command[1] == "directory-link-roots":
                catalogue = path.parent
                self.assertEqual({item.name for item in catalogue.iterdir()}, {"data", "a", "b"})
                for name, value in (("a/target", 11), ("b/target", 22),
                                    ("a/d/c", 31), ("a/other/mark", 41),
                                    ("b/other/mark", 42)):
                    self.assertEqual((catalogue / name).read_bytes(), bytes([value]))
                for name, target in (("b/l", "target"), ("b/dang", "missing"),
                                     ("b/dirlink", "other"), ("b/self", "../a/d"),
                                     ("a/d/inside", "../target")):
                    self.assertEqual((catalogue / name).readlink(), Path(target))
            elif command[1] == "mutable-initial-links":
                self.assertEqual({item.name for item in path.parent.iterdir()},
                                 {"data", "empty", "initial", "initial-dir"})
                self.assertEqual((path.parent / "initial").readlink(), Path("data"))
                self.assertEqual((path.parent / "initial-dir").readlink(), Path("empty"))
            elif command[1] in ("symbolic-link-mutations", "symbolic-link-creation", "symbolic-link-unlink", "symbolic-link-rename"):
                catalogue = path.parent.parent
                self.assertEqual({item.name for item in catalogue.iterdir()}, {"static", "work"})
                self.assertEqual({item.name for item in path.parent.iterdir()}, {"data"})
                self.assertEqual({item.name for item in (catalogue / "static").iterdir()},
                                 {"alias", "data-link", "missing-link"})
                self.assertEqual((catalogue / "static" / "alias").readlink(), Path("../work"))
                self.assertEqual((catalogue / "static" / "data-link").readlink(), Path("../work/data"))
                self.assertFalse((catalogue / "static" / "missing-link").exists())
            else:
                self.assertEqual({item.name for item in path.parent.iterdir()}, {"data", "empty"})
            return subprocess.CompletedProcess(command, 37, b"", b"")
        with (mock.patch.object(reference.subprocess, "run", side_effect=execute),
              mock.patch.object(reference, "set_native_attribute") as attributes):
            results = reference.execute_cases(Path("native"),
                [("directory-entries", 37, b""), ("symbolic-links", 37, b""),
                 ("symbolic-link-mutations", 37, b""),
                 ("symbolic-link-creation", 37, b""),
                 ("symbolic-link-unlink", 37, b""),
                 ("symbolic-link-rename", 37, b""),
                 ("mutable-initial-links", 37, b""),
                 ("directory-link-roots", 37, b""),
                 ("directory-entries", 37, b""),
                 ("kernel-pathconf", 37, b""), ("common-attributes", 37, b""),
                 ("extended-attributes", 37, b""), ("attribute-names", 37, b""), ("bulk-attributes", 37, b""), ("xattr-mutations", 37, b""), ("hard-links", 37, b""), ("symbolic-descriptors", 37, b""), ("nonblocking-descriptors", 37, b"")])
            self.assertEqual(attributes.call_count, 3)
            self.assertEqual([(call.args[1], call.args[2]) for call in attributes.call_args_list],
                             [("user.neverd.beta", b"\x00\xffA\x00\x80B\n"),
                              ("user.neverd.alpha", b"alpha"), ("user.neverd.empty", b"")])
        self.assertTrue(all(result["passed"] for result in results))
        self.assertEqual(roots[0], roots[8])
        self.assertNotIn(roots[9], roots[:9])
        self.assertNotIn(roots[10], roots[:10])
        self.assertNotIn(roots[11], roots[:11])
        self.assertNotIn(roots[12], roots[:12])
        self.assertNotIn(roots[13], roots[:13])
        self.assertNotIn(roots[15], roots[:15])
        self.assertNotEqual(roots[0], roots[1])
        self.assertNotIn(roots[2], roots[:2])
        self.assertNotIn(roots[3], roots[:3])
        self.assertNotIn(roots[4], roots[:4])
        self.assertNotIn(roots[5], roots[:5])
        self.assertNotIn(roots[6], roots[:6])
        self.assertNotIn(roots[7], roots[:7])
        self.assertTrue(all(not path.exists() for path in roots))

    def test_native_attribute_setup_failures_preserve_the_case_and_do_not_run_it(self):
        with (mock.patch.object(reference, "set_native_attribute",
                                side_effect=OSError(1, "private seed refused")),
              mock.patch.object(reference.subprocess, "run") as execute):
            result = reference.execute_cases(Path("native"),
                                             [("extended-attributes", 37, b"X")])
        execute.assert_not_called()
        self.assertEqual(len(result), 1)
        self.assertFalse(result[0]["passed"])
        self.assertIsNone(result[0]["exit_status"])
        self.assertEqual(result[0]["error"], "native attribute setup failed")
        self.assertIn("private seed refused", result[0]["setup_error"])

    def test_native_file_cases_receive_real_isolated_input_bytes(self):
        paths = []
        def execute(command, **kwargs):
            path = Path(command[2])
            paths.append(path)
            self.assertTrue(path.is_absolute())
            self.assertEqual(path.read_bytes(), b"0123456789")
            path.write_bytes(b"previous mutation must not leak into next case")
            return subprocess.CompletedProcess(command, 37, b"f", b"")
        with mock.patch.object(reference.subprocess, "run", side_effect=execute):
            results = reference.execute_cases(
                Path("native"), [("files", 37, b"f"), ("files-nocancel", 37, b"f")])
        self.assertTrue(all(result["passed"] for result in results))
        self.assertEqual(paths[0], paths[1])
        self.assertFalse(paths[0].exists())


if __name__ == "__main__":
    unittest.main()
