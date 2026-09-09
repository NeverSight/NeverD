"""Source-profile guards using self-authored files and mocked commands only.

The fake Gradle ZIP and APK bytes are not executable applications or evidence
of an app result. Actual Gradle, SDK, and NeverD runs are reserved for Actions.
"""
from __future__ import annotations

import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from scripts.mobile_real_apps_android_build import (
    SourceBuildError, build_source_apk, capture_dependencies, select_apk,
    source_profile, unpack_gradle, verify_effective_settings, wrapper_url,
)


APP = {"source_commit": "a" * 40, "license_path": "LICENSE", "android": {
    "compile_sdk": 36, "release_task": ":app:assembleFossRelease", "debug_task": ":app:assembleFossDebug",
    "source_build": {"gradle_version": "8.13", "distribution_url": "https://services.gradle.org/distributions/gradle-8.13-bin.zip",
                     "distribution_sha256": "b" * 64, "jdk_major": 21, "agp_version": "8.11.1", "build_tools": "35.0.0",
                     "module_path": "app", "profiles": {
                         "gradle-release": {"variant": "fossRelease", "application_id": "org.owned.math",
                                            "minify": True, "shrink_resources": True},
                         "gradle-debug": {"variant": "fossDebug", "application_id": "org.owned.math.debug",
                                          "minify": False, "shrink_resources": False}}}}}


def output_metadata(expected):
    return {"version": 3, "artifactType": {"type": "APK", "kind": "Directory"},
            "applicationId": expected["application_id"], "variantName": expected["variant"],
            "elements": [{"type": "SINGLE", "filters": [], "attributes": [], "versionCode": 10,
                          "versionName": "1.4.0", "outputFile": "arbitrary-upstream-name-unsigned.apk"}],
            "elementType": "File"}


def observed_settings(expected, java_home):
    return {"schema_version": 1, "gradle_version": expected["gradle_version"], "agp_version": expected["agp_version"],
            "module": expected["module_id"], "variant": expected["variant"], "application_id": expected["application_id"],
            "build_type": expected["build_type"], "compile_sdk": "android-" + str(expected["compile_sdk"]),
            "build_tools": expected["build_tools"], "minify": expected["minify"], "shrink_resources": expected["shrink_resources"],
            "tasks": ["clean", expected["task"]], "java_version": "21.0.12", "java_home": str(java_home)}


def fake_distribution(entries=None):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in entries or [("gradle-8.13/bin/gradle", b"self-authored mock launcher, never executed")]:
            archive.writestr(name, data)
    return output.getvalue()


class BuildContext:
    def __init__(self, root, profile="gradle-release"):
        self.app = copy.deepcopy(APP)
        self.variant = {"id": "owned-" + profile, "profile": profile}
        self.source, self.work = root / "source", root / "evidence"
        self.source.mkdir()
        self.work.mkdir()
        self.timeout, self.commands = 90, []
        self.distribution = fake_distribution()
        self.app["android"]["source_build"]["distribution_sha256"] = hashlib.sha256(self.distribution).hexdigest()
        self.tracked = {
            "gradle/wrapper/gradle-wrapper.properties": "distributionUrl=https\\://services.gradle.org/distributions/gradle-8.13-bin.zip\n",
            "gradlew": "self-owned wrapper fixture; never executed\n",
            "settings.gradle.kts": 'include(":app")\n',
            "app/build.gradle.kts": "// Self-owned original optimization settings fixture.\n",
            "app/proguard-rules.pro": "-keep class org.owned.Kept { *; }\n",
            "app/proguard-empty.pro": "",
            "LICENSE": "Self-owned test inputs\n",
        }
        for name, content in self.tracked.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        self.java_home, self.sdk = root / "jdk", root / "sdk"
        for name, content in {"release": 'JAVA_VERSION="21.0.12"\n', "bin/java": "fake java", "bin/javac": "fake javac"}.items():
            path = self.java_home / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        for name in ("platforms/android-36/android.jar", "build-tools/35.0.0/source.properties"):
            path = self.sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"self-owned SDK fixture bytes")
        self.dirty, self.build_failure = False, False
        self.metadata_mutation, self.settings_mutation = None, None
        self.apk_bytes = b"self-owned APK artifact bytes; not a valid executable fixture"

    def write_json(self, name, data):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def command(self, name, argv, **kwargs):
        self.commands.append((name, argv, kwargs))
        text = ""
        if name == "android-source-clean":
            text = " M app/build.gradle.kts\n" if self.dirty else ""
        elif name == "android-tracked-build-inputs":
            text = "\x00".join(self.tracked) + "\x00"
        elif name == "android-source-epoch":
            text = "1700000000\n"
        elif name == "android-download-gradle":
            Path(argv[argv.index("--output") + 1]).write_bytes(self.distribution)
        elif name == "android-gradle-original-build":
            expected = source_profile(self.app, self.variant["profile"])
            effective = observed_settings(expected, self.java_home)
            if self.settings_mutation:
                self.settings_mutation(effective)
            self.write_json("android-gradle-effective.json", effective)
            self.write_json("android-gradle-resolutions.ndjson", {"schema_version": 1, "kind": "resolution", "project": ":app",
                            "scope": "project", "configuration": "fossReleaseRuntimeClasspath", "components": [], "dependencies": []})
            cache = Path(kwargs["env"]["GRADLE_USER_HOME"]) / "caches/modules-2/files-2.1/org.owned/library/1.0/hash/library-1.0.jar"
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"self-owned dependency bytes")
            if self.build_failure:
                raise RuntimeError("original Gradle compilation failed")
            directory = self.source / "app/build/outputs/apk" / expected["variant"]
            directory.mkdir(parents=True)
            metadata = output_metadata(expected)
            if self.metadata_mutation:
                self.metadata_mutation(metadata)
            (directory / "output-metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
            (directory / "arbitrary-upstream-name-unsigned.apk").write_bytes(self.apk_bytes)
            if expected["minify"]:
                mapping = self.source / "app/build/outputs/mapping" / expected["variant"]
                mapping.mkdir(parents=True)
                (mapping / "mapping.txt").write_text("org.owned.A -> a:\n", encoding="utf-8")
                (mapping / "usage.txt").write_bytes(b"")
        else:
            raise AssertionError("Unexpected command: " + name)
        return subprocess.CompletedProcess(argv, 0, text, "")


class SourceBuildInputTests(unittest.TestCase):
    def test_exact_tasks_and_optimization_profiles_are_bound(self):
        release = source_profile(APP, "gradle-release")
        debug = source_profile(APP, "gradle-debug")
        self.assertEqual(release["task"], ":app:assembleFossRelease")
        self.assertEqual(debug["application_id"], "org.owned.math.debug")
        self.assertTrue(release["minify"] and release["shrink_resources"])
        self.assertFalse(debug["minify"] or debug["shrink_resources"])
        changed = copy.deepcopy(APP)
        changed["android"]["release_task"] = ":app:assembleFossDebug"
        with self.assertRaisesRegex(SourceBuildError, "task disagrees"):
            source_profile(changed, "gradle-release")

    def test_wrapper_url_must_be_unambiguous_and_not_a_continuation(self):
        self.assertEqual(wrapper_url("distributionUrl=https\\://services.gradle.org/distributions/gradle-8.13-bin.zip\n")["distributionUrl"],
                         APP["android"]["source_build"]["distribution_url"])
        for text in ("distributionUrl=a\ndistributionUrl=b\n", "distributionUrl=https\\\n", "unknown=only\n"):
            with self.subTest(text=text), self.assertRaises(SourceBuildError):
                wrapper_url(text)

    def test_distribution_paths_cannot_escape_or_change_gradle_version(self):
        for name in ("../outside", "gradle-8.13/../../outside", "gradle-9.0/bin/gradle", "/absolute", "gradle-8.13//bin/gradle"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / "distribution.zip"
                archive.write_bytes(fake_distribution([(name, b"self-owned")]))
                with self.assertRaises(SourceBuildError):
                    unpack_gradle(archive, root / "unpacked", "8.13")

    def test_reported_build_settings_cannot_hide_changed_optimization_or_variant(self):
        expected = source_profile(APP, "gradle-release")
        original = observed_settings(expected, Path("/jdk"))
        verify_effective_settings(original, expected)
        mutations = {"schema_version": True, "minify": False, "shrink_resources": False, "compile_sdk": "android-35",
                     "build_tools": "36.0.0", "agp_version": "8.11.0", "variant": "fossDebug",
                     "application_id": "org.owned.math.debug", "tasks": [":app:assembleFossDebug"]}
        for key, value in mutations.items():
            with self.subTest(field=key), self.assertRaises(SourceBuildError):
                verify_effective_settings({**original, key: value}, expected)


class APKSelectionTests(unittest.TestCase):
    def test_output_metadata_selects_the_real_filename_without_guessing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = source_profile(APP, "gradle-release")
            metadata = output_metadata(expected)
            (root / "output-metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
            path = root / metadata["elements"][0]["outputFile"]
            path.write_bytes(b"self-owned APK evidence")
            actual, _, details = select_apk(root, expected)
            self.assertEqual(actual, path)
            self.assertEqual(details["variantName"], "fossRelease")

    def test_ambiguous_split_missing_or_wrong_variant_apks_are_rejected(self):
        def split(value): value["elements"].append(copy.deepcopy(value["elements"][0]))
        def filtered(value): value["elements"][0]["filters"] = [{"filterType": "ABI", "value": "arm64-v8a"}]
        def wrong_variant(value): value["variantName"] = "fossDebug"
        def wrong_package(value): value["applicationId"] = "org.other"
        def traversal(value): value["elements"][0]["outputFile"] = "../outside.apk"
        def wrong_type(value): value["artifactType"]["type"] = "MERGED_MANIFESTS"
        def wrong_schema(value): value["version"] = True
        def no_filters(value): value["elements"][0].pop("filters")
        for mutate in (split, filtered, wrong_variant, wrong_package, traversal, wrong_type, wrong_schema, no_filters):
            with self.subTest(mutation=mutate.__name__), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                expected = source_profile(APP, "gradle-release")
                metadata = output_metadata(expected)
                mutate(metadata)
                (root / "output-metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
                (root / "arbitrary-upstream-name-unsigned.apk").write_bytes(b"self-owned")
                with self.assertRaises(SourceBuildError):
                    select_apk(root, expected)

    def test_extra_apk_or_metadata_cannot_be_silently_ignored(self):
        for extra in ("extra.apk", "extra/output-metadata.json"):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                expected = source_profile(APP, "gradle-release")
                (root / "output-metadata.json").write_text(json.dumps(output_metadata(expected)), encoding="utf-8")
                (root / "arbitrary-upstream-name-unsigned.apk").write_bytes(b"self-owned")
                other = root / extra
                other.parent.mkdir(parents=True, exist_ok=True)
                other.write_bytes(b"additional artifact")
                with self.assertRaises(SourceBuildError):
                    select_apk(root, expected)


@unittest.skipUnless(os.name == "posix", "Source-profile Actions jobs run on Linux")
class SourceBuildWorkflowTests(unittest.TestCase):
    def run_build(self, ctx):
        with patch.dict(os.environ, {"JAVA_HOME": str(ctx.java_home), "ANDROID_SDK_ROOT": str(ctx.sdk)}):
            return build_source_apk(ctx)

    def test_both_profiles_build_original_tasks_and_preserve_dependency_unknowns(self):
        for profile in ("gradle-release", "gradle-debug"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as temporary:
                ctx = BuildContext(Path(temporary), profile)
                result = self.run_build(ctx)
                self.assertEqual(result["apk"].read_bytes(), ctx.apk_bytes)
                self.assertEqual(result["sha256"], hashlib.sha256(ctx.apk_bytes).hexdigest())
                self.assertFalse(result["dependency_provenance"]["lock_verified"])
                self.assertEqual(result["dependency_provenance"]["observed_resolution_count"], 1)
                self.assertEqual(len(result["dependency_provenance"]["cache_artifacts"]), 1)
                self.assertTrue(all((ctx.work / name).is_file() for name in result["evidence"]))
                for name, contents in ctx.tracked.items():
                    self.assertEqual((ctx.source / name).read_text(encoding="utf-8"), contents)
                inputs = json.loads((ctx.work / "android-build-inputs.json").read_text(encoding="utf-8"))
                empty = next(row for row in inputs["tracked_build_inputs"] if row["path"] == "app/proguard-empty.pro")
                self.assertEqual(empty["size"], 0)
                self.assertEqual(empty["sha256"], hashlib.sha256(b"").hexdigest())
                self.assertEqual((ctx.work / "android-build-inputs/app/proguard-empty.pro").read_bytes(), b"")
                _, argv, options = next(row for row in ctx.commands if row[0] == "android-gradle-original-build")
                self.assertIn("--no-daemon", argv)
                self.assertEqual(argv[-2:], ["clean", source_profile(ctx.app, profile)["task"]])
                self.assertFalse(Path(options["env"]["GRADLE_USER_HOME"]).is_relative_to(ctx.work))
                self.assertEqual(options["env"]["SOURCE_DATE_EPOCH"], "1700000000")
                self.assertFalse(any("official" in name for name, _, _ in ctx.commands))
                if profile == "gradle-release":
                    self.assertTrue((ctx.work / "android-mapping/mapping.txt").is_file())
                    self.assertEqual((ctx.work / "android-mapping/usage.txt").read_bytes(), b"")

    def test_checksum_or_dirty_source_failure_never_runs_the_original_task(self):
        for failure in ("checksum", "dirty"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                ctx = BuildContext(Path(temporary))
                if failure == "dirty": ctx.dirty = True
                else: ctx.app["android"]["source_build"]["distribution_sha256"] = "0" * 64
                with self.assertRaises(SourceBuildError): self.run_build(ctx)
                self.assertFalse(any(name == "android-gradle-original-build" for name, _, _ in ctx.commands))

    def test_gradle_failure_preserves_dependency_logs_without_substituting_an_apk(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = BuildContext(Path(temporary))
            ctx.build_failure = True
            with self.assertRaisesRegex(SourceBuildError, "Original Gradle task failed"):
                self.run_build(ctx)
            self.assertTrue((ctx.work / "android-source-build.json").is_file())
            self.assertTrue((ctx.work / "android-dependency-provenance.json").is_file())
            self.assertFalse((ctx.work / "source-built.apk").exists())

    def test_changed_effective_optimization_preserves_apk_but_fails_qualification(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = BuildContext(Path(temporary))
            ctx.settings_mutation = lambda value: value.update(minify=False)
            with self.assertRaisesRegex(SourceBuildError, "optimization settings differ"):
                self.run_build(ctx)
            self.assertTrue((ctx.work / "source-built.apk").is_file())
            self.assertTrue((ctx.work / "android-gradle-effective.json").is_file())

    def test_lock_file_existence_does_not_forge_a_complete_dependency_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = BuildContext(Path(temporary))
            cache = Path(temporary) / "cache"
            cache.mkdir()
            result = capture_dependencies(ctx, cache, [{"path": "app/gradle.lockfile", "sha256": "a" * 64}])
            self.assertFalse(result["lock_verified"])
            self.assertEqual(result["status"], "incomplete")
            self.assertTrue(result["tracked_lock_inputs"])
            self.assertTrue(result["errors"])


if __name__ == "__main__":
    unittest.main()
