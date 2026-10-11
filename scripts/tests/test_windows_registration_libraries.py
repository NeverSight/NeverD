from pathlib import Path
import copy
import json
import tempfile
import unittest

from scripts import windows_registration_libraries as libraries


class RegistrationLibrariesTests(unittest.TestCase):
    def source(self, root):
        source = root / "source"
        source.mkdir()
        for name in libraries.LIBRARIES:
            (source / name).write_bytes(b"!<arch>\n" + name.encode())
        return source

    def test_capture_uses_native_search_order_and_loads_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.source(root)
            first = root / "first"
            first.mkdir()
            (first / "msvcrt.lib").write_bytes(b"!<arch>\nfirst archive")
            output = root / "output"
            captured = libraries.capture_libraries(output, [first, source], "14.44")
            paths, manifest = libraries.load_libraries(output)
            self.assertEqual(manifest, captured)
            self.assertEqual([path.name for path in paths], list(libraries.LIBRARIES))
            for path in paths:
                expected = first if path.name == "msvcrt.lib" else source
                self.assertEqual(path.read_bytes(), (expected / path.name).read_bytes())

    def test_missing_or_invalid_sources_cannot_publish_a_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.source(root)
            output = root / "output"
            for data in (None, b"", b"!<arch>\n", b"not an archive"):
                path = source / "kernel32.lib"
                if data is None:
                    path.unlink()
                else:
                    path.write_bytes(data)
                with self.subTest(data=data), self.assertRaises((ValueError, OSError)):
                    libraries.capture_libraries(output, [source], "14.44")
                self.assertFalse((output / "manifest.json").exists())

    def test_load_rejects_changed_library_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.source(root)
            output = root / "output"
            libraries.capture_libraries(output, [source], "14.44")
            for name in libraries.LIBRARIES:
                path = output / name
                original = path.read_bytes()
                with self.subTest(name=name):
                    path.write_bytes(original + b"changed")
                    with self.assertRaises(ValueError):
                        libraries.load_libraries(output)
                    path.unlink()
                    with self.assertRaises(OSError):
                        libraries.load_libraries(output)
                    path.write_bytes(original)

    def test_load_rejects_incomplete_or_different_library_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.source(root)
            output = root / "output"
            captured = libraries.capture_libraries(output, [source], "14.44")
            for mutation in range(8):
                changed = copy.deepcopy(captured)
                if mutation == 0:
                    changed["architecture"] = "x64"
                if mutation == 1:
                    changed["files"].pop()
                if mutation == 2:
                    changed["files"][0] = changed["files"][1]
                if mutation == 3:
                    changed["files"][0]["name"] = "../msvcrt.lib"
                if mutation == 4:
                    changed["files"][0]["sha256"] = "g" * 64
                if mutation == 5:
                    changed["toolset"] = ""
                if mutation == 6:
                    changed["schema"] = 2
                if mutation == 7:
                    changed = None
                (output / "manifest.json").write_text(json.dumps(changed))
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    libraries.load_libraries(output)


if __name__ == "__main__":
    unittest.main()
