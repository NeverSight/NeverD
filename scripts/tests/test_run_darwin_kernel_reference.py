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
            elif command[1] in ("symbolic-link-mutations", "symbolic-link-creation", "symbolic-link-unlink"):
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
        with mock.patch.object(reference.subprocess, "run", side_effect=execute):
            results = reference.execute_cases(Path("native"),
                [("directory-entries", 37, b""), ("symbolic-links", 37, b""),
                 ("symbolic-link-mutations", 37, b""),
                 ("symbolic-link-creation", 37, b""),
                 ("symbolic-link-unlink", 37, b""),
                 ("directory-entries", 37, b"")])
        self.assertTrue(all(result["passed"] for result in results))
        self.assertEqual(roots[0], roots[5])
        self.assertNotEqual(roots[0], roots[1])
        self.assertNotIn(roots[2], roots[:2])
        self.assertNotIn(roots[3], roots[:3])
        self.assertNotIn(roots[4], roots[:4])
        self.assertTrue(all(not path.exists() for path in roots))

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
