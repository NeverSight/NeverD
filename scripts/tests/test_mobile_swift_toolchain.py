import tempfile
import unittest
from pathlib import Path

from scripts.qualify_mobile_swift_toolchain import (
    SIGNER, bundle_identifier, installed_bundle, installed_tool, trusted_package_signature,
)


class SwiftToolchainIdentityTests(unittest.TestCase):
    def signature(self, leaf=SIGNER, status="signed by a certificate trusted by macOS"):
        return (f'Package "snapshot.pkg":\n   Status: {status}\n'
                '   Signed with a trusted timestamp on: 2026-09-04\n'
                f'   Certificate Chain:\n    1. {leaf}\n'
                '       SHA256 Fingerprint:\n           AA BB CC\n'
                '    2. Developer ID Certification Authority\n'
                '    3. Apple Root CA\n')

    def test_trusted_leaf_is_read_from_the_certificate_chain(self):
        for status in ("signed by a certificate trusted by macOS",
                       "signed by a developer certificate issued by Apple for distribution"):
            with self.subTest(status=status):
                identity = trusted_package_signature(self.signature(status=status))
                self.assertEqual(identity["leaf"], SIGNER)
                self.assertEqual(identity["status"], status)
                self.assertEqual(len(identity["certificate_chain"]), 3)

    def test_untrusted_or_ambiguous_status_cannot_pass(self):
        for text in (self.signature(status="no signature"),
                     self.signature(status="signed by an untrusted certificate"),
                     self.signature(status="revoked signature"),
                     self.signature(status="expired signature"),
                     self.signature(status="signed by a certificate trusted by macOS but expired"),
                     self.signature() + "Status: no signature\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                trusted_package_signature(text)

    def test_matching_signer_elsewhere_does_not_replace_the_leaf(self):
        for text in (SIGNER + "\n" + self.signature(leaf="Unrelated Installer"),
                     self.signature(leaf="Unrelated Installer") + f"    4. {SIGNER}\n",
                     self.signature(leaf=SIGNER + " extra"),
                     self.signature().replace("Certificate Chain:", "Unknown Chain:"),
                     self.signature() + "Certificate Chain:\n",
                     self.signature().replace("    2.", "    3.")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                trusted_package_signature(text)

    def test_bundle_identifier_must_be_exact_and_not_a_toolchain_alias(self):
        self.assertEqual(bundle_identifier({"CFBundleIdentifier": "org.swift.example.snapshot"}),
                         "org.swift.example.snapshot")
        for value in (None, "", "swift", "XcodeDefault", "org.swift.",
                      "org.swift.example\n", "org.swift.example, XcodeDefault", "--other"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bundle_identifier({"CFBundleIdentifier": value})

    def test_xcrun_path_and_symlink_target_must_belong_to_the_new_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / "snapshot.xctoolchain"
            bundle.mkdir()
            executable = bundle / "swift-frontend"
            executable.write_bytes(b"fixture")
            alias = bundle / "swiftc"
            alias.symlink_to(executable.name)
            self.assertEqual(installed_tool(bundle, str(alias) + "\n"), executable.resolve())
            outside = root / "swift-other"
            outside.write_bytes(b"other")
            escape = bundle / "escaping"
            escape.symlink_to(outside)
            for path in (str(outside), str(escape), str(bundle), "swiftc"):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    installed_tool(bundle, path)

    def test_latest_alias_selects_only_the_real_new_snapshot_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / "snapshot.xctoolchain"
            bundle.mkdir()
            selected, aliases = installed_bundle(root, set(), {bundle.name})
            self.assertEqual(selected, bundle.resolve())
            self.assertEqual(aliases, [])
            latest = root / "swift-latest.xctoolchain"
            latest.symlink_to(bundle.name)
            selected, aliases = installed_bundle(root, set(), {bundle.name, latest.name})
            self.assertEqual(selected, bundle.resolve())
            self.assertEqual(aliases, [{"path": str(latest), "link_target": bundle.name,
                                        "resolved_target": str(bundle.resolve())}])

    def test_multiple_bundles_or_unrelated_aliases_do_not_pick_an_arbitrary_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first, second = root / "first.xctoolchain", root / "second.xctoolchain"
            first.mkdir()
            second.mkdir()
            latest = root / "swift-latest.xctoolchain"
            latest.symlink_to(second.name)
            cases = ((set(), {first.name, second.name, latest.name}),
                     ({second.name}, {first.name, second.name, latest.name}),
                     ({first.name}, {second.name}),
                     (set(), {latest.name}))
            for before, after in cases:
                with self.subTest(before=before, after=after), self.assertRaises(ValueError):
                    installed_bundle(root, before, after)


if __name__ == "__main__":
    unittest.main()
