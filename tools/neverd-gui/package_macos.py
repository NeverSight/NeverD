#!/usr/bin/env python3
"""Assemble and audit an ad-hoc-signed, relocatable NeverD macOS application."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys

Path = pathlib.Path
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
LICENSE_PATTERN = re.compile(r"license|licence|copying|copyright|notice|authors|^(?:l?gpl|agpl|bsd|mit)[-.]", re.I)


def run(*args: str | Path) -> None:
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"Command failed: {args[0]}\n{result.stderr}\n{result.stdout}")


def output(*args: str | Path) -> str:
    return subprocess.check_output([str(arg) for arg in args], text=True)


def macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in {bytes.fromhex(value) for value in
                                 ("cffaedfe", "cefaedfe", "feedfacf", "feedface", "cafebabe", "bebafeca", "cafebabf", "bfbafeca")}


def dependencies(path: Path) -> list[str]:
    return [line.strip().split(" (", 1)[0] for line in output("otool", "-L", path).splitlines()[1:]
            if line.startswith("\t")]


def install_id(path: Path) -> str | None:
    lines = [line for line in output("otool", "-D", path).splitlines()
             if line and not line.endswith(":") and not line.startswith("Archive :")]
    return lines[0] if lines else None


def rpaths(path: Path) -> list[str]:
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset", output("otool", "-l", path))


def framework_relative(path: str) -> Path:
    if ".framework/" in path:
        prefix, suffix = path.split(".framework/", 1)
        return Path(Path(prefix).name + ".framework") / suffix
    return Path(Path(path).name)


def homebrew_prefix(origin: Path) -> Path | None:
    parts = origin.resolve().parts
    if "Cellar" not in parts:
        return None
    cellar = parts.index("Cellar")
    return Path(*parts[:cellar + 3]) if len(parts) >= cellar + 3 else None


def copy_dependency(origin: Path, relative: Path, frameworks: Path, origins: set[Path]) -> None:
    origin = origin.resolve()
    origins.add(origin)
    destination = frameworks / relative
    if destination.exists():
        return
    if ".framework" in str(relative):
        framework_name = relative.parts[0]
        origin_framework = Path(str(origin).split(".framework/", 1)[0] + ".framework")
        shutil.copytree(origin_framework, frameworks / framework_name, symlinks=True, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("Headers", "*.prl", "_CodeSignature", "__pycache__",
                                                      "site-packages", "test", "tests"))
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin, destination)


def resolve_embedded(dependency: str, image: Path, bundle: Path) -> Path | None:
    frameworks = bundle / "Contents/Frameworks"
    if dependency.startswith("@loader_path/"):
        candidate = image.parent / dependency.removeprefix("@loader_path/")
    elif dependency.startswith("@executable_path/"):
        candidate = bundle / "Contents/MacOS" / dependency.removeprefix("@executable_path/")
    elif dependency.startswith("@rpath/"):
        candidate = frameworks / dependency.removeprefix("@rpath/")
    else:
        return None
    candidate = candidate.resolve()
    if candidate.is_relative_to(bundle) and candidate.exists():
        return candidate
    return None


def repair_dependencies(bundle: Path, qt: Path, origins: set[Path]) -> None:
    frameworks = bundle / "Contents/Frameworks"
    qt_libs = Path(output(qt / "bin/qmake", "-query", "QT_INSTALL_LIBS").strip())
    qt_qml = Path(output(qt / "bin/qmake", "-query", "QT_INSTALL_QML").strip())
    qt_plugins = Path(output(qt / "bin/qmake", "-query", "QT_INSTALL_PLUGINS").strip())
    brew_formula = homebrew_prefix(qt)
    brew_lib = brew_formula.parents[2] / "lib" if brew_formula else qt_libs
    inspected: set[Path] = set()
    while True:
        candidates = [path for path in bundle.rglob("*") if path not in inspected and macho(path)]
        if not candidates:
            break
        for image in candidates:
            inspected.add(image)
            # Track sources even for libraries already copied by macdeployqt.
            source_candidates = [brew_lib / image.name]
            if image.is_relative_to(frameworks):
                source_candidates.insert(0, qt_libs / image.relative_to(frameworks))
            for directory, origin_directory in ((bundle / "Contents/PlugIns", qt_plugins),
                                                 (bundle / "Contents/Resources/qml", qt_qml)):
                if image.is_relative_to(directory):
                    source_candidates.insert(0, origin_directory / image.relative_to(directory))
            for candidate in source_candidates:
                if candidate.is_file():
                    origins.add(candidate.resolve()); break
            identity = install_id(image)
            if identity:
                new_id = ("@rpath/" + str(image.relative_to(frameworks))) if image.is_relative_to(frameworks) else "@loader_path/" + image.name
                if identity != new_id:
                    run("install_name_tool", "-id", new_id, image)
                identity = new_id
            for dependency in dependencies(image):
                if dependency == identity or dependency.startswith(SYSTEM_PREFIXES):
                    continue
                if resolve_embedded(dependency, image, bundle):
                    continue
                if dependency.startswith("/"):
                    origin = Path(dependency)
                    relative = Path(framework_relative(dependency))
                elif dependency.startswith("@rpath/"):
                    relative = Path(dependency.removeprefix("@rpath/"))
                    origin = qt_libs / relative
                    if not origin.exists():
                        origin = qt / "lib" / relative
                else:
                    raise RuntimeError(f"Unresolved or escaping relative dependency {dependency} in {image}")
                if not origin.is_file():
                    raise RuntimeError(f"Unresolved dependency {dependency} in {image}")
                copy_dependency(origin, relative, frameworks, origins)
                replacement = "@rpath/" + str(relative)
                if replacement != dependency:
                    run("install_name_tool", "-change", dependency, replacement, image)
            # Every remaining search path must be relative to the bundle/system.
            for search in rpaths(image):
                if search.startswith("/") and not search.startswith(SYSTEM_PREFIXES):
                    run("install_name_tool", "-delete_rpath", search, image)
            local_frameworks = "@loader_path/" + os.path.relpath(frameworks, image.parent)
            if any(dependency.startswith("@rpath/") and dependency != identity for dependency in dependencies(image)) and local_frameworks not in rpaths(image):
                run("install_name_tool", "-add_rpath", local_frameworks, image)


def audit_bundle(bundle: Path) -> dict:
    problems = []
    minimum = (0, 0)
    images = []
    for path in bundle.rglob("*"):
        if path.is_symlink():
            target = path.resolve()
            if not target.is_relative_to(bundle) or not target.exists():
                problems.append(f"Escaping or dangling symlink: {path.relative_to(bundle)} -> {os.readlink(path)}")
        if not macho(path):
            continue
        image_dependencies = dependencies(path)
        identity = install_id(path)
        for dependency in image_dependencies:
            if dependency == identity or dependency.startswith(SYSTEM_PREFIXES):
                continue
            if not resolve_embedded(dependency, path, bundle):
                problems.append(f"Unresolved dependency in {path.relative_to(bundle)}: {dependency}")
        commands = output("otool", "-l", path)
        versions = re.findall(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform 1\s+minos ([0-9.]+)", commands)
        versions += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", commands)
        for version in versions:
            minimum = max(minimum, tuple(int(part) for part in version.split(".")))
        images.append({"path": str(path.relative_to(bundle)), "dependencies": image_dependencies,
                       "minimum_macos": versions, "sha256_before_signing": hashlib.sha256(path.read_bytes()).hexdigest()})
    if problems:
        raise RuntimeError("Bundle audit failed:\n" + "\n".join(problems))
    return {"minimum_macos": ".".join(map(str, minimum)), "images": images}


def copy_notices(bundle: Path, source: Path, origins: set[Path], qt_version: str,
                 qt_licenses: Path, kddw_source: Path, json_license: Path) -> None:
    licenses = bundle / "Contents/Resources/Licenses"
    licenses.mkdir()
    for name in ("LICENSE", "NOTICE", "ATTRIBUTION.md"):
        shutil.copy2(source.parents[1] / name, licenses / ("NeverD-" + name))
    if not qt_licenses.is_dir():
        raise RuntimeError(f"Qt {qt_version} license directory required: {qt_licenses}")
    shutil.copytree(qt_licenses, licenses / ("Qt-" + qt_version))
    if not kddw_source.is_dir() or not json_license.is_file():
        raise RuntimeError("Supply KDDockWidgets source and nlohmann/json license from the matching build")
    shutil.copytree(kddw_source / "LICENSES", licenses / "KDDockWidgets-2.4.1")
    for name in ("LICENSE.txt", "3RDPARTY.md"):
        shutil.copy2(kddw_source / name, licenses / "KDDockWidgets-2.4.1" / name)
    shutil.copy2(json_license, licenses / "nlohmann-json-LICENSE.MIT")
    components = []
    for prefix in sorted({prefix for origin in origins if (prefix := homebrew_prefix(origin))}):
        destination = licenses / (prefix.parent.name + "-" + prefix.name)
        destination.mkdir(exist_ok=True)
        files = [path for path in prefix.iterdir() if path.is_file() and LICENSE_PATTERN.search(path.name)]
        sbom = prefix / "sbom.spdx.json"
        if sbom.is_file():
            files.append(sbom)
        for path in files:
            shutil.copy2(path, destination / path.name)
        for path in (prefix / "share/qt/sbom").glob("*.spdx"):
            shutil.copy2(path, destination / path.name)
        components.append({"component": prefix.parent.name, "version": prefix.name,
                           "notices": str(destination.relative_to(bundle)), "source": str(prefix)})
    (licenses / "COMPONENTS.json").write_text(json.dumps(components, indent=2) + "\n")
    (licenses / "README.txt").write_text(
        "NeverD is AGPL-3.0-only; see its complete license and notices here.\n"
        "KDDockWidgets 2.4.1 is included under GPL-3.0-only; upstream alternative notices are preserved.\n"
        "Qt library/module licenses and their third-party notices are recorded in the bundled SPDX catalogs.\n"
        "Python and additional Homebrew runtime component notices are in versioned component folders.\n"
        "Source: https://github.com/NeverSight/NeverD (including the GUI changes used for this build).\n"
        "This local development bundle is ad-hoc signed, not notarized or a public release.\n"
        "Before distribution, provide matching complete corresponding source and dependency source as required.\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--qt-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-licenses", type=Path)
    parser.add_argument("--kddw-source", type=Path)
    parser.add_argument("--json-license", type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This packaging script requires macOS")
    bundle = args.output.resolve()
    if bundle.suffix != ".app" or bundle.exists():
        parser.error("--output must be a new .app path")
    source = Path(__file__).resolve().parent
    macos, frameworks, resources = (bundle / "Contents" / name for name in ("MacOS", "Frameworks", "Resources"))
    for directory in (macos, frameworks, resources):
        directory.mkdir(parents=True)
    for name in ("neverd-gui", "neverd-worker"):
        shutil.copy2(args.build_dir / "bin" / name, macos / name)
    shutil.copy2(args.engine, frameworks / "libneverd.dylib")
    build_inputs = [{"name": name, "source": str(origin.resolve()),
                     "sha256": hashlib.sha256(copied.read_bytes()).hexdigest()}
                    for name, origin, copied in (
                        ("neverd-gui", args.build_dir / "bin/neverd-gui", macos / "neverd-gui"),
                        ("neverd-worker", args.build_dir / "bin/neverd-worker", macos / "neverd-worker"),
                        ("libneverd.dylib", args.engine, frameworks / "libneverd.dylib"))]
    origins = {args.engine.resolve()}
    for dependency in dependencies(args.engine):
        if dependency.startswith("/") and "/Python.framework/" in dependency:
            relative = Path(framework_relative(dependency))
            copy_dependency(Path(dependency), relative, frameworks, origins)
            run("install_name_tool", "-change", dependency, "@rpath/" + str(relative), frameworks / "libneverd.dylib")
    shutil.copytree(source.parent / "neverd-mcp", resources / "mcp", ignore=shutil.ignore_patterns("__pycache__", "tests"))
    python_versions = sorted((frameworks / "Python.framework/Versions").glob("3.*"))
    launcher = macos / "neverd-mcp"
    compiler = ["xcrun", "clang", "-Os", "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=11.0"]
    for architecture in output("lipo", "-archs", macos / "neverd-gui").split():
        compiler += ["-arch", architecture]
    if python_versions:
        compiler += ['-DNEVERD_BUNDLED_PYTHON="' + python_versions[-1].name + '"']
    run(*compiler, source / "resources/mcp_launcher.c", "-o", launcher)
    plist = {"CFBundleName": "NeverD", "CFBundleDisplayName": "NeverD", "CFBundleExecutable": "neverd-gui",
             "CFBundleIdentifier": "dev.neversight.neverd", "CFBundlePackageType": "APPL",
             "CFBundleShortVersionString": "3389.0.1", "CFBundleVersion": "3389.0.1", "NSHighResolutionCapable": True}
    icon = source / "resources/NeverD.icns"
    if icon.is_file():
        shutil.copy2(icon, resources / icon.name)
        plist["CFBundleIconFile"] = icon.name
    with (bundle / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump(plist, stream)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", macos / "neverd-worker")
    run(args.qt_dir / "bin/macdeployqt", bundle, "-qmldir=" + str(source / "qml"),
        "-executable=" + str(macos / "neverd-worker"), "-libpath=" + str(args.qt_dir / "lib"),
        "-libpath=" + str(frameworks), "-no-codesign")
    repair_dependencies(bundle, args.qt_dir, origins)
    report = audit_bundle(bundle)
    report["build_inputs"] = build_inputs
    plist["LSMinimumSystemVersion"] = report["minimum_macos"]
    with (bundle / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump(plist, stream)
    version = output(args.qt_dir / "bin/qmake", "-query", "QT_VERSION").strip()
    kddw_source = args.kddw_source or args.build_dir / "_deps/kddockwidgets-src"
    json_license = args.json_license or args.build_dir / "_deps/neverd_worker_json-src/LICENSE.MIT"
    copy_notices(bundle, source, origins, version, args.qt_licenses or source / "licenses" / ("Qt-" + version),
                 kddw_source, json_license)
    (resources / "dependency-audit.json").write_text(json.dumps(report, indent=2) + "\n")
    # Python extension modules and executables in framework bin/ directories
    # are not all discovered by codesign --deep. Sign every Mach-O leaf first.
    for image in sorted((path for path in bundle.rglob("*") if macho(path)), key=lambda path: len(path.parts), reverse=True):
        run("codesign", "--force", "--sign", "-", image)
    run("codesign", "--force", "--deep", "--sign", "-", bundle)
    run("codesign", "--verify", "--deep", "--strict", bundle)
    print(json.dumps({"bundle": str(bundle), "minimum_macos": report["minimum_macos"], "macho_images": len(report["images"])}))


if __name__ == "__main__":
    main()
