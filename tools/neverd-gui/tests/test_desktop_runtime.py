"""Exercise deployment with real ELF files and cross-compiled PE import tables."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


COLLECTOR = Path(__file__).resolve().parents[3] / "cmake/NeverDDesktopRuntime.cmake"
LLVM_OBJDUMP = os.environ.get("LLVM_OBJDUMP") or shutil.which("llvm-objdump")


@unittest.skipUnless(sys.platform.startswith("linux") and all(
    shutil.which(tool) for tool in ("cc", "cmake", "patchelf")), "Linux deployment tools required")
class DesktopRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-deployment-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.prefix = self.root / "package"
        self.external = self.root / "external"
        self.sql = self.prefix / "plugins/sqldrivers"
        for directory in (self.prefix / "bin", self.prefix / "lib", self.external, self.sql):
            directory.mkdir(parents=True, exist_ok=True)
        for name in ("neverd-gui", "neverd-worker", "neverd-mcp"):
            shutil.copy2(shutil.which("true"), self.prefix / "bin" / name)
        self.compile("int desktopDependency(void) { return 42; }",
                     self.external / "libdesktopdependency.so")
        self.compile("extern int desktopDependency(void); "
                     "int desktopPlugin(void) { return desktopDependency(); }",
                     self.sql / "libqsqlite.so", "-L" + str(self.external),
                     "-ldesktopdependency", "-Wl,-rpath," + str(self.external))
        self.optional = self.sql / "libqsqlpsql.so"
        self.optional.write_text("An unused SQL connector is removed before dependency discovery.")
        self.original = (self.external / "libdesktopdependency.so").read_bytes()

    def compile(self, source, output, *arguments):
        subprocess.run(["cc", "-shared", "-fPIC", "-x", "c", "-", "-o", str(output),
                        *arguments], input=source, text=True, check=True, capture_output=True)

    def collect(self, runtime_library=None, search_directories=True, source_file=None):
        script = self.root / "deploy.cmake"
        script.write_text(f'''cmake_minimum_required(VERSION 3.24)
set(QT_DEPLOY_PREFIX "{self.prefix}")
set(QT_DEPLOY_LIB_DIR lib)
set(QT_DEPLOY_PLUGINS_DIR plugins)
set(NEVERD_DEPLOY_SYSTEM Linux)
set(NEVERD_DEPLOY_CONFIG Release)
set(NEVERD_DEPLOY_SEARCH_DIRS "{self.external if search_directories else ''}")
set(NEVERD_DEPLOY_RUNTIME_LIBRARIES "{runtime_library or ''}")
set(NEVERD_DEPLOY_SOURCE_FILES "{source_file or ''}")
include("{COLLECTOR}")
''')
        environment = {key: value for key, value in os.environ.items()
                       if key != "LD_LIBRARY_PATH"}
        return subprocess.run(["cmake", "-P", str(script)], env=environment,
                              text=True, capture_output=True, timeout=30)

    def test_transitive_dependency_survives_relocation(self):
        result = self.collect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.optional.exists())
        self.assertEqual((self.external / "libdesktopdependency.so").read_bytes(), self.original)
        moved = self.root / "relocated"
        self.prefix.rename(moved)
        shutil.rmtree(self.external)
        plugin = moved / "plugins/sqldrivers/libqsqlite.so"
        subprocess.run([sys.executable, "-c",
                        "import ctypes, sys; assert ctypes.CDLL(sys.argv[1]).desktopPlugin() == 42",
                        str(plugin)], check=True)
        audit = (moved / "share/neverd/dependency-audit.txt").read_text()
        digest = hashlib.sha256(plugin.read_bytes()).hexdigest()
        self.assertIn(digest + "  plugins/sqldrivers/libqsqlite.so", audit)

    def test_missing_sqlite_preserves_optional_drivers(self):
        (self.sql / "libqsqlite.so").unlink()
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.exists())

    def test_original_runpath_is_used_before_relocation(self):
        result = self.collect(search_directories=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.prefix / "lib/libdesktopdependency.so").is_file())

    def test_source_origin_is_used_before_relocation(self):
        original = self.external / "bin/neverd-gui"
        original.parent.mkdir()
        subprocess.run(["cc", "-x", "c", "-", "-o", str(original),
                        "-L" + str(self.external), "-ldesktopdependency", "-Wl,-rpath,$ORIGIN/.."],
                       input="extern int desktopDependency(void); "
                             "int main(void) { return desktopDependency() != 42; }",
                       text=True, check=True, capture_output=True)
        shutil.copy2(original, self.prefix / "bin/neverd-gui")
        self.compile("int desktopPlugin(void) { return 0; }", self.sql / "libqsqlite.so")
        result = self.collect(search_directories=False, source_file=original)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        moved = self.root / "relocated"
        self.prefix.rename(moved)
        shutil.rmtree(self.external)
        subprocess.run([str(moved / "bin/neverd-gui")], check=True)

    def test_sql_directory_symlink_cannot_remove_external_files(self):
        external_sql = self.external / "sqldrivers"
        self.sql.rename(external_sql)
        self.sql.symlink_to(external_sql, target_is_directory=True)
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((external_sql / self.optional.name).is_file(), result.stderr)

    def test_hardlinked_library_does_not_modify_build_input(self):
        library = self.external / "libdesktopdependency.so"
        os.link(library, self.prefix / "lib" / library.name)
        result = self.collect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(library.read_bytes(), self.original,
                         "Relocating the installation must preserve its build inputs")

    def test_runtime_copy_does_not_modify_hardlinked_build_input(self):
        original = self.external / "libdesktopoptional.so"
        self.compile("int optionalFeature(void) { return 7; }", original)
        content = original.read_bytes()
        os.link(original, self.prefix / "lib" / original.name)
        replacement = self.external / "replacement" / original.name
        replacement.parent.mkdir()
        self.compile("int optionalFeature(void) { return 13; }", replacement)
        result = self.collect(replacement)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(original.read_bytes(), content)
        subprocess.run([sys.executable, "-c",
                        "import ctypes, sys; assert ctypes.CDLL(sys.argv[1]).optionalFeature() == 13",
                        str(self.prefix / "lib" / original.name)], check=True)

    def test_audit_does_not_modify_hardlinked_external_file(self):
        original = self.external / "original-audit.txt"
        original.write_text("Original build audit must survive packaging.\n")
        destination = self.prefix / "share/neverd/dependency-audit.txt"
        destination.parent.mkdir(parents=True)
        os.link(original, destination)
        result = self.collect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(original.read_text(), "Original build audit must survive packaging.\n")

    def test_audit_parent_symlink_cannot_write_external_files(self):
        (self.prefix / "share").symlink_to(self.external, target_is_directory=True)
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.external / "neverd").exists())
        self.assertTrue(self.optional.exists())

    def test_explicit_runtime_library_is_copied_and_audited(self):
        self.assertEqual(self.collect().returncode, 0)
        runtime = self.external / "libdesktopoptional.so"
        self.compile("extern int desktopDependency(void); "
                     "int optionalFeature(void) { return desktopDependency(); }",
                     runtime, "-L" + str(self.external), "-ldesktopdependency",
                     "-Wl,-rpath," + str(self.external))
        result = self.collect(runtime)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.prefix / "lib" / runtime.name).is_file())
        audit = (self.prefix / "share/neverd/dependency-audit.txt").read_text()
        self.assertIn("lib/libdesktopoptional.so", audit)

    def test_unknown_driver_preserves_optional_drivers(self):
        (self.sql / "unknown.so").write_text("unknown")
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.exists())

    def test_sql_symlink_is_rejected_before_removal(self):
        self.optional.unlink()
        self.optional.symlink_to(self.external / "libdesktopdependency.so")
        self.assertNotEqual(self.collect().returncode, 0)
        self.assertTrue(self.optional.is_symlink())

    def test_escaping_library_is_never_patched(self):
        (self.prefix / "lib/libescape.so").symlink_to(self.external / "libdesktopdependency.so")
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the package", result.stderr)
        self.assertEqual((self.external / "libdesktopdependency.so").read_bytes(), self.original)

    def test_unresolved_dependency_stops_packaging(self):
        (self.external / "libdesktopdependency.so").unlink()
        result = self.collect()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libdesktopdependency.so", result.stderr)


@unittest.skipUnless(sys.platform.startswith("linux") and LLVM_OBJDUMP and all(
    shutil.which(tool) for tool in ("clang", "lld-link", "cmake")),
    "Clang, lld-link and LLVM_OBJDUMP required for static PE deployment checks")
class WindowsDesktopRuntimeTests(unittest.TestCase):
    def check_dependencies(self, target, configuration):
        with tempfile.TemporaryDirectory(prefix="neverd-pe-deployment-") as temporary:
            root = Path(temporary)
            prefix = root / "package"
            vendor = root / "vendor/Windows/libraries"
            system = root / "Operating System.(test)/System32"
            sql = prefix / "plugins/sqldrivers"
            for directory in (prefix / "bin", vendor, system, sql):
                directory.mkdir(parents=True)

            def compile(source, output, *imports):
                obj = output.with_suffix(".obj")
                subprocess.run(["clang", "-target", target, "-x", "c", "-", "-c", "-o", str(obj)],
                               input=source, text=True, check=True, capture_output=True)
                flags = ["/dll", "/noentry"] if output.suffix == ".dll" else ["/entry:main", "/subsystem:console"]
                subprocess.run(["lld-link", "/out:" + str(output), *flags, str(obj),
                                *map(str, imports)], check=True, capture_output=True)
                obj.unlink()
                return output.with_suffix(".lib")

            dependency = compile("__declspec(dllexport) int vendorFunction(void) { return 42; }",
                                 vendor / "desktopvendor.dll")
            os_dependency = compile("__declspec(dllexport) int systemFunction(void) { return 7; }",
                                    system / "systemfixture.dll")
            suffix = "d" if configuration == "Debug" else ""
            compile("__declspec(dllimport) int vendorFunction(void); "
                    "__declspec(dllimport) int systemFunction(void); "
                    "__declspec(dllexport) int desktopPlugin(void) { "
                    "return vendorFunction() + systemFunction(); }",
                    sql / f"qsqlite{suffix}.dll", dependency, os_dependency)
            for auxiliary in sql.glob("*.lib"):
                auxiliary.unlink()
            (sql / f"qsqlpsql{suffix}.dll").write_text("Unused optional connector")
            executable = prefix / "bin/neverd-gui.exe"
            compile("int main(void) { return 0; }", executable)
            for name in ("neverd-worker.exe", "neverd-mcp.exe"):
                shutil.copy2(executable, prefix / "bin" / name)
            script = root / "deploy.cmake"
            script.write_text(f'''cmake_minimum_required(VERSION 3.24)
set(QT_DEPLOY_PREFIX "{prefix}")
set(QT_DEPLOY_PLUGINS_DIR plugins)
set(NEVERD_DEPLOY_SYSTEM Windows)
set(NEVERD_DEPLOY_CONFIG {configuration})
set(NEVERD_DEPLOY_EXECUTABLE_SUFFIX .exe)
set(NEVERD_DEPLOY_SEARCH_DIRS "{vendor}" "{system}")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "{LLVM_OBJDUMP}")
include("{COLLECTOR}")
''')
            result = subprocess.run(["cmake", "-P", str(script)],
                                    env={**os.environ, "SystemRoot": str(system.parent)},
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((prefix / "bin/desktopvendor.dll").is_file(),
                            "A vendor directory named Windows must not be treated as the OS")
            self.assertFalse((prefix / "bin/systemfixture.dll").exists())
            self.assertFalse((sql / f"qsqlpsql{suffix}.dll").exists())
            audit = (prefix / "share/neverd/dependency-audit.txt").read_text()
            self.assertIn("bin/desktopvendor.dll", audit)
            self.assertNotIn("systemfixture.dll", audit)

    def test_windows_import_tables(self):
        for target in ("i686-pc-windows-msvc", "x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc"):
            for configuration in ("Release", "Debug"):
                with self.subTest(target=target, configuration=configuration):
                    self.check_dependencies(target, configuration)


if __name__ == "__main__":
    unittest.main()
