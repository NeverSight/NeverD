"""CLI policy and transactional output publication for mobile recovery."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

from .common import Limits, MobileError, validate_tree, workspace_budget


def detect_platform(source: Path) -> str:
    if source.is_dir():
        return "ios" if source.suffix.lower() == ".app" else "android"
    suffix = source.suffix.lower()
    if suffix in (".apk", ".dex", ".smali"):
        return "android"
    if suffix == ".ipa":
        return "ios"
    with source.open("rb") as stream:
        magic = stream.read(4)
    if magic in (b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                 b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                 b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
        return "ios"
    raise MobileError("unsupported input; expected APK, DEX, smali, IPA, .app, or Mach-O")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="neverd mobile", description="Recover mobile application sources and metadata")
    result.add_argument("input", type=Path)
    result.add_argument("-o", "--output", required=True, type=Path, help="new output directory")
    result.add_argument("--platform", choices=("auto", "android", "ios"), default="auto")
    result.add_argument("--jadx", help="explicit external Android backend executable; default uses the builtin engine")
    result.add_argument("--neverd", default="neverd", help=argparse.SUPPRESS)
    result.add_argument("--arch", choices=("auto", "arm64", "arm", "x86_64", "i386"), default="auto")
    result.add_argument("--artifact", help="iOS executable path relative to application bundle")
    result.add_argument("--swift-demangle", help="iOS Swift demangler executable (default: NEVERD_SWIFT_DEMANGLE, PATH, or Apple toolchain)")
    result.add_argument("--metadata-only", action="store_true", help="recover iOS metadata without native decompilation")
    result.add_argument("--max-func", type=int, default=0, help="maximum native functions; 0 means all")
    result.add_argument("--timeout", type=int, default=300, help="seconds for builtin analysis or each backend process")
    result.add_argument("--max-files", type=int, default=20000)
    result.add_argument("--max-bytes", type=int, default=2 * 1024 * 1024 * 1024)
    result.add_argument("--json", action="store_true", help="print the report as JSON")
    return result


def recover(args: argparse.Namespace) -> dict:
    limits = Limits(args.timeout, args.max_files, args.max_bytes)
    if args.max_func < 0:
        raise MobileError("--max-func must be nonnegative")
    source = args.input.absolute()
    if source.is_symlink() or not (source.is_file() or source.is_dir()):
        raise MobileError("input must be a regular file or directory, not a symbolic link")
    source = source.resolve()
    if source.is_file() and source.stat().st_size > limits.max_bytes:
        raise MobileError("input exceeds the size limit")
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise MobileError("output already exists; choose a new directory")
    output = output.resolve()
    if source.is_dir() and (source == output or source in output.parents):
        raise MobileError("output must be outside the input directory")
    platform = detect_platform(source) if args.platform == "auto" else args.platform
    if platform == "android" and (args.metadata_only or args.artifact or args.arch != "auto" or args.max_func or args.swift_demangle is not None):
        raise MobileError("--metadata-only, --artifact, --arch, --max-func and --swift-demangle apply only to iOS")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".neverd-mobile-", dir=output.parent))
    try:
        with workspace_budget(staging, limits):
            if platform == "android":
                from .android import decompile_android
                report = decompile_android(source, staging, jadx=args.jadx, limits=limits)
            else:
                from .ios import decompile_ios
                report = decompile_ios(source, staging, neverd=args.neverd, arch=args.arch,
                                       artifact=args.artifact, metadata_only=args.metadata_only,
                                       max_func=args.max_func, limits=limits,
                                       swift_demangle=args.swift_demangle)
        # Do not publish transient paths or private input locations in reports.
        report.update(schema_version=1, source=source.name, platform=platform, status="success")
        payload = json.dumps(report, indent=2, ensure_ascii=True) + "\n"
        if (staging / "report.json").exists():
            raise MobileError("backend output conflicts with the recovery report")
        validate_tree(staging, limits, extra_bytes=len(payload.encode("utf-8")), extra_files=1)
        (staging / "report.json").write_text(payload, encoding="utf-8")
        validate_tree(staging, limits)
        if os.name == "nt":
            # Windows rename refuses an existing destination atomically.
            staging.rename(output)
        else:
            # POSIX rename replaces empty directories. Reserve the name first
            # so a preexisting user directory is never replaced.
            output.mkdir()
            try:
                staging.rename(output)
            except BaseException:
                output.rmdir()
                raise
        return report
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        report = recover(args)
    except (MobileError, OSError, ValueError) as exc:
        if args.json:
            print(json.dumps({"schema_version": 1, "status": "error", "error": str(exc)}))
        else:
            print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("error: mobile recovery interrupted", file=sys.stderr)
        return 130
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=True))
    else:
        print(f"Mobile recovery written to {args.output}\nReport: {args.output / 'report.json'}")
    return 0
