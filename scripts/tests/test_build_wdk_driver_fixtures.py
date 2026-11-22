from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
from pathlib import Path
import re
import tempfile
from threading import Barrier
import unittest
from unittest import mock
import zipfile

from scripts import build_wdk_driver_fixtures as fixtures
from scripts import check_docs_i18n as i18n
from scripts import run_native_cpu_ci as native


class WDKDriverFixtureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def archive(self, name, files):
        path = self.root / name
        with zipfile.ZipFile(path, "w") as archive:
            for filename, data in files.items():
                archive.writestr(filename, data)
        return path

    def test_corrupt_cached_package_is_rejected_without_network(self):
        package = self.root / "package.nupkg"
        package.write_bytes(b"modified bytes")
        with mock.patch.object(fixtures.urllib.request, "urlopen") as fetch:
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                fixtures.download(self.root, package.name, "unused", "0" * 64)
        fetch.assert_not_called()

    def test_formatted_package_identity_is_one_complete_string(self):
        definition = self.root / "inputs.def"
        definition.write_text('NEVERD_WDK_DOWNLOAD("archive",\n'
                              ' "https://example.invalid/"\n "package", "sha")\n')
        self.assertEqual(fixtures.declarations(definition), {
            "DOWNLOAD": [["archive", "https://example.invalid/package", "sha"]],
        })

    def test_failed_download_cannot_publish_partial_package(self):
        package = self.root / "package.nupkg"
        expected = hashlib.sha256(b"complete bytes").hexdigest()
        response = mock.MagicMock()
        response.__enter__.return_value.read.side_effect = [b"truncated", b""]
        with mock.patch.object(fixtures.urllib.request, "urlopen",
                               return_value=response):
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                fixtures.download(self.root, package.name, "unused", expected)
        self.assertFalse(package.exists())
        self.assertEqual(list(self.root.glob("*.part")), [])

    def test_concurrent_targets_publish_the_same_verified_package(self):
        data = b"official header"
        expected = hashlib.sha256(data).hexdigest()
        together = Barrier(2)

        def fetch(*args, **kwargs):
            together.wait(timeout=5)
            return io.BytesIO(data)

        with mock.patch.object(fixtures.urllib.request, "urlopen", side_effect=fetch), \
                ThreadPoolExecutor(max_workers=2) as targets:
            results = [targets.submit(fixtures.download, self.root, "header.h", "unused", expected)
                       for _ in range(2)]
            self.assertEqual([result.result().read_bytes() for result in results], [data, data])
        self.assertEqual(list(self.root.glob("*.part")), [])

    def test_extract_preserves_original_case_bytes_and_license(self):
        files = {"c/Include/Native.h": b"original header\r\n",
                 "LICENSE.txt": b"original terms\r\n",
                 "tools/unneeded.exe": b"not selected"}
        archive = self.archive("kit.zip", files)
        kit = self.root / "kit"
        records = fixtures.extract([archive], kit, ["c/Include/", "LICENSE.txt"])
        self.assertEqual({item["path"] for item in records},
                         {"c/Include/Native.h", "LICENSE.txt"})
        for item in records:
            self.assertEqual((kit / item["path"]).read_bytes(), files[item["path"]])
            self.assertEqual(fixtures.digest(kit / item["path"]), item["sha256"])
        self.assertFalse((kit / "tools").exists())
        overlay = fixtures.overlay_directory(kit / "c/Include", True)
        self.assertEqual(overlay["contents"][0]["name"], "Native.h")

    def test_missing_package_member_and_conflicting_headers_fail(self):
        first = self.archive("a.zip", {"include/header.h": b"first"})
        second = self.archive("b.zip", {"include/header.h": b"changed"})
        kit = self.root / "kit"
        with self.assertRaisesRegex(ValueError, "missing Microsoft package"):
            fixtures.extract([first], kit, ["include/", "missing.lib"])
        with self.assertRaisesRegex(ValueError, "conflicting Microsoft"):
            fixtures.extract([first, second], kit, ["include/"])

    def test_stale_unverified_header_cannot_enter_include_overlay(self):
        archive = self.archive("kit.zip", {"include/header.h": b"verified"})
        kit = self.root / "kit"
        (kit / "include").mkdir(parents=True)
        (kit / "include/stale.h").write_bytes(b"unverified")
        with self.assertRaisesRegex(ValueError, "unverified files"):
            fixtures.extract([archive], kit, ["include/"])

    def test_selected_archive_path_cannot_escape_destination(self):
        archive = self.archive("kit.zip", {"include/../../escape.h": b"bad"})
        with self.assertRaisesRegex(ValueError, "invalid Microsoft archive path"):
            fixtures.extract([archive], self.root / "kit", ["include/"])
        self.assertFalse((self.root / "escape.h").exists())

    def test_failed_build_removes_previous_completion_cache(self):
        output = self.root / "output"
        output.mkdir()
        (output / "fixtures.cmake").write_text("stale complete build")
        (output / "build-manifest.json").write_text("stale successful evidence")
        with mock.patch.object(fixtures, "download", side_effect=ValueError("bad")):
            with self.assertRaisesRegex(ValueError, "bad"):
                fixtures.build(output, self.root / "cache", "clang", "lld-link")
        self.assertFalse((output / "fixtures.cmake").exists())
        self.assertFalse((output / "build-manifest.json").exists())

    def test_cmake_paths_preserve_spaces_and_reject_list_or_code_expansion(self):
        image = self.root / "directory with spaces" / "driver.sys"
        text = fixtures.cache_entry("NEVERD_TEST_FIXTURE", image)
        self.assertIn('"' + image.resolve().as_posix() + '"', text)
        for name in ('x;y.sys', '${VAR}.sys', 'x"y.sys'):
            with self.subTest(name=name):
                with mock.patch.object(Path, "resolve", side_effect=AssertionError):
                    with self.assertRaisesRegex(ValueError, "unsupported CMake"):
                        fixtures.cache_entry("NEVERD_TEST_FIXTURE", self.root / name)

    def test_every_original_wdk_image_has_a_declared_build_and_native_gate(self):
        inventory = fixtures.declarations()
        variables = ["NEVERD_" + name + suffix + "_FIXTURE"
                     for name, *_ in inventory["FIXTURE"]
                     for suffix in ("", "_CFG")]
        self.assertEqual(len(set(variables)), len(variables))
        source = (fixtures.ROOT / "unittests/emulation/DriverBackendParityCases.def")
        images = re.findall(r"^NEVERD_PARITY_IMAGE\(\s*(\w+),\s*(\w+)\)",
                            source.read_text(), re.M)
        self.assertEqual(set(variables), {variable for _, variable in images})
        _, required = native.declared_inventory(fixtures.ROOT, with_drivers=True)
        scenarios = re.findall(
            r'^NEVERD_PARITY_SCENARIO\(\s*(\w+),\s*(\w+),\s*"([^"]+)"\)',
            source.read_text(), re.M,
        )
        builtins = re.findall(
            r"^NEVERD_PARITY_IMAGE\(\s*(\w+),",
            (source.parent / "DriverBuiltinImages.def").read_text(), re.M,
        )
        names = [name for name, _ in images] + [row[0] for row in scenarios]
        names += builtins
        self.assertEqual(len(names), len(set(names)))
        for name in names:
            for suffix in ("Original", "Rebased"):
                self.assertTrue(any(test.endswith(f"/whp_{suffix}_{name}")
                                    for test in required))
        _, cpu_required = native.declared_inventory(fixtures.ROOT)
        seh_required = {
            "DriverKernelSEH.ScopeEndLabelMayOverlapTheHandlerLandingPad",
            "DriverKernelSEH.OverlappingScopeStillHasAnExclusiveEnd",
            "DriverKernelSEH.OverlappingHandlerRetainsTargetValidationAndCanRetry",
            "DriverKernelSEH.FinallyRespectsRawScopeEndAtHandlerTarget",
            "DriverKernelSEH.KernelSSERecordsCaptureAllXmmAndPreserveFaultControls",
            "DriverKernelSEH.KernelSSEContinuationUsesTopLevelControlsAndAllRegisters",
            "DriverKernelSEH.KernelSSEFilterEditsPreserveUnwindAndRejectX87",
        }
        v2_model = re.findall(
            r"TEST_F\(DriverKernelSEH,\s*(V2\w+)",
            (source.parent / "KernelSEHTests.cpp").read_text(),
        )
        self.assertTrue(v2_model)
        seh_required.update(f"DriverKernelSEH.{name}" for name in v2_model)
        v2_native = re.findall(
            r"TEST_P\(DriverSEHV2Execution,\s*(\w+)\)",
            (source.parent / "DriverSEHV2Tests.cpp").read_text(),
        )
        self.assertTrue(v2_native)
        for backend in ("kvm", "whp"):
            _, backend_required = native.declared_inventory(
                fixtures.ROOT, True, backend,
            )
            cases = {
                f"Native/DriverSEHV2Execution.{name}/{backend}"
                for name in v2_native
            }
            self.assertTrue(cases <= backend_required,
                            sorted(cases - backend_required))
            if backend == "whp":
                seh_required.update(cases)
        modes = {"Handle", "MaskRetry", "OperandRetry", "Constant", "RejectX87"}
        for contract in ("driver", "checked"):
            seh_required.update(
                "Native/DriverSIMDSEH.OriginalDriverCatchesAndRetriesSSEFaults/"
                f"whp_{contract}_{mode}" for mode in modes
            )
        mutex_modes = re.findall(
            r"^NEVERD_SEH_MUTEX_CASE\(\s*(\w+),",
            (source.parent / "fixtures/driver_seh_mutex.def").read_text(), re.M,
        )
        self.assertTrue(mutex_modes)
        for contract in ("driver", "checked"):
            seh_required.update(
                "Native/DriverMutexThread.NestedAndBlockedCallsRetainThreadOwnership/"
                f"whp_{contract}_{mode}" for mode in mutex_modes
            )
        scheduling_required = {
            test for test in required
            if test.startswith(("DriverSchedulingPolicy.",
                                "Preemptive/DriverScheduling"))
        }
        scheduled_cases = set()
        families = (
            ("DriverThreadPriorityCases.def", "NEVERD_DRIVER_PRIORITY_CASE",
             "DriverSchedulingPriority.RuntimePriorityControlsDispatchAndPreemption"),
            ("DriverPreemptiveCases.def", "NEVERD_PREEMPT_CASE",
             "DriverScheduling.BusyGuestMakesProgressWithoutCooperativeYield"),
            ("DriverPreemptiveCases.def", "NEVERD_PREEMPT_POFX",
             "DriverSchedulingPoFx.BlockingCallbacksRetainTheirOriginalThread"),
            ("DriverPowerPreemptiveCases.def", "NEVERD_POWER_PREEMPT_CASE",
             "DriverSchedulingPower.BusyDispatchAllowsIndependentPassivePowerCallbacks"),
        )
        for filename, macro, suite in families:
            cases = re.findall(rf"^{macro}\(\s*(\w+),",
                               (source.parent / "fixtures" / filename).read_text(),
                               re.M)
            self.assertTrue(cases)
            for contract in ("driver", "checked"):
                scheduled_cases.update(
                    f"Preemptive/{suite}/whp_{contract}_{case}" for case in cases
                )
        self.assertTrue(scheduled_cases <= scheduling_required)
        self.assertEqual(
            scheduling_required - scheduled_cases,
            {test for test in required if test.startswith("DriverSchedulingPolicy.")},
        )
        wait_cases = re.findall(
            r"^NEVERD_MULTI_WAIT_CASE\(\s*(\w+),",
            (source.parent / "fixtures/DriverMultipleWaitCases.def").read_text(),
            re.M,
        )
        self.assertTrue(wait_cases)
        wait_required = {
            test for test in required if test.startswith("KernelMultipleWait.")
        }
        self.assertTrue(wait_required)
        for contract in ("driver", "checked"):
            wait_required.update(
                "Native/DriverMultipleWait.OriginalWaitSetsPreserveSignalsAndThreadLifetimes/"
                f"whp_{contract}_{case}" for case in wait_cases
            )
        self.assertTrue(wait_required <= required)
        arguments = {row[0]: row[1:] for row in inventory["ARGUMENTS"]}
        self.assertIn("-fasynchronous-unwind-tables", arguments["seh_compile"])
        self.assertTrue(seh_required <= required)
        unpack_source = (source.parent / "UnpackDriverTests.cpp").read_text()
        unpack_required = {
            f"Backends/UnpackDriver.{name}/Whp"
            for name in re.findall(r"TEST_P\(UnpackDriver,\s*(\w+)\)", unpack_source)
            # This case requires the shared C API/CLI, disabled in this profile.
            if name != "CAPIAndCLIUseTheDriverEnvironment"
        }
        unpack_required.update(
            f"DriverChecksum.{name}"
            for name in re.findall(r"TEST\(DriverChecksum,\s*(\w+)\)", unpack_source)
        )
        self.assertTrue(unpack_required)
        self.assertTrue(unpack_required <= required)
        timestamp_source = (source.parent / "DriverTimestampTests.cpp").read_text()
        timestamp_required = {
            f"Native/DriverTimestamp.{name}/whp_{contract}"
            for name in re.findall(r"TEST_P\(DriverTimestamp,\s*(\w+)\)",
                                   timestamp_source)
            for contract in ("driver", "checked")
        }
        self.assertTrue(timestamp_required)
        self.assertTrue(timestamp_required <= required)
        image_source = (source.parent / "DriverImageMemoryTests.cpp").read_text()
        image_required = {
            f"Native/DriverImageMemory.{name}/whp_{contract}"
            for name in re.findall(r"TEST_P\(DriverImageMemory,\s*(\w+)\)",
                                   image_source)
            for contract in ("driver", "checked")
        }
        self.assertTrue(image_required)
        self.assertTrue(image_required <= required)
        cpuid_source = (source.parent / "DriverCPUIDTests.cpp").read_text()
        cpuid_required = {
            f"Native/DriverCPUIDExecution.{name}/whp_{contract}"
            for name in re.findall(r"TEST_P\(DriverCPUIDExecution,\s*(\w+)\)",
                                   cpuid_source)
            for contract in ("driver", "checked")
        }
        self.assertTrue(cpuid_required)
        self.assertTrue(cpuid_required <= required)
        snapshot_source = (source.parent / "KernelSnapshotTests.cpp").read_text()
        snapshot_required = {
            f"Native/KernelSnapshot.{name}/whp_checked"
            for name in re.findall(r"TEST_P\(KernelSnapshot,\s*(\w+)\)",
                                   snapshot_source)
        }
        self.assertTrue(snapshot_required)
        self.assertTrue(snapshot_required <= required)
        pool_source = (source.parent / "KernelUnpackStateTests.cpp").read_text()
        pool_required = {
            f"KernelUnpackState.{name}"
            for name in re.findall(r"TEST_F\(KernelUnpackState,\s*(\w+)\)",
                                   pool_source)
        }
        self.assertTrue(pool_required)
        self.assertTrue(pool_required <= required)
        caller_source = (source.parent / "DriverEntryABITests.cpp").read_text()
        caller_required = {
            f"Backends/DriverEntryCaller.{name}/Whp"
            for name in re.findall(r"TEST_P\(DriverEntryCaller,\s*(\w+)\)",
                                   caller_source)
        }
        self.assertTrue(caller_required)
        self.assertTrue(caller_required <= required)
        # Derive every environment case from its real source. The two native
        # transports must require the same behavior, including pure model guards.
        environment_families = (
            ("KernelLoaderViewTests.cpp", ()),
            ("KernelResidentQueryTests.cpp", ()),
            ("KernelBugCheckTests.cpp", ()),
            ("KernelVersionQueryTests.cpp", ()),
            ("KernelPhysicalInventoryTests.cpp", ()),
            ("KernelVaShadowQueryTests.cpp", ()),
            ("KernelMSRReadTests.cpp", ()),
            ("KernelProcessPebTests.cpp", ()),
            ("KernelContextCaptureTests.cpp", ()),
            ("DriverLoaderViewTests.cpp", ("{backend}_checked",)),
            ("DriverBugCheckTests.cpp", ("{backend}",)),
            ("DriverVersionQueryTests.cpp", ("{backend}",)),
            ("DriverPhysicalInventoryTests.cpp", ("{backend}",)),
            ("DriverVaShadowQueryTests.cpp", ("{backend}",)),
            ("DriverMSRReadTests.cpp", ("{backend}",)),
            ("DriverProcessPebTests.cpp", ("{backend}",)),
            ("DriverContextCaptureTests.cpp", ("{backend}",)),
            ("DriverShadowStackTests.cpp",
             ("{backend}_driver", "{backend}_checked")),
        )
        environment_required = set()
        for backend in ("kvm", "whp"):
            _, backend_required = native.declared_inventory(
                fixtures.ROOT, True, backend,
            )
            expected = set()
            for filename, suffixes in environment_families:
                text = (source.parent / filename).read_text()
                cases = {
                    f"{suite}.{name}"
                    for suite, name in re.findall(
                        r"TEST(?:_F)?\((\w+),\s*(\w+)\)", text,
                    )
                }
                cases.update(
                    f"Native/{suite}.{name}/" + suffix.format(backend=backend)
                    for suite, name in re.findall(
                        r"TEST_P\((\w+),\s*(\w+)\)", text,
                    )
                    for suffix in suffixes
                )
                self.assertTrue(cases, filename)
                expected.update(cases)
            self.assertTrue(expected <= backend_required,
                            sorted(expected - backend_required))
            if backend == "whp":
                environment_required = expected
        api_required = set()
        for filename in (
            "DriverObjectTests.cpp", "DriverDataExportTests.cpp",
            "DriverNamespaceTests.cpp", "DriverRegistryObjectTests.cpp",
            "DriverDeviceQueryTests.cpp", "DriverFileObjectTests.cpp",
            "DriverProcessorTests.cpp", "DriverIntegerFormattingTests.cpp",
            "DriverRuntimeStringTests.cpp",
            "DriverFileSystemExecutionTests.cpp",
        ):
            cases = re.findall(r"TEST_P\((\w+),\s*(\w+)\)",
                               (source.parent / filename).read_text())
            self.assertTrue(cases, filename)
            api_required.update(
                f"Backends/{suite}.{name}/whp{contract}{base}"
                for suite, name in cases
                for contract in ("Legacy", "Checked")
                for base in ("Preferred", "Rebased")
            )
        self.assertTrue(api_required <= required,
                        sorted(api_required - required))
        self.assertEqual(len(required), len(cpu_required) + 2 * len(names)
                         + len(seh_required) + len(scheduling_required)
                         + len(wait_required) + len(unpack_required)
                         + len(timestamp_required) + len(image_required)
                         + len(cpuid_required) + len(snapshot_required)
                         + len(pool_required) + len(caller_required)
                         + len(environment_required) + len(api_required))
        formula = (f"{len(cpu_required)} CPU + {2 * len(names)} WHP + "
                   f"{len(seh_required)} SEH + {len(scheduling_required)} scheduling "
                   f"+ {len(wait_required)} wait sets "
                   f"+ {len(unpack_required)} driver UNPACK "
                   f"+ {len(timestamp_required)} clock reads "
                   f"+ {len(image_required)} image memory checks "
                   f"+ {len(cpuid_required)} CPUID "
                   f"+ {len(snapshot_required)} kernel snapshots "
                   f"+ {len(pool_required)} pool captures "
                   f"+ {len(caller_required)} DriverEntry caller checks "
                   f"+ {len(environment_required)} driver environment checks "
                   f"+ {len(api_required)} driver API checks = {len(required)}")
        definitions = (fixtures.ROOT / "scripts/EmulationDocumentation.def")
        tokens = i18n.emulation_document_tokens(definitions.read_text(encoding="utf-8"))
        self.assertIn(formula, tokens["NativeDriverCI"])
        guides = [fixtures.ROOT / "docs/testing.md"]
        guides.extend((fixtures.ROOT / "docs").glob("*/testing.md"))
        for guide in guides:
            with self.subTest(guide=guide):
                paragraph = next(
                    part for part in guide.read_text(encoding="utf-8").split("\n\n")
                    if "`NativeDriverTests.def`" in part
                    and "`DriverBuiltinImages.def`" in part
                )
                self.assertIn(formula, paragraph)
                for count in (len(names), len(images), len(scenarios)):
                    self.assertRegex(paragraph, rf"(?<!\d){count}(?!\d)")
        for _, filename, *_ in inventory["FIXTURE"]:
            self.assertTrue((fixtures.ROOT / "unittests/emulation/fixtures"
                             / filename).is_file())

    def test_all_wdk_sources_and_configured_paths_have_reproducible_builds(self):
        inventory = fixtures.declarations()["FIXTURE"]
        sources = fixtures.ROOT / "unittests/emulation/fixtures"
        original = {path.name for pattern in ("driver_wdm_*.c", "driver_kmdf_*.c")
                    for path in sources.glob(pattern)}
        # Discover dual-mode sources independently: an available WDK branch
        # must not silently disappear from reproducible builds and native CI.
        portable = {path.name for path in sources.glob("*.c")
                    if re.search(r"^#ifdef NEVERD_WDK_FIXTURE\s*$",
                                 path.read_text(), re.M)}
        self.assertEqual(portable, {
            source for _, source, _, _, profile in inventory
            if profile == "public_api"
        })
        original.update(portable)
        self.assertEqual({source for _, source, *_ in inventory}, original)
        configured = re.findall(
            r'^set\((NEVERD_(?:KMDF|WDM)\w*_FIXTURE) "" CACHE FILEPATH',
            (sources.parent / "CMakeLists.txt").read_text(), re.M,
        )
        variables = {"NEVERD_" + name + suffix + "_FIXTURE"
                     for name, *_ in inventory for suffix in ("", "_CFG")}
        self.assertEqual(variables, set(configured))

    def test_every_published_driver_scenario_has_normal_and_cfg_native_cases(self):
        source = fixtures.ROOT / "unittests/emulation/DriverBackendParityCases.def"
        text = source.read_text()
        images = dict(re.findall(
            r"^NEVERD_PARITY_IMAGE\(\s*(\w+),\s*(\w+)\)", text, re.M,
        ))
        scenarios = re.findall(
            r'^NEVERD_PARITY_SCENARIO\(\s*(\w+),\s*(\w+),\s*"([^"]+)"\)',
            text, re.M,
        )
        actual = Counter((path, images[image].endswith("_CFG_FIXTURE"))
                         for _, image, path in scenarios)
        expected = Counter((path.name, cfg)
                           for path in (fixtures.ROOT / "docs/examples")
                           .glob("driver-*-scenario.json")
                           for cfg in (False, True))
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
