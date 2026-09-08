"""Package validation and explicit Android source-backend selection."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import stat
import tempfile

from .common import Limits, MobileError, extract_zip, relative_member, run_tool, safe_copy_tree, walk_error, validate_tree


_DEX_NAME = re.compile(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex\Z")
_LOG_ERROR = re.compile(r"(?:^|[\r\n])\s*(?:\x1b\[[0-9;]*m)*ERROR\s*[-:]", re.I)
_CODE_ERROR = re.compile(r"^\s*(?:/\*|\*)\s*(?:JADX ERROR:|Code decompiled incorrectly)", re.I)
_PARTIAL_WARNINGS = ("Failed to load code for plugin:", "Found duplicated class:")
_MIN_JADX = (1, 5, 6)


def _launcher(jadx: str) -> list[str]:
    """Use the distribution JAR directly for Windows batch launchers."""
    executable = Path(jadx)
    if executable.parent == Path("."):
        executable = Path(shutil.which(jadx) or jadx)
    if executable.suffix.lower() not in {".bat", ".cmd", ".jar"}:
        return [str(executable)]
    if executable.suffix.lower() == ".jar":
        jars = [executable]
    else:
        jars = sorted((executable.parent.parent / "lib").glob("jadx-*-all.jar"))
    if len(jars) != 1 or not jars[0].is_file():
        raise MobileError("JADX batch launcher requires one lib/jadx-*-all.jar in its distribution")
    java_home = os.environ.get("JAVA_HOME")
    java = Path(java_home) / "bin" / ("java.exe" if os.name == "nt" else "java") if java_home else None
    java_path = str(java) if java and java.is_file() else shutil.which("java")
    if not java_path:
        raise MobileError("JADX requires Java 11 or newer; set JAVA_HOME or add java to PATH")
    return [java_path, "-cp", str(jars[0].resolve()), "jadx.cli.JadxCLI"]


def _copy_input(source: Path, dest: Path, limits: Limits) -> None:
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode):
        raise MobileError("Android input must be a regular file, not a symlink or device")
    if info.st_size > limits.max_bytes:
        raise MobileError("Android input exceeds the byte limit")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as reader, dest.open("xb") as writer:
        copied = 0
        while chunk := reader.read(1024 * 1024):
            copied += len(chunk)
            if copied > limits.max_bytes:
                raise MobileError("Android input exceeds the byte limit")
            writer.write(chunk)


def _check_dex(path: Path) -> None:
    with path.open("rb") as stream:
        magic = stream.read(8)
    if not re.fullmatch(rb"dex\n[0-9]{3}\x00", magic):
        raise MobileError(f"Invalid DEX header: {path.name}")


def _stage_inputs(source: Path, work: Path, limits: Limits) -> tuple[str, Path, list[str]]:
    """Hand only validated code files to plugins, never a whole unpacked APK."""
    code = work / "code"
    code.mkdir()
    names: list[str] = []
    if source.is_dir():
        kind = "smali-directory"
        tree = work / "tree"
        safe_copy_tree(source, tree, limits)
        for path in sorted(tree.rglob("*")):
            if path.is_file() and path.suffix.lower() == ".smali":
                names.append(path.relative_to(tree).as_posix())
                path.rename(code / f"{len(names):06d}.smali")
        if not names:
            raise MobileError("Smali directory contains no .smali files")
    else:
        kind = source.suffix.lower().lstrip(".")
        if kind not in {"apk", "dex", "smali"}:
            raise MobileError("Android input must be an APK, DEX, smali file, or smali directory")
        if kind == "apk":
            archive = work / "input.apk"
            _copy_input(source, archive, limits)
            tree = work / "archive"
            extracted = extract_zip(archive, tree, limits)
            dex_paths = sorted(p for p in extracted if p.parent == tree and _DEX_NAME.fullmatch(p.name))
            if not dex_paths:
                raise MobileError("APK contains no root classes.dex or classesN.dex bytecode")
            for path in dex_paths:
                _check_dex(path)
                names.append(path.name)
                path.rename(code / path.name)
        else:
            path = code / f"input.{kind}"
            _copy_input(source, path, limits)
            if kind == "dex":
                _check_dex(path)
            names.append(source.name)
    return kind, code, names


def _check_output(sources: Path, limits: Limits) -> list[Path]:
    java_files: list[Path] = []
    total_bytes = 0
    files_count = 0
    if sources.is_symlink():
        raise MobileError("JADX produced an unsafe source directory")
    if not sources.is_dir():
        raise MobileError("JADX produced no Java sources; verify the input and backend plugins")
    for directory, directories, names in os.walk(sources, followlinks=False, onerror=walk_error):
        for name in directories + names:
            path = Path(directory) / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
                raise MobileError("JADX produced an unsafe output entry")
            files_count += 1
            total_bytes += info.st_size if stat.S_ISREG(info.st_mode) else 0
            if files_count > limits.max_files or total_bytes > limits.max_bytes:
                raise MobileError("JADX output exceeds the configured file or byte limit")
            if stat.S_ISDIR(info.st_mode):
                continue
            if path.suffix == ".java":
                if info.st_size == 0:
                    raise MobileError("JADX produced an empty Java source file")
                with path.open(encoding="utf-8", errors="replace") as stream:
                    for line in stream:
                        if _CODE_ERROR.search(line):
                            raise MobileError("JADX produced incomplete Java code; see logs/jadx.log")
                java_files.append(path)
    if not java_files:
        raise MobileError("JADX produced no Java sources; verify the input and backend plugins")
    return sorted(java_files)


def decompile_android(source: Path, output: Path, *, jadx: str | None = None, limits: Limits) -> dict:
    if jadx is None:
        return _decompile_builtin(source, output, limits=limits)
    """Populate an empty staging directory; failures never constitute success."""
    source = source.absolute()
    output = output.absolute()
    output.mkdir(parents=True, exist_ok=True)
    logs = output / "logs"
    logs.mkdir()
    command = _launcher(jadx)
    with tempfile.TemporaryDirectory(prefix=".android-work-", dir=output) as temporary:
        work = Path(temporary)
        environment = os.environ.copy()
        for flag in ("JADX_DISABLE_XML_SECURITY", "JADX_DISABLE_ZIP_SECURITY", "JADX_DISABLE_ALL_SECURITY_FLAGS"):
            environment.pop(flag, None)
        for suffix in ("CONFIG", "CACHE", "TMP"):
            directory = work / "runtime" / suffix.lower()
            directory.mkdir(parents=True)
            environment[f"JADX_{suffix}_DIR"] = str(directory)
        version_log = logs / "jadx-version.log"
        run_tool(command + ["--version"], version_log, limits.timeout, env=environment)
        version_text = version_log.read_text(encoding="utf-8", errors="replace").strip()
        match = re.search(r"(?m)^(\d+)\.(\d+)\.(\d+)(?:[-+][^\s]+)?\s*$", version_text)
        if not match or tuple(map(int, match.groups())) < _MIN_JADX:
            raise MobileError("Android support requires JADX 1.5.6 or newer with dex-input and smali-input plugins")
        version = match.group(0).strip()
        kind, code, names = _stage_inputs(source, work, limits)
        log = logs / "jadx.log"
        run_tool(command + [
            "--config", "none",
            "--no-res",
            "--output-format", "java",
            "--decompilation-mode", "restructure",
            "--comments-level", "warn",
            "--log-level", "warn",
            "--deobf-cfg-file-mode", "ignore",
            "--output-dir", str(output),
            str(code),
        ], log, limits.timeout, env=environment)
        diagnostics = log.read_text(encoding="utf-8", errors="replace")
        if _LOG_ERROR.search(diagnostics) or any(marker in diagnostics for marker in _PARTIAL_WARNINGS):
            raise MobileError("JADX reported input or decompilation errors; see logs/jadx.log")
        sources = _check_output(output / "sources", limits)
    limitations = [
        "Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored.",
        "A successful backend run does not prove semantic equivalence or that every method can be recompiled.",
        "Android resources, manifests, native libraries, and dynamically downloaded or encrypted code are not decompiled.",
    ]
    if kind == "smali":
        limitations.append("Single smali input has only one class; supply its directory to resolve sibling and nested classes together.")
    return {
        "status": "success",
        "platform": "android",
        "input_kind": kind,
        "backend": {"name": "jadx", "version": version},
        "input_code_files": names,
        "dex_count": len(names) if kind in {"apk", "dex"} else 0,
        "smali_count": len(names) if kind in {"smali", "smali-directory"} else 0,
        "java_source_count": len(sources),
        "java_sources": [p.relative_to(output).as_posix() for p in sources],
        "logs": ["logs/jadx-version.log", "logs/jadx.log"],
        "limitations": limitations,
    }


def _decompile_builtin(source: Path, output: Path, *, limits: Limits) -> dict:
    from .dalvik_model import Budget, link_classes
    from .dalvik_dex import parse_dex
    from .dalvik_smali import parse_smali
    from .dalvik_java import recover_java
    import json

    budget = Budget(limits)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".android-input-", dir=output) as temporary:
        kind, code, names = _stage_inputs(source, Path(temporary), limits)
        classes = []
        paths = sorted(p for p in code.iterdir() if p.is_file())
        if len(paths) != len(names):
            raise MobileError("Android staged input inventory is inconsistent")
        for path, input_id in zip(paths, names):
            budget.tick(path.stat().st_size)
            if kind in {"apk", "dex"}:
                classes.extend(parse_dex(path.read_bytes(), input_id=input_id, budget=budget))
            else:
                try:
                    text = path.read_text(encoding="utf-8")
                except UnicodeError as error:
                    raise MobileError("smali input is not valid UTF-8") from error
                classes.append(parse_smali(text, input_id=input_id, budget=budget))
        coverage = recover_java(link_classes(classes, budget), budget=budget)
    units = coverage.pop("source_units")
    metadata_path = Path("metadata/android-methods.json")
    metadata_bytes = (json.dumps(coverage, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
    budget.output(len(metadata_bytes))
    entries = {}
    pending = []
    total_bytes = len(metadata_bytes)

    def reserve(path: Path, directory: bool) -> None:
        name = path.as_posix()
        key = name.casefold()
        previous = entries.get(key)
        if previous is not None and (previous != (name, directory) or not directory):
            raise MobileError("Android Java output has conflicting class or directory paths")
        entries[key] = (name, directory)
        if len(entries) > limits.max_files:
            raise MobileError("Android output exceeds the file-count limit including directories")

    reserve(metadata_path.parent, True)
    reserve(metadata_path, False)
    for unit in units:
        relative = Path("sources") / relative_member(unit["path"])
        for parent in relative.parents:
            if parent != Path("."):
                reserve(parent, True)
        reserve(relative, False)
        encoded = unit["source"].encode("utf-8")
        budget.tick(len(encoded))
        total_bytes += len(encoded)
        if total_bytes > limits.max_bytes:
            raise MobileError("Android source and metadata output exceed the byte limit")
        pending.append((relative, encoded))
    if total_bytes > limits.max_bytes:
        raise MobileError("Android metadata output exceeds the byte limit")
    # Validate the complete output set before any source directory or file is
    # created. A late bad class must not leave earlier classes in staging.
    for relative, encoded in pending:
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(encoded)
    (output / metadata_path.parent).mkdir(exist_ok=True)
    (output / metadata_path).write_bytes(metadata_bytes)
    sources = [relative.as_posix() for relative, _ in pending]
    validate_tree(output, limits)
    return {
        "status": "success", "platform": "android", "input_kind": kind,
        "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
        "input_code_files": names, "dex_count": len(names) if kind in {"apk", "dex"} else 0,
        "smali_count": len(names) if kind in {"smali", "smali-directory"} else 0,
        "java_source_count": len(sources), "java_sources": sources, "logs": [],
        "android_method_recovery": coverage,
        "limitations": [
            "Java is reconstructed from bytecode; compilation-lost source text and identifiers cannot be restored.",
            "Unsupported instructions, declarations and unproven register flows reject the input instead of publishing missing bodies.",
            "Generated method control flow can use a Java dispatch loop; it does not execute the input DEX or call an external decompiler.",
            "Native and abstract declarations remain declaration-only and are counted separately from recovered bodies.",
            "Android resources, manifests and native libraries are outside this Java recovery workflow.",
            "Successful recovery does not certify behavior for arbitrary applications."
        ],
    }
