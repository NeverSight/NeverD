"""Check allocator policy without downloading dependencies or building LLVM."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class MimallocConfigureTests(unittest.TestCase):
    def configure(self, settings, platform="Linux", requested=True):
        with tempfile.TemporaryDirectory(prefix="neverd-allocator-policy-") as temp:
            source = pathlib.Path(temp)
            preference = "ON" if requested else "OFF"
            source.joinpath("CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(AllocatorPolicy NONE)\n"
                f'set(CMAKE_SYSTEM_NAME "{platform}")\n'
                f'set(WIN32 {"ON" if platform == "Windows" else "OFF"})\n'
                f'include("{ROOT.as_posix()}/cmake/NeverDMimalloc.cmake")\n'
                "if(NEVERD_MIMALLOC_ENABLED OR TARGET mimalloc-obj OR TARGET mimalloc)\n"
                '  message(FATAL_ERROR "allocator unexpectedly enabled")\n'
                "endif()\n"
                f'if(NOT NEVERD_ENABLE_MIMALLOC STREQUAL "{preference}")\n'
                '  message(FATAL_ERROR "user preference was overwritten")\n'
                "endif()\n",
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    "cmake", "-S", str(source), "-B", str(source / "build"),
                    f"-DNEVERD_ENABLE_MIMALLOC={preference}",
                    *[f"-D{name}={value}" for name, value in settings.items()],
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )

    def test_allocator_sanitizers_disable_override(self):
        cases = [
            {"LLVM_USE_SANITIZER": "Address;Undefined"},
            {"LLVM_USE_SANITIZER": "MemoryWithOrigins"},
            {"CMAKE_C_FLAGS": "-fsanitize=undefined,address"},
            {"CMAKE_CXX_FLAGS_RELEASE": "-fsanitize=thread"},
            {"CMAKE_EXE_LINKER_FLAGS": "-fsanitize=leak"},
            {"CMAKE_C_FLAGS_DEBUG": "-fsanitize=hwaddress"},
            {"CMAKE_CXX_FLAGS": "/fsanitize=address"},
            {"CMAKE_CONFIGURATION_TYPES": "Release;Checked",
             "CMAKE_CXX_FLAGS_CHECKED": "-fsanitize=memory"},
        ]
        for settings in cases:
            with self.subTest(settings=settings):
                result = self.configure(settings)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("allocator-intercepting sanitizer", result.stdout)

    def test_explicit_off_needs_no_dependency(self):
        result = self.configure({}, requested=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unsupported_platform(self):
        result = self.configure({}, platform="Generic")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("supports Linux, macOS and Windows only", result.stderr)

    def test_windows_requires_msvc_abi(self):
        result = self.configure({"MSVC": "OFF"}, platform="Windows")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires an MSVC ABI toolchain", result.stderr)

    def test_windows_rejects_static_crt(self):
        cases = [
            {"CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded"},
            {"CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDebug"},
            {"CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded$<$<CONFIG:Debug>:Debug>"},
            {"CMAKE_CXX_FLAGS": "/O2 /MT"},
            {"CMAKE_C_FLAGS_DEBUG": "/MTd"},
            {"CMAKE_CONFIGURATION_TYPES": "Release;Checked",
             "CMAKE_CXX_FLAGS_CHECKED": "-MTd"},
        ]
        for settings in cases:
            with self.subTest(settings=settings):
                result = self.configure({"MSVC": "ON", **settings}, platform="Windows")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires the dynamic CRT", result.stderr)

    def test_windows_sanitizer_and_explicit_off_need_no_dependency(self):
        for settings, requested in [
            ({"CMAKE_CXX_FLAGS": "/fsanitize=address"}, True),
            ({"CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded"}, False),
        ]:
            with self.subTest(settings=settings):
                result = self.configure(
                    {"MSVC": "ON", **settings}, platform="Windows", requested=requested
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_current_mainline_settings_are_preserved(self):
        # Mainline accepts DEFAULT strings. Never reinterpret them as BOOL ON
        # or discard a user's explicit debug/padding configuration.
        with tempfile.TemporaryDirectory(prefix="neverd-allocator-cache-") as temp:
            source = pathlib.Path(temp)
            source.joinpath("CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(AllocatorSettings NONE)\n"
                f'include("{ROOT.as_posix()}/cmake/NeverDMimalloc.cmake")\n'
                'if(NOT MI_DEBUG STREQUAL "DEFAULT" OR NOT MI_GUARDED STREQUAL "DEFAULT" OR\n'
                '   NOT MI_PADDING OR NOT MI_SECURE STREQUAL "OFF")\n'
                '  message(FATAL_ERROR "Allocator settings changed")\n'
                "endif()\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-S", temp, "-B", str(source / "build"),
                 "-DNEVERD_ENABLE_MIMALLOC=OFF", "-DMI_DEBUG:STRING=DEFAULT",
                 "-DMI_GUARDED:STRING=DEFAULT", "-DMI_SECURE:STRING=OFF",
                 "-DMI_PADDING:BOOL=ON"],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_thp_settings_are_preserved(self):
        # A local interface target exposes the value delivered to the fork;
        # this fixture neither downloads nor compiles the allocator.
        cases = [("Linux", None, "DEFAULT"), ("Darwin", None, "DEFAULT")]
        cases += [("Linux", choice, choice)
                  for choice in ("OFF", "ON", "FULL", "DEFAULT")]
        for platform, choice, expected in cases:
            with self.subTest(platform=platform, choice=choice):
                with tempfile.TemporaryDirectory(prefix="neverd-thp-policy-") as temp:
                    source = pathlib.Path(temp)
                    fork = source / "allocator"
                    fork.mkdir()
                    fork.joinpath("CMakeLists.txt").write_text(
                        'set(MI_ALLOW_THP "DEFAULT" CACHE STRING "Fixture THP policy")\n'
                        'file(WRITE "${CMAKE_BINARY_DIR}/observed-thp.txt" "${MI_ALLOW_THP}")\n'
                        "add_library(mimalloc-obj INTERFACE)\n"
                        'set(mi_libraries "")\n',
                        encoding="utf-8",
                    )
                    source.joinpath("CMakeLists.txt").write_text(
                        "cmake_minimum_required(VERSION 3.20)\n"
                        "project(AllocatorTHPPolicy NONE)\n"
                        f'set(CMAKE_SYSTEM_NAME "{platform}")\n'
                        "set(WIN32 OFF)\nset(APPLE OFF)\n"
                        f'include("{ROOT.as_posix()}/cmake/NeverDMimalloc.cmake")\n',
                        encoding="utf-8",
                    )
                    command = ["cmake", "-S", temp, "-B", str(source / "build"),
                               "-DNEVERD_ENABLE_MIMALLOC=ON",
                               f"-DFETCHCONTENT_SOURCE_DIR_NEVERD_MIMALLOC={fork}"]
                    if choice is not None:
                        command.append(f"-DMI_ALLOW_THP:STRING={choice}")
                    result = subprocess.run(command, capture_output=True, text=True,
                                            timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    observed = source / "build" / "observed-thp.txt"
                    self.assertEqual(observed.read_text(encoding="utf-8"), expected)
                    # A subsequent configure must retain the cached selection
                    # even after the explicit command-line override is removed.
                    result = subprocess.run(command[:6], capture_output=True, text=True,
                                            timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(observed.read_text(encoding="utf-8"), expected)

    def test_worker_reuses_process_policy_and_preserves_legacy_opt_out(self):
        # A pre-existing process allocator must prevent the worker wrapper
        # from fetching another version or rejecting non-Linux platforms.
        for platform in ("Linux", "Darwin", "Windows"):
            for effective, legacy in ((True, None), (True, "ON"), (True, "OFF"),
                                      (False, None), (False, "ON")):
                with self.subTest(platform=platform, effective=effective, legacy=legacy):
                    with tempfile.TemporaryDirectory(prefix="neverd-worker-policy-") as temp:
                        source = pathlib.Path(temp)
                        expected = "mimalloc" if effective and legacy != "OFF" else "system"
                        source.joinpath("worker.cpp").write_text(
                            "int main() { return 0; }\n", encoding="utf-8"
                        )
                        source.joinpath("CMakeLists.txt").write_text(
                            "cmake_minimum_required(VERSION 3.20)\n"
                            "project(WorkerAllocatorPolicy CXX)\n"
                            f'set(CMAKE_SYSTEM_NAME "{platform}")\n'
                            "set(NEVERD_ENABLE_MIMALLOC ON)\n"
                            f'set(NEVERD_MIMALLOC_ENABLED {"ON" if effective else "OFF"})\n'
                            # Model an allocator already owned by the outer project.
                            "function(neverd_link_mimalloc target)\n"
                            "  set_property(TARGET ${target} PROPERTY TEST_ALLOCATOR_LINKED ON)\n"
                            "endfunction()\n"
                            # The wrapper configures a compiled executable, including
                            # private definitions that imported targets cannot own.
                            "add_executable(worker worker.cpp)\n"
                            f'include("{ROOT.as_posix()}/cmake/NeverDWorkerAllocator.cmake")\n'
                            "neverd_worker_allocator(worker)\n"
                            "get_target_property(actual worker NEVERD_WORKER_ALLOCATOR)\n"
                            f'if(NOT actual STREQUAL "{expected}")\n'
                            '  message(FATAL_ERROR "Unexpected worker allocator: ${actual}")\n'
                            "endif()\n"
                            "get_target_property(linked worker TEST_ALLOCATOR_LINKED)\n"
                            'if((actual STREQUAL "mimalloc" AND NOT linked) OR\n'
                            '   (actual STREQUAL "system" AND linked))\n'
                            '  message(FATAL_ERROR "Worker did not follow process allocator policy")\n'
                            "endif()\n"
                            "get_target_property(definitions worker COMPILE_DEFINITIONS)\n"
                            'if((actual STREQUAL "mimalloc" AND NOT\n'
                            '    definitions STREQUAL "NEVERD_WORKER_HAS_MIMALLOC=1") OR\n'
                            '   (actual STREQUAL "system" AND definitions))\n'
                            '  message(FATAL_ERROR "Worker allocator definition disagrees with linkage")\n'
                            "endif()\n"
                            "get_target_property(public_definitions worker INTERFACE_COMPILE_DEFINITIONS)\n"
                            "if(public_definitions)\n"
                            '  message(FATAL_ERROR "Worker allocator definition must stay private")\n'
                            "endif()\n",
                            encoding="utf-8",
                        )
                        command = ["cmake", "-S", temp, "-B", str(source / "build")]
                        if legacy is not None:
                            command.append(f"-DNEVERD_WORKER_MIMALLOC={legacy}")
                        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
