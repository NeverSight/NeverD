import io
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import check_clang_format


class ClangFormatCheckTests(unittest.TestCase):
    def run_git(self, root: Path, *arguments: str) -> str:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        return completed.stdout.strip()

    def write(self, root: Path, relative_path: str, contents: str) -> None:
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def test_diff_checks_only_changed_first_party_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            self.run_git(root, "init", "--quiet")
            self.run_git(root, "config", "user.email", "test@example.com")
            self.run_git(root, "config", "user.name", "Test User")

            self.write(root, "lib/changed.cpp", "int changed;\n")
            self.write(root, "include/old.h", "int renamed;\n")
            self.write(root, "tools/deleted.cpp", "int deleted;\n")
            self.write(root, "third_party/vendor.cpp", "int vendor;\n")
            self.write(root, "docs/example.cpp", "int documentation;\n")
            self.run_git(root, "add", ".")
            self.run_git(root, "commit", "--quiet", "-m", "base")
            base = self.run_git(root, "rev-parse", "HEAD")

            self.write(root, "lib/changed.cpp", "int changed_again;\n")
            self.run_git(root, "mv", "include/old.h", "include/renamed.h")
            self.run_git(root, "rm", "--quiet", "tools/deleted.cpp")
            self.write(root, "unittests/space name/added.h", "int added;\n")
            self.write(root, "plugins/added.inc", "int included;\n")
            self.write(root, "include/added.def", "HANDLE_ENTRY(included)\n")
            self.write(root, "third_party/vendor.cpp", "int vendor_again;\n")
            self.write(root, "docs/example.cpp", "int documentation_again;\n")
            self.run_git(root, "add", ".")
            self.run_git(root, "commit", "--quiet", "-m", "change")
            head = self.run_git(root, "rev-parse", "HEAD")

            checked_paths: list[str] = []

            def run_command(command: list[str], **kwargs: object):
                if command[0] == "git":
                    return subprocess.run(command, **kwargs)
                if command[1:] == ["--version"]:
                    return subprocess.CompletedProcess(
                        command, 0, "clang-format version 22.1.2\n", ""
                    )
                checked_paths.extend(command[3:])
                return subprocess.CompletedProcess(command, 0, "", "")

            stderr = io.StringIO()
            result = check_clang_format.main(
                [
                    "--base",
                    base,
                    "--head",
                    head,
                    "--clang-format",
                    "test-clang-format",
                ],
                repo_root=root,
                run_command=run_command,
                stderr=stderr,
            )

            self.assertEqual(result, 0, stderr.getvalue())
            self.assertEqual(
                checked_paths,
                [
                    "include/added.def",
                    "include/renamed.h",
                    "lib/changed.cpp",
                    "plugins/added.inc",
                    "unittests/space name/added.h",
                ],
            )

    def test_rejects_an_unpinned_formatter_before_checking_sources(self) -> None:
        commands: list[list[str]] = []

        def run_command(command: list[str], **kwargs: object):
            commands.append(command)
            if command[0] == "git":
                return subprocess.CompletedProcess(
                    command, 0, b"lib/example.cpp\0", b""
                )
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(
                    command, 0, "clang-format version 21.1.0\n", ""
                )
            return subprocess.CompletedProcess(command, 0, "", "")

        stderr = io.StringIO()
        result = check_clang_format.main(
            ["--base", "BASE", "--clang-format", "test-clang-format"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=stderr,
        )

        self.assertEqual(result, 2)
        self.assertIn("requires clang-format 22.1.2", stderr.getvalue())
        self.assertFalse(any("--dry-run" in command for command in commands))

    def test_large_file_sets_are_checked_in_bounded_batches(self) -> None:
        paths = [f"lib/file_{index:03}.cpp" for index in range(129)]
        format_commands: list[list[str]] = []

        def run_command(command: list[str], **kwargs: object):
            if command[0] == "git":
                output = b"\0".join(path.encode("utf-8") for path in paths) + b"\0"
                return subprocess.CompletedProcess(command, 0, output, b"")
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(
                    command, 0, "clang-format version 22.1.2\n", ""
                )
            format_commands.append(command)
            return subprocess.CompletedProcess(command, 0, "", "")

        result = check_clang_format.main(
            ["--base", "BASE", "--clang-format", "test-clang-format"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=io.StringIO(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(len(format_commands), 3)
        self.assertTrue(all(len(command[3:]) <= 64 for command in format_commands))
        self.assertEqual(
            [path for command in format_commands for path in command[3:]], paths
        )

    def test_format_violation_reports_the_checked_path(self) -> None:
        def run_command(command: list[str], **kwargs: object):
            if command[0] == "git":
                return subprocess.CompletedProcess(
                    command, 0, b"lib/example.cpp\0", b""
                )
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(
                    command, 0, "clang-format version 22.1.2\n", ""
                )
            return subprocess.CompletedProcess(command, 1, "", "style error\n")

        stderr = io.StringIO()
        result = check_clang_format.main(
            ["--base", "BASE", "--clang-format", "test-clang-format"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=stderr,
        )

        self.assertEqual(result, 1)
        self.assertIn("style error", stderr.getvalue())
        self.assertIn("lib/example.cpp", stderr.getvalue())

    def test_null_push_base_falls_back_to_the_previous_commit(self) -> None:
        git_commands: list[list[str]] = []

        def run_command(command: list[str], **kwargs: object):
            if command[0] == "git":
                git_commands.append(command)
                return subprocess.CompletedProcess(command, 0, b"", b"")
            self.fail(f"unexpected formatter command: {command}")

        null_oid = "0" * 40
        result = check_clang_format.main(
            ["--base", null_oid, "--head", "HEAD_SHA"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=io.StringIO(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(len(git_commands), 1)
        self.assertIn("HEAD_SHA^", git_commands[0])
        self.assertNotIn(null_oid, git_commands[0])

    def test_no_source_change_skips_the_formatter(self) -> None:
        def run_command(command: list[str], **kwargs: object):
            if command[0] == "git":
                return subprocess.CompletedProcess(command, 0, b"docs/readme.md\0", b"")
            self.fail(f"unexpected formatter command: {command}")

        result = check_clang_format.main(
            ["--base", "BASE"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=io.StringIO(),
        )

        self.assertEqual(result, 0)

    def test_formatter_execution_error_is_an_infrastructure_failure(self) -> None:
        def run_command(command: list[str], **kwargs: object):
            if command[0] == "git":
                return subprocess.CompletedProcess(
                    command, 0, b"lib/example.cpp\0", b""
                )
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(
                    command, 0, "clang-format version 22.1.2\n", ""
                )
            return subprocess.CompletedProcess(command, 7, "", "tool failure\n")

        stderr = io.StringIO()
        result = check_clang_format.main(
            ["--base", "BASE"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=stderr,
        )

        self.assertEqual(result, 2)
        self.assertIn("tool failure", stderr.getvalue())
        self.assertIn("exited with status 7", stderr.getvalue())

    def test_all_mode_checks_only_tracked_first_party_sources(self) -> None:
        commands: list[list[str]] = []

        def run_command(command: list[str], **kwargs: object):
            commands.append(command)
            if command[0] == "git":
                return subprocess.CompletedProcess(
                    command, 0, b"lib/example.cpp\0docs/ignored.cpp\0", b""
                )
            if command[1:] == ["--version"]:
                return subprocess.CompletedProcess(
                    command, 0, "clang-format version 22.1.2\n", ""
                )
            return subprocess.CompletedProcess(command, 0, "", "")

        result = check_clang_format.main(
            ["--all"],
            repo_root=Path("/repo"),
            run_command=run_command,
            stderr=io.StringIO(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(commands[0][:3], ["git", "ls-files", "-z"])
        self.assertEqual(commands[-1][3:], ["lib/example.cpp"])


if __name__ == "__main__":
    unittest.main()
