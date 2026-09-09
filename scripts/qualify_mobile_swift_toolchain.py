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


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
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


def qualify(work):
    if os.environ.get("GITHUB_ACTIONS") != "true" or sys.platform != "darwin":
        raise RuntimeError("Swift toolchain qualification runs only in macOS GitHub Actions")
    work.mkdir(parents=True, exist_ok=False)
    output = work / "evidence"
    output.mkdir()
    receipt = {"schema_version": 1, "scope": "swift-toolchain-installation-identity",
               "status": "incomplete", "application_build_verified": False,
               "consumer_commit": os.environ.get("GITHUB_SHA"),
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

    def command(name, argv, timeout=120):
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
        package_sha = digest(package)
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
        if digest(package) != package_sha:
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
        for sdk in ("iphoneos", "iphonesimulator"):
            prefix = ["/usr/bin/xcrun", "--toolchain", identifier, "--sdk", sdk]
            version = command(sdk + "-version", [*prefix, "--show-sdk-version"]).strip()
            sdk_root = Path(command(sdk + "-path", [*prefix, "--show-sdk-path"]).strip())
            if version != "26.5" or not sdk_root.resolve().is_relative_to(Path(DEVELOPER_DIR)):
                raise RuntimeError("Toolchain selection changed the pinned iOS SDK")
            receipt["sdks"].append({"name": sdk, "version": version, "path": str(sdk_root),
                                     "settings_sha256": digest(sdk_root / "SDKSettings.json")})
        receipt["status"] = "success"
    except Exception as error:
        receipt["status"], receipt["error"] = "failed", str(error)
        raise
    finally:
        (output / "qualification.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        qualify(args.work_dir.resolve())
    except Exception as error:
        print(f"Swift toolchain qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
