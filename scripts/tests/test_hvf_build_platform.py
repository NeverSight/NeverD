"""Exercise target-platform selection without claiming an Apple SDK build."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "requires CMake")
class HVFBuildPlatformTests(unittest.TestCase):
    def configure(self, system, apple=True, enabled=True):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "dummy.c").write_text("int main(void) { return 0; }\n")
            # Discover the local C compiler first, then exercise the production
            # CMake with target metadata. No SDK, cross compiler, LLVM or actual
            # framework is needed to test whether linking/signing is selected.
            (source / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.20)
project(HvfPlatformGuard LANGUAGES C)
set(CMAKE_SYSTEM_NAME "${TEST_SYSTEM}")
set(APPLE "${TEST_APPLE}")
set(WIN32 FALSE)
set(NEVERD_ENABLE_CPU_EMULATION ON)
set(NEVERD_ENABLE_DRIVER_EMULATION OFF)
set(NEVERD_EMULATION_BACKEND_UNICORN OFF)
set(NEVERD_EMULATION_BACKEND_KVM OFF)
set(NEVERD_EMULATION_BACKEND_WHP OFF)
set(NEVERD_EMULATION_BACKEND_HVF "${TEST_ENABLED}")
set(NEVERD_HYPERVISOR_FRAMEWORK "${CMAKE_BINARY_DIR}/Hypervisor.framework")
set(NEVERD_CODESIGN "${CMAKE_COMMAND}")
function(add_neverd_component_library target)
  # Inspect link properties without generating foreign-platform archive links.
  add_library(${target} OBJECT "${CMAKE_SOURCE_DIR}/dummy.c")
endfunction()
add_subdirectory("${NEVERD_SOURCE}/lib/emulation" emulation)
include("${NEVERD_SOURCE}/cmake/NeverDHypervisor.cmake")
add_executable(OwnEngine dummy.c)
add_executable(ImportedEngine dummy.c)
neverd_sign_hypervisor(OwnEngine)
neverd_sign_hypervisor(ImportedEngine IMPORTED_ENGINE)
get_target_property(native NeverDEmulationNative COMPILE_DEFINITIONS)
get_target_property(cpu NeverDEmulationCPU COMPILE_DEFINITIONS)
get_target_property(libraries NeverDEmulationNative LINK_LIBRARIES)
get_target_property(own OwnEngine LINK_DEPENDS)
get_target_property(imported ImportedEngine LINK_DEPENDS)
file(WRITE "${CMAKE_BINARY_DIR}/selection.txt"
  "native=${native}\\ncpu=${cpu}\\nlibraries=${libraries}\\nown=${own}\\nimported=${imported}\\n")
''')
            result = subprocess.run([
                "cmake", "-S", str(source), "-B", str(source / "build"),
                "-DNEVERD_SOURCE=" + ROOT.as_posix(), "-DTEST_SYSTEM=" + system,
                "-DTEST_APPLE=" + ("ON" if apple else "OFF"),
                "-DTEST_ENABLED=" + ("ON" if enabled else "OFF"),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            return dict(line.split("=", 1) for line in
                        (source / "build/selection.txt").read_text().splitlines())

    def test_non_macos_targets_never_link_hvf_or_acquire_its_entitlement(self):
        for system, apple in (("iOS", True), ("tvOS", True), ("watchOS", True),
                               ("visionOS", True), ("Linux", False)):
            with self.subTest(system=system):
                values = self.configure(system, apple)
                self.assertNotIn("NEVERD_EMULATION_HVF", values["native"])
                self.assertNotIn("NEVERD_EMULATION_HVF", values["cpu"])
                self.assertNotIn("Hypervisor.framework", values["libraries"])
                self.assertNotIn("neverd-hypervisor.entitlements", values["own"])
                self.assertNotIn("neverd-hypervisor.entitlements", values["imported"])

    def test_macos_keeps_hvf_linkage_and_process_signing(self):
        values = self.configure("Darwin")
        self.assertIn("NEVERD_EMULATION_HVF=1", values["native"])
        self.assertIn("NEVERD_EMULATION_HVF=1", values["cpu"])
        self.assertIn("Hypervisor.framework", values["libraries"])
        self.assertIn("neverd-hypervisor.entitlements", values["own"])
        self.assertIn("neverd-hypervisor.entitlements", values["imported"])

    def test_disabled_macos_backend_does_not_disable_imported_engine_signing(self):
        values = self.configure("Darwin", enabled=False)
        self.assertNotIn("NEVERD_EMULATION_HVF", values["native"])
        self.assertNotIn("Hypervisor.framework", values["libraries"])
        self.assertNotIn("neverd-hypervisor.entitlements", values["own"])
        self.assertIn("neverd-hypervisor.entitlements", values["imported"])
