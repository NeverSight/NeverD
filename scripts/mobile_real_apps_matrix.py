#!/usr/bin/env python3
"""Generate the entire Actions app matrix from its pinned manifest.

This module reads configuration and writes JSON only. It never downloads,
builds or executes an application. baseline_cases describe initial evidence;
they are never a selection filter for required_cases.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


class MatrixError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise MatrixError(message)


def mapping(value, label):
    require(isinstance(value, dict), f"{label} must be an object")
    return value


def token(value, pattern, label):
    require(isinstance(value, str) and re.fullmatch(pattern, value) is not None,
            f"{label} is missing, floating or invalid")
    return value


def android_row(case, app):
    config = mapping(app.get("android"), "Android configuration")
    require(case["architecture"] == "dex", "Android matrix architecture must identify DEX input")
    sdk = config.get("compile_sdk")
    require(type(sdk) is int and sdk > 0 and case["sdk"] == str(sdk),
            "Android case SDK differs from its pinned compile_sdk")
    tools = token(config.get("build_tools"), r"[0-9]+\.[0-9]+\.[0-9]+", "inventory build_tools")
    java = token(config.get("java"), r"[1-9][0-9]*", "Android Java version")
    row = {"java_version": java, "compile_sdk": sdk, "build_tools": tools}
    packages = {f"build-tools;{tools}", f"platforms;android-{sdk}"}
    source = config.get("source_build")
    if source is not None:
        source = mapping(source, "Android source_build")
        source_tools = token(source.get("build_tools"), r"[0-9]+\.[0-9]+\.[0-9]+", "source build_tools")
        jdk = source.get("jdk_major")
        require(type(jdk) is int and jdk > 0 and str(jdk) == java,
                "source_build JDK differs from Android Java setup")
        packages.add(f"build-tools;{source_tools}")
    profile = case["profile"]
    if profile == "official-release":
        asset = mapping(app.get("official_apk"), "official APK")
        token(asset.get("sha256"), r"[0-9a-f]{64}", "official APK SHA256")
        require(isinstance(asset.get("url"), str) and asset["url"].startswith("https://")
                and not any(char.isspace() for char in asset["url"]), "official APK requires a pinned HTTPS URL")
    elif profile in ("gradle-release", "gradle-debug"):
        require(source is not None, "Gradle case lacks source_build configuration")
        profiles = mapping(source.get("profiles"), "source_build profiles")
        declared = mapping(profiles.get(profile), f"source_build profile {profile}")
        variant = token(declared.get("variant"), r"[A-Za-z][A-Za-z0-9]*", "Gradle variant")
        key = "release_task" if profile == "gradle-release" else "debug_task"
        task = token(config.get(key), r"(?::[A-Za-z0-9_][A-Za-z0-9_.-]*)+", f"Android {key}")
        require(task.rsplit(":", 1)[-1] == "assemble" + variant[0].upper() + variant[1:],
                "Gradle task differs from the pinned source_build variant")
        row["gradle_task"] = task
    else:
        raise MatrixError(f"unknown Android case profile: {profile}")
    return row, packages


def ios_row(case, app):
    config = mapping(app.get("ios"), "iOS configuration")
    require(case["profile"] in ("release", "size"), "unknown iOS case profile")
    require((case["architecture"], case["sdk"]) in {
        ("arm64", "iphonesimulator"), ("x86_64", "iphonesimulator"), ("arm64", "iphoneos")},
        "unsupported iOS SDK/architecture combination")
    xcode = token(config.get("xcode"), r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", "Xcode version")
    return {"xcode": xcode, "developer_dir": f"/Applications/Xcode_{xcode}.app/Contents/Developer"}


def generate(manifest):
    mapping(manifest, "manifest")
    require(type(manifest.get("schema_version")) is int and manifest["schema_version"] == 1,
            "unsupported manifest schema")
    apps = mapping(manifest.get("apps"), "manifest apps")
    cases = manifest.get("required_cases")
    require(isinstance(cases, list) and cases, "required_cases must contain the complete nonempty matrix")
    matrices, seen, used, sdk_packages = {"android": [], "ios": []}, set(), set(), set()
    for case in cases:
        mapping(case, "required case")
        case_id = token(case.get("id"), r"[a-z0-9][a-z0-9_-]*", "case id")
        require(case_id not in seen, f"duplicate required case id: {case_id}")
        seen.add(case_id)
        app_id = token(case.get("app"), r"[a-z0-9][a-z0-9_-]*", "case app id")
        require(app_id in apps, f"unknown app for required case: {case_id}")
        app = mapping(apps[app_id], f"app {app_id}")
        used.add(app_id)
        platform = case.get("platform")
        require(platform in matrices, f"unknown platform for required case: {case_id}")
        for key in ("profile", "architecture", "sdk"):
            token(case.get(key), r"[A-Za-z0-9_.-]+", f"case {key}")
        row = {"case_id": case_id, "app": app_id, "platform": platform,
               "repository": token(app.get("repository"), r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", "app repository"),
               "source_commit": token(app.get("source_commit"), r"[0-9a-f]{40}", "app source commit"),
               "profile": case["profile"], "architecture": case["architecture"], "sdk": case["sdk"],
               "variant": dict(case)}
        if platform == "android":
            fields, packages = android_row(case, app)
            row.update(fields)
            sdk_packages.update(packages)
        else:
            row.update(ios_row(case, app))
        # Feature implementation or qualification status must never decide
        # whether a required case is scheduled. Such cases report their gaps.
        matrices[platform].append(row)
    require(used == set(apps), "manifest app has no required case")
    require(all(0 < len(rows) <= 256 for rows in matrices.values()),
            "each platform requires 1..256 cases; do not silently truncate the Actions matrix")
    baseline = manifest.get("baseline_cases")
    require(isinstance(baseline, list) and all(isinstance(item, str) for item in baseline),
            "baseline_cases must be an explicit list")
    require(len(set(baseline)) == len(baseline) and set(baseline) <= seen,
            "baseline_cases contain duplicates or cases outside required_cases")
    for row in matrices["android"]:
        # Every Android job has both the inventory and source-build tools from
        # the same manifest, including the other application's compile SDK.
        row["sdk_packages"] = sorted(sdk_packages)
    return {"schema_version": 1, "android_matrix": {"include": matrices["android"]},
            "ios_matrix": {"include": matrices["ios"]}, "required_case_ids": sorted(seen),
            "baseline_case_ids": list(baseline), "required_case_count": len(seen),
            "baseline_case_count": len(baseline)}


def load_manifest(path):
    require(path.stat().st_size <= 4 * 1024 * 1024, "manifest exceeds configuration size limit")
    data = path.read_bytes()

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON field: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise MatrixError(f"invalid JSON number: {value}")

    manifest = json.loads(data, object_pairs_hook=unique, parse_constant=invalid_constant)
    result = generate(manifest)
    result["manifest_sha256"] = hashlib.sha256(data).hexdigest()
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("mobile_real_apps.json"))
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = load_manifest(args.manifest)
        outputs = "".join(f"{name}={json.dumps(result[name], separators=(',', ':'), ensure_ascii=True)}\n"
                          for name in ("android_matrix", "ios_matrix"))
        require(len(outputs.encode("utf-16-le")) <= 1024 * 1024, "matrix exceeds GitHub job output limit")
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as stream:
                stream.write(outputs)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
