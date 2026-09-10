"""Pure-file tests; importing the helper does not run packaging commands."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("package_macos", Path(__file__).parents[1] / "package_macos.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
OPTIONAL = ("libqsqlmimer.dylib", "libqsqlodbc.dylib", "libqsqlpsql.dylib")


class SqlDriverDeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def bundle(self, name="case"):
        bundle = self.root / name / "NeverD.app"
        drivers = bundle / "Contents/PlugIns/sqldrivers"
        drivers.mkdir(parents=True)
        for name in ("libqsqlite.dylib", *OPTIONAL):
            (drivers / name).write_text(name)
        return bundle, drivers

    def assert_optional_unchanged(self, drivers):
        for name in OPTIONAL:
            self.assertEqual((drivers / name).read_text(), name)

    def test_keeps_sqlite_and_localstorage_removes_three_optional_drivers(self):
        bundle, drivers = self.bundle()
        retained = ("Contents/Frameworks/QtSql.framework/keep",
                    "Contents/Frameworks/QtQmlLocalStorage.framework/keep",
                    "Contents/Resources/qml/QtQuick/LocalStorage/keep")
        for relative in retained:
            path = bundle / relative
            path.parent.mkdir(parents=True)
            path.write_text(relative)
        PACKAGE.validate_and_prune_sql_drivers(bundle)
        self.assertEqual({p.name for p in drivers.iterdir()}, {"libqsqlite.dylib"})
        self.assertEqual((drivers / "libqsqlite.dylib").read_text(), "libqsqlite.dylib")
        for relative in retained:
            self.assertEqual((bundle / relative).read_text(), relative)


    def test_sqlite_alone_is_supported(self):
        bundle, drivers = self.bundle()
        for name in OPTIONAL:
            (drivers / name).unlink()
        PACKAGE.validate_and_prune_sql_drivers(bundle)
        self.assertEqual((drivers / "libqsqlite.dylib").read_text(), "libqsqlite.dylib")

    def test_missing_sqlite_fails_before_removing_optional_drivers(self):
        bundle, drivers = self.bundle()
        (drivers / "libqsqlite.dylib").unlink()
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_and_prune_sql_drivers(bundle)
        self.assert_optional_unchanged(drivers)

    def test_invalid_entries_fail_before_removing_optional_drivers(self):
        kinds = ("unknown-file", "unknown-directory", "unknown-symlink",
                 "sqlite-directory", "sqlite-symlink", "sqlite-dangling-symlink",
                 "optional-symlink")
        for kind in kinds:
            with self.subTest(kind=kind):
                bundle, drivers = self.bundle(kind)
                bad = drivers / ("libqsqlite.dylib" if kind.startswith("sqlite-") else
                                 OPTIONAL[0] if kind == "optional-symlink" else "zz-unknown")
                if bad.exists():
                    bad.unlink()
                if kind.endswith("directory"):
                    bad.mkdir()
                elif kind.endswith("symlink"):
                    target = self.root / (kind + "-outside")
                    if "dangling" not in kind:
                        target.write_text(bad.name)
                    bad.symlink_to(target)
                else:
                    bad.write_text("unknown")
                with self.assertRaises(RuntimeError):
                    PACKAGE.validate_and_prune_sql_drivers(bundle)
                self.assert_optional_unchanged(drivers)
                self.assertTrue(bad.exists() or bad.is_symlink())

    def test_symlink_ancestors_fail_without_touching_external_driver_files(self):
        for level in range(5):
            with self.subTest(level=level):
                bundle, drivers = self.bundle("link-" + str(level))
                ancestor = (bundle.parent, bundle, bundle / "Contents",
                            bundle / "Contents/PlugIns", drivers)[level]
                relative = drivers.relative_to(ancestor)
                outside = self.root / ("outside-link-" + str(level))
                ancestor.rename(outside)
                ancestor.symlink_to(outside, target_is_directory=True)
                with self.assertRaises(RuntimeError):
                    PACKAGE.validate_and_prune_sql_drivers(bundle)
                self.assert_optional_unchanged(outside / relative)
                self.assertTrue(ancestor.is_symlink())

    def test_nondirectory_ancestors_fail_without_removing_optional_drivers(self):
        for level in range(5):
            with self.subTest(level=level):
                bundle, drivers = self.bundle("file-" + str(level))
                ancestor = (bundle.parent, bundle, bundle / "Contents",
                            bundle / "Contents/PlugIns", drivers)[level]
                relative = drivers.relative_to(ancestor)
                outside = self.root / ("outside-file-" + str(level))
                ancestor.rename(outside)
                ancestor.write_text("not a directory")
                with self.assertRaises(RuntimeError):
                    PACKAGE.validate_and_prune_sql_drivers(bundle)
                self.assert_optional_unchanged(outside / relative)
                self.assertEqual(ancestor.read_text(), "not a directory")

    def test_absent_sql_directory_is_allowed_without_deployed_sql_consumers(self):
        for relative in ("", "Contents/PlugIns"):
            with self.subTest(existing=relative):
                bundle = self.root / ("no-sql-" + str(len(relative))) / "NeverD.app"
                (bundle / relative).mkdir(parents=True)
                before = sorted(str(p.relative_to(bundle)) for p in bundle.rglob("*"))
                PACKAGE.validate_and_prune_sql_drivers(bundle)
                self.assertEqual(sorted(str(p.relative_to(bundle)) for p in bundle.rglob("*")), before)

    def test_absent_sql_directory_is_rejected_for_each_deployed_sql_consumer(self):
        consumers = ("Contents/Frameworks/QtSql.framework",
                     "Contents/Frameworks/QtQmlLocalStorage.framework",
                     "Contents/Resources/qml/QtQuick/LocalStorage")
        for index, relative in enumerate(consumers):
            with self.subTest(consumer=relative):
                bundle = self.root / ("consumer-" + str(index)) / "NeverD.app"
                marker = bundle / relative
                marker.mkdir(parents=True)
                with self.assertRaises(RuntimeError):
                    PACKAGE.validate_and_prune_sql_drivers(bundle)
                self.assertTrue(marker.is_dir())
                self.assertFalse((bundle / "Contents/PlugIns/sqldrivers").exists())


if __name__ == "__main__":
    unittest.main()
