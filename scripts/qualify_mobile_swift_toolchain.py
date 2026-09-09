#!/usr/bin/env python3
"""Collect a fixed Swift snapshot's installation identity in macOS Actions.

The package digest is pinned from the signed download in Actions run
34342548149. Successful installation does not qualify app compilation, method
recovery, or behavior. Production NeverD never invokes this script.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time


SNAPSHOT = "swift-6.4.x-DEVELOPMENT-SNAPSHOT-2026-09-04-a"
PACKAGE_URL = f"https://download.swift.org/swift-6.4.x-branch/xcode/{SNAPSHOT}/{SNAPSHOT}-osx.pkg"
PACKAGE_SHA256 = "264a82a3c876ccf821e7fd593bb1256b125136a709497f9ce6bc884f598b195f"
SOURCE_REFERENCE = "d2e983b81b18217da61b818e06e58589ecbfd67f"
SIGNER = "Developer ID Installer: Swift Open Source (V9AUD2URP3)"
TRUSTED_SIGNATURE_STATUSES = {
    "signed by a certificate trusted by macOS",
    "signed by a developer certificate issued by Apple for distribution",
}
DEVELOPER_DIR = "/Applications/Xcode_26.5.app/Contents/Developer"
MAX_PACKAGE_BYTES = 3 * 1024 * 1024 * 1024
QUALIFICATION_RUN_ID = 34344298423
BUNDLE_IDENTIFIER = "org.swift.64202609041a"
INFO_PLIST_SHA256 = "84ac4795ba5d9c9ce58729c0101908dd619df7fcb586f65e3e4be46781e08b29"
TOOL_SHA256 = {
    "swiftc": "70c364e10c4d5201f77f5cb9568d68abb48f38a726ccffb52c451e2b51329dde",
    "swift-frontend": "dc1046434e4dff77d0afcbe45e77cd3921af1a34bddf94c1ed0e5d4f4ab2f8e5",
    "swift-demangle": "09904ae84f7af1b5250b2aaf729807296a1432babb800dec04d798bf57b265a1",
}
SDK_SETTINGS_SHA256 = {
    "iphoneos": "6d01ffc01efe09ccb1bbaf5e90a96a34b07bfe049ee77e7352303984193f83f0",
    "iphonesimulator": "038a8a5fb9ac4ba60db102748ed117bf674cf64545cce8092eec0c6c44048adc",
}
INSTALLATION_SECONDS = 2700
REQUIRED_INSTALLATION_COMMANDS = {
    "consumer-source-head", "xcode-version", "macos-version", "download", "package-signature",
    "gatekeeper", "install", "xcode-toolchain-selection",
    *(name + "-" + operation for name in TOOL_SHA256
      for operation in ("path", "signature", "signature-details", "version")),
    *(name + "-" + operation for name in SDK_SETTINGS_SHA256 for operation in ("path", "version")),
}


def consumer_identity(explicit, environment):
    # workflow_run's GITHUB_SHA identifies workflow code, not the verified
    # producer revision. The dispatch-only qualification entry remains usable.
    value = explicit
    if value is None and environment.get("GITHUB_EVENT_NAME") == "workflow_dispatch":
        value = environment.get("GITHUB_SHA")
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("An explicit exact consumer commit is required")
    return value


def verify_installation(receipt, consumer_commit, run_id, run_attempt, check=lambda: None, case_id=None,
                        *, workflow_commit):
    """Reconcile this job's receipt against pinned identities and current files.

    This validates installation identity only; it never certifies app builds.
    Callers copy the receipt into their own evidence before executing app code.
    """
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(isinstance(receipt, dict) and receipt.get("schema_version") == 1
            and receipt.get("scope") == "swift-toolchain-installation-identity"
            and receipt.get("status") == "success"
            and receipt.get("installation_pin_evidence_run_id") == QUALIFICATION_RUN_ID,
            "Toolchain installation receipt is incomplete")
    require(re.fullmatch(r"[0-9a-f]{40}", consumer_commit or "") is not None
            and receipt.get("consumer_commit") == consumer_commit,
            "Toolchain receipt differs from the current consumer commit")
    require(isinstance(workflow_commit, str)
            and re.fullmatch(r"[0-9a-f]{40}", workflow_commit) is not None
            and receipt.get("workflow_commit") == workflow_commit,
            "Toolchain receipt differs from the current workflow source identity")
    require(all(isinstance(value, str) and re.fullmatch(r"[1-9][0-9]*", value)
                for value in (run_id, run_attempt))
            and receipt.get("run_id") == run_id and receipt.get("run_attempt") == run_attempt,
            "Toolchain receipt is from another workflow run or attempt")
    if case_id is not None:
        require(receipt.get("case_id") == case_id, "Toolchain receipt belongs to another application case")
    require(receipt.get("runner_os") == "macOS" and receipt.get("runner_arch") == "ARM64",
            "Toolchain receipt does not identify the qualified macOS ARM64 host")
    package = receipt.get("package", {})
    require(isinstance(package, dict) and package.get("snapshot") == SNAPSHOT
            and package.get("url") == PACKAGE_URL
            and package.get("expected_sha256") == PACKAGE_SHA256
            and package.get("observed_sha256") == PACKAGE_SHA256
            and package.get("source_reference") == SOURCE_REFERENCE
            and package.get("identity_status") == "sha256-and-system-signature-verified"
            and package.get("gatekeeper") == "accepted",
            "Toolchain package identity differs from the fixed signed package")
    signature = package.get("signature", {})
    require(isinstance(signature, dict) and signature.get("leaf") == SIGNER
            and signature.get("status") in TRUSTED_SIGNATURE_STATUSES
            and signature.get("certificate_chain") == [SIGNER, "Developer ID Certification Authority", "Apple Root CA"],
            "Toolchain receipt lacks the required installer signature chain")
    identity = receipt.get("toolchain", {})
    require(isinstance(identity, dict) and identity.get("bundle_identifier") == BUNDLE_IDENTIFIER
            and identity.get("info_plist_sha256") == INFO_PLIST_SHA256,
            "Toolchain bundle identity differs from the qualified snapshot")
    bundle = Path(identity.get("path", ""))
    expected = Path.home() / "Library/Developer/Toolchains" / (SNAPSHOT + ".xctoolchain")
    require(bundle == expected and bundle.is_dir() and not bundle.is_symlink(),
            "Toolchain is not the installed fixed user bundle")
    check()
    require(digest(bundle / "Info.plist", check) == INFO_PLIST_SHA256,
            "Installed toolchain Info.plist changed")
    records = receipt.get("tools")
    require(isinstance(records, list) and len(records) == len(TOOL_SHA256),
            "Toolchain receipt lacks the complete Swift tool set")
    seen = set()
    for row in records:
        require(isinstance(row, dict) and row.get("name") in TOOL_SHA256
                and row["name"] not in seen, "Duplicate or unknown Swift tool identity")
        name = row["name"]
        seen.add(name)
        reported = row.get("reported_path")
        require(reported == str(bundle / "usr/bin" / name), "Swift tool path differs from its fixed bundle member")
        executable = installed_tool(bundle, reported)
        require(row.get("resolved_path") == str(executable), "Swift tool symlink target changed")
        check()
        require(row.get("sha256") == TOOL_SHA256[name] and digest(executable, check) == TOOL_SHA256[name],
                "Installed Swift tool bytes differ from the qualified snapshot")
    xcode = receipt.get("xcode", {})
    require(isinstance(xcode, dict) and xcode.get("developer_dir") == DEVELOPER_DIR
            and xcode.get("version", "").startswith("Xcode 26.5\n"),
            "Toolchain receipt changed the pinned Xcode installation")
    sdks = receipt.get("sdks")
    require(isinstance(sdks, list) and len(sdks) == len(SDK_SETTINGS_SHA256),
            "Toolchain receipt lacks both pinned SDK identities")
    seen = set()
    for row in sdks:
        require(isinstance(row, dict) and row.get("name") in SDK_SETTINGS_SHA256
                and row["name"] not in seen, "Duplicate or unknown toolchain SDK identity")
        name = row["name"]
        seen.add(name)
        path = Path(row.get("path", ""))
        require(path.is_absolute() and path.resolve(strict=True).is_relative_to(Path(DEVELOPER_DIR).resolve(strict=True))
                and row.get("version") == "26.5", "Toolchain SDK path or version changed")
        check()
        require(row.get("settings_sha256") == SDK_SETTINGS_SHA256[name]
                and digest(path / "SDKSettings.json", check) == SDK_SETTINGS_SHA256[name],
                "Installed SDK identity differs from the qualified SDK")
    return receipt


def digest(path, check=lambda: None):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            check()
            result.update(block)
    return result.hexdigest()


def trusted_package_signature(text):
    """Require the trusted status and the actual first certificate-chain row."""
    statuses = re.findall(r"^\s*Status:\s*(.*?)\s*$", text, re.MULTILINE)
    if len(statuses) != 1 or statuses[0] not in TRUSTED_SIGNATURE_STATUSES:
        raise ValueError("Installer package does not have the required trusted signature status")
    parts = text.split("Certificate Chain:")
    if len(parts) != 2:
        raise ValueError("Installer signature has no unambiguous certificate chain")
    certificates = re.findall(r"^\s*(\d+)\.\s+([^\r\n]+)", parts[1], re.MULTILINE)
    if not certificates or certificates[0] != ("1", SIGNER):
        raise ValueError("Installer leaf certificate is not the Swift Open Source identity")
    if [int(number) for number, _ in certificates] != list(range(1, len(certificates) + 1)):
        raise ValueError("Installer certificate chain numbering is inconsistent")
    return {"status": statuses[0], "leaf": certificates[0][1],
            "certificate_chain": [name for _, name in certificates]}


def bundle_identifier(info):
    value = info.get("CFBundleIdentifier") if isinstance(info, dict) else None
    if not isinstance(value, str) or not re.fullmatch(r"org\.swift\.[A-Za-z0-9._-]+", value):
        raise ValueError("Installed toolchain has no exact Swift bundle identifier")
    return value


def installed_tool(bundle, reported):
    path = Path(reported.strip())
    if not path.is_absolute():
        raise ValueError("xcrun did not return an absolute tool path")
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(bundle.resolve(strict=True)) or not resolved.is_file():
        raise ValueError("xcrun selected a tool outside the installed snapshot")
    return resolved


def installed_bundle(root, before, after):
    added = after - before
    bundles = [root / name for name in added
               if (root / name).is_dir() and not (root / name).is_symlink()]
    if len(bundles) != 1 or not before.issubset(after):
        raise ValueError("Installation did not add exactly one new user toolchain bundle")
    bundle = bundles[0].resolve(strict=True)
    aliases = []
    for name in sorted(added - {bundles[0].name}):
        path = root / name
        if not path.is_symlink() or path.resolve(strict=True) != bundle:
            raise ValueError("New toolchain alias does not resolve to the installed snapshot")
        aliases.append({"path": str(path), "link_target": os.readlink(path),
                        "resolved_target": str(bundle)})
    # Select the actual directory, never the mutable swift-latest alias.
    return bundle, aliases


def qualify(work, consumer_commit=None, case_id=None):
    if os.environ.get("GITHUB_ACTIONS") != "true" or sys.platform != "darwin":
        raise RuntimeError("Swift toolchain qualification runs only in macOS GitHub Actions")
    consumer_commit = consumer_identity(consumer_commit, os.environ)
    if case_id is not None and not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", case_id):
        raise ValueError("Invalid toolchain installation case identity")
    deadline = time.monotonic() + INSTALLATION_SECONDS
    work.mkdir(parents=True, exist_ok=False)
    output = work / "evidence"
    output.mkdir()
    receipt = {"schema_version": 1, "scope": "swift-toolchain-installation-identity",
               "status": "incomplete", "application_build_verified": False,
               "installation_pin_evidence_run_id": QUALIFICATION_RUN_ID,
               "consumer_commit": consumer_commit,
               "workflow_commit": os.environ.get("GITHUB_SHA"),
               "case_id": case_id,
               "run_id": os.environ.get("GITHUB_RUN_ID"),
               "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
               "runner_os": os.environ.get("RUNNER_OS"),
               "runner_arch": os.environ.get("RUNNER_ARCH"),
               "package": {"snapshot": SNAPSHOT, "url": PACKAGE_URL,
                           "identity_status": "pinned-package-verification-pending",
                           "expected_sha256": PACKAGE_SHA256,
                           "pin_evidence_run_id": 34342548149,
                           "observed_sha256": None,
                           "source_reference": SOURCE_REFERENCE,
                           "package_build_source_verified": False},
               "commands": [], "tools": [], "sdks": []}
    environment = {key: os.environ[key] for key in ("HOME", "USER", "LOGNAME", "TMPDIR")
                   if key in os.environ}
    environment.update({"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C", "LC_ALL": "C",
                        "DEVELOPER_DIR": DEVELOPER_DIR})

    def remaining():
        value = deadline - time.monotonic()
        if value <= 0:
            raise RuntimeError("Toolchain installation deadline exhausted")
        return value

    def command(name, argv, timeout=120):
        timeout = min(timeout, remaining())
        stdout, stderr = output / (name + ".stdout"), output / (name + ".stderr")
        record = {"name": name, "argv": [str(value) for value in argv],
                  "stdout": stdout.name, "stderr": stderr.name, "exitcode": None}
        receipt["commands"].append(record)
        try:
            with stdout.open("wb") as out, stderr.open("wb") as err:
                completed = subprocess.run(argv, cwd=work, env=environment, stdout=out,
                                           stderr=err, timeout=timeout, check=False)
            record["exitcode"] = completed.returncode
        except subprocess.TimeoutExpired:
            record["timed_out"] = True
            raise RuntimeError(f"{name} exceeded its time budget; see retained logs")
        finally:
            for key, path in (("stdout", stdout), ("stderr", stderr)):
                if path.is_file():
                    record[key + "_sha256"] = digest(path)
        if completed.returncode:
            raise RuntimeError(f"{name} exited {completed.returncode}; see retained logs")
        if stdout.stat().st_size > 4 * 1024 * 1024:
            raise RuntimeError(f"{name} returned unexpectedly large text evidence")
        return stdout.read_text(encoding="utf-8")

    try:
        head = command("consumer-source-head", ["/usr/bin/git", "-C", Path(__file__).resolve().parent.parent,
                                                "rev-parse", "HEAD"]).strip()
        if head != consumer_commit:
            raise RuntimeError("Toolchain installer script checkout differs from the requested consumer commit")
        if not Path(DEVELOPER_DIR).is_dir():
            raise RuntimeError("The pinned Xcode 26.5 installation is unavailable")
        xcode = command("xcode-version", ["/usr/bin/xcodebuild", "-version"])
        if not xcode.startswith("Xcode 26.5\n"):
            raise RuntimeError("The selected Xcode version is not 26.5")
        receipt["xcode"] = {"developer_dir": DEVELOPER_DIR, "version": xcode.strip()}
        command("macos-version", ["/usr/bin/sw_vers"])

        package = work / (SNAPSHOT + "-osx.pkg")
        download = command("download", ["/usr/bin/curl", "--fail", "--location", "--silent",
                           "--show-error", "--proto", "=https", "--proto-redir", "=https",
                           "--retry", "3", "--max-time", "1800", "--max-filesize",
                           str(MAX_PACKAGE_BYTES), "--dump-header", output / "download.headers",
                           "--write-out", "%{url_effective}\n%{http_code}\n", "--output", package,
                           PACKAGE_URL], timeout=1900).splitlines()
        if len(download) != 2 or download[1] != "200" or not download[0].startswith("https://"):
            raise RuntimeError("The fixed package download did not complete over HTTPS")
        size = package.stat().st_size
        if not 0 < size <= MAX_PACKAGE_BYTES:
            raise RuntimeError("The downloaded package exceeds its byte budget")
        package_sha = digest(package, remaining)
        receipt["package"].update({"observed_sha256": package_sha, "size": size,
                                   "effective_url": download[0]})
        if package_sha != PACKAGE_SHA256:
            raise RuntimeError("The package SHA-256 differs from the previously signed download")
        receipt["package"]["identity_status"] = "sha256-matched"
        signature = command("package-signature", ["/usr/sbin/pkgutil", "--check-signature", package])
        receipt["package"]["signature"] = trusted_package_signature(signature)
        command("gatekeeper", ["/usr/sbin/spctl", "--assess", "--type", "install",
                               "--verbose=4", package], timeout=300)
        receipt["package"]["gatekeeper"] = "accepted"
        receipt["package"]["identity_status"] = "sha256-and-system-signature-verified"
        if digest(package, remaining) != package_sha:
            raise RuntimeError("The package changed during signature assessment")

        root = Path(environment["HOME"]) / "Library/Developer/Toolchains"
        if root.is_symlink():
            raise RuntimeError("The user toolchain directory is a symlink")
        before = {path.name for path in root.glob("*.xctoolchain")}
        receipt["toolchain_directories_before"] = sorted(before)
        command("install", ["/usr/sbin/installer", "-target", "CurrentUserHomeDirectory",
                            "-pkg", package], timeout=900)
        after = {path.name for path in root.glob("*.xctoolchain")}
        receipt["toolchain_directories_after"] = sorted(after)
        bundle, aliases = installed_bundle(root, before, after)
        receipt["toolchain_aliases"] = aliases
        info_path = bundle / "Info.plist"
        info_bytes = info_path.read_bytes()
        if len(info_bytes) > 1024 * 1024:
            raise RuntimeError("Toolchain Info.plist exceeds its parsing budget")
        info = plistlib.loads(info_bytes)
        identifier = bundle_identifier(info)
        if identifier != BUNDLE_IDENTIFIER or digest(info_path) != INFO_PLIST_SHA256:
            raise RuntimeError("Installed bundle differs from the previously qualified snapshot")
        (output / "toolchain-Info.plist").write_bytes(info_bytes)
        receipt["toolchain"] = {"path": str(bundle), "bundle_identifier": identifier,
                                 "info_plist_sha256": digest(info_path)}
        command("xcode-toolchain-selection", ["/usr/bin/xcodebuild", "-toolchain", identifier,
                                               "-version"])
        for name in ("swiftc", "swift-frontend", "swift-demangle"):
            prefix = ["/usr/bin/xcrun", "--toolchain", identifier]
            reported = command(name + "-path", [*prefix, "--find", name])
            executable = installed_tool(bundle, reported)
            command(name + "-signature", ["/usr/bin/codesign", "--verify", "--strict",
                                           "--verbose=4", executable])
            command(name + "-signature-details", ["/usr/bin/codesign", "--display",
                                                   "--verbose=4", executable])
            version = command(name + "-version", [*prefix, name, "--version"])
            if name == "swiftc" and not re.search(r"\bSwift version 6\.4(?:\D|$)", version):
                raise RuntimeError("The installed Swift compiler is not the requested 6.4 series")
            receipt["tools"].append({"name": name, "reported_path": reported.strip(),
                                      "resolved_path": str(executable),
                                      "sha256": digest(executable), "version": version.strip()})
            if receipt["tools"][-1]["sha256"] != TOOL_SHA256[name]:
                raise RuntimeError("Installed Swift tool differs from its qualified byte identity")
        for sdk in ("iphoneos", "iphonesimulator"):
            prefix = ["/usr/bin/xcrun", "--toolchain", identifier, "--sdk", sdk]
            version = command(sdk + "-version", [*prefix, "--show-sdk-version"]).strip()
            sdk_root = Path(command(sdk + "-path", [*prefix, "--show-sdk-path"]).strip())
            if version != "26.5" or not sdk_root.resolve().is_relative_to(Path(DEVELOPER_DIR)):
                raise RuntimeError("Toolchain selection changed the pinned iOS SDK")
            receipt["sdks"].append({"name": sdk, "version": version, "path": str(sdk_root),
                                     "settings_sha256": digest(sdk_root / "SDKSettings.json")})
            if receipt["sdks"][-1]["settings_sha256"] != SDK_SETTINGS_SHA256[sdk]:
                raise RuntimeError("Installed SDK differs from its qualified byte identity")
        receipt["status"] = "success"
        verify_installation(receipt, consumer_commit, os.environ.get("GITHUB_RUN_ID"),
                            os.environ.get("GITHUB_RUN_ATTEMPT"), remaining, case_id=case_id,
                            workflow_commit=os.environ.get("GITHUB_SHA"))
    except Exception as error:
        receipt["status"], receipt["error"] = "failed", str(error)
        raise
    finally:
        (output / "qualification.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--consumer-commit", help="Exact verified consumer SHA; required for workflow_run")
    parser.add_argument("--case-id", help="Independent application comparison case receiving this installation")
    args = parser.parse_args()
    try:
        qualify(args.work_dir.resolve(), args.consumer_commit, args.case_id)
    except Exception as error:
        print(f"Swift toolchain qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
