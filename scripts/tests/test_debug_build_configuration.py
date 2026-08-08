import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET_SOURCES = (
    "third_party/capstone/MCInst.c",
    "lib/decode/Decoder.cpp",
)
DEBUG_FORBIDDEN_FLAGS = frozenset(
    {
        "-o1",
        "-o2",
        "-o3",
        "-os",
        "-oz",
        "-ofast",
        "-og",
        "/o1",
        "/o2",
        "/os",
        "/ot",
        "/ox",
        "/oy",
        "-fomit-frame-pointer",
        "-dndebug",
        "/dndebug",
    }
)


def command_tokens(entry: dict) -> set[str]:
    if "arguments" in entry:
        tokens = entry["arguments"]
    else:
        tokens = re.findall(r'"[^"]*"|\S+', entry["command"])
    return {token.strip('"').casefold() for token in tokens}


class DebugBuildConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._temporary_directory = tempfile.TemporaryDirectory(
            prefix="neverd-build-config-"
        )
        cls.addClassCleanup(cls._temporary_directory.cleanup)
        cls._compile_commands = {}

        temporary_root = Path(cls._temporary_directory.name)
        for configuration in ("Debug", "Release"):
            build_directory = temporary_root / configuration.casefold()
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    str(build_directory),
                    "-G",
                    "Ninja",
                    f"-DCMAKE_BUILD_TYPE={configuration}",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    "-DBUILD_TESTING=OFF",
                    "-DNEVERD_BUILD_PLUGINS=OFF",
                    "-DNEVERD_BUILD_SHARED=OFF",
                    "-DNEVERD_LLVM_PREBUILT=OFF",
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"CMake {configuration} configuration failed:\n{result.stdout}"
                )

            compile_commands_path = build_directory / "compile_commands.json"
            cls._compile_commands[configuration] = json.loads(
                compile_commands_path.read_text(encoding="utf-8")
            )

    def tokens_for(self, configuration: str, source_suffix: str) -> set[str]:
        matches = [
            entry
            for entry in self._compile_commands[configuration]
            if entry["file"].replace("\\", "/").endswith(source_suffix)
        ]
        self.assertEqual(
            len(matches),
            1,
            f"expected one {configuration} compile command for {source_suffix}",
        )
        return command_tokens(matches[0])

    def test_debug_hot_paths_are_unoptimized_and_keep_assertions(self):
        for source_suffix in TARGET_SOURCES:
            with self.subTest(source=source_suffix):
                tokens = self.tokens_for("Debug", source_suffix)
                present = sorted(tokens & DEBUG_FORBIDDEN_FLAGS)
                self.assertEqual(
                    present,
                    [],
                    f"Debug compile command contains release-only flags: {present}",
                )

    def test_release_hot_paths_remain_optimized(self):
        for source_suffix in TARGET_SOURCES:
            with self.subTest(source=source_suffix):
                tokens = self.tokens_for("Release", source_suffix)
                is_msvc = "/d_windows" in tokens
                required = (
                    {"/o2", "/dndebug"}
                    if is_msvc
                    else {"-o3", "-fomit-frame-pointer", "-dndebug"}
                )
                missing = sorted(required - tokens)
                self.assertEqual(
                    missing,
                    [],
                    f"Release compile command is missing performance flags: {missing}",
                )


if __name__ == "__main__":
    unittest.main()
