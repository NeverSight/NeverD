"""Qualify flat Windows APIs from the matching Microsoft SDK headers and COFF import libraries."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

import build_wdk_driver_fixtures as wdk
import microsoft_ucrt_types as ucrt


LEGACY_INPUTS = Path(__file__).with_name("MicrosoftLegacySDKInputs.json")
WEBAUTHN_INPUTS = Path(__file__).with_name("MicrosoftWebAuthnInputs.json")
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


def package(cache, record):
    return wdk.download(cache, record["file"], record["url"], record["sha256"])


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
            identity = {"module": Path(library).stem.lower() + ".dll", "file": str(path),
                        "architecture": architecture, "sha256": wdk.digest(path),
                        "command": [readobj, "--coff-imports", str(path)]}
            # SensorsUtils.lib carries short imports for the versioned system DLL.
            if name == "SensorUtilities":
                identity["module"] = "sensorsutilsv2.dll"
            # Both SDK libraries decorate their unannotated x86 APIs as stdcall.
            # Keep the original compiler and the declaration reader on that
            # target convention. verify_abis checks every selected object symbol.
            stdcall = architecture == "x86" and name in ("TransactionManager", "SensorUtilities")
            defaults = ["-Xclang", "-fdefault-calling-conv=stdcall"] if stdcall else []
            source = directory / (name + "-sdk.c")
            headers = common_headers + headers
            # The SDK maps the default PSAPI profile to Kernel32's K32*
            # exports. Version 1 declares the routines exported by PSAPI.dll.
            prelude = "#define PSAPI_VERSION 1\n" if name == "ProcessStatus" else ""
            source.write_text(prelude + "".join(f"#include <{h}.h>\n" for h in headers), encoding="utf-8")
            prefixes = {"ProcessSnapshots": "Pss", "InternetControlMessage": "Icmp"}
            scope = {"import_prefix": prefixes[name]} if name in prefixes else {}
            units.append({"name": name, "source": source, "compiler": compiler + defaults,
                          "headers": headers, "default_calling_convention": "__stdcall" if stdcall else "",
                          "bridge": {"source": str(source), "source_sha256": wdk.digest(source),
                                     "arguments": original + defaults, "language": language,
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
    names = sorted(eligible & set(declared))
    if not names:
        raise ValueError("Microsoft SDK headers and import library share no named API")
    proof.update(projected_functions=names, imports_without_declaration=sorted(eligible-set(names)))
    return names


def verify_abis(proof, header, compiler, directory, prototypes, run):
    names = proof["projected_functions"]
    if not set(names) <= set(prototypes):
        raise ValueError("published SDK import inventory is incomplete")
    library = proof.get("import_library")
    imports = read_symbols(library, directory, run) if library else None
    stem = library["module"] if library else proof["runtime_exports"]["module"] + "-runtime"
    bindings = "".join(f"__typeof__(&{name}) neverd_ucrt_abi_{i} = &{name};\n"
                       for i, name in enumerate(names))
    contracts = []
    for tag, include, options in (("original", proof["source"], proof["arguments"]),
                                  ("published", header, compiler)):
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
        if imports is None:
            continue
        symbol = contracts[0][name][0]
        if symbol.startswith('@"\\01') and symbol.endswith('"'):
            linker = symbol[5:-1]
        elif re.fullmatch(r"@[A-Za-z_]\w*", symbol):
            linker = ("_" if library["architecture"] == "x86" else "") + symbol[1:]
        else:
            raise ValueError("unrecognized Microsoft SDK linkage: " + symbol)
        if linker not in imports[name]:
            raise ValueError("original Microsoft ABI cannot link to its SDK import: " + name)
    proof.update(verified_functions=list(names), c_declaration_sha256=wdk.digest(header))
