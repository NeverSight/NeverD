import hashlib
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = (
    ROOT
    / "third_party"
    / "llvm-project"
    / ".github"
    / "workflows"
    / "neverd-release.yml"
)
CONSUMER = ROOT / "cmake" / "NeverDLLVMPrebuilt.cmake"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
README = ROOT / "README.md"
TESTING_DOC = ROOT / "docs" / "testing.md"


class PrebuiltLlvmReleaseWorkflowTests(unittest.TestCase):
    def test_release_matrix_contains_every_supported_package(self):
        source = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        expected_packages = {
            "neverd-llvm-macos-arm64": ("macos-14", "tar.xz"),
            "neverd-llvm-linux-x86_64": ("ubuntu-24.04", "tar.xz"),
            "neverd-llvm-windows-x64": ("windows-latest", "zip"),
        }

        self.assertEqual(source.count("            pkg: neverd-llvm-"), 3)
        for package, (runner, archive) in expected_packages.items():
            with self.subTest(package=package):
                self.assertEqual(source.count(f"            pkg: {package}\n"), 1)
                self.assertIn(f"            runner: {runner}\n", source)
                self.assertIn(f"            archive: {archive}\n", source)

    def test_release_workflow_has_explicit_existing_release_policy(self):
        source = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        required_contract = (
            "overwrite_existing_assets:",
            "jobs:\n  prepare:",
            "release_exists:",
            "immutable",
            "needs: prepare",
            "needs: [prepare, build]",
            "overwrite_files: ${{ needs.prepare.outputs.overwrite }}",
            "tag_name: ${{ needs.prepare.outputs.tag }}",
            "target_commitish: ${{ github.sha }}",
            "generate_release_notes: ${{ needs.prepare.outputs.release_exists != 'true' }}",
        )
        for expected in required_contract:
            with self.subTest(expected=expected):
                self.assertIn(expected, source)

    def test_release_workflow_declares_platform_cache_strategies(self):
        source = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("-DLLVM_CCACHE_BUILD=ON", source)
        self.assertIn("mozilla-actions/sccache-action@", source)
        self.assertIn("SCCACHE_GHA_ENABLED: 'true'", source)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=sccache", source)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", source)

    def test_release_workflow_records_and_publishes_required_metadata(self):
        source = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        for field in (
            "llvm_version:",
            "llvm_commit:",
            "platform:",
            "host_arch:",
            "runner:",
            "targets:",
            "build_type:",
            "link:",
            "built_at:",
            "built_by:",
        ):
            with self.subTest(field=field):
                self.assertIn(field, source)

        self.assertIn("include/llvm/MC/BinaryRewrite.h", source)

        for asset_glob in (
            "dist/*.tar.xz",
            "dist/*.tar.xz.sha256",
            "dist/*.zip",
            "dist/*.zip.sha256",
        ):
            with self.subTest(asset_glob=asset_glob):
                self.assertIn(asset_glob, source)

    def test_checksum_manifests_use_portable_relative_asset_names(self):
        source = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn('( cd "$GITHUB_WORKSPACE" &&', source)
        self.assertIn(
            'cmake -E sha256sum "${pkg}.${{ matrix.archive }}"', source
        )
        self.assertIn(
            '$checksum = cmake -E sha256sum "$pkg.${{ matrix.archive }}"',
            source,
        )
        self.assertNotIn('cmake -E sha256sum "$archive"', source)
        self.assertNotIn("cmake -E sha256sum $archive", source)

    def test_ci_defaults_to_source_llvm_and_manual_dispatch_can_opt_in(self):
        source = CI_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn(
            """  workflow_dispatch:
    inputs:
      use_prebuilt_llvm:
        description: 'Use the published prebuilt NeverD LLVM packages'
        required: true
        type: boolean
        default: false
""",
            source,
        )
        self.assertIn(
            "NEVERD_LLVM_PREBUILT_MODE: "
            "${{ github.event_name == 'workflow_dispatch' "
            "&& inputs.use_prebuilt_llvm && 'ON' || 'OFF' }}",
            source,
        )
        self.assertEqual(source.count("-DNEVERD_LLVM_PREBUILT="), 1)
        self.assertIn(
            '-DNEVERD_LLVM_PREBUILT="$NEVERD_LLVM_PREBUILT_MODE"',
            source,
        )
        self.assertNotIn("-DNEVERD_LLVM_PREBUILT=ON", source)
        self.assertNotIn("-DNEVERD_LLVM_PREBUILT=OFF", source)
        for matrix_name in ("Linux x64", "macOS arm64", "Windows x64"):
            with self.subTest(matrix_name=matrix_name):
                self.assertIn(f"          - name: {matrix_name}\n", source)


class PrebuiltLlvmPackageResolutionTests(unittest.TestCase):
    def test_legacy_mutable_default_is_migrated_to_the_revisioned_pin(self):
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-migration-") as tmp:
            script = Path(tmp) / "migrate.cmake"
            script.write_text(
                f"""
set(NEVERD_LLVM_PREBUILT_TAG "neverd-llvm-v23.0.0" CACHE STRING "" FORCE)
include("{CONSUMER.as_posix()}")
message(STATUS "MIGRATED_TAG=${{NEVERD_LLVM_PREBUILT_TAG}}")
""".lstrip(),
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(
            "MIGRATED_TAG=neverd-llvm-v23.0.0-r1", result.stdout
        )

    def run_buildinfo_validator(
        self, buildinfo: str | None, expected_commit: str
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-buildinfo-") as tmp:
            prefix = Path(tmp) / "package"
            prefix.mkdir()
            if buildinfo is not None:
                (prefix / "BUILDINFO.txt").write_text(buildinfo, encoding="utf-8")
            script = Path(tmp) / "validate.cmake"
            script.write_text(
                f"""
include("{CONSUMER.as_posix()}")
_neverd_validate_prebuilt_llvm_buildinfo(
  "{prefix.as_posix()}" "{expected_commit}")
""".lstrip(),
                encoding="utf-8",
            )
            return subprocess.run(
                ["cmake", "-P", str(script)],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

    def test_pinned_package_buildinfo_must_name_the_expected_commit(self):
        expected_commit = "a" * 40
        valid = self.run_buildinfo_validator(
            f"name: package\nllvm_commit: {expected_commit}\n", expected_commit
        )
        self.assertEqual(valid.returncode, 0, valid.stdout)

        wrong = self.run_buildinfo_validator(
            f"llvm_commit: {'b' * 40}\n", expected_commit
        )
        self.assertNotEqual(wrong.returncode, 0, wrong.stdout)
        self.assertIn("BUILDINFO LLVM commit mismatch", wrong.stdout)

        missing = self.run_buildinfo_validator(None, expected_commit)
        self.assertNotEqual(missing.returncode, 0, missing.stdout)
        self.assertIn("missing BUILDINFO.txt", missing.stdout)

    def run_resolver(
        self,
        system_name: str,
        processor: str,
        osx_architectures: str = "",
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-resolver-") as tmp:
            script = Path(tmp) / "resolve.cmake"
            script.write_text(
                f"""
set(CMAKE_HOST_SYSTEM_NAME "${{TEST_SYSTEM_NAME}}")
set(CMAKE_HOST_SYSTEM_PROCESSOR "${{TEST_PROCESSOR}}")
set(CMAKE_OSX_ARCHITECTURES "${{TEST_OSX_ARCHITECTURES}}")
include("{CONSUMER.as_posix()}")
_neverd_resolve_prebuilt_llvm_package(
  resolved_platform resolved_arch resolved_pkg resolved_archive)
message(STATUS
  "NEVERD_RESULT=${{resolved_platform}};${{resolved_arch}};${{resolved_pkg}};${{resolved_archive}}")
""".lstrip(),
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    "cmake",
                    f"-DTEST_SYSTEM_NAME={system_name}",
                    f"-DTEST_PROCESSOR={processor}",
                    f"-DTEST_OSX_ARCHITECTURES={osx_architectures}",
                    "-P",
                    str(script),
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

    def test_supported_hosts_resolve_to_published_packages(self):
        cases = (
            (
                "Darwin",
                "arm64",
                "",
                "macos;arm64;neverd-llvm-macos-arm64;tar.xz",
            ),
            (
                "Darwin",
                "x86_64",
                "arm64",
                "macos;arm64;neverd-llvm-macos-arm64;tar.xz",
            ),
            (
                "Linux",
                "x86_64",
                "",
                "linux;x86_64;neverd-llvm-linux-x86_64;tar.xz",
            ),
            (
                "Windows",
                "AMD64",
                "",
                "windows;x64;neverd-llvm-windows-x64;zip",
            ),
        )

        for system_name, processor, architectures, expected in cases:
            with self.subTest(
                system_name=system_name,
                processor=processor,
                architectures=architectures,
            ):
                result = self.run_resolver(system_name, processor, architectures)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"NEVERD_RESULT={expected}", result.stdout)

    def test_unsupported_hosts_fail_with_actionable_diagnostics(self):
        cases = (
            ("Darwin", "arm64", "arm64;x86_64", "universal builds"),
            ("Darwin", "x86_64", "", "only publishes arm64"),
            ("Linux", "aarch64", "", "only publishes x86_64"),
            ("Windows", "ARM64", "", "only publishes x64"),
            ("FreeBSD", "x86_64", "", "does not publish"),
        )

        for system_name, processor, architectures, diagnostic in cases:
            with self.subTest(
                system_name=system_name,
                processor=processor,
                architectures=architectures,
            ):
                result = self.run_resolver(system_name, processor, architectures)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, result.stdout)

    def create_test_package(
        self,
        release_directory: Path,
        package: str,
        archive_extension: str,
        checksum: str | None = None,
    ) -> Path:
        package_directory = release_directory / package
        llvm_config = package_directory / "lib" / "cmake" / "llvm" / "LLVMConfig.cmake"
        llvm_config.parent.mkdir(parents=True)
        llvm_config.write_text("set(LLVM_PACKAGE_VERSION 23.0.0)\n", encoding="utf-8")

        archive = release_directory / f"{package}.{archive_extension}"
        if archive_extension == "tar.xz":
            with tarfile.open(archive, "w:xz") as output:
                output.add(package_directory, arcname=package)
        elif archive_extension == "zip":
            with zipfile.ZipFile(
                archive, "w", compression=zipfile.ZIP_DEFLATED
            ) as output:
                for source in sorted(package_directory.rglob("*")):
                    if source.is_file():
                        output.write(source, source.relative_to(release_directory))
        else:
            self.fail(f"unsupported test archive extension: {archive_extension}")

        shutil.rmtree(package_directory)
        actual_checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_name(f"{archive.name}.sha256").write_text(
            f"{checksum or actual_checksum}  {archive.name}\n",
            encoding="ascii",
        )
        return archive

    def run_fetcher(
        self,
        system_name: str,
        processor: str,
        release_directory: Path,
        cache_directory: Path,
        sha256: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        # A digest known before the fetch is what puts the cache under scrutiny
        # at all; without one the module has nothing to compare a cached tree
        # against and reuses it on sight.
        pin = ""
        if sha256 is not None:
            pin = (
                f'set(NEVERD_LLVM_PREBUILT_SHA256 "{sha256}" '
                'CACHE STRING "" FORCE)\n'
            )
        script = release_directory / "fetch.cmake"
        script.write_text(
            f"""
set(CMAKE_HOST_SYSTEM_NAME "{system_name}")
set(CMAKE_HOST_SYSTEM_PROCESSOR "{processor}")
set(CMAKE_OSX_ARCHITECTURES "")
set(NEVERD_LLVM_PREBUILT_TAG "test-tag" CACHE STRING "" FORCE)
set(NEVERD_LLVM_PREBUILT_BASE_URL "{release_directory.as_uri()}" CACHE STRING "" FORCE)
set(NEVERD_LLVM_PREBUILT_CACHE_DIR "{cache_directory.as_posix()}" CACHE PATH "" FORCE)
{pin}include("{CONSUMER.as_posix()}")
neverd_fetch_prebuilt_llvm()
message(STATUS "FETCHED_LLVM_DIR=${{LLVM_DIR}}")
""".lstrip(),
            encoding="utf-8",
        )
        return subprocess.run(
            ["cmake", "-P", str(script)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )

    def test_each_archive_format_is_verified_and_extracted(self):
        cases = (
            (
                "Darwin",
                "arm64",
                "arm64",
                "neverd-llvm-macos-arm64",
                "tar.xz",
            ),
            (
                "Linux",
                "x86_64",
                "x86_64",
                "neverd-llvm-linux-x86_64",
                "tar.xz",
            ),
            (
                "Windows",
                "AMD64",
                "x64",
                "neverd-llvm-windows-x64",
                "zip",
            ),
        )

        for system_name, processor, arch, package, extension in cases:
            with self.subTest(system_name=system_name), tempfile.TemporaryDirectory(
                prefix="neverd-prebuilt-fetch-"
            ) as tmp:
                temporary_root = Path(tmp)
                release_directory = temporary_root / "release"
                cache_directory = temporary_root / "cache"
                release_directory.mkdir()
                self.create_test_package(release_directory, package, extension)

                result = self.run_fetcher(
                    system_name,
                    processor,
                    release_directory,
                    cache_directory,
                )

                self.assertEqual(result.returncode, 0, result.stdout)
                expected_config = (
                    cache_directory
                    / "test-tag"
                    / arch
                    / package
                    / "lib"
                    / "cmake"
                    / "llvm"
                    / "LLVMConfig.cmake"
                )
                self.assertTrue(expected_config.is_file(), result.stdout)
                self.assertIn("NeverD prebuilt LLVM: checksum OK", result.stdout)

    def test_checksum_mismatch_rejects_and_removes_archive(self):
        with tempfile.TemporaryDirectory(
            prefix="neverd-prebuilt-checksum-"
        ) as tmp:
            temporary_root = Path(tmp)
            release_directory = temporary_root / "release"
            cache_directory = temporary_root / "cache"
            release_directory.mkdir()
            package = "neverd-llvm-linux-x86_64"
            archive = self.create_test_package(
                release_directory,
                package,
                "tar.xz",
                checksum="0" * 64,
            )

            result = self.run_fetcher(
                "Linux", "x86_64", release_directory, cache_directory
            )

            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("SHA256 mismatch", result.stdout)
            cached_archive = cache_directory / "test-tag" / "x86_64" / archive.name
            self.assertFalse(cached_archive.exists(), result.stdout)

    # The release tag is republished in place, so the name of a cache
    # directory says which tag it came from and nothing about which build.
    # These two tests are what make the digest, rather than the tag, decide
    # whether an extracted tree is still the one being asked for.
    def test_a_cache_holding_the_expected_build_is_reused(self):
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-reuse-") as tmp:
            temporary_root = Path(tmp)
            release_directory = temporary_root / "release"
            cache_directory = temporary_root / "cache"
            release_directory.mkdir()
            archive = self.create_test_package(
                release_directory, "neverd-llvm-linux-x86_64", "tar.xz"
            )
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()

            first = self.run_fetcher(
                "Linux", "x86_64", release_directory, cache_directory, digest
            )
            self.assertEqual(first.returncode, 0, first.stdout)
            self.assertIn("downloading", first.stdout)

            second = self.run_fetcher(
                "Linux", "x86_64", release_directory, cache_directory, digest
            )
            self.assertEqual(second.returncode, 0, second.stdout)
            self.assertIn("reusing cached", second.stdout)
            self.assertNotIn("downloading", second.stdout)

    def test_a_cache_holding_a_different_build_is_refetched(self):
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-stale-") as tmp:
            temporary_root = Path(tmp)
            release_directory = temporary_root / "release"
            cache_directory = temporary_root / "cache"
            release_directory.mkdir()
            archive = self.create_test_package(
                release_directory, "neverd-llvm-linux-x86_64", "tar.xz"
            )
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()

            first = self.run_fetcher(
                "Linux", "x86_64", release_directory, cache_directory, digest
            )
            self.assertEqual(first.returncode, 0, first.stdout)

            # What a republished tag looks like on disk: an extracted tree that
            # came from bytes nobody is asking for any more.
            stamp = (
                cache_directory
                / "test-tag"
                / "x86_64"
                / f"{archive.name}.stamp"
            )
            self.assertTrue(stamp.is_file(), first.stdout)
            stamp.write_text(f"{'a' * 64}\n", encoding="ascii")

            second = self.run_fetcher(
                "Linux", "x86_64", release_directory, cache_directory, digest
            )
            self.assertEqual(second.returncode, 0, second.stdout)
            self.assertIn("refetching", second.stdout)
            self.assertIn("downloading", second.stdout)
            self.assertEqual(stamp.read_text(encoding="ascii").strip(), digest)

    def test_cache_default_uses_userprofile_when_home_is_unset(self):
        with tempfile.TemporaryDirectory(
            prefix="neverd-prebuilt-home-"
        ) as tmp:
            temporary_root = Path(tmp)
            user_profile = temporary_root / "windows-user"
            script = temporary_root / "cache-default.cmake"
            script.write_text(
                f"""
include("{CONSUMER.as_posix()}")
message(STATUS "DEFAULT_CACHE=${{NEVERD_LLVM_PREBUILT_CACHE_DIR}}")
""".lstrip(),
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment.pop("HOME", None)
            environment["USERPROFILE"] = str(user_profile)

            result = subprocess.run(
                ["cmake", "-P", str(script)],
                cwd=ROOT,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(
                f"DEFAULT_CACHE={user_profile.as_posix()}/.cache/neverd-llvm",
                result.stdout,
            )


class PrebuiltLlvmPinTests(unittest.TestCase):
    PACKAGES = {
        "neverd-llvm-linux-x86_64": "_NEVERD_LLVM_PIN_LINUX_X86_64",
        "neverd-llvm-macos-arm64": "_NEVERD_LLVM_PIN_MACOS_ARM64",
        "neverd-llvm-windows-x64": "_NEVERD_LLVM_PIN_WINDOWS_X64",
    }

    def test_every_published_package_has_a_well_formed_pin(self):
        source = CONSUMER.read_text(encoding="utf-8")

        for package, variable in self.PACKAGES.items():
            with self.subTest(package=package):
                match = re.search(
                    rf'set\({variable}\s+"([0-9a-f]{{64}})"\)', source
                )
                self.assertIsNotNone(
                    match,
                    f"{variable} must hold a lowercase 64-character SHA-256",
                )

    def resolve_pin(self, tag: str, package: str) -> str:
        with tempfile.TemporaryDirectory(prefix="neverd-prebuilt-pin-") as tmp:
            script = Path(tmp) / "pin.cmake"
            script.write_text(
                f"""
set(NEVERD_LLVM_PREBUILT_TAG "{tag}" CACHE STRING "" FORCE)
include("{CONSUMER.as_posix()}")
_neverd_pinned_llvm_sha256(resolved "{package}")
message(STATUS "PIN=[${{resolved}}]")
""".lstrip(),
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            match = re.search(r"PIN=\[([0-9a-f]*)\]", result.stdout)
            self.assertIsNotNone(match, result.stdout)
            return match.group(1)

    def test_the_pinned_tag_resolves_every_package(self):
        source = CONSUMER.read_text(encoding="utf-8")
        tag = re.search(
            r'set\(NEVERD_LLVM_PREBUILT_PINNED_TAG "([^"]+)"\)', source
        )
        self.assertIsNotNone(tag, source)

        for package in self.PACKAGES:
            with self.subTest(package=package):
                self.assertRegex(
                    self.resolve_pin(tag.group(1), package), r"^[0-9a-f]{64}$"
                )

    def test_default_tag_and_pinned_commit_match_the_llvm_gitlink(self):
        source = CONSUMER.read_text(encoding="utf-8")
        default_tag = re.search(
            r'set\(NEVERD_LLVM_PREBUILT_TAG "([^"]+)"', source
        )
        pinned_tag = re.search(
            r'set\(NEVERD_LLVM_PREBUILT_PINNED_TAG "([^"]+)"\)', source
        )
        pinned_commit = re.search(
            r'set\(NEVERD_LLVM_PREBUILT_PINNED_COMMIT\s+"([0-9a-f]{40})"\)',
            source,
        )
        self.assertIsNotNone(default_tag, source)
        self.assertIsNotNone(pinned_tag, source)
        self.assertIsNotNone(pinned_commit, source)
        self.assertEqual(default_tag.group(1), pinned_tag.group(1))

        gitlink = subprocess.run(
            ["git", "ls-tree", "HEAD", "third_party/llvm-project"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        self.assertEqual(gitlink.returncode, 0, gitlink.stdout)
        fields = gitlink.stdout.split()
        self.assertGreaterEqual(len(fields), 3, gitlink.stdout)
        self.assertEqual(fields[1], "commit", gitlink.stdout)
        self.assertEqual(pinned_commit.group(1), fields[2])

    # A caller who overrode the tag is asking for a build this file makes no
    # claim about, so the pins must not be applied to it: doing so would reject
    # a perfectly good archive for failing to match a digest describing an
    # entirely different release.
    def test_another_tag_resolves_no_pin(self):
        for package in self.PACKAGES:
            with self.subTest(package=package):
                self.assertEqual(self.resolve_pin("some-other-tag", package), "")


class PrebuiltLlvmDocumentationTests(unittest.TestCase):
    def test_sanitizer_profile_includes_the_prebuilt_integration_target(self):
        source = TESTING_DOC.read_text(encoding="utf-8")

        self.assertIn(
            "NeverDSBFSolanaModelTests NeverDSBFIntegrationTests", source
        )
        self.assertNotIn("-E 'SBFIntegration'", source)
        self.assertNotIn("prebuilt package also omits", source)

    def test_readme_documents_hosts_rebuilds_and_caches(self):
        source = README.read_text(encoding="utf-8")

        for expected in (
            "macOS arm64",
            "Linux x86_64",
            "Windows x64",
            "neverd-llvm-v23.0.0-r1",
            "neverd-llvm-v23.0.0-r2",
            "cmake/NeverDLLVMPrebuilt.cmake",
            "scripts/audit_prebuilt_llvm_release.py",
            "every six hours",
            "sccache",
            "GitHub Actions cache",
            ".cache/neverd-llvm/neverd-llvm-v23.0.0-r1",
            "use_prebuilt_llvm",
            "push and pull-request CI",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, source)

        self.assertRegex(source, r"only a manually\s+selected `true`")
        self.assertIn(
            "-DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r1", source
        )
        self.assertNotIn(
            "-DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0\n", source
        )


if __name__ == "__main__":
    unittest.main()
