from contextlib import contextmanager, ExitStack
from copy import deepcopy
import hashlib
import plistlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import qualify_mobile_swift_toolchain as toolchain

from scripts.qualify_mobile_swift_toolchain import (
    SIGNER, bundle_identifier, installed_bundle, installed_tool, trusted_package_signature,
)


@contextmanager
def installed_identity_fixture(root, module=toolchain, case_id="icecubes-release-arm64-simulator-swift64"):
    """Real small files exercise confinement and byte checks, never Apple tools."""
    home = root / "home"
    bundle = home / "Library/Developer/Toolchains" / (module.SNAPSHOT + ".xctoolchain")
    executable_root = bundle / "usr/bin"
    executable_root.mkdir(parents=True)
    info = plistlib.dumps({"CFBundleIdentifier": module.BUNDLE_IDENTIFIER})
    (bundle / "Info.plist").write_bytes(info)
    records, hashes = [], {}
    for name in ("swiftc", "swift-frontend", "swift-demangle"):
        resolved = executable_root / ("swift-driver" if name == "swiftc" else name)
        resolved.write_bytes(("self-owned tool identity fixture: " + name).encode())
        reported = executable_root / name
        if name == "swiftc":
            reported.symlink_to(resolved.name)
        hashes[name] = hashlib.sha256(resolved.read_bytes()).hexdigest()
        records.append({"name": name, "reported_path": str(reported),
                        "resolved_path": str(resolved.resolve()), "sha256": hashes[name]})
    developer = root / "Xcode_26.5.app/Contents/Developer"
    sdks, sdk_hashes = [], {}
    for name in ("iphoneos", "iphonesimulator"):
        sdk = developer / "Platforms" / name / "SDKs/Fixture26.5.sdk"
        sdk.mkdir(parents=True)
        settings = ("self-owned SDK identity fixture: " + name).encode()
        (sdk / "SDKSettings.json").write_bytes(settings)
        sdk_hashes[name] = hashlib.sha256(settings).hexdigest()
        sdks.append({"name": name, "path": str(sdk), "version": "26.5",
                     "settings_sha256": sdk_hashes[name]})
    info_hash = hashlib.sha256(info).hexdigest()
    receipt = {
        "schema_version": 1, "scope": "swift-toolchain-installation-identity", "status": "success",
        "installation_pin_evidence_run_id": module.QUALIFICATION_RUN_ID,
        "application_build_verified": False, "consumer_commit": "c" * 40, "workflow_commit": "d" * 40,
        "case_id": case_id, "run_id": "123", "run_attempt": "2", "runner_os": "macOS", "runner_arch": "ARM64",
        "package": {"snapshot": module.SNAPSHOT, "url": module.PACKAGE_URL,
                    "expected_sha256": module.PACKAGE_SHA256, "observed_sha256": module.PACKAGE_SHA256,
                    "source_reference": module.SOURCE_REFERENCE, "gatekeeper": "accepted",
                    "identity_status": "sha256-and-system-signature-verified",
                    "signature": {"status": "signed by a certificate trusted by macOS", "leaf": module.SIGNER,
                                  "certificate_chain": [module.SIGNER, "Developer ID Certification Authority", "Apple Root CA"]}},
        "toolchain": {"path": str(bundle), "bundle_identifier": module.BUNDLE_IDENTIFIER,
                      "info_plist_sha256": info_hash},
        "xcode": {"developer_dir": str(developer), "version": "Xcode 26.5\nBuild version TEST"},
        "tools": records, "sdks": sdks,
    }
    with ExitStack() as stack:
        stack.enter_context(patch.object(module.Path, "home", return_value=home))
        for name, value in {"DEVELOPER_DIR": str(developer), "INFO_PLIST_SHA256": info_hash,
                            "TOOL_SHA256": hashes, "SDK_SETTINGS_SHA256": sdk_hashes}.items():
            stack.enter_context(patch.object(module, name, value))
        yield receipt


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

    def test_workflow_run_never_uses_workflow_sha_as_the_consumer(self):
        environment = {"GITHUB_EVENT_NAME": "workflow_run", "GITHUB_SHA": "a" * 40}
        with self.assertRaisesRegex(ValueError, "explicit exact consumer"):
            toolchain.consumer_identity(None, environment)
        self.assertEqual(toolchain.consumer_identity("b" * 40, environment), "b" * 40)
        environment["GITHUB_EVENT_NAME"] = "workflow_dispatch"
        self.assertEqual(toolchain.consumer_identity(None, environment), "a" * 40)
        for value in ("main", "HEAD", "a" * 39, "a" * 40 + "\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                toolchain.consumer_identity(value, environment)

    def test_current_job_receipt_reconciles_real_files_and_keeps_distinct_workflow_identity(self):
        with tempfile.TemporaryDirectory() as tmp, installed_identity_fixture(Path(tmp)) as receipt:
            verified = toolchain.verify_installation(receipt, "c" * 40, "123", "2", case_id=receipt["case_id"],
                                                     workflow_commit="d" * 40)
            self.assertIs(verified, receipt)
            self.assertNotEqual(verified["consumer_commit"], verified["workflow_commit"])
            self.assertFalse(verified["application_build_verified"])

    def test_receipts_from_other_cases_runs_consumers_or_unqualified_identities_fail(self):
        with tempfile.TemporaryDirectory() as tmp, installed_identity_fixture(Path(tmp)) as receipt:
            mutations = [
                ("status", "failed"), ("consumer_commit", "e" * 40), ("workflow_commit", "main"),
                ("workflow_commit", "e" * 40), ("workflow_commit", "c" * 40),
                ("run_id", "124"), ("run_attempt", "1"), ("case_id", "different-case"),
                ("runner_arch", "X64"), ("runner_os", "Linux"),
            ]
            for key, value in mutations:
                changed = deepcopy(receipt)
                changed[key] = value
                with self.subTest(field=key), self.assertRaises(ValueError):
                    toolchain.verify_installation(changed, "c" * 40, "123", "2", case_id=receipt["case_id"],
                                                 workflow_commit="d" * 40)
            for mutation in ("package-sha", "signature", "gatekeeper", "bundle-id", "tool-sha",
                             "duplicate-tool", "sdk-sha", "sdk-version", "missing-sdk"):
                changed = deepcopy(receipt)
                if mutation == "package-sha":
                    changed["package"]["observed_sha256"] = "0" * 64
                elif mutation == "signature":
                    changed["package"]["signature"]["certificate_chain"][0] = "Unrelated Installer"
                elif mutation == "gatekeeper":
                    changed["package"]["gatekeeper"] = "rejected"
                elif mutation == "bundle-id":
                    changed["toolchain"]["bundle_identifier"] = "swift-latest"
                elif mutation == "tool-sha":
                    changed["tools"][0]["sha256"] = "0" * 64
                elif mutation == "duplicate-tool":
                    changed["tools"][1] = deepcopy(changed["tools"][0])
                elif mutation == "sdk-sha":
                    changed["sdks"][0]["settings_sha256"] = "0" * 64
                elif mutation == "sdk-version":
                    changed["sdks"][0]["version"] = "26.4"
                else:
                    changed["sdks"].pop()
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    toolchain.verify_installation(changed, "c" * 40, "123", "2", case_id=receipt["case_id"],
                                                 workflow_commit="d" * 40)

    def test_installed_info_each_tool_and_sdk_are_rehashed_before_app_execution(self):
        with tempfile.TemporaryDirectory() as tmp, installed_identity_fixture(Path(tmp)) as receipt:
            paths = [Path(receipt["toolchain"]["path"]) / "Info.plist"]
            paths += [Path(row["resolved_path"]) for row in receipt["tools"]]
            paths += [Path(row["path"]) / "SDKSettings.json" for row in receipt["sdks"]]
            for path in paths:
                original = path.read_bytes()
                path.write_bytes(b"changed installed identity")
                with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "changed|differ"):
                    toolchain.verify_installation(receipt, "c" * 40, "123", "2", case_id=receipt["case_id"],
                                                 workflow_commit="d" * 40)
                path.write_bytes(original)

    def test_reported_tool_alias_cannot_escape_even_with_matching_receipt_hash(self):
        with tempfile.TemporaryDirectory() as tmp, installed_identity_fixture(Path(tmp)) as receipt:
            alias = Path(receipt["tools"][0]["reported_path"])
            outside = Path(tmp) / "outside-swift-driver"
            outside.write_bytes(Path(receipt["tools"][0]["resolved_path"]).read_bytes())
            alias.unlink()
            alias.symlink_to(outside)
            receipt["tools"][0]["resolved_path"] = str(outside.resolve())
            with self.assertRaisesRegex(ValueError, "outside"):
                toolchain.verify_installation(receipt, "c" * 40, "123", "2", case_id=receipt["case_id"],
                                             workflow_commit="d" * 40)

    def test_hashing_observes_remaining_budget_between_chunks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bounded-file"
            path.write_bytes(bytes(2 * 1024 * 1024))
            checks = []
            def check():
                checks.append(True)
                if len(checks) == 2:
                    raise RuntimeError("deadline exhausted")
            with self.assertRaisesRegex(RuntimeError, "deadline"):
                toolchain.digest(path, check)
            self.assertEqual(len(checks), 2)


if __name__ == "__main__":
    unittest.main()
