"""CI-only evidence for complete, published Android application inputs.

The independent inventory consumes Android SDK dexdump's plain output, not
NeverD's decoder. The format contract is documented by AOSP dumpClassDef,
dumpClass and dumpMethod:
https://android.googlesource.com/platform/art/%2B/119a885/dexdump/dexdump.cc
The driver documents -f/-h/-l and the verification-disabling flags we never use:
https://android.googlesource.com/platform/art/%2B/6b36d8025de5237b57e7bf23033bfc61a112d6cd/dexdump/dexdump_main.cc

Source reconstruction is not a behavior certificate. Until a separate APK
rebuild and ART behavior comparison exist, those stages remain incomplete.
This module is test infrastructure, never a production mobile runtime.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
from urllib.parse import urlsplit
import zipfile

try:
    from .mobile_real_apps_android_build import build_source_apk
except ImportError:
    from mobile_real_apps_android_build import build_source_apk


STAGES = ("provenance", "original_build", "inventory", "recovery", "recompile", "behavior")
MAX_APK_BYTES = 256 * 1024 * 1024
MAX_DEX_BYTES = 512 * 1024 * 1024
MAX_TOTAL_DEX_BYTES = 1024 * 1024 * 1024
MAX_ZIP_ENTRIES = 100_000
MAX_DUMP_BYTES = 256 * 1024 * 1024
MAX_DEFINITIONS = 1_000_000
MAX_SOURCE_BYTES = 1024 * 1024 * 1024
MAX_SOURCE_ENTRIES = 100_000


class AndroidEvidenceError(RuntimeError):
    """An input, independent inventory, or claimed recovery failed its gate."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AndroidEvidenceError(message)


def integer(value, name: str, minimum: int = 0) -> int:
    require(type(value) is int and value >= minimum, f"Invalid integer: {name}")
    return value


def sha256_file(path: Path, limit: int) -> str:
    digest, total = hashlib.sha256(), 0
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            total += len(data)
            require(total <= limit, f"File exceeds evidence byte limit: {path.name}")
            digest.update(data)
    require(total > 0, f"Empty evidence file: {path.name}")
    return digest.hexdigest()


def dex_header(data: bytes) -> dict:
    """Only fixed header facts, independently checked against the SDK dump."""
    require(len(data) >= 112, "Truncated DEX header")
    require(data[:4] == b"dex\n" and data[7] == 0, "Invalid DEX magic")
    require(data[4:7] in (b"035", b"037", b"038", b"039", b"040"),
            "Independent inventory does not support this DEX version")
    size, header_size, endian = struct.unpack_from("<III", data, 32)
    require(size == len(data) and header_size == 112 and endian == 0x12345678,
            "DEX fixed header has inconsistent length or endianness")
    method_ids, classes = struct.unpack_from("<I", data, 88)[0], struct.unpack_from("<I", data, 96)[0]
    require(classes <= MAX_DEFINITIONS and method_ids <= 65536, "DEX definition budget exceeded")
    return {"version": data[4:7].decode("ascii"), "file_size": size,
            "class_defs_size": classes, "method_ids_size": method_ids}


def extract_dex_inputs(apk: Path, directory: Path) -> list[dict]:
    """Copy only DEX bytes to numbered paths; retain original ZIP identities.

    All .dex entries are inventoried, including nonstandard asset locations.
    A recovery that silently ignores any such entry will fail comparison.
    """
    directory.mkdir(parents=True, exist_ok=False)
    result, explicit, node_types, total = [], set(), {}, 0
    with zipfile.ZipFile(apk) as archive:
        entries = archive.infolist()
        require(len(entries) <= MAX_ZIP_ENTRIES, "APK entry budget exceeded")
        for entry in entries:
            name = entry.filename
            parts = (name[:-1] if name.endswith("/") else name).split("/")
            require(name and not name.startswith("/") and "\\" not in name
                    and "\x00" not in name and not any(part in ("", ".", "..") for part in parts)
                    and not re.match(r"^[A-Za-z]:", name), "Unsafe APK entry name")
            # ZIP names are case-sensitive. Android resources routinely use
            # both res/-A.xml and res/-a.xml; only numbered DEX copies reach
            # the host filesystem, so resource names need no case folding.
            canonical = "/".join(parts)
            require(canonical not in explicit, "Duplicate APK entry")
            explicit.add(canonical)
            for index in range(1, len(parts) + 1):
                node = "/".join(parts[:index])
                directory_node = index < len(parts) or entry.is_dir()
                require(node not in node_types or node_types[node] == directory_node,
                        "APK file/directory path conflict")
                node_types[node] = directory_node
            require(not stat.S_ISLNK(entry.external_attr >> 16), "APK contains a symlink")
            require(not entry.flag_bits & 1, "Encrypted ZIP entries are unsupported")
            if entry.is_dir() or not name.lower().endswith(".dex"):
                continue
            require(0 < entry.file_size <= MAX_DEX_BYTES, "DEX entry exceeds evidence byte limit")
            total += entry.file_size
            require(total <= MAX_TOTAL_DEX_BYTES, "Total DEX evidence byte limit exceeded")
            with archive.open(entry) as stream:
                data = stream.read(entry.file_size + 1)
            require(len(data) == entry.file_size, "DEX entry length disagrees with ZIP metadata")
            header = dex_header(data)
            path = directory / f"{len(result):04d}.dex"
            with path.open("xb") as stream:
                stream.write(data)
            result.append({"input": name, "path": path, "sha256": hashlib.sha256(data).hexdigest(),
                           "header": header})
    require(result, "Application APK contains no DEX input")
    return sorted(result, key=lambda item: item["input"])


def _prototype(text: str) -> None:
    def one(position: int, allow_void: bool = False) -> int:
        start = position
        while position < len(text) and text[position] == "[":
            position += 1
        require(position - start <= 255 and position < len(text), "Malformed DEX prototype")
        char = text[position]
        if char in "ZBCSIFJD" or (char == "V" and allow_void and position == start):
            return position + 1
        require(char == "L", "Malformed DEX prototype")
        end = text.find(";", position + 1)
        require(end > position + 1 and not any(c in text[position + 1:end] for c in ".;[()\n\r\0"),
                "Malformed DEX object descriptor")
        return end + 1

    require(text.startswith("("), "Malformed DEX prototype")
    position = 1
    while position < len(text) and text[position] != ")":
        position = one(position)
    require(position < len(text), "Unterminated DEX prototype")
    require(one(position + 1, True) == len(text), "Trailing DEX prototype data")


def parse_dexdump(text: str, input_name: str, header: dict) -> dict:
    """Parse -f -h -l plain output, requiring all class_data definitions.

    Per-section header counts and contiguous class/method ordinals detect
    truncation, omission, repeated entries, or accidental exported-only dumps.
    Method IDs in the file header are references, not the recovery denominator.
    """
    require(isinstance(text, str) and len(text.encode("utf-8")) <= MAX_DUMP_BYTES,
            "DEX dump exceeds evidence byte limit")
    classes, methods, identities, class_names = [], [], set(), set()
    file_facts, class_header, current, pending = {}, None, None, None
    section, section_order = None, []
    counts = {}
    sections = ("Interfaces", "Static fields", "Instance fields", "Direct methods", "Virtual methods")
    count_keys = {"Static fields": "static_fields_size", "Instance fields": "instance_fields_size",
                  "Direct methods": "direct_methods_size", "Virtual methods": "virtual_methods_size"}

    def finish_method() -> None:
        nonlocal pending
        if pending is None:
            return
        require(all(key in pending for key in ("name", "prototype", "access_flags", "has_code")),
                "Incomplete SDK method definition")
        require(bool(pending["name"]), "Empty SDK method name")
        _prototype(pending["prototype"])
        flags = pending["access_flags"]
        abstract, native = bool(flags & 0x400), bool(flags & 0x100)
        require(not (abstract and native), "Method cannot be both abstract and native")
        require(pending["has_code"] == (not abstract and not native),
                "Method flags and code presence disagree")
        units = pending.get("code_units", 0)
        require((units > 0) == pending["has_code"], "Missing or unexpected method code size")
        identity = pending["class"] + "->" + pending["name"] + pending["prototype"]
        require(identity not in identities, "Duplicate SDK method identity")
        identities.add(identity)
        pending.update(identity=identity, input=input_name,
                       role="abstract" if abstract else "native" if native else "body", code_units=units)
        methods.append(pending)
        require(len(methods) <= MAX_DEFINITIONS, "SDK method inventory budget exceeded")
        pending = None

    def unique(target: dict, key: str, value) -> None:
        require(key not in target, f"Duplicate SDK inventory field: {key}")
        target[key] = value

    for raw in text.splitlines():
        require(len(raw) <= 65536, "SDK dump line exceeds evidence limit")
        line = raw.strip()
        if not line:
            continue
        match = re.fullmatch(r"Class #(\d+) header:", line)
        if match:
            require(current is None and class_header is None and pending is None,
                    "Truncated class before the next SDK class header")
            require(int(match[1]) == len(classes), "Missing or duplicate SDK class ordinal")
            class_header = {"index": int(match[1])}
            continue
        match = re.fullmatch(r"Class #(\d+)\s*-", line)
        if match:
            require(class_header is not None and current is None
                    and int(match[1]) == class_header["index"], "SDK class body lacks its matching header")
            require(all(key in class_header for key in count_keys.values()), "SDK class header lacks definition counts")
            current, class_header = class_header, None
            section, section_order, counts = None, [], {}
            continue
        if class_header is not None:
            match = re.fullmatch(r"(static_fields_size|instance_fields_size|direct_methods_size|virtual_methods_size)\s*:\s*(\d+)", line)
            if match:
                value = int(match[2])
                require(value <= MAX_DEFINITIONS, "SDK class definition budget exceeded")
                unique(class_header, match[1], value)
            continue
        if current is None:
            match = re.fullmatch(r"(file_size|class_defs_size|method_ids_size)\s*:\s*(\d+)", line)
            if match:
                unique(file_facts, match[1], int(match[2]))
            # Processing/Open/header lines are informational. A detached
            # definition must not disappear into that preamble.
            require(not re.match(r"(?:Class descriptor|Direct methods|Virtual methods|#\d+\s*:|name\s*:|code\s*[:-])", line),
                    "SDK definition is outside a complete class")
            continue
        match = re.fullmatch(r"Class descriptor\s*:\s*'(.*)'", line)
        if match:
            name = match[1]
            require(name.startswith("L") and name.endswith(";") and len(name) > 2,
                    "Malformed SDK class descriptor")
            require(name not in class_names, "Duplicate SDK class descriptor")
            unique(current, "descriptor", name)
            class_names.add(name)
            continue
        match = re.fullmatch(r"(Interfaces|Static fields|Instance fields|Direct methods|Virtual methods)\s*-", line)
        if match:
            finish_method()
            require(len(section_order) < len(sections) and match[1] == sections[len(section_order)],
                    "Missing, repeated, or reordered SDK class section")
            section = match[1]
            section_order.append(section)
            counts[section] = 0
            continue
        if re.match(r"source_file_idx\s*:", line):
            finish_method()
            require("descriptor" in current and tuple(section_order) == sections,
                    "Incomplete SDK class body")
            require(all(counts.get(key) == current[value] for key, value in count_keys.items()),
                    "SDK class_data definition count disagrees with dump entries")
            classes.append(current)
            current, section = None, None
            continue
        match = re.fullmatch(r"#(\d+)\s*:\s*\(in (.*)\)", line)
        if match:
            require(section in count_keys and "descriptor" in current,
                    "SDK member is outside a fields/methods section")
            finish_method()
            require(int(match[1]) == counts[section], "Missing or duplicate SDK member ordinal")
            require(match[2] == current["descriptor"], "SDK member belongs to a different class")
            counts[section] += 1
            if section in ("Direct methods", "Virtual methods"):
                pending = {"class": match[2], "section": section, "ordinal": int(match[1])}
            continue
        # Interfaces have a different entry layout and no executable members.
        if section == "Interfaces" and re.fullmatch(r"#\d+\s*:\s*'.*'", line):
            continue
        if section in ("Direct methods", "Virtual methods"):
            require(pending is not None, "Unexpected text outside an SDK method definition")
            match = re.fullmatch(r"(name|type)\s*:\s*'(.*)'", line)
            if match:
                unique(pending, "prototype" if match[1] == "type" else "name", match[2])
                continue
            match = re.fullmatch(r"access\s*:\s*0x([0-9a-fA-F]+)\s*\(.*\)", line)
            if match:
                unique(pending, "access_flags", int(match[1], 16))
                continue
            if re.fullmatch(r"code\s*:\s*\(none\)", line):
                unique(pending, "has_code", False)
                continue
            if re.fullmatch(r"code\s*-", line):
                unique(pending, "has_code", True)
                continue
            match = re.fullmatch(r"insns size\s*:\s*(\d+) 16-bit code units", line)
            if match:
                unique(pending, "code_units", int(match[1]))
                continue
            require(not re.match(r"(?:name|type|access|code|insns size)\s*[:\-]", line),
                    "Unrecognized SDK method identity/code field")

    require(class_header is None and current is None and pending is None, "Truncated SDK class or method dump")
    require(all(file_facts.get(key) == header.get(key) for key in
                ("file_size", "class_defs_size", "method_ids_size")), "SDK file header differs from the input DEX")
    require(len(classes) == header["class_defs_size"], "SDK class inventory is incomplete")
    require(len(methods) <= header["method_ids_size"], "Definitions exceed the DEX method reference table")
    return {"input": input_name, "header": header, "classes": classes, "methods": methods}


def inventory_mutation_checks(text: str, input_name: str, header: dict) -> list[str]:
    """Exercise rejection guards against the actual SDK output in CI."""
    checks = []

    def reject(name: str, mutated: str) -> None:
        # Consume each mutation immediately rather than retaining six complete
        # copies of a potentially large real-application SDK dump.
        try:
            parse_dexdump(mutated, input_name, header)
        except AndroidEvidenceError:
            checks.append(name)
            return
        raise AndroidEvidenceError(f"Independent inventory accepted mutation: {name}")

    for name, pattern, replacement in (
        ("changed-file-class-count", r"(?m)^class_defs_size[ \t]*:[ \t]*\d+$",
         f"class_defs_size : {header['class_defs_size'] + 1}"),
        ("changed-class-method-count", r"(?m)^direct_methods_size[ \t]*:[ \t]*(\d+)$",
         lambda match: f"direct_methods_size : {int(match[1]) + 1}"),
    ):
        mutated, count = re.subn(pattern, replacement, text, count=1)
        if count:
            reject(name, mutated)
        del mutated
    section, offset = None, 0
    for line in text.splitlines(keepends=True):
        match = re.fullmatch(r"[ \t]+(Interfaces|Static fields|Instance fields|Direct methods|Virtual methods)[ \t]*-[ \t]*", line.rstrip("\r\n"))
        if match:
            section = match[1]
        if line.startswith("Class #") or line.lstrip().startswith("source_file_idx"):
            section = None
        if section in ("Direct methods", "Virtual methods") and re.match(r"[ \t]+name[ \t]*:", line):
            reject("missing-method-name", text[:offset] + text[offset + len(line):])
            reject("duplicate-method-name", text[:offset] + line + text[offset:])
            break
        offset += len(line)
    footers = list(re.finditer(r"(?m)^[ \t]+source_file_idx[ \t]*:.*$", text))
    if footers:
        reject("truncated-last-class", text[:footers[-1].start()])
    match = re.search(r"(?m)^[ \t]+code[ \t]*(?:-|:[ \t]*\(none\))[ \t]*$", text)
    if match:
        replacement = "      code : (none)" if match[0].rstrip().endswith("-") else "      code -"
        reject("changed-body-presence", text[:match.start()] + replacement + text[match.end():])
    require(len(checks) >= (3 if header["class_defs_size"] else 1),
            "SDK output did not provide enough structure for mutation guards")
    return sorted(checks)


def combine_inventory(dumps: list[dict]) -> dict:
    classes, methods, inputs = {}, {}, set()
    for dump in dumps:
        name = dump["input"]
        require(name not in inputs, "Repeated DEX input inventory")
        inputs.add(name)
        for cls in dump["classes"]:
            identity = cls["descriptor"]
            require(identity not in classes, "Duplicate class across DEX inputs")
            classes[identity] = {**cls, "input": name}
        for method in dump["methods"]:
            identity = method["identity"]
            require(identity not in methods, "Duplicate method across DEX inputs")
            methods[identity] = method
    require(classes and methods, "No complete application method definitions were inventoried")
    return {"schema_version": 1, "provider": "android-sdk-dexdump", "inputs": sorted(inputs),
            "classes": classes, "methods": methods,
            "body_count": sum(row["role"] == "body" for row in methods.values()),
            "native_count": sum(row["role"] == "native" for row in methods.values()),
            "abstract_count": sum(row["role"] == "abstract" for row in methods.values())}


def compare_recovery(report: dict, coverage: dict, inventory: dict) -> dict:
    require(isinstance(report, dict) and isinstance(coverage, dict), "Recovery reports must be JSON objects")
    require(report.get("schema_version") == 1 and type(report.get("schema_version")) is int
            and report.get("status") == "success" and report.get("platform") == "android"
            and report.get("input_kind") == "apk", "Recovery did not report a successful complete APK")
    require(report.get("backend") == {"name": "neverd", "version": "1", "execution": "builtin"},
            "Recovery did not use the built-in NeverD engine")
    require(report.get("android_method_recovery") == coverage and coverage.get("status") == "recovered"
            and type(coverage.get("schema_version")) is int and coverage["schema_version"] == 1,
            "Standalone method coverage disagrees with the main report")
    require(report.get("input_code_files") == inventory["inputs"]
            and integer(report.get("dex_count"), "dex_count") == len(inventory["inputs"])
            and integer(report.get("smali_count"), "smali_count") == 0,
            "Recovery omitted or changed DEX input ownership")
    rows, seen = coverage.get("methods"), set()
    require(isinstance(rows, list), "Missing reported method definitions")
    for row in rows:
        require(isinstance(row, dict), "Invalid reported method definition")
        require(all(isinstance(row.get(key), str) for key in ("identity", "class", "name", "prototype", "input")),
                "Missing reported method identity")
        identity = row["identity"]
        require(identity not in seen and identity in inventory["methods"], "Duplicate or unexpected reported method")
        seen.add(identity)
        original = inventory["methods"][identity]
        require(all(row[key] == original[key] for key in ("class", "name", "prototype", "input"))
                and identity == row["class"] + "->" + row["name"] + row["prototype"],
                "Reported identity or DEX ownership changed")
        body = original["role"] == "body"
        require(row.get("status") == ("recovered" if body else "declaration-only"),
                "Original method body/declaration role changed")
        count = integer(row.get("instruction_count"), "instruction_count")
        require((count > 0) == body, "Claimed method body has no instructions or declaration has a body")
        if body:
            require(count <= original["code_units"], "Instruction count exceeds original code-unit count")
        else:
            require(isinstance(row.get("reason"), str) and row["reason"].strip(),
                    "Declaration-only method lacks an explanation")
    require(seen == set(inventory["methods"]), "Recovery omitted original method definitions")
    counts = {"class_count": len(inventory["classes"]), "method_count": len(inventory["methods"]),
              "recovered_method_count": inventory["body_count"],
              "declaration_only_method_count": inventory["native_count"] + inventory["abstract_count"],
              "unrecovered_method_count": 0}
    require(all(integer(coverage.get(key), key) == value for key, value in counts.items()),
            "Reported aggregate counts disagree with independent definitions")
    return counts


def source_artifacts(report: dict, output: Path) -> list[dict]:
    paths = report.get("java_sources")
    require(isinstance(paths, list) and paths and all(isinstance(name, str) for name in paths),
            "Missing Java source artifact inventory")
    require(len(paths) <= MAX_SOURCE_ENTRIES, "Java source artifact count exceeds evidence limit")
    require(len(set(paths)) == len(paths), "Duplicate Java source artifact")
    require((output / "sources").is_dir() and not (output / "sources").is_symlink(),
            "Missing or symlinked recovered source directory")
    actual, result, entries, total_bytes = set(), [], 0, 0
    def walk_error(error):
        raise error
    for directory, dirs, files in os.walk(output / "sources", followlinks=False,
                                          onerror=walk_error):
        entries += len(dirs) + len(files)
        require(entries <= MAX_SOURCE_ENTRIES, "Source tree entry count exceeds evidence limit")
        require(not any((Path(directory) / name).is_symlink() for name in dirs + files),
                "Recovered source tree contains a symlink")
        for name in files:
            path = Path(directory) / name
            require(path.is_file(), "Recovered source artifact is not a regular file")
            total_bytes += path.stat().st_size
            require(total_bytes <= MAX_SOURCE_BYTES, "Source tree exceeds evidence byte limit")
            if path.suffix == ".java":
                actual.add(path.relative_to(output).as_posix())
    require(set(paths) == actual, "Java source artifacts disagree with the report")
    require(integer(report.get("java_source_count"), "java_source_count", 1) == len(paths),
            "Java source count disagrees with artifacts")
    for name in sorted(paths):
        parts = PurePosixPath(name).parts
        require(parts and parts[0] == "sources" and ".." not in parts and "\\" not in name,
                "Unsafe reported source path")
        path = output / name
        result.append({"path": name, "sha256": sha256_file(path, MAX_DEX_BYTES), "size": path.stat().st_size})
    return result


def _read_json(path: Path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_DUMP_BYTES,
            "Missing, unsafe, or oversized JSON recovery artifact")
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, "Duplicate JSON field in recovery artifact")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pairs)


def run_android(ctx) -> None:
    """Run evidence stages; incomplete rebuild/behavior can never pass maturity."""
    profile = ctx.variant.get("profile")
    if profile not in ("official-release", "gradle-release", "gradle-debug"):
        for name in STAGES:
            ctx.stage(name, "incomplete", reason="Unknown Android application profile")
        ctx.fail("Android profile has no acceptance implementation")
        return
    stage, completed = "provenance", set()
    try:
        app, variant = ctx.app, ctx.variant
        commit = app.get("source_commit", "")
        require(isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit), "Missing fixed source commit")
        require(app.get("repository") and app.get("license"), "Missing application source/license provenance")
        original = ctx.command("android-source-head", ["git", "rev-parse", "HEAD"], cwd=ctx.source)
        require(original.stdout.strip() == commit, "Checked-out upstream source differs from its fixed commit")
        provenance = {"repository": app["repository"], "source_commit": commit,
                      "license": app["license"], "variant": variant}
        if profile == "official-release":
            asset = app.get("official_apk", {})
            url, expected_sha = asset.get("url", ""), asset.get("sha256", "")
            parsed = urlsplit(url)
            require(parsed.scheme == "https" and parsed.hostname == "github.com" and not parsed.username
                    and not parsed.password and not parsed.query and not parsed.fragment
                    and "/releases/download/" in parsed.path, "Official APK needs a fixed HTTPS GitHub release URL")
            require(isinstance(expected_sha, str) and re.fullmatch(r"[0-9a-f]{64}", expected_sha),
                    "Official APK needs a fixed SHA-256 digest")
            provenance["official_apk"] = asset
        else:
            provenance["source_build"] = app.get("android", {}).get("source_build")
        ctx.write_json("android-provenance.json", provenance)
        ctx.stage(stage, "success", source_commit=commit, repository=app["repository"], license=app["license"],
                  evidence=["android-provenance.json"])
        completed.add(stage)
        stage = "original_build"
        if profile == "official-release":
            apk = ctx.work / "official.apk"
            ctx.command("android-download-official-apk", ["curl", "--fail", "--location", "--proto", "=https",
                        "--proto-redir", "=https", "--max-time", str(ctx.timeout), "--max-filesize", str(MAX_APK_BYTES),
                        "--output", str(apk), url])
            actual_sha = sha256_file(apk, MAX_APK_BYTES)
            require(actual_sha == expected_sha, "Official APK SHA-256 mismatch")
            ctx.stage(stage, "success", input_kind="published-release", source_build=False,
                      apk_sha256=actual_sha, original_apk="official.apk", evidence=["official.apk"])
        else:
            built = build_source_apk(ctx)
            apk, actual_sha = built["apk"], built["sha256"]
            ctx.stage(stage, "success", **built["details"], apk_sha256=actual_sha,
                      original_apk=apk.relative_to(ctx.work).as_posix(), evidence=built["evidence"])
            dependency = built["dependency_provenance"]
            if not dependency.get("lock_verified"):
                provenance["dependency_qualification"] = dependency
                ctx.write_json("android-provenance.json", provenance)
                ctx.stage("provenance", "incomplete", source_commit=commit,
                          source_identity_verified=True, dependency_lock_verified=False,
                          reason=dependency["reason"],
                          evidence=["android-provenance.json", "android-dependency-provenance.json"])
                ctx.fail("Android source dependency provenance is incomplete: " + dependency["reason"])
        completed.add(stage)
        stage = "inventory"
        inventory = None
        try:
            android = app["android"]
            version = str(android["build_tools"])
            require(re.fullmatch(r"\d+\.\d+\.\d+", version), "Unpinned Android build-tools version")
            sdk_root = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
            require(bool(sdk_root), "ANDROID_SDK_ROOT or ANDROID_HOME must identify the CI SDK")
            tool = Path(sdk_root) / "build-tools" / version / ("dexdump.exe" if os.name == "nt" else "dexdump")
            require(tool.is_file(), "The pinned SDK dexdump executable is missing")
            inputs = extract_dex_inputs(apk, ctx.work / "dex-inputs")
            ctx.write_json("android-dex-inputs.json", [{**row, "path": row["path"].relative_to(ctx.work).as_posix()} for row in inputs])
            dumps = []
            for index, row in enumerate(inputs):
                dump = ctx.command(f"android-dexdump-{index:04d}", [str(tool), "-f", "-h", "-l", "plain", str(row["path"])])
                parsed_dump = parse_dexdump(dump.stdout, row["input"], row["header"])
                ctx.write_json(f"android-dex-inventory-{index:04d}.json", parsed_dump)
                mutation_names = inventory_mutation_checks(dump.stdout, row["input"], row["header"])
                ctx.write_json(f"android-dex-mutation-checks-{index:04d}.json", {"input": row["input"], "rejected": mutation_names})
                dumps.append(parsed_dump)
            inventory = combine_inventory(dumps)
            ctx.write_json("android-inventory.json", inventory)
            ctx.stage(stage, "success", provider="android-sdk-dexdump", build_tools=version,
                      dexdump_sha256=sha256_file(tool, MAX_DEX_BYTES), dex_count=len(inputs),
                      class_count=len(inventory["classes"]), method_count=len(inventory["methods"]),
                      body_count=inventory["body_count"], native_count=inventory["native_count"],
                      abstract_count=inventory["abstract_count"],
                      evidence=["android-inventory.json", "android-dex-inputs.json"]
                      + [f"android-dex-mutation-checks-{index:04d}.json" for index in range(len(inputs))])
        except Exception as error:
            # The original APK has an established source/release identity and
            # digest. An independent SDK/parser failure must not suppress the
            # native decoder evidence, but it leaves the denominator unknown.
            inventory = None
            ctx.write_json("android-inventory-failure.json", {"reason": str(error),
                           "independent_denominator_known": False, "apk_sha256": actual_sha})
            ctx.stage(stage, "failed", reason=str(error), evidence=["android-inventory-failure.json"])
            ctx.fail(f"Android inventory evidence failed: {error}")
        completed.add(stage)
        stage = "recovery"
        output = ctx.work / "recovered"
        env = os.environ.copy()
        env["NEVERD_JADX"] = str(ctx.work / "external-decompiler-must-not-run")
        cli = ctx.command("android-neverd-mobile", [str(ctx.neverd), "mobile", str(apk), "-o", str(output),
                          "--timeout", str(ctx.timeout), "--json"], env=env, allow_failure=True)
        if cli.returncode != 0:
            ctx.write_json("android-recovery-failure.json", {"returncode": cli.returncode,
                           "published_output": output.exists(), "independent_denominator_known": inventory is not None,
                           "expected_method_count": len(inventory["methods"]) if inventory is not None else None,
                           "expected_body_count": inventory["body_count"] if inventory is not None else None})
            require(not output.exists(), "Failed recovery published an output directory")
            raise AndroidEvidenceError("NeverD rejected the complete application APK; see android-neverd-mobile logs")
        report = _read_json(output / "report.json")
        coverage = _read_json(output / "metadata/android-methods.json")
        if inventory is None:
            require(isinstance(report, dict) and report.get("platform") == "android"
                    and report.get("input_kind") == "apk" and report.get("status") in ("success", "partial"),
                    "NeverD returned an invalid complete-APK report")
            require(isinstance(coverage, dict) and report.get("android_method_recovery") == coverage,
                    "NeverD recovery report and method metadata disagree")
            ctx.stage(stage, "incomplete", returncode=cli.returncode, report_status=report["status"],
                      independent_denominator_known=False,
                      reason="NeverD analyzed the complete APK, but independent inventory failed; coverage is unverified",
                      evidence=["recovered/report.json", "recovered/metadata/android-methods.json"])
        else:
            counts = compare_recovery(report, coverage, inventory)
            artifacts = source_artifacts(report, output)
            ctx.write_json("android-source-artifacts.json", artifacts)
            ctx.stage(stage, "success", **counts, java_source_count=len(artifacts),
                      scope="all method definitions across every inventoried DEX",
                      evidence=["recovered/report.json", "recovered/metadata/android-methods.json",
                                "android-source-artifacts.json"])
        completed.add(stage)
    except Exception as error:
        ctx.stage(stage, "failed", reason=str(error))
        for name in STAGES:
            if name not in completed and name != stage:
                ctx.stage(name, "incomplete", reason=f"Blocked by {stage} failure")
        ctx.fail(f"Android {stage} evidence failed: {error}")
        return

    ctx.stage("recompile", "incomplete", reason="Independent Java-only compilation and complete APK reconstruction are not implemented")
    ctx.stage("behavior", "incomplete", reason="Original/reconstructed APK ART business behavior comparison is not implemented")
    ctx.fail("Android recovery inventory alone cannot establish application maturity: recompile and behavior are incomplete")
