"""Qualify flat Windows APIs from the matching Microsoft SDK headers and COFF import libraries."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

import build_wdk_driver_fixtures as wdk
import microsoft_ordinal_imports as ordinal
import microsoft_ucrt_types as ucrt


LEGACY_INPUTS = Path(__file__).with_name("MicrosoftLegacySDKInputs.json")
WEBAUTHN_INPUTS = Path(__file__).with_name("MicrosoftWebAuthnInputs.json")
NAMED_RUNTIME_INPUTS = Path(__file__).with_name("MicrosoftNamedRuntimeSDKInputs.json")
SYSTEM_RUNTIME_INPUTS = Path(__file__).with_name("MicrosoftSystemRuntimeSDKInputs.json")
NAMED_RUNTIME = json.loads(NAMED_RUNTIME_INPUTS.read_text(encoding="utf-8"))
SYSTEM_RUNTIME = json.loads(SYSTEM_RUNTIME_INPUTS.read_text(encoding="utf-8"))["providers"]
RUNTIME_PROFILES = NAMED_RUNTIME["providers"] | SYSTEM_RUNTIME

# The 32-bit SDK has no host-compute or hypervisor-platform import library.
WIDE_ONLY = {"HostCompute", "HostNetwork", "HypervisorPlatform", "ComputeStorage"}
CURRENT = {
    "ColorManagement": ("Mscms.Lib", ("icm",)),
    "CatalogTrust": ("WinTrust.Lib", ("mscat",)),
    "MonitorConfiguration": ("dxva2.lib", ("d3d9", "dxva2api", "dxvahd",
                             "physicalmonitorenumerationapi", "highlevelmonitorconfigurationapi",
                             "lowlevelmonitorconfigurationapi")),
    "DiagnosticData": ("DiagnosticDataQuery.Lib", ("diagnosticdataquery",)),
    "WSDevices": ("wsdapi.lib", ("wsdapi",)),
    "HostCompute": ("computecore.lib", ("computecore",)),
    "HostNetwork": ("computenetwork.lib", ("computenetwork",)),
    "HypervisorPlatform": ("WinHvPlatform.lib", ("winhvplatform",)),
    "KeyProtection": ("ncrypt.lib", ("ncrypt", "ncryptprotect")),
    "SocketProviders": ("WS2_32.Lib", ("ws2spi", "sporder")),
    "TrustPolicies": ("AdvAPI32.Lib", ("winsafer", "perflib", "appmgmt")),
    "Direct3D10": ("d3d10.lib", ("d3d10", "d3d10effect", "d3d10shader", "d3d10misc")),
    "InkRecognition": ("inkobjcore.lib", ("recapis",)),
    "ImageHelpers": ("ImageHlp.Lib", ("imagehlp",)),
    "OLEDialogs": ("OleDlg.Lib", ("oledlg",)),
    "XMLCryptography": ("cryptxml.lib", ("cryptxml",)),
    "DAVClient": ("davclnt.lib", ("davclnt",)),
    "Accessibility": ("OleAcc.Lib", ("oleacc",)),
    "InputMethod": ("Imm32.Lib", ("imm", "immdev")),
    "InteractionContext": ("ninput.lib", ("interactioncontext",)),
    "DeltaCompression": ("msdelta.lib", ("msdelta",)),
    "PatchAPI": ("mspatcha.lib", ("patchapi",)),
    "NetworkDiagnostics": ("ndfapi.lib", ("ndfapi",)),
    "SetupAdministration": ("advpack.Lib", ("advpub",)),
    "RealtimeWorkQueue": ("RTWorkQ.lib", ("RTWorkQ",)),
    "WindowsRemoteManagement": ("wsmsvc.lib", ("wsman",)),
    "ProcessStatus": ("psapi.lib", ("psapi",)),
    "ProcessSnapshots": ("kernel32.Lib", ("processsnapshot",)),
    "InternetControlMessage": ("iphlpapi.lib", ("iphlpapi", "icmpapi")),
    "TransactionManager": ("ktmw32.lib", ("ktmw32",)),
    "SNMPUtilities": ("SnmpAPI.Lib", ("snmp",)),
    "WebAuthentication": ("webauthn.lib", ("webauthn",)),
    "SensorUtilities": ("SensorsUtils.lib", ("sensorsdef", "sensorsutils")),
    "EventCollector": ("wecapi.lib", ("evcoll",)),
    "DeviceManagementRegistration": ("MDMRegistration.lib", ("mdmregistration",)),
    "NetworkTrafficControl": ("Traffic.Lib", ("ntddndis", "traffic")),
    "ComputeStorage": ("computestorage.lib", ("computestorage",)),
    "WebSockets": ("websocket.lib", ("websocket",)),
    "RestartManager": ("rstrtmgr.lib", ("restartmanager",)),
    "FontEmbedding": ("t2embed.lib", ("t2embapi",)),
    "PerformanceCounters": ("LoadPerf.Lib", ("loadperf",)),
    "QoSTraffic": ("qwave.lib", ("qos2",)),
    "OverlayFiles": ("wofutil.lib", ("wofapi",)),
}
LEGACY = {"AllJoyn": "MSAJApi.lib", "PeerNetworking": "p2p.lib", "PeerGraph": "p2pgraph.lib"}
WORK_QUEUE_END = "RtwqEndUnregisterWorkQueueWithMMCSS"
WORK_QUEUE_END_DOC = ("https://learn.microsoft.com/en-us/windows/win32/procthread/"
                      "rtwqendunregisterworkqueuewithmmcss")
CPP_IMPORT_PREFIXES = {"RealtimeWorkQueue": "Rtwq", "WindowsRemoteManagement": "WSMan"}


def package(cache, record):
    return wdk.download(cache, record["file"], record["url"], record["sha256"])


def work_queue_c_types(header):
    """Give the three COM interfaces their C object layout for the flat API."""
    text = header.read_text(encoding="utf-8")
    # RTWorkQ.h declares the same derived interfaces even when preprocessed as C.
    # None is passed by value. The original C++ compiler verifies the flat ABI.
    for tag in ("IRtwqAsyncResult", "IRtwqAsyncCallback", "IRtwqPlatformEvents"):
        pattern = rf"\bstruct\s+{tag}\s*:\s*public\s+IUnknown\s*\{{[^{{}}]*\}}\s*;"
        layout = f"typedef struct {tag} {{ const struct {tag}Vtbl *lpVtbl; }} {tag};"
        text, count = re.subn(pattern, layout, text)
        if count != 1:
            raise ValueError(f"unexpected RTWorkQ SDK declaration for {tag}")
    derived = r"(typedef\s+struct\s+tagRTWQASYNCRESULT)\s*:\s*public\s+IRtwqAsyncResult\s*\{"
    text, count = re.subn(derived, r"\1 { IRtwqAsyncResult base;", text)
    if count != 1:
        raise ValueError("unexpected RTWorkQ SDK async result record")
    # An empty parameter list in the original C++ declaration means zero arguments.
    text = re.sub(r"\b(Rtwq\w+)\s*\(\s*\)\s*;", r"\1(void);", text)
    header.write_text(text, encoding="utf-8")


def wsman_c_types(header):
    """Project the SDK 1.1 startup record with its inherited 1.0 field names."""
    text = header.read_text(encoding="utf-8")
    base = r"typedef\s+struct\s+_WSMAN_SHELL_STARTUP_INFO_V10\s*\{([^{}]+)\}\s*WSMAN_SHELL_STARTUP_INFO_V10\s*;"
    derived = (r"(typedef\s+struct\s+_WSMAN_SHELL_STARTUP_INFO_V11)\s*:\s*_WSMAN_SHELL_STARTUP_INFO_V10"
               r"\s*\{([^{}]+)\}\s*WSMAN_SHELL_STARTUP_INFO_V11\s*;")
    fields = re.findall(base, text)
    if len(fields) != 1:
        raise ValueError("unexpected WSMan SDK 1.0 startup record")
    text, count = re.subn(derived, lambda m: m[1] + " {" + fields[0] + m[2] +
                          "} WSMAN_SHELL_STARTUP_INFO_V11;", text)
    if count != 1:
        raise ValueError("unexpected WSMan SDK 1.1 startup record")
    header.write_text(text, encoding="utf-8")


def cpp_import_names(name, proof, directory, commands, run):
    """Inventory the original C++ translation unit independently of the C projection."""
    prefix = CPP_IMPORT_PREFIXES[name]
    command = proof["arguments"] + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                                    "-Xclang", "-ast-dump-filter=" + prefix, proof["source"]]
    commands.append(command)
    dump = run(command, directory / "original-cpp-ast.log")
    decoder = json.JSONDecoder()
    names = set()
    offset = 0
    while offset < len(dump):
        if dump[offset].isspace():
            offset += 1
            continue
        node, offset = decoder.raw_decode(dump, offset)
        if node["kind"] == "FunctionDecl" and node["name"].startswith(prefix):
            names.add(node["name"])
    return names


def named_runtime_exports(profile, target, metadata):
    common, extra = profile["exports"], target["additional_exports"]
    if common.keys() & extra.keys():
        raise ValueError("duplicate named SDK runtime export for target")
    counts = {**common, **extra}
    return {"module": profile["module"], "names": sorted(counts), "parameter_counts": counts,
            "image_sha256": target["image_sha256"], "metadata": metadata}


def named_runtime_libraries(output, cache, architecture, readobj):
    targets = {name: profile["targets"][architecture]["import_library"]
               for name, profile in RUNTIME_PROFILES.items()
               if "import_library" in profile["targets"].get(architecture, {})}
    if not targets:
        return {}
    record = json.loads(ucrt.INPUTS.read_text(encoding="utf-8"))["import_packages"][architecture]
    kit = output / (Path(record["file"]).stem + "-named-runtime-imports")
    prefix = f"c/um/{architecture}/"
    wdk.extract([package(cache, record)], kit, sorted({prefix + name for name in targets.values()}))
    return {name: import_identity(kit / (prefix + filename), RUNTIME_PROFILES[name]["module"],
                                  architecture, readobj) for name, filename in targets.items()}


def import_identity(path, module, architecture, readobj):
    return {"module": module, "file": str(path), "architecture": architecture,
            "sha256": wdk.digest(path), "command": [readobj, "--coff-imports", str(path)]}


def named_runtime_units(output, architecture, compiler, libraries):
    """Project documented named PE exports qualified for this target."""
    directory = output / "sdk-import-sources"
    directory.mkdir(parents=True, exist_ok=True)
    units = []
    for name, profile in RUNTIME_PROFILES.items():
        if architecture not in profile["targets"]:
            continue
        target = profile["targets"][architecture]
        headers = ["winsock2", "windows", *profile["headers"]]
        source = directory / (name + "-sdk.c")
        source.write_text("".join(f"#include <{h}.h>\n" for h in headers), encoding="utf-8")
        # In C23 an SDK declaration such as EngCreateClip() has zero parameters.
        # Bind the published prototype against the original C declaration in that dialect.
        convention = target.get("default_calling_convention", "")
        if convention and (architecture != "x86" or convention != "__stdcall" or name not in libraries):
            raise ValueError("unverified default calling convention for " + name)
        options = compiler + (["-Xclang", "-fdefault-calling-conv=stdcall"] if convention else [])
        bridge = {"source": str(source), "source_sha256": wdk.digest(source),
                  "arguments": [*options, "-std=c23"], "language": "c", "commands": [],
                  "runtime_exports": named_runtime_exports(profile, target, NAMED_RUNTIME["win32_metadata"])}
        if "import_library" in target:
            bridge["runtime_import_library"] = libraries[name]
        units.append({"name": name, "source": source, "compiler": options, "headers": headers,
                      "default_calling_convention": convention, "bridge": bridge})
    return units


def webauthn_runtime_unit(output, cache, compiler):
    """Use Microsoft's public headers for APIs resolved from webauthn.dll at runtime."""
    inputs = json.loads(WEBAUTHN_INPUTS.read_text(encoding="utf-8"))
    headers = [package(cache, record) for record in inputs["headers"]]
    license_path = package(cache, inputs["license"])
    directory = output / "sdk-import-sources"
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / (inputs["provider"] + "-sdk.c")
    source.write_text("#include <winsock2.h>\n#include <windows.h>\n" +
                      "".join(f'#include "{h.resolve().as_posix()}"\n' for h in headers),
                      encoding="utf-8")
    bridge = {"source": str(source), "source_sha256": wdk.digest(source),
              "arguments": compiler, "language": "c", "commands": [],
              "runtime_exports": {"module": inputs["module"], "names": inputs["exports"],
                                  "metadata": inputs["metadata"]}}
    unit = {"name": inputs["provider"], "source": source, "compiler": compiler,
            "headers": [h.name for h in headers], "default_calling_convention": "",
            "bridge": bridge}
    return unit, inputs["headers"], license_path


def alljoyn_notice(headers, output, record):
    notices = set()
    for header in headers.glob("*.h"):
        blocks = [block for block in re.findall(rb"/\*.*?\*/", header.read_bytes(), re.DOTALL)
                  if b"Copyright AllSeen Alliance." in block]
        if len(blocks) != 1:
            raise ValueError("missing or ambiguous original AllJoyn notice in " + header.name)
        notices.add(blocks[0])
    if len(notices) != 1:
        raise ValueError("archived Microsoft AllJoyn headers have inconsistent notices")
    notice = notices.pop()
    if hashlib.sha256(notice).hexdigest() != record["sha256"]:
        raise ValueError("original AllJoyn notice differs from the pinned SDK")
    path = output / record["file"]
    path.write_bytes(notice)
    return path


def legacy_headers(root, cache):
    legacy = json.loads(LEGACY_INPUTS.read_text(encoding="utf-8"))
    runtime = json.loads(ucrt.INPUTS.read_text(encoding="utf-8"))
    kit = root / "legacy-sdk-headers"
    relative = "c/Include/" + legacy["sdk_version"] + "/"
    archives = [package(cache, legacy["headers"]), package(cache, runtime["vc_headers"])]
    wdk.extract(archives, kit, [relative + part + "/" for part in ("um", "shared", "ucrt")]
                + [runtime["vc_include"] + "/"])
    includes = kit / relative
    overlay = root / "legacy-sdk-vfs.json"
    overlay.write_text(json.dumps({"version": 0, "case-sensitive": False,
                                   "roots": [wdk.overlay_directory(includes, True)]}),
                       encoding="utf-8")
    return {"includes": includes, "vc": kit / runtime["vc_include"], "overlay": overlay,
            "packages": [legacy["headers"], runtime["vc_headers"]],
            "notices": [package(cache, runtime["vc_license"]),
                        alljoyn_notice(includes / "um/alljoyn_c", root, legacy["alljoyn_notice"])]}


def read_symbols(inputs, directory, run):
    if wdk.digest(Path(inputs["file"])) != inputs["sha256"]:
        raise ValueError("Microsoft SDK import library changed: " + inputs["file"])
    if witnessed := inputs.get("ordinal_export_witness"):
        if inputs["module"] != witnessed["module"]:
            raise ValueError("SDK ordinal witness belongs to a different DLL")
        return ordinal.symbols(Path(inputs["file"]), inputs["architecture"],
                               inputs["module"], witnessed["exports"], witnessed.get("pe_export_names"))
    text = run(inputs["command"], directory / (inputs["module"] + "-imports.log"))
    symbols = ucrt.module_import_symbols(text, inputs["architecture"], inputs["module"])
    if not symbols:
        raise ValueError("Microsoft SDK has no native named code imports for " + inputs["module"])
    return symbols


def source_units(output, cache, architecture, readobj, primary, legacy):
    """Keep each import library, header profile and target architecture together."""
    current = json.loads(ucrt.INPUTS.read_text(encoding="utf-8"))
    groups = {key: value for key, value in CURRENT.items()
              if architecture != "x86" or key not in WIDE_ONLY}
    ordinal_profiles = ordinal.profiles()
    groups.update({name: (profile["library"], tuple(profile["headers"]))
                   for name, profile in ordinal_profiles.items() if architecture in profile["targets"]})
    specifications = [(current["import_packages"][architecture], groups, primary,
                       primary, "c", ("winsock2", "windows"))]
    if legacy is not None:
        includes = legacy["includes"]
        alljoyn = sorted(path.relative_to(includes / "um").as_posix()[:-2]
                         for path in (includes / "um/alljoyn_c").glob("*.h"))
        if "alljoyn_c/AjAPI" not in alljoyn:
            raise ValueError("archived Microsoft AllJoyn headers are incomplete")
        old = json.loads(LEGACY_INPUTS.read_text(encoding="utf-8"))
        groups = {name: (library, ("stdbool", "msajtransport", *alljoyn) if name == "AllJoyn" else ("p2p",))
                  for name, library in LEGACY.items()}
        common = primary[:primary.index("-I")] + ["-ivfsoverlay", str(legacy["overlay"]),
                  "-DQCC_OS_GROUP_WINDOWS=1", "-I", str(includes / "um"), "-I", str(includes / "shared")]
        crt = primary[primary.index("-I"):]
        sdk_c = common + crt
        sdk_cpp = common + ["-I", str(legacy["vc"]), "-I", str(includes / "ucrt"),
                             "-fms-compatibility-version=19.44", "-x", "c++", "-std=c++17"]
        specifications.append((old["import_packages"][architecture], groups, sdk_c, sdk_cpp, "c++",
                               ("winsock2", "windows")))
    directory = output / "sdk-import-sources"
    directory.mkdir(parents=True, exist_ok=True)
    units, packages = [], []
    for record, groups, compiler, original, language, common_headers in specifications:
        kit = output / (Path(record["file"]).stem + "-imports")
        arch_dir = f"c/um/{architecture}/"
        members = [arch_dir + library for library, _ in groups.values()]
        wdk.extract([package(cache, record)], kit, members)
        packages.append(dict(record, architecture=architecture))
        for name, (library, headers) in groups.items():
            path = kit / (arch_dir + library)
            identity = import_identity(path, Path(library).stem.lower() + ".dll", architecture, readobj)
            # SensorsUtils.lib carries short imports for the versioned system DLL.
            if name == "SensorUtilities":
                identity["module"] = "sensorsutilsv2.dll"
            if name in ordinal_profiles:
                record = ordinal.witness(ordinal_profiles[name], architecture)
                identity.update(module=record["module"], ordinal_export_witness=record)
            # Both SDK libraries decorate their unannotated x86 APIs as stdcall.
            # Keep the original compiler and the declaration reader on that
            # target convention. verify_abis checks every selected object symbol.
            stdcall = architecture == "x86" and name in ("TransactionManager", "SensorUtilities")
            defaults = ["-Xclang", "-fdefault-calling-conv=stdcall"] if stdcall else []
            source = directory / (name + "-sdk.c")
            headers = common_headers + headers
            # PSAPI v1 selects PSAPI.dll. WinRM requires an explicit API version.
            preludes = {"ProcessStatus": "#define PSAPI_VERSION 1\n",
                        "WindowsRemoteManagement": "#define WSMAN_API_VERSION_1_1\n"}
            prelude = preludes.get(name, "")
            body = prelude + "".join(f"#include <{h}.h>\n" for h in headers)
            prefixes = {"ProcessSnapshots": "Pss", "InternetControlMessage": "Icmp", **CPP_IMPORT_PREFIXES}
            scope = {"import_prefix": prefixes[name]} if name in prefixes else {}
            if name == "SetupAdministration":
                scope["c_linkage_imports_only"] = True
            if name == "RealtimeWorkQueue":
                # Microsoft documents this import without an SDK header. Its
                # callback result is IMFAsyncResult in that public declaration.
                body += ("struct IMFAsyncResult;\n"
                         f"STDAPI {WORK_QUEUE_END}(struct IMFAsyncResult *pResult);\n")
                scope["documented_declarations"] = {WORK_QUEUE_END: WORK_QUEUE_END_DOC}
            source.write_text(body, encoding="utf-8")
            cpp_header = name in CPP_IMPORT_PREFIXES
            original_options = original + defaults
            if cpp_header:
                original_options += ["-x", "c++", "-std=c++17"]
            units.append({"name": name, "source": source, "compiler": compiler + defaults,
                          "headers": headers, "default_calling_convention": "__stdcall" if stdcall else "",
                          "bridge": {"source": str(source), "source_sha256": wdk.digest(source),
                                     "arguments": original_options,
                                     "language": "c++" if cpp_header else language,
                                     "import_library": identity, "commands": [], **scope}})
    return units, packages


def select(proof, declared, directory, run):
    if "runtime_exports" in proof:
        names = sorted(proof["runtime_exports"]["names"])
        if not names or len(names) != len(set(names)):
            raise ValueError("runtime export profile must name each routine exactly once")
        if missing := set(names) - set(declared):
            raise ValueError("Microsoft headers lack documented runtime exports: " +
                             ", ".join(sorted(missing)))
        proof["projected_functions"] = names
        return names
    symbols = read_symbols(proof["import_library"], directory, run)
    eligible = {name for name in symbols if name.startswith(proof.get("import_prefix", ""))}
    declarations = set(declared)
    missing_declarations = sorted(eligible - declarations)
    if proof.get("c_linkage_imports_only"):
        # Advpack exports several C names whose SDK archive exposes exclusively
        # C++ linker symbols. Preserve the archive evidence for those names.
        cpp_only = {name: sorted(symbols[name]) for name in sorted(eligible)
                    if all(symbol.startswith(("?", "__imp_?")) for symbol in symbols[name])}
        proof["cpp_only_linker_symbols"] = cpp_only
        eligible.difference_update(cpp_only)
    names = sorted(eligible & declarations)
    if "ordinal_export_witness" in proof["import_library"] and (missing := eligible - set(names)):
        raise ValueError("Microsoft headers lack witnessed ordinal exports: " + ", ".join(sorted(missing)))
    if not names:
        raise ValueError("Microsoft SDK headers and import library share no named API")
    proof.update(projected_functions=names, imports_without_declaration=missing_declarations)
    return names


def verify_parameter_counts(header, compiler, counts, source, commands, run):
    """Count fixed source parameters, before the target lowers aggregates or adds an sret pointer."""
    aliases = {f"neverd_api_{i}": name for i, name in enumerate(counts)}
    declarations = "".join(f"  extern __typeof__({name}) {alias};\n" for alias, name in aliases.items())
    probe = "neverd_check_parameters"
    source.write_text(f'#include "{Path(header).as_posix()}"\nvoid {probe}(void) {{\n' +
                      declarations + "}\n", encoding="utf-8")
    command = compiler + ["-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
                          "-ast-dump-filter=" + probe, str(source)]
    commands.append(command)
    tree = json.loads(run(command, source.with_suffix(".log")))
    body = next(child for child in tree["inner"] if child["kind"] == "CompoundStmt")
    actual = {decl["name"]: sum(p["kind"] == "ParmVarDecl" for p in decl.get("inner", ()))
              for stmt in body.get("inner", ()) for decl in stmt["inner"]
              if decl["kind"] == "FunctionDecl"}
    for alias, name in aliases.items():
        if actual.get(alias) != counts[name]:
            raise ValueError(f"parameter list disagrees with Win32 metadata for {name}: "
                             f"expected {counts[name]}, got {actual.get(alias)} in {header}")


def verify_linkage(contracts, imports, library):
    for name, contract in contracts.items():
        symbol = contract[0]
        if symbol.startswith('@"\\01') and symbol.endswith('"'):
            linker = symbol[5:-1]
        elif re.fullmatch(r"@[A-Za-z_]\w*", symbol):
            linker = ("_" if library["architecture"] == "x86" else "") + symbol[1:]
        else:
            raise ValueError("unrecognized Microsoft SDK linkage: " + symbol)
        if linker not in imports.get(name, ()):
            raise ValueError("original Microsoft ABI cannot link to its SDK import: " + name)


def verify_abis(proof, header, compiler, directory, prototypes, run):
    names = proof["projected_functions"]
    if not set(names) <= set(prototypes):
        raise ValueError("published SDK import inventory is incomplete")
    library = proof.get("import_library") or proof.get("runtime_import_library")
    imports = read_symbols(library, directory, run) if library else None
    if "runtime_import_library" in proof:
        proof["commands"].append(library["command"])
    stem = library["module"] if "import_library" in proof else header.stem + "-runtime"
    bindings = "".join(f"__typeof__(&{name}) neverd_ucrt_abi_{i} = &{name};\n"
                       for i, name in enumerate(names))
    contracts = []
    counts = proof.get("runtime_exports", {}).get("parameter_counts")
    for tag, include, options in (("original", proof["source"], proof["arguments"]),
                                  ("published", header, compiler)):
        if counts is not None:
            verify_parameter_counts(include, options, counts, directory / (stem + "-" + tag + "-arity.c"),
                                    proof["commands"], run)
        source = directory / (stem + "-" + tag + "-abi.c")
        body = ('extern "C" {\n' + bindings + '}\n'
                if tag == "original" and proof["language"] == "c++" else bindings)
        source.write_text('#include "' + Path(include).as_posix() + '"\n' + body, encoding="utf-8")
        command = options + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
        ir = run(command, source.with_suffix(".log"), source.with_suffix(".ll"))
        proof["commands"].append(command)
        contracts.append(ucrt.normalized_bridge_abis(ir, names))
    for name in names:
        if contracts[0][name] != contracts[1][name]:
            raise ValueError("published compiler ABI differs from Microsoft SDK: " + name)
    if library:
        verify_linkage(contracts[0], imports, library)
    proof.update(verified_functions=list(names), c_declaration_sha256=wdk.digest(header))
