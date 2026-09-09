"""Actions-only original Android source builds and their provenance.

This builds the pinned application's own Gradle tasks. It does not rebuild
NeverD's generated Java, and it does not establish ART behavior equivalence.
AGP's output-metadata.json identifies the APK; filenames are never guessed.
Public contracts: https://gradle.org/release-checksums/ and
https://developer.android.com/reference/tools/gradle-api/8.1/com/android/build/api/variant/BuiltArtifacts
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import zipfile

try:
    from .mobile_real_apps_common import load_json
except ImportError:
    from mobile_real_apps_common import load_json


MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_METADATA_BYTES = 64 * 1024 * 1024
MAX_TREE_BYTES = 1024 * 1024 * 1024
MAX_ENTRIES = 100_000


class SourceBuildError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise SourceBuildError(message)


def relative_path(value):
    require(isinstance(value, str) and value and "\\" not in value and "\x00" not in value,
            "Invalid source-build relative path")
    parts = value.split("/")
    require(not any(part in ("", ".", "..") or ":" in part for part in parts),
            "Unsafe source-build relative path")
    return Path(*PurePosixPath(value).parts)


def regular_file(path, root, limit=MAX_FILE_BYTES, *, allow_empty=False):
    root = root.resolve()
    require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root),
            f"Missing or unsafe source-build file: {path.name}")
    relative = path.relative_to(root)
    require(not any((root / Path(*relative.parts[:index])).is_symlink()
                    for index in range(1, len(relative.parts))), "Symlinked source-build directory")
    require((0 if allow_empty else 1) <= path.stat().st_size <= limit, f"Empty or oversized source-build file: {path.name}")
    return path


def file_record(path, root, limit=MAX_FILE_BYTES, *, allow_empty=False):
    regular_file(path, root, limit, allow_empty=allow_empty)
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            total += len(chunk)
            require(total <= limit, "Source-build file grew beyond its byte limit")
            digest.update(chunk)
    return {"path": path.relative_to(root).as_posix(), "size": total, "sha256": digest.hexdigest()}


def files_under(root):
    require(root.is_dir() and not root.is_symlink(), f"Missing or unsafe source-build directory: {root.name}")
    count = 0
    def walk_error(error):
        raise SourceBuildError(f"Cannot enumerate source-build artifacts: {error}") from error
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        count += len(dirs) + len(files)
        require(count <= MAX_ENTRIES, "Source-build tree entry budget exceeded")
        require(not any((Path(directory) / name).is_symlink() for name in dirs + files),
                "Source-build artifact tree contains a symlink")
        for name in sorted(files):
            yield Path(directory) / name


def source_profile(app, profile):
    require(profile in ("gradle-release", "gradle-debug"), "Unsupported Gradle source profile")
    android = app.get("android", {})
    build = android.get("source_build")
    require(isinstance(build, dict), "Missing pinned Android source-build configuration")
    selected = build.get("profiles", {}).get(profile)
    require(isinstance(selected, dict), "Missing pinned Gradle variant")
    version = build.get("gradle_version", "")
    require(isinstance(version, str) and re.fullmatch(r"\d+\.\d+(?:\.\d+)?", version), "Unpinned Gradle version")
    require(build.get("distribution_url") == f"https://services.gradle.org/distributions/gradle-{version}-bin.zip",
            "Gradle distribution must be the pinned official binary ZIP")
    checksum = build.get("distribution_sha256", "")
    require(isinstance(checksum, str) and re.fullmatch(r"[0-9a-f]{64}", checksum), "Missing Gradle distribution SHA-256")
    require(type(build.get("jdk_major")) is int and build["jdk_major"] >= 17, "Missing supported JDK major version")
    require(isinstance(build.get("agp_version"), str) and re.fullmatch(r"\d+\.\d+\.\d+", build["agp_version"]),
            "Missing fixed AGP version")
    require(isinstance(build.get("build_tools"), str) and re.fullmatch(r"\d+\.\d+\.\d+", build["build_tools"]),
            "Missing fixed source-build SDK tools version")
    require(type(android.get("compile_sdk")) is int and android["compile_sdk"] > 0, "Missing compile SDK")
    module = relative_path(build.get("module_path"))
    require(all(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9-]*", part) for part in module.parts), "Unsafe Gradle module name")
    variant = selected.get("variant", "")
    kind = "release" if profile == "gradle-release" else "debug"
    require(isinstance(variant, str) and re.fullmatch(r"[A-Za-z][A-Za-z_0-9]*", variant)
            and (variant == kind or variant.endswith(kind.capitalize())), "Pinned Gradle variant disagrees with profile")
    module_id = ":" + ":".join(module.parts)
    task = android.get("release_task" if kind == "release" else "debug_task")
    require(task == module_id + ":assemble" + variant[0].upper() + variant[1:],
            "Pinned Gradle task disagrees with the exact module and variant")
    require(isinstance(selected.get("application_id"), str)
            and re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)+", selected["application_id"]),
            "Missing exact variant application ID")
    require(all(type(selected.get(key)) is bool for key in ("minify", "shrink_resources")),
            "Missing explicit original optimization settings")
    return {**build, **selected, "profile": profile, "task": task, "module_id": module_id,
            "compile_sdk": android["compile_sdk"], "build_type": kind}


def wrapper_url(text):
    # The two pinned wrappers use simple key=value properties. Reject an
    # ambiguous encoding instead of interpreting a different download URL.
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "!")):
            continue
        require("=" in line and not line.endswith("\\"), "Unsupported Gradle wrapper properties syntax")
        key, value = (part.strip() for part in line.split("=", 1))
        require(key not in values, "Duplicate Gradle wrapper property")
        values[key] = value.replace("\\:", ":")
    require("distributionUrl" in values, "Wrapper has no distribution URL")
    return values


def unpack_gradle(archive, destination, version):
    destination.mkdir(parents=True, exist_ok=False)
    total, seen = 0, set()
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        require(len(members) <= 10000, "Gradle distribution entry limit exceeded")
        for member in members:
            relative = relative_path(member.filename.rstrip("/"))
            require(relative.parts[0] == "gradle-" + version, "Unexpected Gradle distribution root")
            key = relative.as_posix().casefold()
            require(key not in seen, "Duplicate Gradle distribution path")
            seen.add(key)
            require(not stat.S_ISLNK(member.external_attr >> 16) and not member.flag_bits & 1,
                    "Unsafe Gradle distribution member")
            total += member.file_size
            require(total <= MAX_TREE_BYTES and member.file_size <= MAX_FILE_BYTES,
                    "Gradle distribution byte limit exceeded")
            path = destination / relative
            if member.is_dir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with source.open(member) as incoming, path.open("xb") as outgoing:
                copied = 0
                for chunk in iter(lambda: incoming.read(1024 * 1024), b""):
                    copied += len(chunk)
                    require(copied <= member.file_size, "Gradle ZIP member exceeded its declared size")
                    outgoing.write(chunk)
                require(copied == member.file_size, "Truncated Gradle ZIP member")
    launcher = destination / ("gradle-" + version) / "bin/gradle"
    regular_file(launcher, destination)
    return launcher


def select_apk(output_root, expected):
    """Require AGP's sole unfiltered complete APK for the exact variant."""
    paths = list(files_under(output_root))
    metadata = [path for path in paths if path.name == "output-metadata.json"]
    require(len(metadata) == 1, "Expected exactly one AGP output-metadata.json after the isolated build")
    regular_file(metadata[0], output_root, MAX_METADATA_BYTES)
    value = load_json(metadata[0])
    require(isinstance(value, dict) and type(value.get("version")) is int and value["version"] == 3,
            "Unsupported AGP output metadata version")
    require(value.get("artifactType") == {"type": "APK", "kind": "Directory"}
            and value.get("elementType") == "File", "AGP metadata is not an APK artifact directory")
    require(value.get("variantName") == expected["variant"]
            and value.get("applicationId") == expected["application_id"], "AGP output variant or application ID differs")
    elements = value.get("elements")
    require(isinstance(elements, list) and len(elements) == 1 and isinstance(elements[0], dict),
            "Split or ambiguous APK output is not a complete application input")
    element = elements[0]
    require(element.get("type") in ("SINGLE", "UNIVERSAL") and element.get("filters") == [],
            "Filtered APK output cannot stand in for the complete application")
    require(type(element.get("versionCode")) is int and element["versionCode"] > 0
            and isinstance(element.get("versionName"), str) and element["versionName"], "Missing APK version identity")
    relative = relative_path(element.get("outputFile"))
    require(relative.suffix == ".apk", "AGP output is not an APK")
    apk = metadata[0].parent / relative
    regular_file(apk, output_root)
    require({path.resolve() for path in paths if path.suffix.lower() == ".apk"} == {apk.resolve()},
            "Unlisted or multiple APK files exist in the output directory")
    return apk, metadata[0], value


def verify_effective_settings(value, expected):
    require(isinstance(value, dict) and type(value.get("schema_version")) is int
            and value["schema_version"] == 1, "Missing Gradle settings observation")
    wanted = {"gradle_version": expected["gradle_version"], "agp_version": expected["agp_version"],
              "module": expected["module_id"], "variant": expected["variant"],
              "application_id": expected["application_id"], "build_type": expected["build_type"],
              "compile_sdk": "android-" + str(expected["compile_sdk"]), "build_tools": expected["build_tools"],
              "minify": expected["minify"], "shrink_resources": expected["shrink_resources"]}
    require(all(type(value.get(key)) is type(wanted_value) and value[key] == wanted_value
                for key, wanted_value in wanted.items()), "Actual Gradle/AGP variant or optimization settings differ from the pinned input")
    require(value.get("tasks") == ["clean", expected["task"]], "Unexpected Gradle build tasks")


# Observations use Gradle's public resolution callbacks and the AGP variant
# object. No build type, source, dependency version, or compiler flag is changed.
# An unresolved/partial observation is retained and never called a lock proof.
GRADLE_OBSERVER = r'''import groovy.json.JsonOutput
def destination = new File(System.getenv('NEVERD_BUILD_EVIDENCE'))
def eventLock = new Object()
def eventFile = new File(destination, 'android-gradle-resolutions.ndjson')
def saveEvent = { Map event ->
    synchronized (eventLock) {
        if (eventFile.length() < 64L * 1024 * 1024) {
            eventFile.append(JsonOutput.toJson(event) + '\n', 'UTF-8')
        }
    }
}
def observe = { project, configuration, scope ->
    configuration.incoming.afterResolve {
        try {
            def resolution = configuration.incoming.resolutionResult
            def components = resolution.allComponents.collect { component ->
                [id: component.id.displayName, module: component.moduleVersion?.toString()]
            }
            def dependencies = resolution.allDependencies.collect { dependency ->
                [from: dependency.from.id.displayName, requested: dependency.requested.displayName,
                 selected: dependency.hasProperty('selected') ? dependency.selected.id.displayName : null,
                 failure: dependency.hasProperty('failure') ? dependency.failure.message : null]
            }
            saveEvent([schema_version: 1, kind: 'resolution', project: project.path,
                       scope: scope, configuration: configuration.name,
                       components: components, dependencies: dependencies])
        } catch (Exception failure) {
            saveEvent([schema_version: 1, kind: 'observation-error', project: project.path,
                       configuration: configuration.name, reason: failure.toString()])
        }
    }
}
gradle.beforeProject { project ->
    project.configurations.configureEach { configuration -> observe(project, configuration, 'project') }
    project.buildscript.configurations.configureEach { configuration -> observe(project, configuration, 'buildscript') }
}
gradle.projectsEvaluated {
    def value
    try {
        def project = gradle.rootProject.findProject(System.getenv('NEVERD_BUILD_MODULE'))
        def android = project.extensions.getByName('android')
        def variant = android.applicationVariants.find { it.name == System.getenv('NEVERD_BUILD_VARIANT') }
        if (variant == null) { throw new IllegalStateException('The requested Android variant was not found') }
        def buildType = android.buildTypes.getByName(variant.buildType.name)
        def versionClass = android.class.classLoader.loadClass('com.android.Version')
        value = [schema_version: 1, gradle_version: gradle.gradleVersion,
                 agp_version: versionClass.getField('ANDROID_GRADLE_PLUGIN_VERSION').get(null),
                 module: project.path, variant: variant.name, application_id: variant.applicationId,
                 build_type: buildType.name, compile_sdk: android.compileSdkVersion,
                 build_tools: android.buildToolsVersion, minify: buildType.minifyEnabled,
                 shrink_resources: buildType.shrinkResources,
                 java_version: System.getProperty('java.version'), java_home: System.getProperty('java.home'),
                 tasks: gradle.startParameter.taskNames]
    } catch (Exception failure) {
        value = [schema_version: 1, error: failure.toString()]
    }
    new File(destination, 'android-gradle-effective.json').setText(JsonOutput.toJson(value), 'UTF-8')
}
'''


def capture_build_inputs(ctx):
    listed = ctx.command("android-tracked-build-inputs", ["git", "ls-files", "-z"], cwd=ctx.source).stdout
    require(listed.endswith("\x00"), "Incomplete tracked source inventory")
    names = listed[:-1].split("\x00")
    require(len(names) <= MAX_ENTRIES and len(set(names)) == len(names), "Invalid tracked source inventory")
    selected = []
    for name in names:
        path = relative_path(name)
        if (path.name in ("gradlew", "gradlew.bat", "gradle-wrapper.jar", "gradle-wrapper.properties", "gradle.lockfile",
                          "verification-metadata.xml", "libs.versions.toml", "gradle.properties")
                or path.name.endswith((".gradle", ".gradle.kts", ".lockfile"))
                or "proguard" in path.name.lower()):
            selected.append(name)
    require("gradle/wrapper/gradle-wrapper.properties" in selected, "Tracked Gradle wrapper properties are missing")
    records = []
    for name in sorted(selected):
        source = ctx.source / relative_path(name)
        # Empty Gradle/ProGuard configuration files are valid tracked inputs;
        # preserve their exact digest instead of treating them as missing.
        record = file_record(source, ctx.source, MAX_METADATA_BYTES, allow_empty=True)
        target = ctx.work / "android-build-inputs" / relative_path(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        records.append(record)
    license_source = ctx.source / relative_path(ctx.app.get("license_path"))
    record = file_record(license_source, ctx.source, MAX_METADATA_BYTES)
    shutil.copyfile(license_source, ctx.work / "android-source-LICENSE.txt")
    ctx.write_json("android-build-inputs.json", {"source_commit": ctx.app["source_commit"],
                   "tracked_build_inputs": records, "license": record})
    return records


def capture_dependencies(ctx, cache, inputs):
    """Observed bytes are evidence, not a reviewed transitive dependency lock."""
    result = {"schema_version": 1, "status": "incomplete", "lock_verified": False,
              "reason": "No reviewed complete dependency-lock and verification closure is implemented; observed resolutions are not a lock",
              "tracked_lock_inputs": [row for row in inputs if row["path"].endswith((".lockfile", "verification-metadata.xml"))],
              "cache_artifacts": [], "observed_resolution_count": 0, "errors": []}
    events = ctx.work / "android-gradle-resolutions.ndjson"
    if events.exists():
        try:
            regular_file(events, ctx.work, MAX_METADATA_BYTES)
            with events.open(encoding="utf-8") as stream:
                for line in stream:
                    require(len(line) <= 8 * 1024 * 1024, "Dependency observation line exceeds budget")
                    value = json.loads(line)
                    require(isinstance(value, dict) and type(value.get("schema_version")) is int
                            and value["schema_version"] == 1, "Invalid dependency observation")
                    if value.get("kind") == "resolution":
                        result["observed_resolution_count"] += 1
                    else:
                        result["errors"].append(value.get("reason", "Unknown dependency observation event"))
        except (OSError, ValueError, SourceBuildError) as error:
            result["errors"].append(str(error))
    else:
        result["errors"].append("Gradle produced no dependency resolution observations")
    artifacts = cache / "gradle-user-home/caches/modules-2/files-2.1"
    try:
        total = 0
        if artifacts.exists():
            for path in files_under(artifacts):
                record = file_record(path, artifacts)
                total += record["size"]
                require(total <= 8 * MAX_TREE_BYTES, "Resolved dependency byte inventory exceeds budget")
                result["cache_artifacts"].append(record)
        else:
            result["errors"].append("Fresh Gradle cache has no Maven artifact inventory")
    except (OSError, ValueError, SourceBuildError) as error:
        result["errors"].append(str(error))
    result["cache_artifacts"].sort(key=lambda row: row["path"])
    ctx.write_json("android-dependency-provenance.json", result)
    return result


def build_source_apk(ctx):
    expected = source_profile(ctx.app, ctx.variant["profile"])
    require(os.name == "posix", "Android source-build cases currently require a POSIX Actions runner")
    clean = ctx.command("android-source-clean", ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=ctx.source)
    require(not clean.stdout.strip(), "Pinned source checkout is dirty before the original build")
    records = capture_build_inputs(ctx)
    properties_path = ctx.source / "gradle/wrapper/gradle-wrapper.properties"
    properties = wrapper_url(properties_path.read_text(encoding="utf-8"))
    require(properties["distributionUrl"] == expected["distribution_url"], "Manifest Gradle distribution differs from the pinned wrapper")
    require("distributionSha256Sum" not in properties or properties["distributionSha256Sum"] == expected["distribution_sha256"],
            "Pinned wrapper checksum differs from the Gradle distribution digest")
    java_home_text = os.environ.get("JAVA_HOME")
    sdk_text = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    require(java_home_text and sdk_text, "JAVA_HOME and the Android SDK must be configured by Actions")
    java_home, sdk = Path(java_home_text).resolve(), Path(sdk_text).resolve()
    java_release = file_record(java_home / "release", java_home, MAX_METADATA_BYTES)
    java_version = re.search(r'^JAVA_VERSION="([^"]+)"$', (java_home / "release").read_text(encoding="utf-8"), re.MULTILINE)
    require(java_version and re.match(r"\d+", java_version[1]) and int(java_version[1].split(".")[0]) == expected["jdk_major"],
            "Actions JDK differs from the configured major version")
    toolchain = {"jdk_release": java_release, "java_version": java_version[1],
                 "java": file_record(java_home / "bin/java", java_home),
                 "javac": file_record(java_home / "bin/javac", java_home),
                 "android_jar": file_record(sdk / f"platforms/android-{expected['compile_sdk']}/android.jar", sdk),
                 "build_tools": file_record(sdk / f"build-tools/{expected['build_tools']}/source.properties", sdk, MAX_METADATA_BYTES)}
    ctx.write_json("android-build-toolchain.json", toolchain)
    epoch = ctx.command("android-source-epoch", ["git", "show", "-s", "--format=%ct", "HEAD"], cwd=ctx.source).stdout.strip()
    require(re.fullmatch(r"[0-9]{1,12}", epoch), "Invalid fixed source timestamp")
    module = ctx.source / relative_path(expected["module_path"])
    require(module.is_dir() and not module.is_symlink() and module.resolve().is_relative_to(ctx.source.resolve()),
            "Missing or unsafe Gradle application module")
    output_root = module / "build/outputs/apk"
    require(not (module / "build").exists(), "Original build directory already exists; stale APK evidence is not allowed")
    cache = Path(tempfile.mkdtemp(prefix="neverd-gradle-", dir=ctx.work.parent))
    require(not cache.resolve().is_relative_to(ctx.work.resolve()), "Gradle cache must remain outside uploaded evidence")
    distribution = cache / "gradle-distribution.zip"
    ctx.command("android-download-gradle", ["curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                "--max-time", str(ctx.timeout), "--max-filesize", str(MAX_FILE_BYTES), "--output", str(distribution), expected["distribution_url"]])
    distribution_record = file_record(distribution, cache)
    require(distribution_record["sha256"] == expected["distribution_sha256"], "Gradle distribution SHA-256 mismatch")
    launcher = unpack_gradle(distribution, cache / "distribution", expected["gradle_version"])
    observer = ctx.work / "android-build-observer.init.gradle"
    observer.write_text(GRADLE_OBSERVER, encoding="utf-8")
    (cache / "empty-maven-local").mkdir()
    env = {"JAVA_HOME": str(java_home), "GRADLE_USER_HOME": str(cache / "gradle-user-home"),
           "ANDROID_HOME": str(sdk), "ANDROID_SDK_ROOT": str(sdk), "SOURCE_DATE_EPOCH": epoch,
           "NEVERD_BUILD_EVIDENCE": str(ctx.work), "NEVERD_BUILD_MODULE": expected["module_id"],
           "NEVERD_BUILD_VARIANT": expected["variant"]}
    inputs = {"schema_version": 1, "source_commit": ctx.app["source_commit"], "profile": expected,
              "gradle_distribution": distribution_record, "gradle_wrapper": properties,
              "source_date_epoch": epoch, "cache_directory": str(cache), "optimization_overrides": [],
              "cache_isolation": "fresh Gradle user home and empty Maven local repository outside evidence"}
    ctx.write_json("android-source-build.json", inputs)
    dependency = None
    build_error = None
    try:
        ctx.command("android-gradle-original-build", ["bash", str(launcher), "--no-daemon", "--console=plain", "--stacktrace",
                    "--no-build-cache", "--no-configuration-cache", "--max-workers=2",
                    "-Dmaven.repo.local=" + str(cache / "empty-maven-local"), "--init-script", str(observer),
                    "clean", expected["task"]], cwd=ctx.source, env=env, timeout=ctx.timeout)
    except Exception as error:
        build_error = error
    finally:
        dependency = capture_dependencies(ctx, cache, records)
    if build_error:
        raise SourceBuildError(f"Original Gradle task failed; logs and dependency evidence were retained: {build_error}") from build_error
    apk, metadata_path, metadata = select_apk(output_root, expected)
    preserved = ctx.work / "source-built.apk"
    shutil.copyfile(apk, preserved)
    shutil.copyfile(metadata_path, ctx.work / "android-agp-output-metadata.json")
    apk_record = file_record(preserved, ctx.work)
    effective_path = ctx.work / "android-gradle-effective.json"
    regular_file(effective_path, ctx.work, MAX_METADATA_BYTES)
    effective = load_json(effective_path)
    verify_effective_settings(effective, expected)
    require(effective.get("java_home") and Path(effective["java_home"]).resolve() == java_home
            and effective.get("java_version") == java_version[1], "Gradle ran under an unexpected Java toolchain")
    mapping_root = module / "build/outputs/mapping" / expected["variant"]
    mapping_records = []
    if mapping_root.exists():
        total = 0
        for path in files_under(mapping_root):
            record = file_record(path, mapping_root, allow_empty=True)
            total += record["size"]
            require(total <= MAX_TREE_BYTES, "Mapping evidence exceeds its byte budget")
            target = ctx.work / "android-mapping" / relative_path(record["path"])
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            mapping_records.append(record)
    require(not expected["minify"] or any(row["path"] == "mapping.txt" for row in mapping_records),
            "Minified source build produced no R8 mapping evidence")
    inputs.update(apk=apk_record, agp_metadata=metadata, effective=effective,
                  mappings=mapping_records, dependency_lock_verified=dependency["lock_verified"])
    ctx.write_json("android-source-build.json", inputs)
    evidence = ["source-built.apk", "android-agp-output-metadata.json", "android-source-build.json",
                "android-build-inputs.json", "android-build-toolchain.json", "android-dependency-provenance.json",
                "android-gradle-effective.json", "android-build-observer.init.gradle", "android-source-LICENSE.txt"]
    if (ctx.work / "android-gradle-resolutions.ndjson").exists():
        evidence.append("android-gradle-resolutions.ndjson")
    evidence += ["android-mapping/" + row["path"] for row in mapping_records]
    return {"apk": preserved, "sha256": apk_record["sha256"], "evidence": evidence,
            "dependency_provenance": dependency,
            "details": {"input_kind": "source-build", "source_build": True,
                        "variant": expected["variant"], "application_id": expected["application_id"],
                        "gradle_task": expected["task"], "dependency_lock_verified": False}}
