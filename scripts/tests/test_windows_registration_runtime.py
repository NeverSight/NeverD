import copy
from pathlib import Path
import struct
import tempfile
import unittest

from scripts import windows_registration_runtime as runtime


def runtime_fixture() -> bytes:
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, 0x14c)
    struct.pack_into("<H", data, 150, 0x2000)
    struct.pack_into("<H", data, 152, 0x10b)
    return bytes(data)


class RegistrationRuntimeTests(unittest.TestCase):
    def test_capture_uses_the_selected_redistributable_and_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            redist = root / "redist"
            source = redist / "x86/Microsoft.VC143.CRT" / runtime.NAME
            source.parent.mkdir(parents=True)
            source.write_bytes(runtime_fixture())
            output = root / "output"
            manifest = runtime.capture_runtime(output, redist, "14.44")
            path, loaded = runtime.load_runtime(output)
            self.assertEqual(manifest, loaded)
            self.assertEqual(path.read_bytes(), source.read_bytes())
            path.write_bytes(source.read_bytes() + b"changed")
            with self.assertRaises(ValueError):
                runtime.load_runtime(output)

    def test_incomplete_runtime_identity_cannot_authorize_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "x86/Microsoft.VC143.CRT" / runtime.NAME
            source.parent.mkdir(parents=True)
            source.write_bytes(runtime_fixture())
            manifest = runtime.capture_runtime(root / "output", root, "14.44")
            for key, value in (("schema", 2), ("provider", "Wine"), ("architecture", "x64"),
                               ("name", "../vcruntime140.dll"), ("toolset", ""),
                               ("sha256", "not a digest")):
                changed = copy.deepcopy(manifest)
                changed[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    runtime.validate_runtime(root / "output", changed)
            for offset, value in ((0, b"XX"), (128, b"XX"), (132, b"\x64\x86"),
                                  (150, b"\0\0"), (152, b"\x0b\x02")):
                data = bytearray(runtime_fixture())
                data[offset:offset + len(value)] = value
                source.write_bytes(data)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    runtime.capture_runtime(root / "output", root, "14.44")

    def test_capture_requires_one_runtime_from_the_selected_toolset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                runtime.capture_runtime(root / "output", root, "14.44")
            for name in ("Microsoft.VC143.CRT", "Microsoft.VC144.CRT"):
                path = root / "x86" / name / runtime.NAME
                path.parent.mkdir(parents=True)
                path.write_bytes(runtime_fixture())
            with self.assertRaises(ValueError):
                runtime.capture_runtime(root / "output", root, "14.44")


if __name__ == "__main__":
    unittest.main()
