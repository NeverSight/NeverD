"""Official header generation rejects partial or ABI-changing inputs."""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location(
    "microsoft_windows_types", SCRIPTS / "generate_microsoft_windows_types.py")
windows = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(windows)


class MicrosoftWindowsTypes(unittest.TestCase):
    def test_isolated_kernel_provider_keeps_default_before_selection(self):
        # Preprocessing removes /Gz. The parser must see that policy before
        # reading both the exported function and its unannotated callback.
        declaration = "typedef int (*Callback)(int);\nlong Read(Callback cb);\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.c"
            source.write_text(declaration)
            seen = []

            def run(command, log, output=None):
                if output:
                    output.write_text(declaration)
                return "fixture record layouts"

            def parsed(neverd, path, log, model):
                seen.append(path.read_text())
                return {"prototypes": ["Read"], "problems": []}

            for convention in ("__stdcall", ""):
                with patch.object(windows, "run", side_effect=run), \
                        patch.object(windows, "checked_types", side_effect=parsed):
                    result = windows.prepare_provider(
                        "Provider", source, ["clang"], root, "neverd", "ILP32",
                        convention=convention)
                expected = (("#pragma neverd_default_calling_convention(__stdcall)\n"
                             if convention else "") + declaration)
                self.assertEqual(seen[-1], expected)
                self.assertEqual(result["source"].read_text(), expected)
                self.assertEqual(result["default_calling_convention"], convention)
                self.assertEqual(result["input_sha256"], windows.wdk.digest(result["source"]))

    def test_x86_driver_compiler_default_requires_official_build_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = Path(directory)
            props = kit / f"c/build/{windows.VERSION}/WindowsDriver.Common.props"
            props.parent.mkdir(parents=True)
            source = ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
                      '<ItemDefinitionGroup><ClCompile>'
                      '<CallingConvention>StdCall</CallingConvention>'
                      '</ClCompile></ItemDefinitionGroup></Project>')
            props.write_text(source)
            self.assertEqual(windows.compiler_defaults(kit, "x86", "WindowsKernel"),
                             (["-Xclang", "-fdefault-calling-conv=stdcall"], "__stdcall"))
            for arch in windows.TARGETS:
                self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsUser"), ([], ""))
                if arch != "x86":
                    self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsKernel"), ([], ""))
            props.write_text(source.replace("StdCall", "unknown"))
            with self.assertRaisesRegex(ValueError, "unrecognized WDK"):
                windows.compiler_defaults(kit, "x86", "WindowsKernel")

    def test_combined_targets_require_all_headers_and_matching_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            targets = []
            for arch, (suffix, _, _, _) in windows.TARGETS.items():
                target = root / arch
                (target / "licenses").mkdir(parents=True)
                notice = target / "licenses" / "notice.txt"
                notice.write_text("Synthetic test notice")
                libraries = []
                for base in windows.LIBRARIES:
                    name = base + suffix
                    header = target / (name + ".h")
                    header.write_text("/* synthetic " + arch + " library */")
                    libraries.append({"name": name, "sha256": windows.wdk.digest(header)})
                record = {"schema": 1, "provider": "Microsoft SDK/WDK",
                          "architecture": arch, "libraries": libraries,
                          "compiler": "fixture compiler", "packages": [],
                          "profiles": {"ucrt": "fixture profile hash"},
                          "target_packages": [{"file": f"sdk.{arch}.nupkg",
                                               "architecture": arch,
                                               "sha256": "fixture package hash"}],
                          "licenses": [{"file": notice.name, "sha256": windows.wdk.digest(notice)}]}
                (target / "manifest.json").write_text(json.dumps(record))
                targets.append(target)
            output = root / "combined"
            windows.combine_targets(output, targets)
            combined = json.loads((output / "manifest.json").read_text())
            self.assertEqual(len(combined["libraries"]), 6)
            self.assertEqual({entry["architecture"] for entry in combined["libraries"]},
                             set(windows.TARGETS))
            self.assertEqual(len(combined["target_packages"]), 3)
            self.assertEqual(combined["profiles"], {"ucrt": "fixture profile hash"})
            for record in combined["libraries"]:
                self.assertEqual(windows.wdk.digest(output / (record["name"] + ".h")), record["sha256"])
            # A mixture of provider policy, generation profile or target
            # packages must never certify a previously published bundle.
            path = targets[-1] / "manifest.json"
            original = path.read_text()
            mutations = [
                ("provider", windows.COMPATIBLE_PROVIDER, "different"),
                ("profiles", {}, "different"),
                ("compiler", "other compiler", "different"),
                ("packages", [{"file": "other"}], "different"),
                ("target_packages", [{"file": "sdk.other.nupkg", "architecture": "wrong"}],
                 "architecture-specific"),
                ("target_packages", [{"file": "sdk.x64.nupkg", "architecture": "arm64"}],
                 "architecture-specific"),
            ]
            for key, value, message in mutations:
                with self.subTest(key=key, value=value):
                    changed = json.loads(original)
                    changed[key] = value
                    path.write_text(json.dumps(changed))
                    with self.assertRaisesRegex(ValueError, message):
                        windows.combine_targets(output, targets)
                    self.assertFalse((output / "manifest.json").exists())
            path.write_text(original)
            with self.assertRaisesRegex(ValueError, "required"):
                windows.combine_targets(output, targets[:2])
            self.assertFalse((output / "manifest.json").exists())
            header = targets[0] / "WindowsKernelX64.h"
            header.write_text("changed bytes")
            with self.assertRaisesRegex(ValueError, "changed Microsoft target header"):
                windows.combine_targets(output, targets)
            self.assertFalse((output / "manifest.json").exists())

    def test_layout_changes_reject_equal_size_and_pointer_width_loss(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct Record\n"
                    "         0 |   void * __ptr32 Pointer\n"
                    "         4 |   unsigned int Length\n"
                    "           | [sizeof=8, align=4]\n")
        self.assertEqual(windows.verify_layouts(original, original), 1)
        for changed in (original.replace("align=4", "align=8"),
                        original.replace("4 |", "8 |"),
                        original.replace("Length", "MissingField"),
                        original.replace("sizeof=8", "sizeof=16")):
            with self.assertRaisesRegex(ValueError, "Record"):
                windows.verify_layouts(original, changed)
        with self.assertRaisesRegex(ValueError, "no shared"):
            windows.verify_layouts(original, "")

    def test_cpp_record_layout_continuations_still_check_size_and_alignment(self):
        source = ("*** Dumping AST Record Layout\n"
                  "         0 | struct Record\n"
                  "         0 |   const char * Text\n"
                  "         8 |   _Bool Owned\n"
                  "           | [sizeof=16, align=8,\n"
                  "           |  nvsize=16, nvalign=8]\n")
        rendered = source.replace(",\n           |  nvsize=16, nvalign=8]", "]")
        self.assertEqual(windows.verify_layouts(source, rendered), 1)
        self.assertEqual(windows.verify_layouts(source.replace("sizeof=16,", "sizeof=16, dsize=16,"),
                                               rendered), 1)
        with self.assertRaisesRegex(ValueError, "layout changed"):
            windows.verify_layouts(source, rendered.replace("8 |", "9 |"))

    def test_provider_layouts_follow_renames_and_require_the_needed_records(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct URL\n"
                    "         0 |   unsigned short * Text\n"
                    "           | [sizeof=8, align=8]\n"
                    "*** Dumping AST Record Layout\n"
                    "         0 | struct Unused\n"
                    "         0 |   int Value\n"
                    "           | [sizeof=4, align=4]\n")
        generated = original.replace("struct URL", "struct Wide_URL")
        renamed = {"URL": "Wide_URL"}
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["URL"]), 1)
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["COUNT"]), 0)
        with self.assertRaisesRegex(ValueError, "missing: URL -> Wide_URL"):
            windows.verify_layouts(original, original, renamed, ["URL"])
        with self.assertRaisesRegex(ValueError, "layout changed: URL"):
            windows.verify_layouts(original, generated.replace("align=8", "align=4"),
                                   renamed, ["URL"])

    def test_failed_update_cannot_certify_stale_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "manifest.json").write_text('{"stale": true}')
            with patch.object(windows.wdk, "declarations", side_effect=ValueError("fixture")):
                with self.assertRaisesRegex(ValueError, "fixture"):
                    windows.generate(root, root / "cache", "neverd", "clang")
            self.assertFalse((root / "manifest.json").exists())

    def test_partial_parser_results_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "parse.log"
            with patch.object(windows, "run", return_value='{"problems": [{"message": "unknown type"}]}'):
                with self.assertRaisesRegex(ValueError, "refused"):
                    windows.checked_types("neverd", Path("test.h"), log)
            self.assertIn("unknown type", log.read_text())

    def test_compatibility_requires_official_provider_before_acquisition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(windows.wdk, "download") as download:
                with self.assertRaisesRegex(ValueError, "requires the official"):
                    windows.generate(root, root / "cache", "neverd", "clang",
                                     ucrt_compatibility=True)
                download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
