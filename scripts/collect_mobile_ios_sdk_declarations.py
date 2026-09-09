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
import subprocess
import sys


# SDK 26.5's complete Messages import produces about 263 MiB of JSON. Keep
# the full declaration closure and a finite ceiling above that measured input.
MAX_AST_BYTES = 384 * 1024 * 1024
IMPORTS = {"Messages/Messages.h": "MSStickerBrowserViewController",
           "UserNotifications/UserNotifications.h": "UNNotificationServiceExtension"}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def declarations(ast, required_class):
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
    if required_class not in classes:
        raise ValueError("SDK imports did not declare the requested system superclass")
    # Protocol names and C struct/enum tags have separate namespaces. Members
    # and method selectors also must not become false class-name conflicts.
    # ObjCInterfaceDecl also represents @class forward declarations. These
    # names occupy the namespace but are not all usable as superclasses.
    return {"interface_declarations": sorted(classes), "ordinary_identifiers": sorted(names)}


def collect(output, sdk_version):
    if os.environ.get("GITHUB_ACTIONS") != "true" or sys.platform != "darwin":
        raise RuntimeError("SDK declaration collection must run on macOS GitHub Actions")
    output.mkdir(parents=True, exist_ok=False)
    evidence = {"schema_version": 1, "scope": "sdk-declarations-only", "status": "incomplete",
                "imports": list(IMPORTS), "commands": [], "sdks": [],
                "consumer_commit": os.environ.get("CONSUMER_COMMIT")}

    def command(name, argv):
        stdout, stderr = output / (name + ".stdout"), output / (name + ".stderr")
        record = {"name": name, "argv": [str(value) for value in argv],
                  "stdout": stdout.name, "stderr": stderr.name, "exitcode": None}
        evidence["commands"].append(record)
        env = os.environ.copy()
        for name in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
            env.pop(name, None)
        with stdout.open("wb") as out, stderr.open("wb") as err:
            completed = subprocess.run(argv, cwd=output, stdout=out, stderr=err,
                                       env=env, timeout=120, check=False)
        record["exitcode"] = completed.returncode
        record["stdout_sha256"], record["stderr_sha256"] = digest(stdout), digest(stderr)
        if completed.returncode:
            raise RuntimeError(f"{record['name']} exited {completed.returncode}; see retained logs")
        if stdout.stat().st_size > MAX_AST_BYTES:
            raise RuntimeError("SDK declaration evidence exceeds its parsing budget")
        return stdout

    try:
        for sdk, target in (("iphoneos", "arm64-apple-ios18.0"),
                            ("iphonesimulator", "arm64-apple-ios18.0-simulator")):
            prefix = ["xcrun", "--toolchain", "XcodeDefault", "--sdk", sdk]
            version = command(sdk + "-version", [*prefix, "--show-sdk-version"]).read_text().strip()
            if version != sdk_version:
                raise RuntimeError(f"{sdk} SDK version differs from the configured {sdk_version}")
            root = Path(command(sdk + "-root", [*prefix, "--show-sdk-path"]).read_text().strip())
            clang = Path(command(sdk + "-clang", [*prefix, "--find", "clang"]).read_text().strip())
            command(sdk + "-compiler-version", [clang, "--version"])
            args = [clang, "-x", "objective-c", "-std=gnu11", "-fobjc-arc", "-fno-modules",
                    "-target", target, "-isysroot", root, "-fsyntax-only"]
            for header, superclass in IMPORTS.items():
                # Keep each import's closure separate: a UserNotifications-only
                # source must not inherit false conflicts from UIKit/Messages.
                name = sdk + "-" + header.split("/")[0]
                source = output / (name + ".m")
                source.write_text(f"#import <{header}>\n", encoding="utf-8")
                ast_path = command(name + "-ast", [*args, "-Xclang", "-ast-dump=json", source])
                inventory = declarations(json.loads(ast_path.read_text(encoding="utf-8")), superclass)
                probe = output / (name + "-subclass.m")
                probe.write_text(f"#import <{header}>\n"
                                 f"@interface NeverDSDKDeclarationProbe : {superclass}\n@end\n",
                                 encoding="utf-8")
                command(name + "-subclass", [*args, probe])
                inventory.update({"sdk": sdk, "version": version, "target": target, "header": header,
                                  "clang_sha256": digest(clang),
                                  "sdk_settings_sha256": digest(root / "SDKSettings.json"),
                                  "ast_sha256": digest(ast_path), "source_sha256": digest(source),
                                  "subclass_declaration_probe": {"superclass": superclass,
                                      "source": probe.name, "sha256": digest(probe),
                                      "status": "success", "instance_layout_verified": False}})
                inventory_path = output / (name + "-declarations.json")
                inventory_path.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
                evidence["sdks"].append({"sdk": sdk, "header": header, "inventory": inventory_path.name,
                                         "sha256": digest(inventory_path)})
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
