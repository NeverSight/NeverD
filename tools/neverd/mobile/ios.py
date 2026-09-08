"""iOS package selection and native source/metadata export."""

from __future__ import annotations

import json
import plistlib
import re
import shutil
from pathlib import Path, PurePosixPath
from xml.parsers.expat import ExpatError

from .common import Limits, MobileError, extract_zip, run_tool, safe_copy_tree
from .macho import objc_header, objc_metadata, select_slice, swift_metadata


def _native_function_count(source: str) -> int:
    """Count this backend's emitted definitions, excluding its inline helpers.

    This is an output-presence check, not C validation or semantic verification.
    The native emitter places each signature before its opening brace and emits
    support routines as static inline definitions.
    """
    # Replace literals before comments so a quoted comment delimiter cannot
    # hide definitions, and quoted function-like strings cannot create them.
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*'
    source = re.sub(tokens, lambda match: "\n" * match.group(0).count("\n"), source)
    source = re.sub(r"^[ \t]*#[^\n]*", "", source, flags=re.MULTILINE)
    definitions = re.finditer(
        r"^[ \t]*(?!static[ \t]+inline\b)[A-Za-z_][A-Za-z_0-9 \t*]*[ \t*]+"
        r"([A-Za-z_][A-Za-z_0-9]*)[ \t]*\([^;{}]*\)[ \t\r\n]*\{", source, flags=re.MULTILINE)
    return sum(1 for _ in definitions)


def _relative_artifact(root: Path, artifact: str) -> Path:
    relative = PurePosixPath(artifact)
    if (not artifact or "\\" in artifact or ":" in artifact or "\0" in artifact or
            relative.is_absolute() or any(part in ("..", ".") for part in artifact.split("/"))):
        raise MobileError("iOS artifact must be a relative path inside the input package")
    candidate = root.joinpath(*relative.parts)
    if not candidate.is_file() or candidate.is_symlink():
        raise MobileError(f"iOS artifact is not a regular file: {artifact}")
    if not candidate.resolve().is_relative_to(root.resolve()):
        raise MobileError("iOS artifact escapes the input package")
    return candidate


def _bundle_info(bundle: Path) -> dict:
    path = bundle / "Info.plist"
    if not path.is_file() or path.is_symlink() or path.stat().st_size > 16 * 1024 * 1024:
        raise MobileError("iOS bundle has no readable bounded Info.plist")
    try:
        value = plistlib.loads(path.read_bytes())
    except (ValueError, TypeError, OverflowError, RecursionError, ExpatError, plistlib.InvalidFileException) as exc:
        raise MobileError(f"invalid iOS Info.plist: {exc}") from exc
    if not isinstance(value, dict):
        raise MobileError("iOS Info.plist root must be a dictionary")
    return value


def _main_executable(bundle: Path, info: dict) -> Path:
    executable = info.get("CFBundleExecutable")
    if (not isinstance(executable, str) or not executable or executable in (".", "..") or
            any(char in executable for char in ("/", "\\", ":", "\0"))):
        raise MobileError("iOS Info.plist has an invalid CFBundleExecutable")
    return _relative_artifact(bundle, executable)


def decompile_ios(source: Path, output: Path, *, neverd: str, arch: str,
                  artifact: str | None, metadata_only: bool, max_func: int,
                  limits: Limits, swift_demangle: str | None = None) -> dict:
    if max_func < 0:
        raise MobileError("max-func must not be negative")
    staged = output / "input"
    staged.mkdir(parents=True)
    bundle_info: dict = {}
    if source.is_dir():
        if source.suffix.lower() != ".app":
            raise MobileError("iOS directory input must be an .app bundle")
        safe_copy_tree(source, staged, limits)
        kind = "app"
        bundle_info = _bundle_info(staged)
        executable = _relative_artifact(staged, artifact) if artifact else _main_executable(staged, bundle_info)
    elif source.suffix.lower() == ".ipa":
        extract_zip(source, staged, limits)
        kind = "ipa"
        payload = staged / "Payload"
        # iterdir propagates unreadable-directory errors; glob may hide them.
        apps = sorted(path for path in payload.iterdir()
                      if path.suffix.lower() == ".app" and path.is_dir()) if payload.is_dir() else []
        if len(apps) != 1:
            raise MobileError("IPA must contain exactly one Payload/*.app")
        bundle_info = _bundle_info(apps[0])
        executable = _relative_artifact(apps[0], artifact) if artifact else _main_executable(apps[0], bundle_info)
    else:
        if artifact:
            raise MobileError("--artifact applies only to IPA or .app input")
        if source.is_symlink() or not source.is_file():
            raise MobileError("iOS binary input must be a regular file")
        kind = "macho"
        executable = source
    if executable.stat().st_size > limits.max_bytes:
        raise MobileError("iOS executable exceeds the input byte limit")
    image, available = select_slice(executable.read_bytes(), arch)
    if image.encrypted:
        raise MobileError("selected Mach-O slice is encrypted (cryptid != 0); supply a decrypted executable")
    metadata = output / "metadata"
    metadata.mkdir()
    artifacts = output / "artifacts"
    artifacts.mkdir()
    thin_path = artifacts / "selected.macho"
    thin_path.write_bytes(image.data)
    objc = objc_metadata(image)
    swift = swift_metadata(image)
    (metadata / "swift.json").write_text(json.dumps(swift, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    outputs = {"selected_binary": "artifacts/selected.macho", "objc_metadata": "metadata/objc.json",
               "objc_declarations": "metadata/objc.h", "swift_metadata": "metadata/swift.json"}
    native_function_count = None
    method_recovery = None
    swift_recovery = None
    native_limitations: list[str] = []
    if not metadata_only:
        sources = output / "sources"
        sources.mkdir()
        log = output / "logs" / "native.log"
        log.parent.mkdir(exist_ok=True)
        native = sources / "native.c"
        batch_path = artifacts / "native-recovery.json"
        command = [neverd, "export", str(thin_path), "--format=objc-methods", "-o", str(batch_path)]
        if max_func:
            command.append(f"--max-func={max_func}")
        run_tool(command, log, limits.timeout)
        if not batch_path.is_file() or not batch_path.stat().st_size:
            raise MobileError("native decompiler did not produce a source report")
        if batch_path.stat().st_size > limits.max_bytes:
            raise MobileError("native decompiler source report exceeds the byte limit")
        try:
            batch = json.loads(batch_path.read_text(encoding="utf-8"))
        except (ValueError, UnicodeError, RecursionError) as exc:
            raise MobileError("native decompiler produced an invalid source report") from exc
        if (not isinstance(batch, dict) or batch.get("schema_version") != 1 or
                batch.get("status") != "success" or not isinstance(batch.get("native_source"), str) or
                not isinstance(batch.get("methods"), list) or
                not isinstance(batch.get("objc_metadata"), dict) or
                not isinstance(batch.get("limitations"), list) or
                not all(isinstance(item, str) for item in batch["limitations"])):
            raise MobileError("native decompiler source report has an unsupported schema")
        native_limitations = batch["limitations"]
        native_function_count = _native_function_count(batch["native_source"])
        if not native_function_count:
            raise MobileError("native decompiler recovered no function bodies; use --metadata-only for metadata inspection")
        if (type(batch.get("native_function_count")) is not int or
                native_function_count != batch["native_function_count"]):
            raise MobileError("native source report disagrees with its emitted function count")
        native.write_text(batch["native_source"], encoding="utf-8")
        objc = batch["objc_metadata"]
        if (not isinstance(objc.get("classes"), list) or
                not isinstance(objc.get("limitations"), list)):
            raise MobileError("native source report has invalid Objective-C metadata")
        from .objc_source import render_objc_sources
        method_source, method_recovery = render_objc_sources(batch, objc)
        if method_recovery["recovered_method_count"]:
            (sources / "objc.m").write_text(method_source, encoding="utf-8")
            outputs["objc_source"] = "sources/objc.m"
        (metadata / "objc-methods.json").write_text(
            json.dumps(method_recovery, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        outputs["objc_method_coverage"] = "metadata/objc-methods.json"
        batch_path.unlink()
        outputs["native_source"] = "sources/native.c"
        outputs["native_log"] = "logs/native.log"
        from .swift_source import recover_swift_sources
        swift_recovery, swift_outputs = recover_swift_sources(
            thin_path, output, symbols=swift["symbols"], neverd=neverd,
            demangler=swift_demangle, pointer_size=image.pointer_size,
            max_func=max_func, limits=limits)
        outputs.update(swift_outputs)
    (metadata / "objc.json").write_text(json.dumps(objc, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    (metadata / "objc.h").write_text(objc_header({**objc, "pointer_size": image.pointer_size}), encoding="utf-8")
    bundle = {key: value for key, value in bundle_info.items()
              if key in ("CFBundleIdentifier", "CFBundleName", "CFBundleExecutable", "CFBundleVersion",
                         "CFBundleShortVersionString", "MinimumOSVersion") and isinstance(value, (str, int, bool))}
    report = {"platform": "ios", "input_kind": kind,
            "selected_artifact": executable.relative_to(staged).as_posix() if kind != "macho" else source.name,
            "architecture": image.architecture, "cpu_subtype": image.cpu_subtype,
            "available_architectures": available, "encrypted": False, "bundle": bundle,
            "metadata_only": metadata_only, "outputs": outputs,
            "native_function_count": native_function_count,
            "objc_method_recovery": method_recovery,
            "swift_method_recovery": swift_recovery,
            "objc_class_count": len(objc["classes"]), "swift_type_count": len(swift["types"]),
            "swift_symbol_count": len(swift["symbols"]),
            "limitations": ["Original comments, formatting, removed names and source constructs lost during compilation cannot be restored.",
                            "Objective-C method coverage lists reconstructed bodies and omissions; it is not a proof of semantic equivalence.",
                            "Swift method coverage separates recovered native bodies, unsupported callable signatures and non-callable metadata; stripped or unclassified symbols can leave coverage unknown.",
                            "Native C types and calling conventions remain approximations outside supported source signatures.",
                            "Only the selected executable is analyzed; use --artifact for embedded frameworks or extensions.",
                            *native_limitations,
                            *(method_recovery["limitations"] if method_recovery else []),
                            *(swift_recovery["limitations"] if swift_recovery else []),
                            *objc["limitations"], *swift["limitations"]]}
    shutil.rmtree(staged)
    return report
