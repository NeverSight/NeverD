#!/usr/bin/env python3
"""Collect complete, failure-preserving real-app evidence in GitHub Actions."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

try:
    from .mobile_real_apps_common import CaseContext, digest, load_json, safe_relative
    from .mobile_real_apps_matrix import generate
except ImportError:
    from mobile_real_apps_common import CaseContext, digest, load_json, safe_relative
    from mobile_real_apps_matrix import generate


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("mobile_real_apps.json"))
    parser.add_argument("--case", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--consumer-commit", required=True)
    parser.add_argument("--timeout", type=int, default=4200)
    parser.add_argument("--toolchain-receipt", type=Path,
                        help="This runner's fresh Swift comparison installation receipt")
    args = parser.parse_args(argv)
    # This entry point is intentionally cloud-only. Unit tests exercise pure
    # parsers/mocked boundaries; app downloads and executions happen in Actions.
    if os.environ.get("GITHUB_ACTIONS") != "true":
        parser.error("Real application builds and acceptance run only in GitHub Actions")
    if not re.fullmatch(r"[0-9a-f]{40}", args.consumer_commit):
        parser.error("--consumer-commit must be an exact Git commit")
    manifest = load_json(args.manifest)
    if manifest.get("schema_version") != 1:
        parser.error("Unsupported manifest schema")
    generate(manifest)
    variants = [row for row in manifest["required_cases"] if row["id"] == args.case]
    if len(variants) != 1:
        parser.error("Case must occur exactly once in required_cases")
    variant = variants[0]
    app = manifest["apps"][variant["app"]]
    ctx = CaseContext(app=app, variant=variant, source=args.source, work=args.work_dir,
                      neverd=args.neverd, timeout=args.timeout,
                      consumer_commit=args.consumer_commit, manifest_sha256=digest(args.manifest))
    try:
        root = Path(__file__).resolve().parent.parent
        head = ctx.command("consumer-source-head", ["git", "rev-parse", "HEAD"], cwd=root).stdout.strip()
        if head != args.consumer_commit:
            raise ValueError("NeverD checkout does not match the requested consumer commit")
        source_head = ctx.command("upstream-source-head", ["git", "rev-parse", "HEAD"], cwd=ctx.source).stdout.strip()
        if source_head != app["source_commit"]:
            raise ValueError("Upstream checkout does not match the pinned application commit")
        if not ctx.neverd.is_file():
            raise ValueError("NeverD build artifact is missing")
        build_receipt = load_json(ctx.neverd.parent / "consumer-build.json")
        if build_receipt.get("consumer_commit") != head or build_receipt.get("neverd_sha256") != digest(ctx.neverd):
            raise ValueError("NeverD binary receipt differs from the requested source or bytes")
        runtime_files = build_receipt.get("runtime_files")
        if not isinstance(runtime_files, list) or not runtime_files:
            raise ValueError("NeverD build receipt lacks native runtime identities")
        recorded_paths = set()
        for row in runtime_files:
            name = row["path"]
            if name in recorded_paths:
                raise ValueError("Duplicate native runtime receipt entry")
            recorded_paths.add(name)
            member = ctx.neverd.parent / safe_relative(name)
            if not member.is_file() or member.is_symlink() or not member.resolve().is_relative_to(ctx.neverd.parent):
                raise ValueError("Native runtime receipt entry is missing or unsafe")
            if row.get("sha256") != digest(member) or row.get("size") != member.stat().st_size:
                raise ValueError(f"Native runtime receipt mismatch: {name}")
        actual_paths = {path.relative_to(ctx.neverd.parent).as_posix()
                        for path in ctx.neverd.parent.rglob("*") if path.is_file()
                        and path.name != "consumer-build.json"}
        if actual_paths != recorded_paths or "neverd" not in actual_paths:
            raise ValueError("Native runtime files differ from the complete build receipt")
        shutil.copyfile(ctx.neverd.parent / "consumer-build.json", ctx.work / "consumer-build.json")
        license_file = ctx.source / safe_relative(app["license_path"])
        if not license_file.is_file() or not license_file.resolve().is_relative_to(ctx.source):
            raise ValueError("Pinned source license is missing or outside its checkout")
        shutil.copyfile(license_file, ctx.work / "UPSTREAM-LICENSE.txt")
        ctx.write_json("input-provenance.json", {"app": app, "variant": variant,
                       "consumer_commit": head, "neverd_sha256": digest(ctx.neverd),
                       "manifest_sha256": digest(args.manifest), "license_sha256": digest(license_file)})
        if variant["platform"] == "android":
            if args.toolchain_receipt:
                raise ValueError("Swift toolchain receipt cannot be applied to an Android case")
            try:
                from .mobile_real_apps_android import run_android
            except ImportError:
                from mobile_real_apps_android import run_android
            run_android(ctx)
        elif variant["platform"] == "ios":
            try:
                from .mobile_real_apps_ios import run_ios
            except ImportError:
                from mobile_real_apps_ios import run_ios
            run_ios(ctx, toolchain_receipt=args.toolchain_receipt, consumer_commit=args.consumer_commit)
        else:
            raise ValueError("Unsupported application platform")
    except Exception as error:
        ctx.fail(f"{type(error).__name__}: {error}")
    finally:
        result = ctx.finish()
    print(f"{args.case}: {result['status']}; evidence: {ctx.work / 'result.json'}")
    for reason in result["failures"]:
        print(reason, file=sys.stderr)
    return 0 if result["status"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
