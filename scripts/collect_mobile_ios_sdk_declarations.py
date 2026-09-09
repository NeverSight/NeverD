#!/usr/bin/env python3
"""Collect real SDK declaration names for the native ObjC import planner.

Actions-only development evidence. This does not certify method recovery,
instance layout, ABI compatibility, or application behavior. The input imports
are maintained here; no application source or build script is executed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


# SDK 26.5's complete Messages import produces about 263 MiB of JSON. Keep
# the full declaration closure and a finite ceiling above that measured input.
MAX_AST_BYTES = 384 * 1024 * 1024
MAX_MACRO_BYTES = 16 * 1024 * 1024
IMPORTS = {"Messages/Messages.h": "MSStickerBrowserViewController",
           "UserNotifications/UserNotifications.h": "UNNotificationServiceExtension"}
SDK_TARGETS = (("iphoneos", "arm64-apple-ios18.0"),
               ("iphonesimulator", "arm64-apple-ios18.0-simulator"))
SOURCE_PREFIX = ("#include <stdint.h>\n#include <stdbool.h>\n"
                 "#import <Foundation/Foundation.h>\n")
SOURCE_PROFILES = {
    "foundation": ((), ()),
    "messages": (("Messages/Messages.h",), ("MSStickerBrowserViewController",)),
    "user-notifications": (("UserNotifications/UserNotifications.h",),
                           ("UNNotificationServiceExtension",)),
    "messages-user-notifications": (
        ("Messages/Messages.h", "UserNotifications/UserNotifications.h"),
        ("MSStickerBrowserViewController", "UNNotificationServiceExtension")),
}
COLLECT_TIMEOUT = 1200


class CollectionDeadline:
    def __init__(self):
        self.end = time.monotonic() + COLLECT_TIMEOUT

    def remaining(self):
        remaining = self.end - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("SDK declaration collection exceeded its total deadline")
        return remaining

    def check(self):
        self.remaining()


def digest(path, check=None):
    result = hashlib.sha256()
    if check:
        check()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            if check:
                check()
            result.update(block)
    if check:
        check()
    return result.hexdigest()


def declarations(ast, required_class=()):
    if not isinstance(ast, dict) or ast.get("kind") != "TranslationUnitDecl":
        raise ValueError("Clang did not produce a translation-unit AST")
    if not isinstance(ast.get("inner"), list):
        raise ValueError("Clang AST has no complete declaration list")
    classes, names = set(), set()
    ordinary = {"ObjCInterfaceDecl", "TypedefDecl", "FunctionDecl", "VarDecl", "EnumConstantDecl"}
    for row in ast["inner"]:
        if not isinstance(row, dict):
            raise ValueError("Invalid Clang declaration record")
        kind, name = row.get("kind"), row.get("name")
        if kind in ordinary and isinstance(name, str) and name:
            names.add(name)
            if kind == "ObjCInterfaceDecl":
                classes.add(name)
        if kind == "EnumDecl":
            for value in row.get("inner", []):
                if value.get("kind") == "EnumConstantDecl" and value.get("name"):
                    names.add(value["name"])
    required = (required_class,) if isinstance(required_class, str) else required_class
    if any(name not in classes for name in required):
        raise ValueError("SDK imports did not declare the requested system superclass")
    # Protocol names and C struct/enum tags have separate namespaces. Members
    # and method selectors also must not become false class-name conflicts.
    # ObjCInterfaceDecl also represents @class forward declarations. These
    # names occupy the namespace but are not all usable as superclasses.
    return {"interface_declarations": sorted(classes), "ordinary_identifiers": sorted(names)}


def macro_identifiers(text):
    """Read the names in Clang's complete -dM output, without evaluating macros."""
    if len(text.encode("utf-8")) > MAX_MACRO_BYTES:
        raise ValueError("SDK macro evidence exceeds its parsing budget")
    names, logical, continued = set(), "", False
    for line in text.splitlines():
        logical += line
        continued = logical.endswith("\\")
        if continued:
            logical = logical[:-1]
            continue
        if not logical.strip():
            logical = ""
            continue
        match = re.match(r"^#define[ \t]+([A-Za-z_][A-Za-z0-9_]*)(?=[ \t(]|$)", logical)
        if not match:
            raise ValueError("Clang macro evidence has an unrecognized definition")
        name = match.group(1)
        if name in names:
            raise ValueError("Clang macro evidence has a duplicate definition")
        names.add(name)
        logical = ""
    if logical or continued:
        raise ValueError("Clang macro evidence has a truncated continuation")
    if not {"__APPLE__", "__OBJC__"}.issubset(names):
        raise ValueError("Clang macro evidence does not identify the Apple Objective-C target")
    return sorted(names)


def source_prefix(profile_id):
    headers, _ = SOURCE_PROFILES[profile_id]
    return SOURCE_PREFIX + "".join(f"#import <{header}>\n" for header in headers)


def source_profile_records(sdk_version):
    records = []
    for sdk, target in SDK_TARGETS:
        for profile_id, (headers, superclasses) in SOURCE_PROFILES.items():
            name = sdk + "-source-" + profile_id
            records.append({
                "sdk": sdk, "profile_id": profile_id, "kind": "objc-source-prefix",
                "version": sdk_version, "target": target,
                "headers": ["stdint.h", "stdbool.h", "Foundation/Foundation.h", *headers],
                "required_superclasses": list(superclasses), "status": "incomplete",
                "prefix": {"path": name + ".m"},
                "inventory": {"path": name + "-declarations.json"}, "subclass_probes": [],
            })
    return records


def check_source_profiles(records, sdk_version):
    expected = {(row["sdk"], row["profile_id"]): row
                for row in source_profile_records(sdk_version)}
    if not isinstance(records, list) or len(records) != len(expected):
        raise ValueError("SDK source profiles are incomplete or duplicated")
    seen = set()
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Invalid SDK source profile")
        key = (record.get("sdk"), record.get("profile_id"))
        if key not in expected or key in seen:
            raise ValueError("SDK source profile identity is missing or duplicated")
        seen.add(key)
        wanted = expected[key]
        if record.get("status") != "success" or any(
                record.get(field) != wanted[field] for field in (
                    "kind", "version", "target", "headers", "required_superclasses")):
            raise ValueError("SDK source profile does not match its complete target and imports")
        for field in ("prefix", "inventory"):
            evidence = record.get(field)
            if (not isinstance(evidence, dict) or evidence.get("path") != wanted[field]["path"]
                    or not isinstance(evidence.get("sha256"), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", evidence["sha256"])):
                raise ValueError("SDK source profile has incomplete file evidence")
        for field in ("clang_sha256", "sdk_settings_sha256"):
            if not isinstance(record.get(field), str) or not re.fullmatch(r"[0-9a-f]{64}", record[field]):
                raise ValueError("SDK source profile has incomplete tool evidence")
        for field, suffix in (("ast_evidence", "-ast.stdout"), ("macro_evidence", "-macros.stdout")):
            evidence = record.get(field)
            if (not isinstance(evidence, dict)
                    or evidence.get("path") != wanted["prefix"]["path"][:-2] + suffix
                    or not isinstance(evidence.get("sha256"), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", evidence["sha256"])):
                raise ValueError("SDK source profile lacks its independent AST or macro evidence")
        if record["macro_evidence"].get("scope") != "preprocessor-identifiers-only":
            raise ValueError("SDK source profile macro evidence has an incorrect scope")
        probes = record.get("subclass_probes")
        required = wanted["required_superclasses"]
        if not isinstance(probes, list) or len(probes) != len(required):
            raise ValueError("SDK source profile has incomplete subclass probes")
        parents = set()
        for probe in probes:
            if not isinstance(probe, dict):
                raise ValueError("Invalid SDK source subclass probe")
            parent = probe.get("superclass")
            if not isinstance(parent, str) or parent not in required or parent in parents:
                raise ValueError("SDK source subclass identity is missing or duplicated")
            parents.add(parent)
            name = wanted["prefix"]["path"][:-2] + "-subclass-" + parent + ".m"
            if (probe.get("status") != "success" or probe.get("source") != name
                    or probe.get("instance_layout_verified") is not False
                    or probe.get("prefix_sha256") != record["prefix"]["sha256"]
                    or not isinstance(probe.get("sha256"), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", probe["sha256"])):
                raise ValueError("SDK source subclass probe has incomplete or mismatched evidence")


def collect(output, sdk_version):
    if os.environ.get("GITHUB_ACTIONS") != "true" or sys.platform != "darwin":
        raise RuntimeError("SDK declaration collection must run on macOS GitHub Actions")
    output.mkdir(parents=True, exist_ok=False)
    deadline = CollectionDeadline()
    evidence = {"schema_version": 1, "scope": "sdk-declarations-only", "status": "incomplete",
                "imports": list(IMPORTS), "commands": [], "sdks": [],
                "consumer_commit": os.environ.get("CONSUMER_COMMIT"),
                "source_profiles": source_profile_records(sdk_version)}
    evidence["expected_source_profile_ids"] = [row["sdk"] + "/" + row["profile_id"]
                                               for row in evidence["source_profiles"]]

    def command(name, argv):
        stdout, stderr = output / (name + ".stdout"), output / (name + ".stderr")
        record = {"name": name, "argv": [str(value) for value in argv],
                  "stdout": stdout.name, "stderr": stderr.name, "exitcode": None}
        evidence["commands"].append(record)
        env = os.environ.copy()
        for name in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
            env.pop(name, None)
        failure = None
        try:
            timeout = min(120, deadline.remaining())
            with stdout.open("wb") as out, stderr.open("wb") as err:
                completed = subprocess.run(argv, cwd=output, stdout=out, stderr=err,
                                           env=env, timeout=timeout, check=False)
            record["exitcode"] = completed.returncode
            if completed.returncode:
                raise RuntimeError(f"{record['name']} exited {completed.returncode}; see retained logs")
        except Exception as error:
            record["error"] = str(error)
            failure = error
        finally:
            # The file contexts have closed, including after TimeoutExpired.
            # Retention errors must not replace the actual command failure.
            for field, path in (("stdout", stdout), ("stderr", stderr)):
                try:
                    record[field + "_sha256"] = digest(path, deadline.check)
                except Exception as error:
                    record.setdefault("evidence_errors", []).append(field + ": " + str(error))
        if failure is not None:
            raise failure
        if record.get("evidence_errors"):
            message = "SDK command evidence could not be retained: " + "; ".join(record["evidence_errors"])
            record["error"] = message
            raise RuntimeError(message)
        if stdout.stat().st_size > MAX_AST_BYTES:
            raise RuntimeError("SDK declaration evidence exceeds its parsing budget")
        return stdout

    def inventory_for(name, source, target_args, required):
        deadline.check()
        ast_path = command(name + "-ast", [*target_args, "-fsyntax-only", "-Xclang", "-ast-dump=json", source])
        deadline.check()
        text = ast_path.read_text(encoding="utf-8")
        deadline.check()
        ast = json.loads(text)
        del text
        deadline.check()
        inventory = declarations(ast, required)
        del ast
        deadline.check()
        macros_path = command(name + "-macros", [*target_args, "-E", "-dM", source])
        if macros_path.stat().st_size > MAX_MACRO_BYTES:
            raise RuntimeError("SDK macro evidence exceeds its parsing budget")
        deadline.check()
        inventory["macro_identifiers"] = macro_identifiers(macros_path.read_text(encoding="utf-8"))
        deadline.check()
        inventory["macro_evidence"] = {"path": macros_path.name,
                                       "sha256": digest(macros_path, deadline.check),
                                       "scope": "preprocessor-identifiers-only"}
        inventory["ast_sha256"] = digest(ast_path, deadline.check)
        inventory["ast_evidence"] = {"path": ast_path.name, "sha256": inventory["ast_sha256"]}
        return inventory

    try:
        for sdk, target in SDK_TARGETS:
            deadline.check()
            prefix = ["xcrun", "--toolchain", "XcodeDefault", "--sdk", sdk]
            version = command(sdk + "-version", [*prefix, "--show-sdk-version"]).read_text().strip()
            if version != sdk_version:
                raise RuntimeError(f"{sdk} SDK version differs from the configured {sdk_version}")
            root = Path(command(sdk + "-root", [*prefix, "--show-sdk-path"]).read_text().strip())
            clang = Path(command(sdk + "-clang", [*prefix, "--find", "clang"]).read_text().strip())
            command(sdk + "-compiler-version", [clang, "--version"])
            target_args = [clang, "-x", "objective-c", "-std=gnu11", "-fobjc-arc", "-fno-modules",
                           "-target", target, "-isysroot", root]
            args = [*target_args, "-fsyntax-only"]
            for header, superclass in IMPORTS.items():
                # Keep each import's closure separate: a UserNotifications-only
                # source must not inherit false conflicts from UIKit/Messages.
                name = sdk + "-" + header.split("/")[0]
                source = output / (name + ".m")
                source.write_text(f"#import <{header}>\n", encoding="utf-8")
                inventory = inventory_for(name, source, target_args, superclass)
                probe = output / (name + "-subclass.m")
                probe.write_text(f"#import <{header}>\n"
                                 f"@interface NeverDSDKDeclarationProbe : {superclass}\n@end\n",
                                 encoding="utf-8")
                command(name + "-subclass", [*args, probe])
                inventory.update({"sdk": sdk, "version": version, "target": target, "header": header,
                                  "clang_sha256": digest(clang, deadline.check),
                                  "sdk_settings_sha256": digest(root / "SDKSettings.json", deadline.check),
                                  "source_sha256": digest(source, deadline.check),
                                  "subclass_declaration_probe": {"superclass": superclass,
                                      "source": probe.name, "sha256": digest(probe, deadline.check),
                                      "status": "success", "instance_layout_verified": False}})
                inventory_path = output / (name + "-declarations.json")
                inventory_path.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
                evidence["sdks"].append({"sdk": sdk, "header": header, "inventory": inventory_path.name,
                                         "sha256": digest(inventory_path, deadline.check)})
            for record in evidence["source_profiles"]:
                if record["sdk"] != sdk:
                    continue
                try:
                    deadline.check()
                    profile_id = record["profile_id"]
                    name = sdk + "-source-" + profile_id
                    prefix_text = source_prefix(profile_id)
                    source = output / record["prefix"]["path"]
                    source.write_text(prefix_text, encoding="utf-8")
                    record["prefix"]["sha256"] = digest(source, deadline.check)
                    inventory = inventory_for(name, source, target_args, record["required_superclasses"])
                    record.update({key: inventory[key] for key in ("ast_evidence", "macro_evidence")})
                    record["clang_sha256"] = digest(clang, deadline.check)
                    record["sdk_settings_sha256"] = digest(root / "SDKSettings.json", deadline.check)
                    for superclass in record["required_superclasses"]:
                        probe = output / (name + "-subclass-" + superclass + ".m")
                        probe.write_text(prefix_text +
                                         f"@interface NeverDSDKDeclarationProbe : {superclass}\n@end\n",
                                         encoding="utf-8")
                        command(name + "-subclass-" + superclass, [*args, probe])
                        record["subclass_probes"].append({
                            "superclass": superclass, "source": probe.name,
                            "sha256": digest(probe, deadline.check),
                            "prefix_sha256": record["prefix"]["sha256"],
                            "status": "success", "instance_layout_verified": False})
                    inventory.update({key: record[key] for key in (
                        "sdk", "version", "target", "kind", "profile_id", "headers", "prefix",
                        "required_superclasses", "subclass_probes", "clang_sha256", "sdk_settings_sha256")})
                    inventory_path = output / record["inventory"]["path"]
                    inventory_path.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
                    record["inventory"]["sha256"] = digest(inventory_path, deadline.check)
                    record["status"] = "success"
                except Exception as error:
                    record.update(status="failed", error=str(error))
                    raise
        deadline.check()
        check_source_profiles(evidence["source_profiles"], sdk_version)
        evidence["status"] = "success"
    except Exception as error:
        evidence["status"], evidence["error"] = "failed", str(error)
        raise
    finally:
        (output / "sdk-declarations.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sdk-version", required=True)
    args = parser.parse_args()
    try:
        collect(args.output.resolve(), args.sdk_version)
    except Exception as error:
        print(f"SDK declaration collection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
