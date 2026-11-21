"""Qualify flat Windows APIs from the matching Microsoft SDK headers and COFF import libraries."""
from __future__ import annotations

import json
from pathlib import Path
import re

import build_wdk_driver_fixtures as wdk
import microsoft_ucrt_types as ucrt


LEGACY_INPUTS = Path(__file__).with_name("MicrosoftLegacySDKInputs.json")
# The 32-bit SDK has no host-compute or hypervisor-platform import library.
WIDE_ONLY = {"HostCompute", "HostNetwork", "HypervisorPlatform"}
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
}
LEGACY = {"AllJoyn": "MSAJApi.lib", "PeerNetworking": "p2p.lib", "PeerGraph": "p2pgraph.lib"}


def package(cache, record):
    return wdk.download(cache, record["file"], record["url"], record["sha256"])


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
            "vc_notice": package(cache, runtime["vc_license"])}


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
    directory = output / "WindowsUser-providers"
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
            source = directory / (name + "-sdk.c")
            headers = common_headers + headers
            source.write_text("".join(f"#include <{h}.h>\n" for h in headers), encoding="utf-8")
            units.append({"name": name, "source": source, "compiler": compiler, "headers": headers,
                          "bridge": {"source": str(source), "source_sha256": wdk.digest(source),
                                     "arguments": original, "language": language,
                                     "import_library": identity, "commands": []}})
    return units, packages


def select(proof, declared, directory, run):
    symbols = read_symbols(proof["import_library"], directory, run)
    names = sorted(symbols.keys() & set(declared))
    if not names:
        raise ValueError("Microsoft SDK headers and import library share no named API")
    proof.update(projected_functions=names, imports_without_declaration=sorted(symbols.keys()-set(names)))
    return names


def verify_abis(proof, header, compiler, directory, prototypes, run):
    names = proof["projected_functions"]
    if not set(names) <= set(prototypes):
        raise ValueError("published SDK import inventory is incomplete")
    imports = read_symbols(proof["import_library"], directory, run)
    bindings = "".join(f"__typeof__(&{name}) neverd_ucrt_abi_{i} = &{name};\n"
                       for i, name in enumerate(names))
    contracts = []
    for tag, include, options in (("original", proof["source"], proof["arguments"]),
                                  ("published", header, compiler)):
        source = directory / (proof["import_library"]["module"] + "-" + tag + "-abi.c")
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
        symbol = contracts[0][name][0]
        if symbol.startswith('@"\\01') and symbol.endswith('"'):
            linker = symbol[5:-1]
        elif re.fullmatch(r"@[A-Za-z_]\w*", symbol):
            linker = ("_" if proof["import_library"]["architecture"] == "x86" else "") + symbol[1:]
        else:
            raise ValueError("unrecognized Microsoft SDK linkage: " + symbol)
        if linker not in imports[name]:
            raise ValueError("original Microsoft ABI cannot link to its SDK import: " + name)
    proof.update(verified_functions=list(names), c_declaration_sha256=wdk.digest(header))
