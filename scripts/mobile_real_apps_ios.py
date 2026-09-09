"""Collect real iOS application evidence in GitHub Actions, without false green gates.

This is CI orchestration, not a production decompiler. Apple tool output is an
independent observation; it is never substituted with NeverD's own inventory.
Raw native/Objective-C inventories are not yet reconciled into complete callable
denominators. Independent source rebuild and behavioral comparison are also
explicitly incomplete, even when every command and upstream app build succeeds.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import time


REQUIRED_STAGES = ("provenance", "original_build", "inventory", "recovery", "recompile", "behavior")
MAX_BUNDLE_FILES = 100_000
MAX_ARTIFACTS = 256
MAX_TEXT_BYTES = 32 * 1024 * 1024
MAX_SWIFT_SYMBOLS = 20_000
MAX_DEMANGLE_BATCHES = 128
MAX_PRESERVED_BYTES = 4 * 1024 * 1024 * 1024
ARTIFACT_INVENTORY_SECONDS = 120
ARTIFACT_RECOVERY_SECONDS = 180
MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}
CALLABLE_ROLES = {
    "Function", "Getter", "Setter", "ModifyAccessor", "ReadAccessor", "Allocator",
    "Constructor", "Destructor", "Deallocator", "TypeMetadataAccessFunction",
    "DispatchThunk", "CurryThunk", "VTableThunk", "ProtocolWitness",
    "MaterializeForSet", "WillSet", "DidSet", "GlobalGetter",
    "UnsafeAddressor", "UnsafeMutableAddressor", "OwningAddressor",
    "OwningMutableAddressor", "NativeOwningAddressor", "NativeOwningMutableAddressor",
}
NONCALLABLE_ROLES = {
    "NominalTypeDescriptor", "ProtocolDescriptor", "ProtocolConformanceDescriptor",
    "PropertyDescriptor", "TypeMetadata", "FullTypeMetadata", "GenericTypeMetadataPattern",
    "Metaclass", "FieldOffset", "ReflectionMetadataFieldDescriptor",
    "ReflectionMetadataBuiltinDescriptor", "ReflectionMetadataAssocTypeDescriptor",
    "AssociatedTypeDescriptor", "ModuleDescriptor", "ExtensionDescriptor",
    "AnonymousDescriptor", "ProtocolSelfConformanceDescriptor",
}


class EvidenceError(RuntimeError):
    pass


class Session:
    def __init__(self, ctx):
        self.ctx = ctx
        self.deadline = min(time.monotonic() + float(ctx.timeout), getattr(ctx, "deadline", float("inf")))
        self.stages = {}
        self.preserved_bytes = 0

    def remaining(self, deadline=None):
        remaining = min(self.deadline, deadline or self.deadline) - time.monotonic()
        if remaining <= 0:
            raise EvidenceError("total case or artifact evidence budget exhausted")
        return remaining

    def command(self, name, argv, *, cwd=None, cap=60, deadline=None, optional=False):
        timeout = min(cap, self.remaining(deadline))
        if argv[0] == "xcrun":
            argv = ["xcrun", "--toolchain", "XcodeDefault", *argv[1:]]
        result = self.ctx.command(name, [str(arg) for arg in argv], cwd=cwd,
                                  timeout=timeout, allow_failure=True)
        if result.returncode and not optional:
            raise EvidenceError(f"{name} exited {result.returncode}; see saved command logs")
        return result

    def stage(self, name, status, **details):
        if status == "success":
            evidence = details.get("evidence")
            if not isinstance(evidence, list) or not evidence:
                raise EvidenceError(f"successful {name} stage has no evidence files")
            for member in evidence:
                path = relative_path(self.ctx.work, member)
                if not path.is_file() or path.is_symlink():
                    raise EvidenceError(f"successful {name} stage evidence is not a standalone file: {member}")
        self.stages[name] = status
        self.ctx.stage(name, status, **details)

    def preserve(self, source, destination):
        info = source.stat()
        if not stat.S_ISREG(info.st_mode):
            raise EvidenceError(f"cannot preserve a nonregular evidence file: {source}")
        if info.st_size > MAX_PRESERVED_BYTES - self.preserved_bytes:
            raise EvidenceError("preserved app/build evidence byte budget exceeded")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.open("rb") as original, destination.open("xb") as output:
            while chunk := original.read(1024 * 1024):
                self.remaining()
                if len(chunk) > MAX_PRESERVED_BYTES - self.preserved_bytes:
                    raise EvidenceError("preserved app/build evidence byte budget exceeded")
                output.write(chunk)
                self.preserved_bytes += len(chunk)
        destination.chmod(stat.S_IMODE(info.st_mode))


def relative_path(root, value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise EvidenceError("manifest path must be a nonempty relative POSIX path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise EvidenceError(f"manifest path escapes its root: {value!r}")
    result = root.joinpath(*path.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise EvidenceError(f"manifest path resolves outside its root: {value!r}")
    return result


def digest(path, session):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            session.remaining()
            value.update(chunk)
    return value.hexdigest()


def read_json(path):
    if path.stat().st_size > MAX_TEXT_BYTES:
        raise EvidenceError(f"JSON evidence exceeds {MAX_TEXT_BYTES} bytes: {path.name}")
    return json.loads(path.read_text(encoding="utf-8"))


def bounded_text(result):
    if len(result.stdout.encode("utf-8")) > MAX_TEXT_BYTES:
        raise EvidenceError("tool output too large for in-memory evidence analysis; raw log retained")
    return result.stdout


def build_arguments(ctx, derived, packages):
    variant = ctx.variant
    profile = variant["profile"]
    if profile not in ("release", "size"):
        raise EvidenceError(f"unsupported explicit iOS profile: {profile}")
    sdk, architecture = variant["sdk"], variant["architecture"]
    if sdk not in ("iphoneos", "iphonesimulator") or architecture not in ("arm64", "x86_64"):
        raise EvidenceError("unsupported explicit iOS SDK/architecture")
    if sdk == "iphoneos" and architecture != "arm64":
        raise EvidenceError("x86_64 is not an iOS device architecture")
    project = relative_path(ctx.source, ctx.app["ios"]["project"])
    destination = "generic/platform=iOS" if sdk == "iphoneos" else "generic/platform=iOS Simulator"
    argv = [
        "xcodebuild", "-project", project, "-scheme", ctx.app["ios"]["scheme"],
        "-configuration", "Release", "-sdk", sdk, "-destination", destination,
        "-derivedDataPath", derived, "-clonedSourcePackagesDirPath", packages,
        "-onlyUsePackageVersionsFromResolvedFile", f"ARCHS={architecture}", "ONLY_ACTIVE_ARCH=NO",
        "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "DEVELOPMENT_TEAM=",
        "BUNDLE_ID_PREFIX=org.neverd.realapps", "DEBUG_INFORMATION_FORMAT=dwarf-with-dsym",
        "GCC_GENERATE_DEBUGGING_SYMBOLS=YES", "LD_GENERATE_MAP_FILE=YES",
        "LD_MAP_FILE_PATH=$(TARGET_TEMP_DIR)/$(PRODUCT_NAME)-$(CURRENT_ARCH)-$(CONFIGURATION)-LinkMap.txt",
        "SWIFT_OPTIMIZATION_LEVEL=" + ("-Osize" if profile == "size" else "-O"),
    ]
    if profile == "size":
        argv.append("GCC_OPTIMIZATION_LEVEL=s")
    return argv


def preserve_bundle(bundle, destination, session):
    """Keep the app outside scratch, rewriting only internal symlink locations."""
    destination.mkdir(parents=True, exist_ok=False)
    entries = 0
    def walk_error(error):
        raise error

    for directory, directories, files in os.walk(bundle, followlinks=False, onerror=walk_error):
        session.remaining()
        for name in sorted([*directories, *files]):
            entries += 1
            if entries > MAX_BUNDLE_FILES:
                raise EvidenceError("app evidence entry budget exceeded")
            source = Path(directory) / name
            target = destination / source.relative_to(bundle)
            if source.is_symlink():
                resolved = source.resolve(strict=True)
                if not resolved.is_relative_to(bundle.resolve()):
                    raise EvidenceError("built app symlink resolves outside the app bundle")
                target.parent.mkdir(parents=True, exist_ok=True)
                new_target = destination / resolved.relative_to(bundle.resolve())
                target.symlink_to(os.path.relpath(new_target, target.parent), target_is_directory=resolved.is_dir())
            elif source.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                session.preserve(source, target)


def resolve_dependencies(session, packages, lock):
    pins = lock.get("pins")
    if not isinstance(pins, list):
        raise EvidenceError("Package.resolved lacks a supported pins array")
    expected = {}
    for pin in pins:
        identity, state = pin.get("identity"), pin.get("state", {})
        revision = state.get("revision")
        if not isinstance(identity, str) or identity in expected or not isinstance(revision, str) \
                or not re.fullmatch(r"[0-9a-fA-F]{40,64}", revision):
            raise EvidenceError("package pin lacks a unique identity and immutable revision")
        expected[identity] = {"revision": revision.lower(), "location": pin.get("location")}
    if not expected:
        return [], []
    state_path = packages / "workspace-state.json"
    if not state_path.is_file():
        return [], ["SwiftPM workspace-state.json missing; actual checkout revisions unverified"]
    state = read_json(state_path)
    dependencies = state.get("object", state).get("dependencies", [])
    records, issues, seen = [], [], set()
    for index, dependency in enumerate(dependencies):
        session.remaining()
        identity = dependency.get("packageRef", {}).get("identity")
        if identity not in expected:
            continue
        if identity in seen:
            issues.append(f"duplicate SwiftPM dependency identity: {identity}")
            continue
        seen.add(identity)
        checkout = relative_path(packages / "checkouts", dependency.get("subpath"))
        result = session.command(f"dependency-{index:04d}-head", ["git", "rev-parse", "HEAD"],
                                 cwd=checkout, optional=True)
        actual = result.stdout.strip() if result.returncode == 0 else None
        matched = actual == expected[identity]["revision"]
        records.append({"identity": identity, **expected[identity], "actual_revision": actual,
                        "checkout": str(checkout), "matched": matched})
        if not matched:
            issues.append(f"resolved revision does not match locked dependency: {identity}")
    issues.extend(f"locked dependency checkout missing: {identity}" for identity in sorted(set(expected) - seen))
    return records, issues


def bundle_artifacts(bundle, session):
    """Inspect all ordinary bundle files, retaining failures instead of losing scope."""
    artifacts, issues, seen = [], [], set()
    count = 0

    def walk_error(error):
        issues.append(f"bundle traversal error: {error}")

    for directory, directories, files in os.walk(bundle, followlinks=False, onerror=walk_error):
        session.remaining()
        directories.sort()
        files.sort()
        for name in directories:
            path = Path(directory) / name
            if path.is_symlink():
                issues.append(f"directory symlink not traversed: {path.relative_to(bundle)}")
        for name in files:
            count += 1
            if count > MAX_BUNDLE_FILES:
                issues.append("bundle file enumeration limit reached; remaining files are unknown")
                return artifacts, issues
            path = Path(directory) / name
            try:
                resolved = path.resolve(strict=True)
                if not resolved.is_relative_to(bundle.resolve()):
                    issues.append(f"bundle file resolves outside bundle: {path.relative_to(bundle)}")
                    continue
                if not stat.S_ISREG(resolved.stat().st_mode):
                    issues.append(f"nonregular bundle member: {path.relative_to(bundle)}")
                    continue
                with resolved.open("rb") as stream:
                    magic = stream.read(4)
                if magic not in MACHO_MAGICS:
                    continue
                relative = path.relative_to(bundle).as_posix()
                if resolved in seen:
                    for item in artifacts:
                        if item["resolved"] == str(resolved):
                            item["aliases"].append(relative)
                            break
                    continue
                if len(artifacts) >= MAX_ARTIFACTS:
                    issues.append("Mach-O artifact limit reached; remaining artifacts are unknown")
                    return artifacts, issues
                seen.add(resolved)
                artifacts.append({"path": relative, "resolved": str(resolved), "aliases": [],
                                  "size": resolved.stat().st_size, "sha256": digest(resolved, session)})
            except (OSError, RuntimeError) as error:
                issues.append(f"cannot inspect bundle member {path.relative_to(bundle)}: {error}")
    return artifacts, issues


def swift_symbols(nm_output):
    # Keep defined symbols and their independent addresses; undefined imports
    # are dependency evidence, not recovered app functions.
    entries = []
    for line in nm_output.splitlines():
        match = re.match(r"^\s*([0-9a-fA-F]{8,16})\s+.*?((?:_?\$[sS]|_+T)\S+)\s*$", line)
        if match:
            entries.append({"entry": hex(int(match[1], 16)), "mangled_symbol": match[2]})
    return entries


def demangle_roles(output, expected):
    blocks = re.split(r"(?m)^Demangling for (.+)\r?$", output)
    records = {}
    for index in range(1, len(blocks), 2):
        symbol, tree = blocks[index], blocks[index + 1]
        if symbol not in expected or symbol in records:
            raise EvidenceError("official demangler returned an unexpected or duplicate symbol")
        top = re.findall(r"(?m)^  kind=([A-Za-z0-9_]+)(?:,.*)?$", tree)
        roles = [role for role in top if role not in ("FunctionSignatureSpecialization", "GenericSpecialization")]
        role = roles[0] if len(roles) == 1 and re.search(r"(?m)^kind=Global$", tree) else None
        classification = ("callable" if role in CALLABLE_ROLES else
                          "metadata" if role in NONCALLABLE_ROLES else "unknown")
        records[symbol] = {"node_kind": role, "classification": classification,
                           "async_tree_evidence": "kind=AsyncAnnotation" in tree}
    if set(records) != set(expected):
        raise EvidenceError("official demangler omitted symbol identities")
    return records


def artifact_inventory(session, item, index, architecture):
    binary = Path(item["resolved"])
    prefix = f"artifact-{index:04d}"
    deadline = min(session.deadline, time.monotonic() + ARTIFACT_INVENTORY_SECONDS)
    outputs, issues = {}, []
    commands = {
        "load-commands": ["xcrun", "otool", "-arch", architecture, "-l", binary],
        "nm": ["xcrun", "nm", "-arch", architecture, "-nm", binary],
        "function-starts": ["xcrun", "dyld_info", "-arch", architecture, "-function_starts", binary],
        "objc": ["xcrun", "dyld_info", "-arch", architecture, "-objc", binary],
        "objc-otool": ["xcrun", "otool", "-arch", architecture, "-ov", binary],
        "dependencies": ["xcrun", "otool", "-arch", architecture, "-L", binary],
        "imports": ["xcrun", "dyld_info", "-arch", architecture, "-imports", binary],
        "uuid": ["xcrun", "dwarfdump", "--uuid", binary],
    }
    for label, command in commands.items():
        try:
            result = session.command(prefix + "-" + label, command, cap=30, deadline=deadline, optional=True)
            if result.returncode:
                issues.append(f"Apple {label} exited {result.returncode}")
            else:
                outputs[label] = bounded_text(result)
        except Exception as error:
            issues.append(f"Apple {label}: {error}")
    starts = sorted({hex(int(value, 16)) for value in
                     re.findall(r"(?m)^\s*(0x[0-9A-Fa-f]+)\s+", outputs.get("function-starts", ""))})
    if not starts:
        issues.append("no independently enumerated native function starts")
    symbols = swift_symbols(outputs.get("nm", ""))
    unique_symbols = list(dict.fromkeys(row["mangled_symbol"] for row in symbols))
    roles = {}
    if len(unique_symbols) > MAX_SWIFT_SYMBOLS:
        issues.append("Swift symbol budget exceeded; excess identities remain unknown")
    selected = unique_symbols[:MAX_SWIFT_SYMBOLS]
    cursor = 0
    for batch_index in range(MAX_DEMANGLE_BATCHES):
        if cursor == len(selected):
            break
        batch, size = [], 0
        while cursor < len(selected) and len(batch) < 64:
            symbol = selected[cursor]
            if len(symbol.encode()) > 16000:
                issues.append("oversized Swift symbol retained as unknown")
                cursor += 1
                continue
            if batch and size + len(symbol.encode()) > 48_000:
                break
            batch.append(symbol)
            size += len(symbol.encode())
            cursor += 1
        if not batch:
            continue
        try:
            result = session.command(prefix + f"-swift-{batch_index:04d}",
                                     ["xcrun", "swift-demangle", "--expand", "--tree-only", *batch],
                                     cap=20, deadline=deadline)
            roles.update(demangle_roles(bounded_text(result), batch))
        except Exception as error:
            issues.append(f"official Swift demangler batch {batch_index}: {error}")
            break
    for row in symbols:
        row.update(roles.get(row["mangled_symbol"], {"classification": "unknown", "node_kind": None}))
    unknown = sum(row["classification"] == "unknown" for row in symbols)
    if unknown:
        issues.append(f"{unknown} Swift symbol identities have unknown independent roles")
    # Neither raw text nor LC_FUNCTION_STARTS alone certifies an exhaustive
    # native/Objective-C callable identity inventory (aliases, methods, thunks).
    issues.extend([
        "native function-start observations are not yet an exhaustive callable denominator",
        "exact Objective-C method identities from Apple metadata are not yet reconciled",
    ])
    return {"artifact": item["path"], "status": "incomplete", "issues": issues,
            "native_function_start_addresses": starts, "native_denominator_known": False,
            "objc_denominator_known": False, "swift_symbols": symbols,
            "swift_unclassified_count": unknown, "swift_denominator_known": False,
            "arc_runtime_import_observed": bool(re.search(r"_objc_(?:retain|release|storeStrong|storeWeak)",
                                                         outputs.get("imports", ""))),
            "arc_compile_and_method_flow_proven": False,
            "evidence_command_prefix": prefix}


def artifact_recovery(session, item, index, architecture):
    output = session.ctx.work / "recovered" / f"artifact-{index:04d}"
    name = f"artifact-{index:04d}-neverd"
    try:
        remaining = min(ARTIFACT_RECOVERY_SECONDS, session.remaining())
        result = session.command(name, [session.ctx.neverd, "mobile", item["resolved"], "-o", output,
                                       f"--arch={architecture}", f"--timeout={max(1, int(remaining))}"],
                                 cap=remaining, optional=True)
        record = {"artifact": item["path"], "output": str(output), "returncode": result.returncode,
                  "command_name": name, "status": "failed" if result.returncode else "incomplete"}
        report_path = output / "report.json"
        if report_path.is_file():
            report = read_json(report_path)
            if not isinstance(report, dict) or report.get("platform") != "ios" \
                    or report.get("architecture") != architecture:
                raise EvidenceError("NeverD report has missing or mismatched platform/architecture")
            record["report_status"] = report.get("status")
            record["report_sha256"] = digest(report_path, session)
            record["objc_method_recovery"] = report.get("objc_method_recovery")
            record["swift_method_recovery"] = report.get("swift_method_recovery")
            if report.get("status") not in ("success", "partial"):
                record["status"] = "failed"
                record["reason"] = "NeverD report does not indicate successful or partial analysis"
        elif result.returncode == 0:
            raise EvidenceError("NeverD exited zero without a report")
        record.setdefault("reason", "independent native/Objective-C denominator and source acceptance remain unproven")
        return record
    except Exception as error:
        return {"artifact": item["path"], "output": str(output), "command_name": name,
                "status": "failed", "reason": str(error)}


def retained_build_files(derived, session):
    records, issues = [], []
    visited = 0
    def walk_error(error):
        issues.append(f"build support traversal failed: {error}")

    for directory, _, files in os.walk(derived, followlinks=False, onerror=walk_error):
        session.remaining()
        for name in files:
            visited += 1
            if visited > MAX_BUNDLE_FILES:
                return records, [*issues, "derived evidence enumeration limit reached"]
            path = Path(directory) / name
            if name.endswith(("LinkMap.txt", ".resp", ".rsp", ".SwiftFileList", ".LinkFileList", "OutputFileMap.json")) \
                    or ".dSYM" in path.as_posix():
                target = session.ctx.work / "build-support" / path.relative_to(derived)
                session.preserve(path, target)
                records.append({"path": str(target.relative_to(session.ctx.work)), "size": target.stat().st_size})
    if not any(row["path"].endswith("LinkMap.txt") for row in records):
        issues.append("no retained linker map found")
    if not any(".dSYM/" in row["path"] for row in records):
        issues.append("no retained dSYM found")
    return records, issues


def run_ios(ctx):
    """Run the pinned app baseline; every unimplemented acceptance gate stays red."""
    session = Session(ctx)
    phase = "provenance"
    try:
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise EvidenceError("real iOS application execution is restricted to GitHub Actions")
        expected_version = ctx.app["ios"].get("xcode")
        if expected_version != "26.5":
            raise EvidenceError("this corpus requires manifest ios.xcode to be 26.5")
        scratch = ctx.work.parent / (ctx.work.name + "-ios-build")
        scratch.mkdir(parents=True, exist_ok=False)
        derived, packages = scratch / "derived-data", scratch / "source-packages"
        argv = build_arguments(ctx, derived, packages)
        head = session.command("source-head", ["git", "rev-parse", "HEAD"], cwd=ctx.source).stdout.strip()
        if not re.fullmatch(r"[0-9a-f]{40,64}", head) or head != ctx.app["source_commit"]:
            raise EvidenceError("source checkout does not match the immutable manifest commit")
        xcode = session.command("xcode-version", ["xcodebuild", "-version"]).stdout
        if not re.search(r"(?m)^Xcode " + re.escape(expected_version) + r"[ \t]*$", xcode):
            raise EvidenceError("selected Xcode version differs from manifest ios.xcode 26.5")
        sdk = {}
        for key, flag in (("path", "--show-sdk-path"), ("version", "--show-sdk-version"),
                          ("build_version", "--show-sdk-build-version")):
            sdk[key] = session.command("sdk-" + key, ["xcrun", "--sdk", ctx.variant["sdk"], flag]).stdout.strip()
            if not sdk[key]:
                raise EvidenceError("selected SDK identity is empty")
        if sdk["version"] != expected_version:
            raise EvidenceError("selected iOS SDK version differs from required 26.5")
        tools = {}
        for tool in ("clang", "swiftc", "swift-demangle", "dyld_info", "otool", "nm"):
            result = session.command("tool-" + tool, ["xcrun", "--sdk", ctx.variant["sdk"], "--find", tool], optional=True)
            tools[tool] = {"path": result.stdout.strip(), "returncode": result.returncode}
        for tool in ("clang", "swiftc", "swift-demangle"):
            session.command("tool-version-" + tool, ["xcrun", tool, "--version"], optional=True)
        config = ctx.app["ios"]
        lock_path = relative_path(ctx.source, config["package_resolved"])
        lock, lock_hash = read_json(lock_path), digest(lock_path, session)
        preparation = []
        license_evidence = None
        if ctx.app.get("license_path"):
            license_path = relative_path(ctx.source, ctx.app["license_path"])
            session.preserve(license_path, ctx.work / "licenses" / license_path.name)
            license_evidence = {"path": ctx.app["license_path"], "sha256": digest(license_path, session)}
        if config.get("xcconfig_template"):
            template = relative_path(ctx.source, config["xcconfig_template"])
            if not template.name.endswith(".template"):
                raise EvidenceError("xcconfig template name must end in .template")
            destination = template.with_name(template.name.removesuffix(".template"))
            if not destination.exists():
                with destination.open("xb") as output:
                    output.write(template.read_bytes())
                preparation.append(str(destination.relative_to(ctx.source)))
        session.command("resolve-packages", [*argv, "-resolvePackageDependencies"], cwd=ctx.source, cap=900)
        dependencies, dependency_issues = resolve_dependencies(session, packages, lock)
        if (packages / "workspace-state.json").is_file():
            session.preserve(packages / "workspace-state.json", ctx.work / "swiftpm-workspace-state.json")
        session.preserve(lock_path, ctx.work / "Package.resolved")
        provenance = {"repository": ctx.app["repository"], "source_commit": head, "license": ctx.app["license"],
                      "variant": ctx.variant, "xcode": xcode, "sdk": sdk, "apple_tools": tools,
                      "package_resolved_sha256": lock_hash, "dependencies": dependencies,
                      "issues": dependency_issues, "generated_configuration_files": preparation,
                      "license_evidence": license_evidence}
        ctx.write_json("ios-provenance.json", provenance)
        session.stage("provenance", "incomplete" if dependency_issues else "success", **provenance,
                      evidence=["ios-provenance.json", "Package.resolved"])
        phase = "original_build"
        session.command("build-settings", [*argv, "-showBuildSettings", "-json"], cwd=ctx.source, cap=180)
        # Leave room for actual artifact analysis even on a slow initial build.
        build_cap = min(2700, max(1, session.remaining() - 300))
        session.command("original-release-build", [*argv, "build"], cwd=ctx.source, cap=build_cap)
        if digest(lock_path, session) != lock_hash:
            raise EvidenceError("Package.resolved changed during the supposedly locked build")
        bundle = relative_path(derived / "Build/Products" / ("Release-" + ctx.variant["sdk"]), config["product"])
        if not bundle.is_dir():
            raise EvidenceError("successful Xcode build did not produce the manifest app bundle")
        preserved_bundle = relative_path(ctx.work / "original", config["product"])
        preserve_bundle(bundle, preserved_bundle, session)
        bundle = preserved_bundle
        retained, retained_issues = retained_build_files(derived, session)
        ctx.write_json("ios-build-artifacts.json", {"bundle": str(bundle), "files": retained, "issues": retained_issues})
        session.stage("original_build", "incomplete" if retained_issues else "success", bundle=str(bundle), profile=ctx.variant["profile"],
                      architecture=ctx.variant["architecture"], sdk=ctx.variant["sdk"],
                      retained_files=retained, evidence_issues=retained_issues,
                      evidence=["ios-build-artifacts.json", *[row["path"] for row in retained]])
        phase = "inventory"
        artifacts, enumeration_issues = bundle_artifacts(bundle, session)
        discovered = {name for row in artifacts for name in (row["path"], *row["aliases"])}
        for required in config.get("required_artifacts", []):
            relative_path(bundle, required)
            if required not in discovered:
                enumeration_issues.append(f"required app artifact absent: {required}")
        if not artifacts:
            enumeration_issues.append("app bundle contains no identifiable Mach-O artifacts")
        ctx.write_json("ios-bundle-inventory.json", {"artifacts": artifacts, "issues": enumeration_issues})
        inventories, recoveries = [], []
        # Interleave observations and recovery: a metadata-tool failure or an
        # unknown denominator must never suppress attempts on real app binaries.
        for index, artifact in enumerate(artifacts):
            try:
                inventory = artifact_inventory(session, artifact, index, ctx.variant["architecture"])
            except Exception as error:
                inventory = {"artifact": artifact["path"], "status": "incomplete", "issues": [str(error)]}
            inventories.append(inventory)
            ctx.write_json(f"ios-artifact-{index:04d}-inventory.json", inventory)
            recovery = artifact_recovery(session, artifact, index, ctx.variant["architecture"])
            recoveries.append(recovery)
            ctx.write_json(f"ios-artifact-{index:04d}-recovery.json", recovery)
        session.stage("inventory", "incomplete", artifact_count=len(artifacts),
                      enumeration_issues=enumeration_issues, artifacts=inventories,
                      reason="exhaustive independent native and Objective-C callable denominators are not implemented")
        phase = "recovery"
        recovery_status = "failed" if any(row["status"] == "failed" for row in recoveries) else "incomplete"
        session.stage("recovery", recovery_status, artifacts=recoveries,
                      reason="raw CLI outcomes retained; complete independent coverage is not established")
    except Exception as error:
        if phase not in session.stages:
            session.stage(phase, "failed", reason=str(error))
        ctx.write_json("ios-orchestration-error.json", {"phase": phase, "error": str(error)})
    for stage in REQUIRED_STAGES:
        if stage not in session.stages:
            reason = ("independent generated-source rebuild against the iOS SDK is not implemented" if stage == "recompile"
                      else "recovered-source upstream behavior comparison is not implemented; original-only tests cannot substitute"
                      if stage == "behavior" else "prior stage did not produce the evidence required to execute this stage")
            session.stage(stage, "incomplete", reason=reason)
    if any(status != "success" for status in session.stages.values()):
        ctx.fail("real iOS app acceptance is incomplete or failed; inspect the six stage records and per-artifact logs")
