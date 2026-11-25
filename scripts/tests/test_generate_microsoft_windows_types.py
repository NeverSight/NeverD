"""Official header generation rejects partial or ABI-changing inputs."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location(
    "microsoft_windows_types", SCRIPTS / "generate_microsoft_windows_types.py")
windows = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(windows)


class MicrosoftWindowsTypes(unittest.TestCase):
    def test_cpdk_members_keep_the_vendor_notice_and_verify_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original = root / "filoriginal"
            original.write_bytes(b"checked header")
            notice = root / "filnotice"
            notice.write_bytes(b"original notice")
            archive = root / "payload.tar"
            with tarfile.open(archive, "w") as output:
                for item in (original, notice):
                    output.add(item, arcname=item.name)
            license_name = "CPDK-License.rtf"
            members = {name: {"member": path.name, "sha256": windows.wdk.digest(path)}
                       for name, path in (("sslprovider.h", original), (license_name, notice))}
            record = {"file": archive.name, "url": archive.as_uri(),
                      "sha256": windows.wdk.digest(archive), "members": members}
            licenses = root / "licenses"
            licenses.mkdir()
            with patch.object(windows.flat, "CPDK", {"packages": [record], "license": license_name}):
                packages = windows.flat.cpdk_headers(root, root, licenses)
                self.assertEqual(packages[0]["sha256"], windows.wdk.digest(archive))
                self.assertEqual((root / "cpdk-headers" / "sslprovider.h").read_bytes(), original.read_bytes())
                self.assertEqual((licenses / license_name).read_bytes(), notice.read_bytes())
                members["sslprovider.h"]["sha256"] = "0" * 64
                with self.assertRaisesRegex(ValueError, "CPDK member hash mismatch"):
                    windows.flat.cpdk_headers(root, root, licenses)

    def test_work_queue_keeps_flat_parameters_and_derived_members(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "work.h"
            interface = "struct {} : public IUnknown {{ void (*Invoke)()=0; }};\n"
            source = "".join(interface.format(tag) for tag in (
                "IRtwqAsyncCallback", "IRtwqAsyncResult", "IRtwqPlatformEvents"))
            source += ("typedef struct tagRTWQASYNCRESULT : public IRtwqAsyncResult "
                       "{ int result; } RTWQASYNCRESULT;\n"
                       "int RtwqStart(); int RtwqRead(IRtwqAsyncResult *r); int Other();\n")
            header.write_text(source, encoding="utf-8")
            windows.flat.work_queue_c_types(header)
            result = header.read_text(encoding="utf-8")
            self.assertIn("const struct IRtwqAsyncResultVtbl *lpVtbl", result)
            self.assertIn("IRtwqAsyncResult base; int result;", result)
            self.assertIn("RtwqStart(void); int RtwqRead(IRtwqAsyncResult *r); int Other();", result)
            header.write_text(source.replace("public IUnknown", "public Different", 1), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "IRtwqAsyncCallback"):
                windows.flat.work_queue_c_types(header)

    def test_wsman_preserves_inherited_fields_in_declaration_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "wsman.h"
            base = ("typedef struct _WSMAN_SHELL_STARTUP_INFO_V10 { int a; void *b; } "
                    "WSMAN_SHELL_STARTUP_INFO_V10;\n")
            derived = ("typedef struct _WSMAN_SHELL_STARTUP_INFO_V11 : _WSMAN_SHELL_STARTUP_INFO_V10 "
                       "{ void *name; } WSMAN_SHELL_STARTUP_INFO_V11;\n")
            header.write_text(base + derived, encoding="utf-8")
            windows.flat.wsman_c_types(header)
            normalized = " ".join(header.read_text(encoding="utf-8").split())
            self.assertIn("_WSMAN_SHELL_STARTUP_INFO_V11 { int a; void *b; void *name; }", normalized)
            header.write_text(base + base + derived, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "1.0 startup record"):
                windows.flat.wsman_c_types(header)

    def test_packaged_manifest_preserves_decorated_symbols(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            sdk.mkdir()
            names = ["WindowsUserX86", "WindowsKernelX86"]
            symbol = "?DoInfInstallW@@YGJPAU_ADVPACKARGSW@@@Z"
            libraries = []
            for name in names:
                data = b"int example(void);\n"
                (sdk / (name + ".h")).write_bytes(data)
                libraries.append({"name": name, "sha256": hashlib.sha256(data).hexdigest(),
                                  "symbols": [symbol, "__imp_" + symbol]})
            manifest = {"provider": "Microsoft SDK/WDK", "architecture": "x86",
                        "schema": 1, "libraries": libraries}
            (sdk / "manifest.json").write_text(json.dumps(manifest))
            # Configure the production packaging rules without building the compiler.
            types = SCRIPTS.parent / "lib" / "types"
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(Manifest LANGUAGES CXX)\n"
                "function(add_neverd_component_library target)\n"
                '  add_library(${target} OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/HeaderLibrary.cpp")\n'
                "endfunction()\n"
                f'add_subdirectory("{types.as_posix()}" types)\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                            "-DNEVERD_WINDOWS_TYPES_DIRECTORY=" + str(sdk)],
                           check=True, capture_output=True)
            packaged = json.loads((build / "neverd-windows-types-inputs.json").read_text())
            self.assertEqual(packaged.pop("supplements"), {
                name: hashlib.sha256((types / "headers" / (name + ".h")).read_bytes()).hexdigest()
                for name in names})
            self.assertEqual(packaged, manifest)

    def test_additional_metadata_preserves_catalog_input_and_i386_calling_convention(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            cache = directory / "cache"
            licenses = directory / "licenses"
            cache.mkdir()
            licenses.mkdir()
            raw = "unsigned int\u00a0Catalog(void);\n".encode()
            records = []
            for name, data in (("cryptcat.h", raw), ("LICENSE", b"source notice")):
                path = cache / ("metadata-" + name)
                path.write_bytes(data)
                records.append({"file": path.name, "name": name, "url": "unused",
                                "sha256": hashlib.sha256(data).hexdigest()})
            with patch.object(windows.flat, "METADATA_HEADERS", {"files": records}):
                windows.flat.metadata_headers(directory, cache, licenses)
            include = directory / "win32-metadata-headers"
            self.assertEqual((include / "cryptcat.h").read_bytes(), raw)
            self.assertEqual((include / "cryptcat-c.h").read_bytes(), b"unsigned int Catalog(void);\n")
            self.assertEqual((licenses / "Microsoft-Win32Metadata-LICENSE.txt").read_bytes(),
                             b"source notice")
            (directory / "sdk-import-sources").mkdir()
            projected, original = windows.flat.metadata_header_source(
                directory, "CatalogDefinitionExports", {"headers": ["cryptcat"]})
            self.assertIn("cryptcat-c.h", projected.read_text())
            self.assertIn("cryptcat.h", original.read_text())

        groups = windows.flat.METADATA_HEADERS["providers"]
        self.assertNotIn("x86", groups["CalendarExports"]["targets"])
        console = groups["ConsolePrivateExports"]
        self.assertNotIn("SetConsolePalette", console["exports"])
        self.assertNotIn("SetConsolePalette", console["targets"]["x86"]["additional_exports"])
        self.assertEqual(console["targets"]["arm64"]["additional_exports"]["SetConsolePalette"], 3)

    def test_named_runtime_profiles_preserve_target_exports_and_parameter_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            def exports(arch):
                libraries = {name: {} for name, profile in windows.flat.RUNTIME_PROFILES.items()
                             if "import_library" in profile["targets"].get(arch, {})}
                compiler = ["clang", "-DNTDDI_VERSION=0x0A00000C"]
                units = windows.flat.named_runtime_units(Path(tmp) / arch, arch, compiler, libraries)
                security = next(u for u in units if u["name"] == "ExtendedSecurityNames")
                self.assertNotIn("-UNTDDI_VERSION", security["compiler"])
                if arch == "arm64":
                    cloud = next(u for u in units if u["name"] == "CloudAuthentication")
                    self.assertEqual(cloud["compiler"][-1], "-UNTDDI_VERSION")
                    self.assertEqual(cloud["bridge"]["arguments"], [*cloud["compiler"], "-std=c23"])
                legacy = [u for u in units if u["name"] == "DbgHelpLegacy"]
                self.assertEqual(len(legacy), int(arch != "x86"))
                if legacy:
                    unit, = legacy
                    self.assertIn("-D_IMAGEHLP_SOURCE_", unit["compiler"])
                    self.assertEqual(unit["bridge"]["arguments"], [*unit["compiler"], "-std=c23"])
                    self.assertIn("runtime_import_library", unit["bridge"])
                    self.assertEqual(unit["bridge"]["runtime_exports"]["parameter_counts"]["StackWalk"], 9)
                self.assertNotIn("-D_IMAGEHLP_SOURCE_", security["compiler"])
                ssl = next(u for u in units if u["name"] == "SslProvider")
                self.assertIn(str(Path(tmp) / arch / "cpdk-headers"), ssl["compiler"])
                self.assertEqual(ssl["bridge"]["runtime_exports"]["parameter_counts"]["SslEnumCipherSuites"], 5)
                routing = next(u for u in units if u["name"] == "RtmRuntime")
                self.assertEqual(routing["default_calling_convention"], "__stdcall" if arch == "x86" else "")
                self.assertEqual("runtime_import_library" in routing["bridge"], arch == "x86")
                return {u["name"]: u["bridge"]["runtime_exports"] for u in units}
            x86, x64, arm64 = exports("x86"), exports("x64"), exports("arm64")
            self.assertEqual(x86["D3d9Runtime"]["parameter_counts"]["D3DPERF_EndEvent"], 0)
            self.assertEqual(arm64["Cfgmgr32Runtime"]["parameter_counts"]["SwDeviceCreate"], 8)
            self.assertIn("SpoolssRuntime", x64)
            self.assertNotIn("SpoolssRuntime", x86)
            self.assertEqual(x64["DisplayControl"]["parameter_counts"]["DCIOpenProvider"], 0)
            self.assertEqual(arm64["DisplayDriverFunctions"]["parameter_counts"]["EngCreateClip"], 0)
            reset = "WldpResetProductionConfiguration"
            self.assertNotIn(reset, x64["LockdownPolicy"]["names"])
            self.assertEqual(arm64["LockdownPolicy"]["parameter_counts"][reset], 0)
            self.assertNotEqual(x64["DisplayControl"]["image_sha256"],
                                arm64["DisplayControl"]["image_sha256"])
            self.assertNotIn("CloudAuthentication", x64)
            self.assertEqual(arm64["CloudAuthentication"]["parameter_counts"]["WTSCloudAuthClose"], 1)
            self.assertNotIn("EnclaveCopyIntoEnclave", x64["EnclaveServices"]["names"])
            self.assertIn("EnclaveCopyIntoEnclave", arm64["EnclaveServices"]["names"])

    def test_recent_sdk_profiles_use_their_header_and_target_imports(self):
        recent = windows.flat.RECENT_RUNTIME
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / recent["headers"]["file"]
            with zipfile.ZipFile(archive, "w") as package:
                for part in ("um", "shared", "winrt"):
                    package.writestr("c/Include/" + recent["sdk_version"] + "/" + part + "/Test.h", part)
            arch = "arm64"
            imports = root / recent["import_packages"][arch]["file"]
            with zipfile.ZipFile(imports, "w") as package:
                for name in ("bcrypt.lib", "kernel32.Lib", "ncrypt.lib"):
                    package.writestr("c/um/arm64/" + name, name)
            compiler = ["clang", "--target=aarch64-pc-windows-msvc"]
            with patch.object(windows.flat, "package", side_effect=lambda cache, rec: cache / rec["file"]):
                units, packages = windows.flat.recent_runtime_units(
                    root / arch, root, arch, "llvm-readobj", compiler, root / "wdk")
                x86 = root / recent["import_packages"]["x86"]["file"]
                with zipfile.ZipFile(x86, "w") as package:
                    for name in ("bcrypt.lib", "ncrypt.lib"):
                        package.writestr("c/um/x86/" + name, name)
                x86_units, x86_packages = windows.flat.recent_runtime_units(
                    root / "x86", root, "x86", "llvm-readobj",
                    ["clang", "--target=i686-pc-windows-msvc"], root / "wdk")
            self.assertEqual(x86_packages, [recent["import_packages"]["x86"]])
            x86_exports = {unit["name"]: unit["bridge"]["runtime_exports"]["names"]
                           for unit in x86_units}
            self.assertEqual(set(x86_exports), {"CryptographyKEM", "KeyStorageKEM", "TouchpadInput"})
            self.assertIn("GetPointerTouchpadInfo", x86_exports["TouchpadInput"])
            self.assertNotIn("InjectTouchpadAction", x86_exports["TouchpadInput"])
            self.assertEqual(packages, [recent["import_packages"][arch]])
            self.assertEqual({u["name"] for u in units}, {"CryptographyKEM", "KeyStorageKEM",
                                                        "TouchpadInput", "RuntimeUnwindV2"})
            for unit in units:
                self.assertIn(str(root / arch / "recent-sdk-vfs.json"), unit["compiler"])
                self.assertIn("-UNTDDI_VERSION", unit["compiler"])
                self.assertEqual(unit["bridge"]["arguments"], [*unit["compiler"], "-std=c23"])
            unwind = next(u["bridge"] for u in units if u["name"] == "RuntimeUnwindV2")
            self.assertEqual(unwind["runtime_exports"]["parameter_counts"]["RtlVirtualUnwind2"], 13)
            self.assertEqual(Path(unwind["runtime_import_library"]["file"]).read_bytes(), b"kernel32.Lib")
            self.assertFalse(set(windows.flat.primary_runtime_profiles()).intersection(recent["providers"]))

    def test_cpp_headers_verify_the_published_c_prototypes(self):
        with tempfile.TemporaryDirectory() as tmp:
            groups = {name: windows.flat.RUNTIME_PROFILES[name]
                      for name in ("AudioStateMonitors", "MediaFoundationCollection", "OlePropertyConversion")}
            units = windows.flat.named_runtime_units(Path(tmp), "arm64", ["clang"],
                                                     dict.fromkeys(groups, {}), profiles=groups)
            for unit in units:
                bridge = unit["bridge"]
                self.assertEqual(bridge["language"], "c++")
                self.assertEqual(bridge["arguments"], ["clang", "-x", "c++", "-std=c++17"])
                original = Path(bridge["source"]).read_text()
                if unit["name"] == "MediaFoundationCollection":
                    self.assertIn("#include <mfapi.h>", original)
                    self.assertNotIn("MFCreateCollection(", original)
                    self.assertIn("MFCreateCollection(IMFCollection **", unit["source"].read_text())
                elif unit["name"] == "OlePropertyConversion":
                    self.assertIn("#include <PropIdl.h>", original)
                    self.assertNotIn("StgConvertPropertyToVariant(", original)
                    self.assertIn("BOOLEAN __stdcall StgConvertPropertyToVariant(", unit["source"].read_text())
                else:
                    self.assertIn("#include <mmdeviceapi.h>", original)
                    self.assertIn("#include <audiostatemonitorapi.h>", original)
        self.assertNotIn("x86", windows.flat.RUNTIME_PROFILES["WindowControl"]["targets"])

    def test_system_cpp_exports_use_the_original_headers_on_each_installed_target(self):
        names = ("DebugDataModelManager", "DirectCompositionClock", "DirectCompositionControls",
                 "MachineLearningOperatorRegistry", "VolumeShadowCopyRuntime")
        profiles = {name: windows.flat.RUNTIME_PROFILES[name] for name in names}
        standard_headers = ["-I", "pinned-msvc-stl"]
        with tempfile.TemporaryDirectory() as directory:
            for arch in ("x86", "x64", "arm64"):
                units = windows.flat.named_runtime_units(Path(directory) / arch, arch, ["clang"],
                                                         dict.fromkeys(names, {}), profiles=profiles,
                                                         cpp_headers=standard_headers)
                by_name = {u["name"]: u for u in units}
                self.assertEqual("DirectCompositionClock" in by_name, arch == "arm64")
                self.assertEqual("MachineLearningOperatorRegistry" in by_name, arch != "arm64")
                for name, unit in by_name.items():
                    bridge = unit["bridge"]
                    self.assertEqual(bridge["language"], "c++")
                    original = Path(bridge["source"]).read_text()
                    published = unit["source"].read_text()
                    for header in profiles[name]["headers"]:
                        self.assertIn(f"#include <{header}.h>", original)
                    interfaces = {"DirectCompositionControls": ("IDCompositionVisual",),
                                  "DebugDataModelManager": ("IDebugHost", "IDataModelManager"),
                                  "MachineLearningOperatorRegistry": ("IMLOperatorRegistry",)}
                    for tag in interfaces.get(name, ()):
                        self.assertIn(f"struct {tag} {{ const struct {tag}Vtbl *lpVtbl;", published)
                    for export in bridge["runtime_exports"]["names"]:
                        self.assertNotIn(export + "(", original)
                        self.assertIn(export + "(", published)
                    self.assertNotIn("pinned-msvc-stl", unit["compiler"])
                    self.assertEqual("pinned-msvc-stl" in bridge["arguments"],
                                     name == "MachineLearningOperatorRegistry")
                self.assertIn("_Bool *pbBlock", by_name["VolumeShadowCopyRuntime"]["source"].read_text())

    def test_named_runtime_archive_coexists_with_other_sdk_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = json.loads(windows.ucrt.INPUTS.read_text())["import_packages"]["x86"]
            archive = root / record["file"]
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("c/um/x86/public.lib", b"ordinary import library")
                package.writestr("c/um/x86/named.lib", b"named import library")
            ordinary = root / (archive.stem + "-imports")
            windows.wdk.extract([archive], ordinary, ["c/um/x86/public.lib"])
            profile = {"module": "named.dll", "targets": {"x86": {"import_library": "named.lib"}}}
            with (patch.object(windows.flat, "RUNTIME_PROFILES", {"Named": profile}),
                  patch.object(windows.flat, "package", return_value=archive)):
                libraries = windows.flat.named_runtime_libraries(root, root, "x86", "llvm-readobj")
            self.assertEqual(Path(libraries["Named"]["file"]).read_bytes(), b"named import library")
            self.assertEqual([p.name for p in ordinary.rglob("*.lib")], ["public.lib"])

    def test_runtime_archive_refuses_same_arity_with_the_wrong_x86_linker_spelling(self):
        symbols = {"Read": {"_Read@4"}}
        target = {"architecture": "x86"}
        with self.assertRaisesRegex(ValueError, "cannot link"):
            windows.flat.verify_linkage({"Read": ("@Read", "i32", "ptr")}, symbols, target)
        windows.flat.verify_linkage({"Read": ('@"\\01_Read@4"', "i32", "ptr")}, symbols, target)

    def test_runtime_abi_rejects_truncating_an_unspecified_parameter_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original, published = root / "original.h", root / "published.h"
            original.write_text("int ReadEntry();\n", encoding="utf-8")
            published.write_text("int ReadEntry(void);\n", encoding="utf-8")
            flags = ["clang", "--target=aarch64-pc-windows-msvc"]
            proof = {"source": str(original), "projected_functions": ["ReadEntry"], "commands": [],
                     "arguments": [*flags, "-std=c23"], "language": "c",
                     "runtime_exports": {"module": "example.dll", "parameter_counts": {"ReadEntry": 2}}}
            with self.assertRaisesRegex(ValueError, "parameter list disagrees"):
                windows.flat.verify_abis(proof, published, flags, root, {"ReadEntry"}, windows.run)

    def test_runtime_parameter_counts_are_source_level(self):
        # On x86 this aggregate result adds a hidden return pointer to LLVM IR.
        sdk = ("struct Result { long value[8]; };\n"
               "struct Result __stdcall Query(int, void (__stdcall *)(char, char));\n")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = root / "sdk.h"
            header.write_text(sdk, encoding="utf-8")
            flags = ["clang", "--target=i686-pc-windows-msvc", "-std=c23"]
            for count in (2, 1, 3, 0):
                with self.subTest(metadata_count=count):
                    proof = {"source": str(header), "arguments": flags, "language": "c",
                             "commands": [], "projected_functions": ["Query"],
                             "runtime_exports": {"module": "example.dll",
                                                 "parameter_counts": {"Query": count}}}
                    if count == 2:
                        windows.flat.verify_abis(proof, header, flags, root, {"Query"}, windows.run)
                    else:
                        with self.assertRaisesRegex(ValueError, "parameter list disagrees.*Query"):
                            windows.flat.verify_abis(proof, header, flags, root, {"Query"}, windows.run)

    def test_c_import_profile_preserves_mixed_and_cpp_only_archive_evidence(self):
        symbols = {"OnlyCpp": {"?OnlyCpp@@YAHXZ", "__imp_?OnlyCpp@@YAHXZ"},
                   "Both": {"?Both@@YAHXZ", "_Both@0"}, "Plain": {"_Plain@4"},
                   "NoDeclaration": {"?NoDeclaration@@YAHXZ"}}
        proof = {"import_library": {}, "c_linkage_imports_only": True}
        with patch.object(windows.flat, "read_symbols", return_value=symbols):
            self.assertEqual(windows.flat.select(proof, ["OnlyCpp", "Both", "Plain"], None, None),
                             ["Both", "Plain"])
            self.assertEqual(proof["cpp_only_linker_symbols"],
                             {"OnlyCpp": sorted(symbols["OnlyCpp"]),
                              "NoDeclaration": sorted(symbols["NoDeclaration"])})
            self.assertEqual(proof["imports_without_declaration"], ["NoDeclaration"])
            self.assertEqual(windows.flat.select({"import_library": {}}, list(symbols),
                                                 None, None), sorted(symbols))

    def test_documented_runtime_profile_requires_every_named_declaration(self):
        proof = {"runtime_exports": {"names": ["ApiB", "ApiA"]}}
        self.assertEqual(windows.flat.select(proof, ["Private", "ApiA", "ApiB"], None, None),
                         ["ApiA", "ApiB"])
        with self.assertRaisesRegex(ValueError, "lack documented runtime exports: ApiB"):
            windows.flat.select(proof, ["ApiA"], None, None)
        with self.assertRaisesRegex(ValueError, "exactly once"):
            windows.flat.select({"runtime_exports": {"names": ["ApiA", "ApiA"]}},
                                ["ApiA"], None, None)

    def test_large_selection_preserves_every_prototype_in_a_response_file(self):
        with tempfile.TemporaryDirectory() as directory:
            response = Path(directory) / "SDK prototypes.rsp"
            base = ["neverd", "types", "--declare", "SDK \U0001f4c1/declarations.h"]
            names = ["NdisCloseConfiguration", "IcmpCloseHandle"]
            expected = ["--import-prototype=" + name for name in names]
            units = len(subprocess.list2cmdline(base + expected).encode("utf-16-le")) // 2 + 1
            with patch.dict(windows.wdk.LIMITS, windows_command_line_units=units):
                self.assertEqual(windows.prototype_selection_command(base, names, response),
                                 base + expected)
                self.assertFalse(response.exists())
            with patch.dict(windows.wdk.LIMITS, windows_command_line_units=units - 1):
                self.assertEqual(windows.prototype_selection_command(base, names, response),
                                 base + ["@" + str(response.resolve())])
                self.assertEqual(response.read_text(encoding="utf-8").splitlines(), expected)

    def test_rdbss_published_abi_must_pass_before_replacing_primary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            primary = root / "kernel.h"
            primary.write_text("/* established declarations */\n")
            source = root / "provider.h"
            source.write_text("/* provider declarations */\n")
            imported = {"declaration": 1, "prefix": "RDBSS", "renamed": [],
                        "dependencies": [], "prototypes": ["RxStartMinirdr"]}
            result = {"source": "/* proposed declarations */\n", "types": [],
                      "prototypes": ["RxStartMinirdr"], "imports": [imported]}
            unit = {"name": "RDBSS", "source": source, "headers": [], "defines": [],
                    "default_calling_convention": "", "input_sha256": "fixture",
                    "selected_prototypes": [],
                    "layouts": "layouts", "parsed": {"prototypes": ["RxStartMinirdr"]},
                    "commands": [], "metadata": {"cpp_header_bridge": {
                        "verified_functions": ["RxStartMinirdr"]}}}
            with patch.dict(windows.ISOLATED_PROVIDERS, {
                    "WindowsKernel": (windows.Provider("RDBSS", ("rxprocs", "mrx")),)}), \
                    patch.object(windows, "prepare_provider", return_value=unit), \
                    patch.object(windows, "run", return_value=json.dumps(result)), \
                    patch.object(windows, "checked_types", return_value={
                        "types": [], "prototypes": ["RxStartMinirdr"]}), \
                    patch.object(windows, "verify_layouts", return_value=1), \
                    patch.object(windows.rdbss, "verify_abis", side_effect=ValueError(
                        "published compiler ABI changed")):
                with self.assertRaisesRegex(ValueError, "published compiler ABI changed"):
                    windows.merge_providers("WindowsKernel", primary, root, ["clang"],
                                            ["clang"], "neverd", "LLP64", "layouts")
            self.assertEqual(primary.read_text(), "/* established declarations */\n")

    def test_rdbss_merge_refuses_an_unproved_companion_without_replacing_primary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            primary = root / "kernel.h"
            primary.write_text("/* established declarations */\n")
            candidate = "/* proposed declarations */\n"
            imported = {"declaration": 1, "prefix": "RDBSS", "renamed": [],
                        "dependencies": [], "prototypes": ["NewCompanion"]}
            result = {"source": candidate, "types": [], "prototypes": ["NewCompanion"],
                      "imports": [imported]}
            unit = {"name": "RDBSS", "source": root / "provider.h", "metadata": {
                "cpp_header_bridge": {"verified_functions": ["RxStartMinirdr"]}}}
            with patch.dict(windows.ISOLATED_PROVIDERS, {
                    "WindowsKernel": (windows.Provider("RDBSS", ("rxprocs", "mrx")),)}), \
                    patch.object(windows, "prepare_provider", return_value=unit), \
                    patch.object(windows, "run", return_value=json.dumps(result)), \
                    patch.object(windows, "checked_types", return_value={
                        "types": [], "prototypes": ["NewCompanion"]}), \
                    patch.object(windows, "verify_layouts", return_value=1):
                with self.assertRaisesRegex(ValueError, "lack an original C\\+\\+ ABI proof"):
                    windows.merge_providers("WindowsKernel", primary, root, ["clang"],
                                            ["clang"], "neverd", "LLP64", "layouts")
            self.assertEqual(primary.read_text(), "/* established declarations */\n")

    def test_rdbss_enum_bridge_precedes_the_type_parser(self):
        create = "typedef enum { Create = 1 } RX_CONTEXT_CREATE_FLAGS;\n"
        lowio = "typedef enum { LowIo = 2 } RX_CONTEXT_LOWIO_FLAGS;\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "RDBSS.c"
            source.write_text("#include <rxprocs.h>\n#include <mrx.h>\n")

            def run(command, log, output=None):
                if "-E" in command:
                    output.write_text(create + (lowio if "c++" in command else lowio.replace(
                        "RX_CONTEXT_LOWIO_FLAGS", "RX_CONTEXT_CREATE_FLAGS")))
                return "fixture layouts"

            def parsed(neverd, path, log, model):
                self.assertEqual(path.read_text().count("} RX_CONTEXT_CREATE_FLAGS;"), 1)
                self.assertIn("} RX_CONTEXT_LOWIO_FLAGS;", path.read_text())
                # Stop before the independent compiler checks; this test owns
                # the order of preprocessing repair and parser admission.
                raise ValueError("fixture parser boundary reached")

            with patch.object(windows, "run", side_effect=run), \
                    patch.object(windows, "checked_types", side_effect=parsed):
                with self.assertRaisesRegex(ValueError, "fixture parser boundary reached"):
                    windows.prepare_provider("RDBSS", source, ["clang"], root,
                                             "neverd", "LLP64")

    def test_original_wdk_sources_do_not_search_current_or_previous_provider_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "WindowsKernel-providers"
            legacy.mkdir()
            (legacy / "TDI.h").write_text("old declarations")
            providers = (windows.Provider("TDI", ("tdikrnl",)),
                         windows.Provider("RDBSS", ("rxprocs",)))
            prepare = windows.prepare_provider
            generated = []

            def compile_header(command, log, output=None):
                if output:
                    output.write_text("void TdiWitness(void);")
                return "fixture layouts"

            def check_location(name, source, arguments, directory, *args, **kwargs):
                if name == "RDBSS":
                    self.assertEqual(generated[0].name, "TDI.h")
                    self.assertTrue(generated[0].is_file())
                    self.assertFalse((source.parent / "TDI.h").exists())
                    self.assertFalse((directory / "TDI.h").exists())
                    raise RuntimeError("checked compiler search directories")
                unit = prepare(name, source, arguments, directory, *args, **kwargs)
                generated.append(unit["source"])
                return unit

            with patch.dict(windows.ISOLATED_PROVIDERS, WindowsKernel=providers), \
                    patch.object(windows, "prepare_provider", side_effect=check_location), \
                    patch.object(windows, "run", side_effect=compile_header), \
                    patch.object(windows, "checked_types", return_value={"prototypes": ["TdiWitness"]}):
                with self.assertRaisesRegex(RuntimeError, "checked compiler search directories"):
                    windows.merge_providers("WindowsKernel", root / "primary.h", root,
                                            ["clang"], ["clang"], "neverd", "LLP64", "layouts")
            self.assertEqual((legacy / "TDI.h").read_text(), "old declarations")

    def test_dxgi_c_parameter_bridge_requires_the_original_cpp_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "DXGI.c"
            original = '#include <windows.h>\n#include <dxgi1_6.h>\n'
            source.write_text(original)
            with patch.object(windows, "run", side_effect=ValueError("changed SDK type")):
                with self.assertRaisesRegex(ValueError, "changed SDK type"):
                    windows.prepare_provider("DXGI", source, ["clang"], root,
                                             "neverd", "LLP64")
            self.assertEqual(source.read_text(), original)
            calls = []

            def run(command, log, output=None):
                calls.append(command)
                if output:
                    output.write_text("long DXGIDeclareAdapterRemovalSupport(void);\n")
                return "record layouts"

            with patch.object(windows, "run", side_effect=run), \
                    patch.object(windows, "checked_types", return_value={"prototypes": []}):
                result = windows.prepare_provider("DXGI", source, ["clang"], root,
                                                  "neverd", "LLP64")
            self.assertIn("c++", calls[0])
            self.assertIn("-fsyntax-only", calls[0])
            self.assertIn("DXGIDeclareAdapterRemovalSupport(void);", source.read_text())
            proof = result["metadata"]["cpp_parameter_bridge"]
            self.assertEqual(proof["sha256"], windows.wdk.digest(Path(proof["source"])))
            self.assertIn("decltype(&DXGIDeclareAdapterRemovalSupport)",
                          Path(proof["source"]).read_text())

    def test_isolated_kernel_provider_keeps_default_before_selection(self):
        # Preprocessing removes /Gz. The parser must see that policy before
        # reading both the exported function and its unannotated callback.
        declaration = "typedef int (*Callback)(int);\nlong Read(Callback cb);\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.c"
            source.write_text(declaration)
            seen = []

            def run(command, log, output=None):
                if output:
                    output.write_text(declaration)
                return "fixture record layouts"

            def parsed(neverd, path, log, model):
                seen.append(path.read_text())
                return {"prototypes": ["Read"], "problems": []}

            for convention in ("__stdcall", ""):
                with patch.object(windows, "run", side_effect=run), \
                        patch.object(windows, "checked_types", side_effect=parsed):
                    result = windows.prepare_provider(
                        "Provider", source, ["clang"], root, "neverd", "ILP32",
                        convention=convention)
                expected = (("#pragma neverd_default_calling_convention(__stdcall)\n"
                             if convention else "") + declaration)
                self.assertEqual(seen[-1], expected)
                self.assertEqual(result["source"].read_text(), expected)
                self.assertEqual(result["default_calling_convention"], convention)
                self.assertEqual(result["input_sha256"], windows.wdk.digest(result["source"]))

    def test_x86_driver_compiler_default_requires_official_build_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = Path(directory)
            props = kit / f"c/build/{windows.VERSION}/WindowsDriver.Common.props"
            props.parent.mkdir(parents=True)
            source = ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
                      '<ItemDefinitionGroup><ClCompile>'
                      '<CallingConvention>StdCall</CallingConvention>'
                      '</ClCompile></ItemDefinitionGroup></Project>')
            props.write_text(source)
            self.assertEqual(windows.compiler_defaults(kit, "x86", "WindowsKernel"),
                             (["-Xclang", "-fdefault-calling-conv=stdcall"], "__stdcall"))
            for arch in windows.TARGETS:
                self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsUser"), ([], ""))
                if arch != "x86":
                    self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsKernel"), ([], ""))
            props.write_text(source.replace("StdCall", "unknown"))
            with self.assertRaisesRegex(ValueError, "unrecognized WDK"):
                windows.compiler_defaults(kit, "x86", "WindowsKernel")

    def test_combined_targets_require_all_headers_and_matching_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            targets = []
            for arch, (suffix, _, _, _) in windows.TARGETS.items():
                target = root / arch
                (target / "licenses").mkdir(parents=True)
                notice = target / "licenses" / "notice.txt"
                notice.write_text("Synthetic test notice")
                libraries = []
                for base in windows.LIBRARIES:
                    name = base + suffix
                    header = target / (name + ".h")
                    header.write_text("/* synthetic " + arch + " library */")
                    libraries.append({"name": name, "sha256": windows.wdk.digest(header)})
                record = {"schema": 1, "provider": "Microsoft SDK/WDK",
                          "architecture": arch, "libraries": libraries,
                          "compiler": "fixture compiler", "packages": [],
                          "profiles": {"ucrt": "fixture profile hash"},
                          "target_packages": [{"file": f"sdk.{arch}.nupkg",
                                               "architecture": arch,
                                               "sha256": "fixture package hash"}],
                          "licenses": [{"file": notice.name, "sha256": windows.wdk.digest(notice)}]}
                (target / "manifest.json").write_text(json.dumps(record))
                targets.append(target)
            output = root / "combined"
            windows.combine_targets(output, targets)
            combined = json.loads((output / "manifest.json").read_text())
            self.assertEqual(len(combined["libraries"]), 6)
            self.assertEqual({entry["architecture"] for entry in combined["libraries"]},
                             set(windows.TARGETS))
            self.assertEqual(len(combined["target_packages"]), 3)
            self.assertEqual(combined["profiles"], {"ucrt": "fixture profile hash"})
            for record in combined["libraries"]:
                self.assertEqual(windows.wdk.digest(output / (record["name"] + ".h")), record["sha256"])
            # A mixture of provider policy, generation profile or target
            # packages must never certify a previously published bundle.
            path = targets[-1] / "manifest.json"
            original = path.read_text()
            mutations = [
                ("provider", windows.COMPATIBLE_PROVIDER, "different"),
                ("profiles", {}, "different"),
                ("compiler", "other compiler", "different"),
                ("packages", [{"file": "other"}], "different"),
                ("target_packages", [{"file": "sdk.other.nupkg", "architecture": "wrong"}],
                 "architecture-specific"),
                ("target_packages", [{"file": "sdk.x64.nupkg", "architecture": "arm64"}],
                 "architecture-specific"),
            ]
            for key, value, message in mutations:
                with self.subTest(key=key, value=value):
                    changed = json.loads(original)
                    changed[key] = value
                    path.write_text(json.dumps(changed))
                    with self.assertRaisesRegex(ValueError, message):
                        windows.combine_targets(output, targets)
                    self.assertFalse((output / "manifest.json").exists())
            path.write_text(original)
            with self.assertRaisesRegex(ValueError, "required"):
                windows.combine_targets(output, targets[:2])
            self.assertFalse((output / "manifest.json").exists())
            header = targets[0] / "WindowsKernelX64.h"
            header.write_text("changed bytes")
            with self.assertRaisesRegex(ValueError, "changed Microsoft target header"):
                windows.combine_targets(output, targets)
            self.assertFalse((output / "manifest.json").exists())

    def test_layout_changes_reject_equal_size_and_pointer_width_loss(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct Record\n"
                    "         0 |   void * __ptr32 Pointer\n"
                    "         4 |   unsigned int Length\n"
                    "           | [sizeof=8, align=4]\n")
        self.assertEqual(windows.verify_layouts(original, original), 1)
        for changed in (original.replace("align=4", "align=8"),
                        original.replace("4 |", "8 |"),
                        original.replace("Length", "MissingField"),
                        original.replace("sizeof=8", "sizeof=16")):
            with self.assertRaisesRegex(ValueError, "Record"):
                windows.verify_layouts(original, changed)
        with self.assertRaisesRegex(ValueError, "no shared"):
            windows.verify_layouts(original, "")

    def test_cpp_record_layout_continuations_still_check_size_and_alignment(self):
        source = ("*** Dumping AST Record Layout\n"
                  "         0 | struct Record\n"
                  "         0 |   const char * Text\n"
                  "         8 |   _Bool Owned\n"
                  "           | [sizeof=16, align=8,\n"
                  "           |  nvsize=16, nvalign=8]\n")
        rendered = source.replace(",\n           |  nvsize=16, nvalign=8]", "]")
        self.assertEqual(windows.verify_layouts(source, rendered), 1)
        self.assertEqual(windows.verify_layouts(source.replace("sizeof=16,", "sizeof=16, dsize=16,"),
                                               rendered), 1)
        with self.assertRaisesRegex(ValueError, "layout changed"):
            windows.verify_layouts(source, rendered.replace("8 |", "9 |"))

    def test_provider_layouts_follow_renames_and_require_the_needed_records(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct URL\n"
                    "         0 |   unsigned short * Text\n"
                    "           | [sizeof=8, align=8]\n"
                    "*** Dumping AST Record Layout\n"
                    "         0 | struct Unused\n"
                    "         0 |   int Value\n"
                    "           | [sizeof=4, align=4]\n")
        generated = original.replace("struct URL", "struct Wide_URL")
        renamed = {"URL": "Wide_URL"}
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["URL"]), 1)
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["COUNT"]), 0)
        with self.assertRaisesRegex(ValueError, "missing: URL -> Wide_URL"):
            windows.verify_layouts(original, original, renamed, ["URL"])
        with self.assertRaisesRegex(ValueError, "layout changed: URL"):
            windows.verify_layouts(original, generated.replace("align=8", "align=4"),
                                   renamed, ["URL"])

    def test_failed_update_cannot_certify_stale_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "manifest.json").write_text('{"stale": true}')
            with patch.object(windows.wdk, "declarations", side_effect=ValueError("fixture")):
                with self.assertRaisesRegex(ValueError, "fixture"):
                    windows.generate(root, root / "cache", "neverd", "clang")
            self.assertFalse((root / "manifest.json").exists())

    def test_partial_parser_results_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "parse.log"
            with patch.object(windows, "run", return_value='{"problems": [{"message": "unknown type"}]}'):
                with self.assertRaisesRegex(ValueError, "refused"):
                    windows.checked_types("neverd", Path("test.h"), log)
            self.assertIn("unknown type", log.read_text())

    def test_compatibility_requires_official_provider_before_acquisition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(windows.wdk, "download") as download:
                with self.assertRaisesRegex(ValueError, "requires the official"):
                    windows.generate(root, root / "cache", "neverd", "clang",
                                     ucrt_compatibility=True)
                download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
