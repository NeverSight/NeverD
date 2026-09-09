"""CI-only diagnostic compilation of the complete generated Java inventory.

This produces a versioned *attempt*, never an independent-recompile proof.
Only copied generated Java and the selected Android platform's declarations
reach javac. D8 consumes only the class files produced by that javac invocation.
Original APK code, application dependencies, and handwritten stubs are not
compiler inputs. APK reconstruction and ART behavior remain separate obligations.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import zipfile


ATTEMPT_FILE = "android-java-compilation-attempt.json"
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_METADATA_BYTES = 256 * 1024 * 1024
MAX_TREE_BYTES = 1024 * 1024 * 1024
MAX_ENTRIES = 100_000
INJECTION_ENV = (
    "CLASSPATH", "JDK_JAVAC_OPTIONS", "JDK_JAVA_OPTIONS", "JAVA_TOOL_OPTIONS",
    "_JAVA_OPTIONS", "JAVA_OPTS", "GRADLE_OPTS", "LD_PRELOAD", "LD_LIBRARY_PATH",
    "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
)


class JavaCompilationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise JavaCompilationError(message)


def relative_source(value):
    require(isinstance(value, str) and value and "\\" not in value
            and not any(ord(char) < 32 or ord(char) == 127 for char in value),
            "Invalid generated Java path")
    parts = value.split("/")
    require(len(parts) > 1 and parts[0] == "sources"
            and all(part not in ("", ".", "..") and ":" not in part for part in parts)
            and value.endswith(".java"), "Unsafe generated Java path")
    return Path(*parts)


def regular_file(path, root, limit=MAX_FILE_BYTES):
    root = root.resolve()
    require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root),
            f"Missing or unsafe compilation input: {path.name}")
    parts = path.relative_to(root).parts
    require(not any((root / Path(*parts[:index])).is_symlink()
                    for index in range(1, len(parts))), "Symlinked compilation input directory")
    require(0 < path.stat().st_size <= limit, f"Empty or oversized compilation input: {path.name}")


def file_record(path, root, limit=MAX_FILE_BYTES):
    regular_file(path, root, limit)
    digest, size = hashlib.sha256(), 0
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(data)
            require(size <= limit, "Compilation input exceeded its byte limit while reading")
            digest.update(data)
    require(size > 0, "Compilation input became empty while reading")
    return {"path": path.relative_to(root).as_posix(), "size": size,
            "sha256": digest.hexdigest()}


def files_under(root):
    require(root.is_dir() and not root.is_symlink(), "Missing or symlinked compilation tree")
    count, total = 0, 0
    def walk_error(error):
        raise JavaCompilationError(f"Cannot enumerate compilation tree: {error}") from error
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        count += len(dirs) + len(files)
        require(count <= MAX_ENTRIES, "Compilation tree entry budget exceeded")
        require(not any((Path(directory) / name).is_symlink() for name in dirs + files),
                "Compilation tree contains a symlink")
        for name in sorted(files):
            path = Path(directory) / name
            require(stat.S_ISREG(path.stat().st_mode), "Compilation tree contains a nonregular file")
            total += path.stat().st_size
            require(total <= MAX_TREE_BYTES, "Compilation tree byte budget exceeded")
            yield path


def copy_checked(source, destination, root, expected):
    require(file_record(source, root) == expected, "Compilation input digest or size changed")
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest, total = hashlib.sha256(), 0
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        for data in iter(lambda: incoming.read(1024 * 1024), b""):
            total += len(data)
            require(total <= expected["size"], "Compilation input grew during snapshot")
            digest.update(data)
            outgoing.write(data)
    require(total == expected["size"] and digest.hexdigest() == expected["sha256"],
            "Compilation input changed during snapshot")


def snapshot_sources(report, sources, output, destination, work):
    require(isinstance(report, dict) and type(report.get("schema_version")) is int
            and report["schema_version"] == 1 and report.get("platform") == "android"
            and report.get("input_kind") == "apk" and report.get("status") in ("success", "partial"),
            "Compilation needs a valid APK recovery report")
    backend = report.get("backend")
    require(isinstance(backend, dict) and backend.get("name") == "neverd"
            and backend.get("execution") == "builtin", "Compilation input is not builtin generated Java")
    require(isinstance(sources, list) and 0 < len(sources) <= MAX_ENTRIES
            and all(isinstance(row, dict) for row in sources), "Missing complete Java input inventory")
    expected = {}
    for row in sources:
        require(set(row) == {"path", "size", "sha256"}, "Malformed Java input inventory row")
        relative_source(row["path"])
        require(type(row["size"]) is int and 0 < row["size"] <= MAX_FILE_BYTES
                and isinstance(row["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", row["sha256"]),
                "Malformed Java input size or digest")
        require(row["path"] not in expected, "Duplicate Java input inventory row")
        expected[row["path"]] = row
    paths = report.get("java_sources")
    require(isinstance(paths, list) and all(isinstance(path, str) for path in paths)
            and len(paths) == len(expected) and set(paths) == set(expected)
            and type(report.get("java_source_count")) is int
            and report["java_source_count"] == len(expected), "Java inventory and report disagree")
    # Scan the entire publication, including unexpected Java outside sources/.
    # Other files are never copied or added to any compiler search path.
    actual = {path.relative_to(output).as_posix() for path in files_under(output)
              if path.suffix == ".java"}
    require(actual == set(expected), "Unlisted or missing generated Java source")
    require(sum(row["size"] for row in sources) <= MAX_TREE_BYTES, "Java source byte budget exceeded")
    snapshots = []
    for name in sorted(expected):
        relative = relative_source(name)
        source = output / relative
        target = destination / Path(*relative.parts[1:])
        copy_checked(source, target, output, expected[name])
        snapshots.append({"generated_path": name, **file_record(target, work)})
    return snapshots


def properties(path):
    regular_file(path, path.parent, 1024 * 1024)
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        require("=" in line, "Unsupported toolchain properties syntax")
        key, value = (part.strip() for part in line.split("=", 1))
        require(key not in result, "Duplicate toolchain property")
        result[key] = value.strip('"')
    return result


def tools_for(ctx):
    android = ctx.app.get("android", {})
    sdk_version, build_tools = android.get("compile_sdk"), android.get("build_tools")
    java_major = android.get("java")
    require(type(sdk_version) is int and sdk_version > 0, "Missing fixed Android platform version")
    require(isinstance(build_tools, str) and re.fullmatch(r"\d+\.\d+\.\d+", build_tools),
            "Missing fixed Android build-tools version")
    require(str(java_major) == "21", "Java compilation currently requires the configured JDK 21")
    sdk_value = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    jdk_value = os.environ.get("JAVA_HOME")
    require(sdk_value and jdk_value, "The configured Android SDK and JAVA_HOME are required")
    sdk, jdk = Path(sdk_value).resolve(), Path(jdk_value).resolve()
    platform = sdk / "platforms" / f"android-{sdk_version}"
    build = sdk / "build-tools" / build_tools
    suffix = ".exe" if os.name == "nt" else ""
    paths = {"java": jdk / ("bin/java" + suffix), "javac": jdk / ("bin/javac" + suffix),
             "jdk_release": jdk / "release", "jdk_modules": jdk / "lib/modules",
             "android_jar": platform / "android.jar", "platform_properties": platform / "source.properties",
             "build_tools_properties": build / "source.properties", "aapt2": build / ("aapt2" + suffix),
             "d8_jar": build / "lib/d8.jar"}
    identities = {}
    for key, path in paths.items():
        root = jdk if key.startswith("jdk_") or key in ("java", "javac") else sdk
        identities[key] = {"root": str(root), **file_record(path, root)}
    release = properties(paths["jdk_release"])
    require(re.fullmatch(r"21(?:[.+_-].*)?", release.get("JAVA_VERSION", "")),
            "Actual JDK release differs from the configured major version")
    require(properties(paths["platform_properties"]).get("AndroidVersion.ApiLevel") == str(sdk_version),
            "Actual SDK platform differs from the configured API level")
    require(properties(paths["build_tools_properties"]).get("Pkg.Revision") == build_tools,
            "Actual SDK build-tools package differs from its configured version")
    return paths, identities, jdk, {"compile_sdk": sdk_version, "build_tools": build_tools,
                                  "java_version": release["JAVA_VERSION"]}


def manifest_facts(text, expected_package):
    packages = re.findall(r"^package: name='([^']+)'(?:\s|$)", text, re.MULTILINE)
    minimums = re.findall(r"^sdkVersion:'([^']+)'\s*$", text, re.MULTILINE)
    require(len(packages) == 1 and isinstance(expected_package, str)
            and packages[0] == expected_package, "APK package identity is missing, ambiguous, or differs")
    require(len(minimums) == 1 and re.fullmatch(r"[1-9][0-9]*", minimums[0])
            and int(minimums[0]) <= 10000, "APK minimum SDK is missing, ambiguous, or nonnumeric")
    return {"package_name": packages[0], "min_sdk": int(minimums[0])}


def javac_argument_file(paths):
    # javac expands this one argument file, not shell syntax or nested @files.
    # Absolute quoted paths avoid both option-prefix and whitespace ambiguity.
    rows = []
    for path in paths:
        value = str(path.resolve())
        require(not any(ord(char) < 32 or ord(char) == 127 for char in value),
                "A Java input path cannot be represented in a javac argument file")
        rows.append('"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"')
    return "\n".join(rows) + "\n"


def class_outputs(directory, work):
    rows = []
    for path in files_under(directory):
        require(path.suffix == ".class", "Unexpected non-class javac output")
        with path.open("rb") as stream:
            header = stream.read(8)
        require(len(header) == 8 and header[:4] == b"\xca\xfe\xba\xbe"
                and struct.unpack_from(">H", header, 6)[0] == 52, "Invalid or unexpected Java 8 class output")
        rows.append(file_record(path, work))
    require(rows, "javac produced no class files")
    return sorted(rows, key=lambda row: row["path"])


def package_classes(rows, directory, destination, work):
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_STORED) as archive:
        for row in rows:
            path = work / row["path"]
            require(file_record(path, work) == row, "javac output changed before D8 packaging")
            member = zipfile.ZipInfo(path.relative_to(directory).as_posix(), (1980, 1, 1, 0, 0, 0))
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            size, digest = 0, hashlib.sha256()
            with path.open("rb") as incoming, archive.open(member, "w") as outgoing:
                for data in iter(lambda: incoming.read(1024 * 1024), b""):
                    size += len(data)
                    require(size <= row["size"], "javac output grew during D8 packaging")
                    digest.update(data)
                    outgoing.write(data)
            require(size == row["size"] and digest.hexdigest() == row["sha256"],
                    "javac output changed during D8 packaging")
    return file_record(destination, work, MAX_TREE_BYTES)


def dex_outputs(path):
    rows, seen, total = [], set(), 0
    with zipfile.ZipFile(path) as archive:
        require(0 < len(archive.infolist()) <= MAX_ENTRIES, "D8 output count exceeds its bounds")
        for member in archive.infolist():
            name = member.filename
            require(re.fullmatch(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex", name)
                    and name not in seen and not member.is_dir() and not member.flag_bits & 1
                    and not stat.S_ISLNK(member.external_attr >> 16), "Unexpected or duplicate D8 output member")
            seen.add(name)
            total += member.file_size
            require(112 <= member.file_size <= MAX_FILE_BYTES and total <= MAX_TREE_BYTES,
                    "D8 output byte budget exceeded")
            digest, size, header = hashlib.sha256(), 0, b""
            with archive.open(member) as stream:
                for data in iter(lambda: stream.read(1024 * 1024), b""):
                    size += len(data)
                    require(size <= member.file_size, "D8 output exceeded its declared size")
                    if not header:
                        header = data[:112]
                    digest.update(data)
            require(size == member.file_size and len(header) == 112
                    and header[:4] == b"dex\n" and header[4:7] in (b"035", b"037", b"038", b"039", b"040")
                    and header[7] == 0 and struct.unpack_from("<III", header, 32)
                    == (size, 112, 0x12345678), "Malformed D8 output header")
            rows.append({"member": name, "size": size, "sha256": digest.hexdigest(),
                         "validation": "container-and-fixed-header-only"})
    require("classes.dex" in seen, "D8 output has no primary DEX")
    return sorted(rows, key=lambda row: row["member"])


def attempt_java_recompile(ctx, *, apk, output, report, sources, recovery_qualified):
    """Keep diagnostic compiler evidence without upgrading recovery or maturity."""
    result = getattr(ctx, "result", {})
    attempt = {"schema_version": 1, "kind": "generated-java-compilation", "status": "incomplete",
               "phase": "prepare", "case_id": result.get("case_id", ctx.variant.get("id")),
               "source_commit": ctx.app.get("source_commit"), "consumer_commit": result.get("consumer_commit"),
               "manifest_sha256": result.get("manifest_sha256"), "execution_role": "application-case-job",
               "independent": False, "maturity_qualified": False, "recovery_qualified": bool(recovery_qualified),
               "scope": "all inventoried generated Java; not the complete original APK implementation",
               "compile_complete": False, "original_code_compiler_inputs": [],
               "handwritten_stub_inputs": [], "additional_application_dependencies": [],
               "apk_reconstructed": False, "art_behavior_verified": False,
               "native_libraries": "retained only in the unchanged original APK; not recompiled or behavior-verified",
               "non_code_resources": "retained only in the unchanged original APK; not assembled into a rebuilt APK",
               "remaining_obligations": ["independent trusted compilation rerun", "complete original method coverage",
                                          "complete APK reconstruction", "ART business behavior comparison",
                                          "native-library behavior where present"],
               "commands": [], "evidence": [ATTEMPT_FILE]}
    ctx.write_json(ATTEMPT_FILE, attempt)
    root, tools, identities, guards = ctx.work / "java-compilation", {}, {}, []
    owns_directory = False
    first_command = len(result.get("commands", []))
    try:
        require(output.resolve().is_relative_to(ctx.work.resolve()) and not output.is_symlink(),
                "Generated output is outside the evidence directory")
        root.mkdir(exist_ok=False)
        owns_directory = True
        directories = {name: root / name for name in ("source", "classes", "platform", "empty", "tmp", "home")}
        for directory in directories.values():
            directory.mkdir()
        report_path = output / "report.json"
        report_record = file_record(report_path, ctx.work, MAX_METADATA_BYTES)
        def unique_pairs(values):
            parsed = {}
            for key, value in values:
                require(key not in parsed, "Duplicate field in compilation recovery report")
                parsed[key] = value
            return parsed
        require(json.loads(report_path.read_text(encoding="utf-8"), object_pairs_hook=unique_pairs) == report,
                "Recovery report changed before Java compilation")
        attempt["recovery_report"] = report_record
        guards.append((report_path, ctx.work, report_record))
        snapshots = snapshot_sources(report, sources, output, directories["source"], ctx.work)
        attempt["sources"] = snapshots
        attempt["source_count"] = len(snapshots)
        guards.extend((output / row["path"], output, row) for row in sources)
        guards.extend((ctx.work / row["path"], ctx.work,
                       {key: row[key] for key in ("path", "size", "sha256")}) for row in snapshots)
        original = file_record(apk, ctx.work)
        attempt["original_apk"] = {**original, "usage": "manifest facts only; never passed to javac or D8"}
        guards.append((apk, ctx.work, original))
        tools, identities, jdk, versions = tools_for(ctx)
        attempt["toolchain"] = {**versions, "files": identities, "identity_status": "observed-not-independently-certified"}
        for key, row in identities.items():
            guards.append((tools[key], Path(row["root"]), {k: row[k] for k in ("path", "size", "sha256")}))
        sdk_root = Path(identities["android_jar"]["root"])
        platform = directories["platform"] / "android.jar"
        copy_checked(tools["android_jar"], platform, sdk_root,
                     {key: identities["android_jar"][key] for key in ("path", "size", "sha256")})
        platform_record = file_record(platform, ctx.work)
        guards.append((platform, ctx.work, platform_record))
        attempt["external_declarations"] = [{"origin": "android-platform-sdk", **platform_record}]
        env = {key: "" for key in INJECTION_ENV}
        env.update(JAVA_HOME=str(jdk), HOME=str(directories["home"]), USERPROFILE=str(directories["home"]),
                   TMPDIR=str(directories["tmp"]), TMP=str(directories["tmp"]), TEMP=str(directories["tmp"]))
        attempt["cleared_injection_environment"] = list(INJECTION_ENV)
        attempt["phase"] = "tool-identities"
        ctx.write_json(ATTEMPT_FILE, attempt)
        def command(name, argv):
            return ctx.command(name, [str(arg) for arg in argv], cwd=root, env=env,
                               timeout=ctx.timeout, allow_failure=True)
        for name, argv in (("javac-version", [tools["javac"], "-version"]),
                           ("java-version", [tools["java"], "--version"]),
                           ("aapt2-version", [tools["aapt2"], "version"]),
                           ("d8-version", [tools["java"], "-cp", tools["d8_jar"], "com.android.tools.r8.D8", "--version"])):
            probe = command("android-recompile-" + name, argv)
            require(probe.returncode == 0, "Compilation tool identity probe failed: " + name)
        attempt["phase"] = "manifest"
        ctx.write_json(ATTEMPT_FILE, attempt)
        badging = command("android-recompile-apk-manifest", [tools["aapt2"], "dump", "badging", apk])
        require(badging.returncode == 0, "SDK aapt2 could not read the original APK manifest")
        android = ctx.app["android"]
        expected_package = android.get("package_name")
        if ctx.variant.get("profile") in ("gradle-release", "gradle-debug"):
            expected_package = android.get("source_build", {}).get("profiles", {}).get(ctx.variant["profile"], {}).get("application_id")
        facts = manifest_facts(badging.stdout, expected_package)
        attempt["manifest"] = facts
        argument_file = root / "javac-sources.args"
        argument_file.write_text(javac_argument_file([ctx.work / row["path"] for row in snapshots]), encoding="utf-8")
        attempt["javac_argument_file"] = file_record(argument_file, ctx.work)
        guards.append((argument_file, ctx.work, attempt["javac_argument_file"]))
        attempt["phase"] = "javac"
        ctx.write_json(ATTEMPT_FILE, attempt)
        compiled = command("android-recompile-javac", [tools["javac"], "-J-Xmx2g", "-encoding", "UTF-8",
            "-source", "8", "-target", "8", "-bootclasspath", platform,
            "-classpath", directories["empty"], "-sourcepath", directories["empty"],
            "-processorpath", directories["empty"], "-proc:none", "-implicit:none",
            "-d", directories["classes"], "@" + str(argument_file)])
        attempt["javac"] = {"returncode": compiled.returncode, "status": "success" if compiled.returncode == 0 else "failed"}
        require(compiled.returncode == 0, "javac rejected the complete generated Java source set; see compiler logs")
        classes = class_outputs(directories["classes"], ctx.work)
        attempt["classes"] = classes
        guards.extend((ctx.work / row["path"], ctx.work, row) for row in classes)
        generated_jar = root / "generated-classes.jar"
        attempt["generated_class_archive"] = package_classes(classes, directories["classes"], generated_jar, ctx.work)
        attempt["phase"] = "d8"
        ctx.write_json(ATTEMPT_FILE, attempt)
        dex_zip = root / "generated-dex.zip"
        mode = "--debug" if ctx.variant.get("profile") == "gradle-debug" else "--release"
        converted = command("android-recompile-d8", [tools["java"], "-Xmx2g", "-cp", tools["d8_jar"],
            "com.android.tools.r8.D8", mode, "--lib", platform, "--min-api", str(facts["min_sdk"]),
            "--output", dex_zip, generated_jar])
        attempt["d8"] = {"returncode": converted.returncode, "mode": mode,
                         "status": "success" if converted.returncode == 0 else "failed"}
        require(converted.returncode == 0, "D8 rejected the newly generated class files; see compiler logs")
        require(not re.search(r"^\s*(?:warning|error|missing class)\b", converted.stdout + "\n" + converted.stderr,
                              re.IGNORECASE | re.MULTILINE), "D8 emitted unresolved diagnostics; see compiler logs")
        attempt["dex_archive"] = file_record(dex_zip, ctx.work, MAX_TREE_BYTES)
        attempt["dex_outputs"] = dex_outputs(dex_zip)
        require(file_record(generated_jar, ctx.work, MAX_TREE_BYTES) == attempt["generated_class_archive"],
                "D8 input archive changed during compilation")
        attempt.update(status="success", phase="complete", compile_complete=True,
                       reason="Generated Java compiled through javac and D8; independent and application behavior obligations remain")
    except Exception as error:
        attempt.update(status="failed", compile_complete=False, reason=str(error))
    finally:
        # Preserve failures and any partial compiler outputs; never delete them
        # or promote an earlier recovery failure after a successful compiler run.
        try:
            for path, base, expected in guards:
                require(file_record(path, base) == expected, "A compilation input changed during tool execution")
        except Exception as error:
            attempt.update(status="failed", compile_complete=False,
                           reason=attempt.get("reason", "") + "; input integrity check failed: " + str(error))
        observed = []
        try:
            classes_directory = root / "classes"
            if owns_directory and classes_directory.exists():
                observed.extend(file_record(path, ctx.work) for path in files_under(classes_directory))
            for name in ("generated-classes.jar", "generated-dex.zip"):
                path = root / name
                if owns_directory and path.exists():
                    observed.append(file_record(path, ctx.work, MAX_TREE_BYTES))
        except Exception as error:
            attempt.update(status="failed", compile_complete=False,
                           reason=attempt.get("reason", "") + "; output inventory failed: " + str(error))
        attempt["observed_compiler_outputs"] = observed
        attempt["commands"] = [row["id"] for row in result.get("commands", [])[first_command:]]
        ctx.write_json(ATTEMPT_FILE, attempt)
    return attempt
