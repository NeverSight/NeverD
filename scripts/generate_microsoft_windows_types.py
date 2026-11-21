#!/usr/bin/env python3
"""Generate local Windows type libraries from Microsoft's SDK/WDK.

Microsoft's original headers, licenses and the generated declarations stay in
the selected local directory. The SDK/WDK is a build-time input only. Configure
NeverD with NEVERD_WINDOWS_TYPES_DIRECTORY pointing at the output directory.
Optional UCRT compatibility declarations come from pinned Wine sources only
where Microsoft inputs supply no prototype; each provider retains its notices.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import NamedTuple
import xml.etree.ElementTree as ET

import build_wdk_driver_fixtures as wdk
import microsoft_gdiplus_types as gdiplus
import microsoft_flat_imports as flat
import microsoft_rdbss_types as rdbss
import microsoft_ucrt_types as ucrt


VERSION = "10.0.26100.0"
KERNEL_HEADERS = (
    "ntifs", "ndis", "storport", "scsiwmi", "usb", "usbdi", "usbdlib",
    "wmilib", "wdmsec", "hidusage", "hidpi", "bcrypt", "fltkernel",
    "netiodef", "fwpmk", "fwpsk",
)
USER_HEADERS = (
    "winsock2", "ws2tcpip", "windows", "winternl", "wininet", "shellapi",
    "shlobj", "shlwapi", "psapi", "tlhelp32", "dbghelp", "bcrypt", "ncrypt",
    "iphlpapi", "setupapi", "wintrust", "softpub", "userenv", "wtsapi32",
    "lm", "sddl", "aclapi", "ntsecapi", "commctrl", "urlmon", "winevt",
    "evntrace", "dwmapi", "uxtheme", "avrt", "mmsystem", "wlanapi",
    "bluetoothapis", "winldap", "winber", "winusb", "hidsdi", "hidpi",
    "virtdisk", "vss", "winspool", "imm", "sensapi", "ras", "rasdlg",
    "raserror", "rtutils", "powrprof", "sspi", "authz", "dpapi", "dhcpcsdk",
    "fwpmu", "fltuser",
)
LIBRARIES = {
    "WindowsKernel": (KERNEL_HEADERS, ["km", "shared", "km/crt", "um"],
                           ["-D_KERNEL_MODE=1", "-DNDIS680"]),
    # WDK's CRT supplies the Microsoft C runtime declarations required by the
    # SDK. It is the same pinned Microsoft package, never a host CRT or MinGW.
    "WindowsUser": (USER_HEADERS, ["um", "shared", "winrt", "km/crt"],
                         ["-DSECURITY_WIN32=1"]),
}


class Provider(NamedTuple):
    name: str
    headers: tuple[str, ...]
    defines: tuple[str, ...] = ()
    prototypes: tuple[str, ...] = ()


# These official headers have incompatible same-named types (for example,
# WinHTTP's UTF-16 URL_COMPONENTS versus WinINet's ANSI default). Parse each
# provider on its own and import only the missing prototypes and their types.
ISOLATED_PROVIDERS = {
    "WindowsKernel": (
        Provider("NetworkInterfaces", ("ntddk", "netioddk", "wsk")),
        Provider("KernelStreaming", ("ntddk", "minwindef", "ks", "swenum", "ksi")),
        Provider("StorageClass", ("ntddk", "scsi", "classpnp")),
        Provider("TDI", ("ntddk", "tdikrnl")),
        Provider("VideoPort", ("miniport", "ntddvdeo", "video")),
        Provider("ATA", ("ntddk", "ata", "irb")),
        Provider("Battery", ("ntddk", "batclass")),
        Provider("HIDPort", ("ntddk", "hidport", "hidpddi")),
        Provider("SmartCard", ("ntddk", "smclib")),
        Provider("SDPort", ("ntddk", "sdport")),
        Provider("StreamClass", ("ntddk", "minwindef", "ks", "strmini")),
        Provider("USBCamera", ("ntddk", "minwindef", "ks", "strmini", "usb", "usbcamdi")),
        Provider("AudioPort", ("ntddk", "portcls")),
        Provider("GraphicsKernel", ("ntddk", "dispmprt")),
        Provider("RDBSS", ("rxprocs", "mrx"), ("MINIRDR__NAME=neverd_rdbss",)),
        Provider("StoragePortAccess", ("ntddk", "storport"), ("DISABLE_INLINE_REGISTER_ACCESS=1",)),
        Provider("TapeClass", ("string", "minitape")),
        Provider("WPPRecorder", ("ntddk", "wpprecorder")),
        Provider("USBPower", ("ntddk", "usbpmapi")),
        Provider("SystemEntropy", ("ntddk", "minwindef", "systemrng")),
        Provider("NetworkPowerDependency", ("ntddk", "KNetPwrDepBroker")),
        Provider("NDISPoll", ("ndis", "ndis/poll"), ("NDIS685",)),
        Provider("NDISChimney", ("ndis", "ndischimney")),
        Provider("NetworkDMA", ("ndis", "netdma"), ("ENABLE_LEGACY_NETDMA=1",)),
    ),
    "WindowsUser": (
        Provider("WindowsSQLite", ("winsqlite/winsqlite3",)),
        Provider("ICU", ("icu",)),
        Provider("WebServices", ("windows", "webservices")),
        Provider("WinHTTP", ("winsock2", "winhttp")),
        Provider("HTTPServer", ("winsock2", "windows", "http")),
        Provider("Esent", ("winsock2", "windows", "esent")),
        Provider("ConfigManager", ("winsock2", "windows", "cfg", "cfgmgr32", "newdev")),
        # PDNS_RECORD is selected by UNICODE even in explicit *_W declarations.
        # Keep the official Unicode profile for these reviewed entry points,
        # then import remaining ANSI/UTF-8 and encoding-neutral declarations.
        Provider("DNSUnicode", ("winsock2", "windows", "windns"),
                 ("UNICODE=1", "_UNICODE=1"),
                 ("DnsQuery_W", "DnsModifyRecordsInSet_W", "DnsReplaceRecordSetW",
                  "DnsExtractRecordsFromMessage_W", "DnsQueryEx", "DnsServiceBrowse")),
        Provider("DNS", ("winsock2", "windows", "windns")),
        Provider("MSI", ("winsock2", "windows", "msi", "msiquery", "msidefs")),
        Provider("Cluster", ("winsock2", "windows", "clusapi")),
        # Match Microsoft's Tapi3 metadata partition. windows.h may otherwise
        # define WIN32 without a value, while tapi.h uses it in #if expressions.
        Provider("TAPI", ("winsock2", "windows", "tapi", "tspi"), ("WIN32=1",)),
        Provider("OpenGL", ("winsock2", "windows", "gl/gl", "gl/glu")),
        Provider("EventTracing", ("windows", "evntprov")),
        Provider("Path", ("windows", "pathcch")),
        Provider("COMRegistration", ("windows", "olectl"),
                 prototypes=("DllRegisterServer", "DllUnregisterServer")),
        Provider("WinRTStrings", ("windows", "winstring", "roapi", "roerrorapi", "robuffer")),
        Provider("RPCProxy", ("windows", "rpcproxy", "midles")),
        Provider("Credentials", ("windows", "wincred")),
        Provider("DeviceQuery", ("windows", "devquery")),
        Provider("ApplicationModel", ("windows", "appmodel", "featurestagingapi")),
        Provider("PropertyVariants", ("windows", "propvarutil")),
        Provider("PerformanceData", ("windows", "pdh", "pdhmsg")),
        Provider("Biometrics", ("windows", "winbio")),
        Provider("BluetoothGATT", ("windows", "bluetoothleapis")),
        Provider("ConnectionManager", ("windows", "wcmapi")),
        Provider("GraphicsThunk", ("windows", "d3dkmthk")),
        Provider("Direct3D11", ("windows", "d3d11"), (), (
            "D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain")),
        Provider("Direct3D12", ("windows", "d3d12"), (), (
            "D3D12CreateDevice", "D3D12GetDebugInterface", "D3D12GetInterface",
            "D3D12SerializeRootSignature", "D3D12CreateRootSignatureDeserializer",
            "D3D12SerializeVersionedRootSignature", "D3D12CreateVersionedRootSignatureDeserializer",
            "D3D12EnableExperimentalFeatures")),
        Provider("DXGI", ("windows", "dxgi1_6"), (), (
            "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
            "DXGIDeclareAdapterRemovalSupport", "DXGIGetDebugInterface1")),
        Provider("ShaderCompiler", ("windows", "d3dcompiler"), (), (
            "D3DReadFileToBlob", "D3DWriteBlobToFile", "D3DCompile", "D3DCompile2",
            "D3DCompileFromFile", "D3DPreprocess", "D3DGetDebugInfo", "D3DReflect",
            "D3DReflectLibrary", "D3DDisassemble", "D3DDisassembleRegion", "D3DCreateLinker",
            "D3DLoadModule", "D3DCreateFunctionLinkingGraph", "D3DGetTraceInstructionOffsets",
            "D3DGetInputSignatureBlob", "D3DGetOutputSignatureBlob",
            "D3DGetInputAndOutputSignatureBlob", "D3DStripShader", "D3DGetBlobPart",
            "D3DSetBlobPart", "D3DCreateBlob", "D3DCompressShaders", "D3DDecompressShaders",
            "D3DDisassemble10Effect")),
        # clfs.h includes the WDK's legacy stdio.h. Import the CLFS API only:
        # those transitive CRT declarations must not take priority over UCRT.
        Provider("CLFS", ("windows", "clfsw32"), (), (
            "AddLogContainer", "AddLogContainerSet", "AdvanceLogBase",
            "AlignReservedLog", "AllocReservedLog", "CloseAndResetLogFile",
            "CreateLogContainerScanContext", "CreateLogFile", "CreateLogMarshallingArea",
            "DeleteLogByHandle", "DeleteLogFile", "DeleteLogMarshallingArea", "DumpLogRecords",
            "FlushLogBuffers", "FlushLogToLsn", "FreeReservedLog", "GetLogContainerName",
            "GetLogFileInformation", "GetLogIoStatistics", "GetLogReservationInfo",
            "GetNextLogArchiveExtent", "LsnBlockOffset", "LsnContainer", "LsnCreate",
            "LsnEqual", "LsnGreater", "LsnIncrement", "LsnInvalid", "LsnLess", "LsnNull",
            "LsnRecordSequence", "PrepareLogArchive", "ReadLogArchiveMetadata", "ReadLogRecord",
            "ReadLogRestartArea", "ReadNextLogRecord", "ReadPreviousLogRestartArea",
            "RemoveLogContainer", "RemoveLogContainerSet", "ReserveAndAppendLog",
            "ReserveAndAppendLogAligned", "ScanLogContainers", "SetEndOfLog",
            "SetLogArchiveMode", "SetLogArchiveTail", "TerminateLogArchive", "TerminateReadLog",
            "TruncateLog", "ValidateLog", "WriteLogRestartArea")),
        Provider("TPM", ("windows", "tbs")),
        Provider("DHCPServer", ("winsock2", "windows", "dhcpsapi")),
        Provider("ODBC", ("windows", "sql", "sqlext", "sqlucode")),
        Provider("MediaFoundation", ("windows", "mfapi", "mfobjects", "mfidl")),
        Provider("TraceData", ("windows", "tdh")),
        Provider("JavaScriptRuntime", ("stdbool", "windows", "jsrt")),
        Provider("RightsManagement", ("windows", "msdrm")),
        Provider("DirectoryService", ("windows", "ntdsapi")),
        Provider("XmlLite", ("windows", "xmllite")),
        Provider("UIAutomation", ("windows", "uiautomationcore", "uiautomationcoreapi")),
        Provider("WinInet", ("winsock2", "windows", "wininet", "winineti")),
        Provider("RoutingTable", ("winsock2", "windows", "rtmv2")),
        Provider("WinSNMP", ("windows", "winsnmp")),
        Provider("AudioCompression", ("windows", "mmsystem", "mmreg", "msacm")),
        Provider("CloudFiles", ("windows", "cfapi")),
        Provider("PeerDistribution", ("windows", "peerdist")),
        Provider("DistributedRouting", ("winsock2", "windows", "drt")),
        Provider("ProjectedFileSystem", ("windows", "projectedfslib")),
        Provider("ISCSI", ("winsock2", "windows", "iscsierr", "iscsidsc")),
        Provider("ClusterResources", ("windows", "clusapi", "resapi")),
        Provider("VideoForWindows", ("windows", "vfw")),
        Provider("Fax", ("windows", "winfax"), ("WIN32=1",)),
        Provider("WindowsErrorReporting", ("windows", "werapi")),
        Provider("Magnification", ("windows", "magnification")),
        Provider("GDIPlus", ("windows", "objidl", "ddraw"), ("GDIPVER=0x0110",)),
    ),
}
TARGETS = {
    "x64": ("X64", "x86_64-pc-windows-msvc", "LLP64", "_AMD64_"),
    "x86": ("X86", "i686-pc-windows-msvc", "ILP32", "_X86_"),
    "arm64": ("ARM64", "aarch64-pc-windows-msvc", "LLP64", "_ARM64_"),
}
OFFICIAL_PROVIDER = "Microsoft SDK/WDK"
COMPATIBLE_PROVIDER = "Microsoft SDK/WDK with Wine compatibility declarations"


def run(command: list[str], log: Path, output: Path | None = None) -> str:
    with log.open("w", encoding="utf-8") as errors:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=errors, timeout=wdk.LIMITS["command_timeout"])
    if result.returncode:
        # The types CLI writes structured rejection diagnostics to stdout.
        # Preserve them before raising, alongside any compiler diagnostics.
        if result.stdout:
            with log.open("a", encoding="utf-8") as errors:
                errors.write(result.stdout)
        raise ValueError(f"command failed ({result.returncode}); see {log}")
    if output:
        output.write_text(result.stdout, encoding="utf-8")
    return result.stdout


def prototype_selection_command(command: list[str], routines, response: Path) -> list[str]:
    """Pass an oversized SDK selection through the types CLI's LLVM response reader."""
    selectors = ["--import-prototype=" + routine for routine in routines]
    direct = command + selectors
    units = len(subprocess.list2cmdline(direct).encode("utf-16-le")) // 2 + 1
    if units <= wdk.LIMITS["windows_command_line_units"]:
        return direct
    # A C identifier is one token in both LLVM's GNU and Windows readers.
    # Keep path arguments on the command line so the host quotes them normally.
    if any(re.search(r"""[\s'"\\\x00]""", selector) for selector in selectors):
        raise ValueError("invalid C routine name in Microsoft prototype selection")
    response.write_text("\n".join(selectors) + "\n", encoding="utf-8")
    return command + ["@" + str(response.resolve())]


def checked_types(neverd: str, source: Path, log: Path,
                  model: str = "LLP64") -> dict:
    result = json.loads(run([neverd, "types", "--data-model", model,
                             "--record-abi", "microsoft", "--declare",
                             str(source), "--keep-going", "--json"], log))
    if result["problems"]:
        log.write_text(json.dumps(result["problems"], indent=2) + "\n",
                       encoding="utf-8")
        raise ValueError(f"official declarations were refused; see {log}")
    return result


def parser_convention(source: Path, convention: str) -> None:
    """Preprocessing loses the compiler's default on unannotated declarations."""
    if convention:
        source.write_text(f"#pragma neverd_default_calling_convention({convention})\n" +
                          source.read_text(encoding="utf-8"), encoding="utf-8")


def record_layouts(text: str) -> dict:
    """Named record sizes, alignment and direct named fields from Clang."""
    records, current = {}, None
    for line in text.splitlines():
        if line.startswith("*** Dumping"):
            current = None
        heading = re.fullmatch(r"\s*0 \| (?:struct|union) (\w+)", line)
        if heading:
            current = {"fields": {}}
            records[heading[1]] = current
        elif current is not None:
            size = re.search(r"\[sizeof=(\d+),(?: dsize=\d+,)? align=(\d+)(?:,|\])", line)
            field = re.fullmatch(r"\s*(\d+(?::\d+-\d+)?) \|   (\S.*)", line)
            if size:
                current.update(size=int(size[1]), alignment=int(size[2]))
            elif field:
                name = re.search(r"\b([A-Za-z_]\w*)(?:\[[^]]*\])*$", field[2])
                if name:
                    current["fields"][name[1]] = field[1]
    return {name: record for name, record in records.items() if "size" in record}


def verify_layouts(original: str, generated: str,
                   renamed: dict[str, str] | None = None,
                   required: list[str] | None = None) -> int:
    source, rendered = record_layouts(original), record_layouts(generated)
    renamed = renamed or {}
    selected = (source.keys() & set(required) if required is not None
                else source.keys() & rendered.keys())
    if not selected and required is None:
        raise ValueError("Clang produced no shared named record layouts")
    for name in sorted(selected):
        imported = renamed.get(name, name)
        if imported not in rendered:
            raise ValueError(f"imported record layout is missing: {name} -> {imported}")
        a, b = source[name], rendered[imported]
        if (a["size"] != b["size"] or a["alignment"] != b["alignment"] or
                any(offset != b["fields"].get(field)
                    for field, offset in a["fields"].items())):
            raise ValueError(f"official record layout changed: {name}: {a} != {b}")
    return len(selected)


# Each isolated unit also includes the compiler or SDK prerequisites. Only
# these API families belong to the provider that brought that unit in.
WDK_EXPORT_PREFIXES = {
    "StoragePortAccess": ("StorPortReadPort", "StorPortReadRegister",
                          "StorPortWritePort", "StorPortWriteRegister"),
    "TapeClass": ("TapeClass", "TapeDebugPrint"),
    "WPPRecorder": ("imp_WppRecorder",),
    "USBPower": ("UsbPm_",),
    "SystemEntropy": ("Entropy",),
    "NetworkPowerDependency": ("NpdBroker",),
    "NDISPoll": ("NdisRegisterPoll", "NdisDeregisterPoll", "NdisRequestPoll", "NdisSetPollAffinity"),
    "NDISChimney": ("NdisInitiateOffload", "NdisInvalidateOffload", "NdisMGetOffloadHandlers",
                    "NdisMInitiateOffload", "NdisMInvalidateOffload", "NdisMOffloadEvent",
                    "NdisMQueryOffload", "NdisMTerminateOffload", "NdisMUpdateOffload",
                    "NdisOffloadTcp", "NdisQueryOffload", "NdisTerminateOffload", "NdisUpdateOffload"),
    "NetworkDMA": ("NetDma",),
}
PROVIDER_PREFIXES = {
    **WDK_EXPORT_PREFIXES,
    "DHCPServer": ("Dhcp",),
    "ODBC": ("SQL", "ODBC"),
    "MediaFoundation": ("MF", "CreatePropertyStore"),
    "TraceData": ("Tdh",),
    "JavaScriptRuntime": ("Js",),
    "RightsManagement": ("DRM",),
    "DirectoryService": ("Ds",),
    "XmlLite": ("CreateXml",),
    "UIAutomation": (
        "Uia", "DockPattern_", "ExpandCollapsePattern_", "GridPattern_", "InvokePattern_",
        "ItemContainerPattern_", "LegacyIAccessiblePattern_", "MultipleViewPattern_",
        "RangeValuePattern_", "ScrollItemPattern_", "ScrollPattern_",
        "SelectionItemPattern_", "SynchronizedInputPattern_", "TextPattern_",
        "TextRange_", "TogglePattern_", "TransformPattern_", "ValuePattern_",
        "VirtualizedItemPattern_", "WindowPattern_",
    ),
    "WinInet": (
        "AppCache", "UrlCache", "Internet", "Http", "FindFirstUrlCache", "FindNextUrlCache",
        "FindCloseUrlCache", "CreateUrlCache", "CommitUrlCache", "DeleteUrlCache",
        "GetUrlCache", "SetUrlCache", "RetrieveUrlCache", "UnlockUrlCache",
        "FreeUrlCache", "IncrementUrlCache",
    ),
    "RoutingTable": ("Rtm",),
    "WinSNMP": ("Snmp",),
    "AudioCompression": ("acm",),
    "CloudFiles": ("Cf",),
    "PeerDistribution": ("PeerDist",),
    "InteractionContext": (
        "CreateInteractionContext", "DestroyInteractionContext",
        "RegisterOutputCallbackInteractionContext", "ProcessInertiaInteractionContext",
        "BufferPointerPacketsInteractionContext",
        "ProcessBufferedPacketsInteractionContext",
        "ProcessPointerFramesInteractionContext", "SetInteraction", "GetInteraction",
        "AddPointer", "RemovePointer", "StopInteraction",
    ),
    "DistributedRouting": ("Drt",),
    "ProjectedFileSystem": ("Prj",),
    "ISCSI": (
        "AddIScsi", "RemoveIScsi", "GetIScsi", "ReportIScsi", "ReportActiveIScsi",
        "SetIScsi", "SendScsi", "SetupPersistentIScsi", "LoginIScsi", "LogoutIScsi",
        "RefreshIScsi", "AddISNS", "RemoveISNS", "RefreshISNS", "SetISNS",
    ),
    "ClusterResources": (
        "ResUtil", "ClusWorker", "ClusAddCluster", "ClusRemoveCluster", "ClusGetCluster",
        "ClusterReg", "OpenClusterCryptProvider", "CloseClusterCryptProvider",
        "ClusterEncrypt", "ClusterDecrypt",
    ),
    "VideoForWindows": (
        "AVI", "DrawDib", "EditStream", "ICClose", "ICCompress", "ICDecompress", "ICDraw",
        "ICGet", "ICImage", "ICInfo", "ICInstall", "ICLocate", "ICOpen", "ICRemove",
        "ICSendMessage", "MCIWnd", "capCreate", "capGet",
    ),
    "Fax": ("Fax",),
    "WindowsErrorReporting": ("Wer",),
    "Magnification": ("Mag",),
    "WindowsSQLite": ("sqlite3_",),
    "ICU": ("u", "UCNV_"),
    "WebServices": ("Ws",),
}


# These function bodies are SDK inline wrappers. Their callers link to other
# routines. Keep them out of the external prototypes for these DLLs.
PROVIDER_INLINE_ROUTINES = {
    "ClusterResources": {"ResUtilGetMultiSzValue"},
    "VideoForWindows": {"ICDecompressEx", "ICDecompressExBegin",
                        "ICDecompressExQuery", "ICDrawSuggestFormat"},
}


# The SDK uses an empty C parameter list for these no-argument C++ exports.
# Authenticate every redeclaration against the target SDK before using (void).
CPP_NOARG_PROVIDERS = {
    "SNMPUtilities": {"SnmpSvcGetUptime": "DWORD __stdcall"},
    "WebAuthentication": {"WebAuthNGetApiVersionNumber": "DWORD WINAPI"},
    "DXGI": {"DXGIDeclareAdapterRemovalSupport": "HRESULT WINAPI"},
    "ODBC": {"ODBCGetTryWaitValue": "DWORD WINAPI"},
    "MediaFoundation": {
        "MFGetSystemTime": "MFTIME WINAPI", "MFLockPlatform": "HRESULT WINAPI",
        "MFShutdown": "HRESULT WINAPI", "MFUnlockDXGIDeviceManager": "HRESULT WINAPI",
        "MFUnlockPlatform": "HRESULT WINAPI", "MFUnregisterPlatformFromMMCSS": "HRESULT WINAPI",
    },
    "RightsManagement": {"DRMRepair": "HRESULT WINAPI"},
    "WinInet": {
        "InternetClearAllPerSiteCookieDecisions": "BOOL WINAPI",
        "UrlCacheReloadSettings": "DWORD WINAPI", "UrlCacheServer": "DWORD WINAPI",
    },
    "UIAutomation": {"UiaClientsAreListening": "BOOL WINAPI", "UiaDisconnectAllProviders": "HRESULT WINAPI"},
    "ISCSI": {"SetupPersistentIScsiDevices": "ISDSC_STATUS WINAPI",
              "SetupPersistentIScsiVolumes": "ISDSC_STATUS WINAPI"},
    "WindowsErrorReporting": {
        "WerUnregisterAppLocalDump": "HRESULT WINAPI", "WerStorePurge": "HRESULT __cdecl",
    },
    "Magnification": {"MagInitialize": "BOOL WINAPI", "MagUninitialize": "BOOL WINAPI"},
}


def prepare_provider(name: str, source: Path, arguments: list[str], directory: Path,
                     neverd: str, model: str, headers=(), defines=(), prototypes=(),
                     metadata: dict | None = None, convention: str = "",
                     gdiplus_imports: dict | None = None,
                     flat_proof: dict | None = None) -> dict:
    """Validate and select a whole provider before it can enter the merge."""
    bridge_commands = []
    noargs = CPP_NOARG_PROVIDERS.get(name)
    if noargs:
        original = source.read_text(encoding="utf-8")
        proof = directory / (name + "-noargs.cpp")
        checks = "".join(f"typedef {spec} neverd_{symbol}_prototype(void);\n"
                         f"static_assert(__is_same(decltype(&{symbol}), "
                         f"neverd_{symbol}_prototype *));\n"
                         for symbol, spec in noargs.items())
        proof.write_text(original + "\n" + checks, encoding="utf-8")
        invocation = arguments + ["-x", "c++", "-std=c++17", "-fsyntax-only", str(proof)]
        run(invocation, directory / (name + "-noargs.log"))
        declarations = "".join(f"{spec} {symbol}(void);\n"
                               for symbol, spec in noargs.items())
        if flat_proof is not None:
            flat_proof["original_source"] = str(source)
            flat_proof["original_source_sha256"] = wdk.digest(source)
            source = directory / (name + "-checked.c")
        source.write_text(original + "\n" + declarations, encoding="utf-8")
        if flat_proof is not None:
            flat_proof.update(source=str(source), source_sha256=wdk.digest(source))
        bridge_commands.append(invocation)
        evidence = {"functions": list(noargs), "source": str(proof), "sha256": wdk.digest(proof)}
        if name == "DXGI":
            evidence.update(function="DXGIDeclareAdapterRemovalSupport",
                            documentation="https://learn.microsoft.com/windows/win32/api/dxgi1_6/nf-dxgi1_6-dxgideclareadapterremovalsupport")
        metadata = dict(metadata or {}, cpp_parameter_bridge=evidence)
    cpp_bridge = None
    if name == "GDIPlus":
        if gdiplus_imports is None:
            raise ValueError("GDI+ requires the original target SDK import library")
        cpp_bridge = gdiplus.prepare(source, arguments, directory, gdiplus_imports, run)
    dump = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]
    preprocessed = directory / (name + ".h")
    command = arguments + ["-E", str(source)]
    run(command, directory / (name + ".pre.log"), preprocessed)
    if name == "RDBSS":
        cpp_bridge = rdbss.prepare(source, preprocessed, arguments, directory, run)
    parser_convention(preprocessed, convention)
    parsed = checked_types(neverd, preprocessed, directory / (name + ".parse.log"), model)
    if flat_proof is not None:
        prototypes = flat.select(flat_proof, parsed["prototypes"], directory, run)
    if cpp_bridge is not None:
        verifier = gdiplus if name == "GDIPlus" else rdbss
        verifier.verify_abis(cpp_bridge, preprocessed, arguments, directory,
                             parsed["prototypes"], run)
        if name == "GDIPlus":
            prototypes = cpp_bridge["verified_functions"]
        bridge_commands += cpp_bridge["commands"]
        metadata = dict(metadata or {}, cpp_header_bridge=cpp_bridge)
    input_hash = wdk.digest(preprocessed)
    commands = [command, *bridge_commands]
    if flat_proof is None and name in PROVIDER_PREFIXES:
        prefixes = PROVIDER_PREFIXES[name]
        prototypes = tuple(sorted(symbol for symbol in parsed["prototypes"]
                                  if symbol.startswith(prefixes) and
                                  symbol not in PROVIDER_INLINE_ROUTINES.get(name, ())))
        if not prototypes:
            raise ValueError(f"official headers declare no {name} functions")
    if prototypes:
        original = directory / (name + ".pre.h")
        preprocessed.replace(original)
        empty = directory / "empty.h"
        empty.write_text("/* Prototype selection starts without existing declarations. */\n",
                         encoding="utf-8")
        selection = [neverd, "types", "--data-model", model, "--record-abi", "microsoft",
                     "--declare", str(empty), "--declare", str(original),
                     "--merge-prototypes", "--all", "--prototypes", "--spelling", "windows",
                     "--no-offsets", "--json"]
        selection = prototype_selection_command(selection, prototypes, directory / (name + ".rsp"))
        selected = json.loads(run(selection, directory / (name + ".select.log")))
        if (selected.get("error") or selected.get("problems") or
                set(selected.get("prototypes", [])) != set(prototypes)):
            raise ValueError(f"official prototype selection failed for {name}")
        preprocessed.write_text(selected["source"], encoding="utf-8")
        commands.append(selection)
    # GDI+ certifies its C record names against the original C++ namespace
    # before using those C names for the subsequent merge layout checks.
    if flat_proof is not None:
        standalone = arguments[:arguments.index("-I")]
        flat.verify_abis(flat_proof, preprocessed, standalone, directory, prototypes, run)
        metadata = dict(metadata or {}, sdk_import_bridge=flat_proof)
        commands += [flat_proof["import_library"]["command"], *flat_proof["commands"]]
    original_profile = flat_proof or (cpp_bridge if name == "RDBSS" else None)
    layout_arguments = original_profile["arguments"] if original_profile else arguments
    layout_source = original_profile["source"] if original_profile else str(source)
    layouts = run(layout_arguments + dump + [layout_source],
                  directory / (name + ".official-clang.log"),
                  directory / (name + ".official-layouts.log"))
    return {"name": name, "source": preprocessed, "headers": headers,
            "defines": defines, "selected_prototypes": prototypes,
            "default_calling_convention": convention,
            "input_sha256": input_hash, "layouts": layouts,
            "parsed": parsed, "commands": commands, "metadata": metadata or {}}


def merge_providers(base: str, primary: Path, output: Path,
                    arguments: list[str], common: list[str], neverd: str,
                    model: str, primary_layouts: str,
                    runtime: dict | None = None,
                    compatibility: dict | None = None,
                    convention: str = "", gdiplus_imports: dict | None = None,
                    flat_units: tuple | list = ()) -> list[dict]:
    providers = ISOLATED_PROVIDERS.get(base, ())
    if not providers and runtime is None and not flat_units:
        return []
    directory = output / (base + "-providers")
    directory.mkdir(exist_ok=True)
    units = []
    dump = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]
    for name, headers, defines, prototypes in providers:
        provider_arguments = arguments + ["-D" + value for value in defines]
        source = directory / (name + ".c")
        source.write_text(include_headers(headers), encoding="utf-8")
        units.append(prepare_provider(name, source, provider_arguments, directory,
                                      neverd, model, headers, defines, prototypes,
                                      convention=convention, gdiplus_imports=gdiplus_imports))
    for profile in flat_units:
        units.append(prepare_provider(profile["name"], profile["source"], profile["compiler"],
                                      directory, neverd, model, profile["headers"],
                                      convention=profile["default_calling_convention"],
                                      flat_proof=profile["bridge"]))
    if runtime is not None:
        units.append(prepare_provider("UCRT", runtime["source"], runtime["arguments"],
                                      directory, neverd, model,
                                      prototypes=runtime["prototypes"],
                                      metadata=runtime["metadata"]))
    if compatibility is not None:
        units.append(prepare_provider("UCRTCompatibility", compatibility["source"],
                                      compatibility["arguments"], directory, neverd, model,
                                      prototypes=compatibility["prototypes"],
                                      metadata=compatibility["metadata"]))
    command = [neverd, "types", "--data-model", model, "--record-abi", "microsoft",
               "--declare", str(primary), "--merge-prototypes", "--all",
               "--prototypes", "--spelling", "windows", "--no-offsets", "--json"]
    for unit in units:
        command += ["--declare", str(unit["source"])]
    result = json.loads(run(command, directory / "merge.log", directory / "merge.json"))
    if result.get("error") or result.get("problems"):
        raise ValueError(f"official provider merge was refused; see {directory / 'merge.json'}")
    imported = result.get("imports", [])
    if len(imported) != len(units):
        raise ValueError("official provider merge did not report every input")
    with tempfile.TemporaryDirectory(dir=directory, prefix="merged-") as scratch:
        temporary = Path(scratch) / primary.name
        temporary.write_text(result["source"], encoding="utf-8")
        reread = checked_types(neverd, temporary, directory / "roundtrip.log", model)
        if (set(result["types"]) - set(reread["types"]) or
                set(result["prototypes"]) - set(reread["prototypes"])):
            raise ValueError("official provider declarations disappeared on round trip")
        layouts = directory / "merged.layouts.h"
        run([neverd, "types", "--data-model", model, "--record-abi", "microsoft",
             "--declare", str(temporary), "--all", "--asserts", "--spelling",
             "windows", "--no-offsets", "-o", str(layouts)], directory / "layouts-render.log")
        generated_layouts = run(common + dump + ["-x", "c", str(layouts)],
                                directory / "generated-clang.log",
                                directory / "generated-layouts.log")
        verify_layouts(primary_layouts, generated_layouts)
        records = []
        for index, (unit, report) in enumerate(zip(units, imported), 1):
            if report["declaration"] != index or report["prefix"] != unit["name"]:
                raise ValueError("official provider provenance does not match its input")
            flat_bridge = unit["metadata"].get("sdk_import_bridge")
            bridge = flat_bridge or unit["metadata"].get("cpp_header_bridge")
            if bridge and not set(report["prototypes"]) <= set(bridge["verified_functions"]):
                language = bridge.get("language", "c++")
                raise ValueError(f"imported {unit['name']} routines lack an original {language.upper()} ABI proof")
            if bridge:
                # Recheck the actual published declarations after parsing,
                # renaming and merging; a correct preprocessed bridge alone
                # cannot certify the parser's emitted function types.
                published = directory / (unit["name"] + "-published")
                published.mkdir(exist_ok=True)
                empty = published / "empty.h"
                empty.write_text("", encoding="utf-8")
                selected = published / "declarations.h"
                selection = [neverd, "types", "--data-model", model,
                             "--record-abi", "microsoft", "--declare", str(empty),
                             "--declare", str(temporary), "--merge-prototypes",
                             "--all", "--prototypes", "--spelling", "windows",
                             "--no-offsets", "-o", str(selected)]
                selection = prototype_selection_command(selection, bridge["verified_functions"],
                                                        published / "selection.rsp")
                run(selection, published / "selection.log")
                proof = dict(bridge, commands=[])
                verifier = flat if flat_bridge else (
                    gdiplus if unit["name"] == "GDIPlus" else rdbss)
                verifier.verify_abis(proof, selected, common, published,
                                     bridge["verified_functions"], run)
                bridge["published_abi"] = proof
                unit["commands"] += [selection, *proof["commands"]]
            renamed = {entry["original"]: entry["name"] for entry in report["renamed"]}
            count = verify_layouts(unit["layouts"], generated_layouts,
                                   renamed, report["dependencies"])
            records.append({"name": unit["name"], "headers": unit["headers"],
                            "defines": unit["defines"],
                            "default_calling_convention": unit["default_calling_convention"],
                            "selected_prototypes": unit["selected_prototypes"],
                            "input_sha256": unit["input_sha256"],
                            "declaration_sha256": wdk.digest(unit["source"]),
                            "parsed_prototypes": len(unit["parsed"]["prototypes"]),
                            "verified_records": count, "import": report,
                            "commands": unit["commands"] + [command],
                            "metadata": unit["metadata"]})
        # Replace the primary only after the complete merge and compiler
        # checks pass. The manifest remains absent until both libraries pass.
        temporary.write_text(
            "// Local declarations generated from Microsoft SDK/WDK.\n"
            "// Source packages, commands and hashes: manifest.json.\n"
            "// Original Microsoft notices: licenses/.\n" + result["source"],
            encoding="utf-8")
        if compatibility is not None:
            temporary.write_text(temporary.read_text(encoding="utf-8").replace(
                "// Source packages, commands and hashes: manifest.json.",
                "// Missing UCRT declarations supplemented from Wine; notices: licenses/.\n"
                "// Source packages, commands and hashes: manifest.json."), encoding="utf-8")
        temporary.replace(primary)
    return records


def include_headers(headers: tuple[str, ...]) -> str:
    lines = []
    for header in headers:
        if header == "ntsecapi":
            # winternl already supplied NTSTATUS, STRING and UNICODE_STRING.
            # Use ntsecapi's own guard against its alternative LSA aliases;
            # restore the macro before any subsequent provider is read.
            lines += ['#pragma push_macro("_NTDEF_")', '#define _NTDEF_ 1']
        lines.append(f"#include <{header}.h>")
        if header == "ntsecapi":
            lines.append('#pragma pop_macro("_NTDEF_")')
    return "\n".join(lines) + "\n"


def compiler_defaults(kit: Path, architecture: str, library: str) -> tuple[list[str], str]:
    """Keep the WDK's compiler default, which preprocessing alone discards."""
    if architecture != "x86" or library != "WindowsKernel":
        return [], ""
    source = kit / f"c/build/{VERSION}/WindowsDriver.Common.props"
    properties = ET.parse(source).getroot()
    conventions = [item.text for item in properties.findall(
        "./{*}ItemDefinitionGroup/{*}ClCompile/{*}CallingConvention")]
    if conventions != ["StdCall"]:
        raise ValueError(f"unrecognized WDK default calling convention in {source}")
    return ["-Xclang", "-fdefault-calling-conv=stdcall"], "__stdcall"


def combine_targets(output: Path, directories: list[Path]) -> None:
    """Publish all target libraries only after every input has been verified."""
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    manifest.unlink(missing_ok=True)
    targets, libraries, target_packages, seen = [], [], [], set()
    common = None
    for directory in directories:
        target = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        arch = target.get("architecture")
        if (target.get("schema") != 1 or target.get("provider") not in (OFFICIAL_PROVIDER, COMPATIBLE_PROVIDER)
                or arch not in TARGETS or arch in seen):
            raise ValueError("invalid or duplicate Microsoft target manifest")
        seen.add(arch)
        expected = {base + TARGETS[arch][0] for base in LIBRARIES}
        actual = [library["name"] for library in target["libraries"]]
        if len(actual) != len(expected) or set(actual) != expected:
            raise ValueError(f"incomplete Microsoft target libraries: {arch}")
        identity = {key: target[key] for key in ["provider", "compiler", "packages", "licenses"]}
        identity["profiles"] = target.get("profiles", {})
        if common is not None and identity != common:
            raise ValueError("Microsoft targets use different compilers, packages or licenses")
        common = identity
        for package in target.get("target_packages", []):
            if (package.get("architecture") != arch or
                    any(package["file"] == other["file"] for other in target_packages)):
                raise ValueError("invalid architecture-specific Microsoft package")
            target_packages.append(package)
        for library in target["libraries"]:
            header = directory / (library["name"] + ".h")
            if wdk.digest(header) != library["sha256"]:
                raise ValueError(f"changed Microsoft target header: {header}")
            libraries.append(dict(library, architecture=arch))
        for notice in target["licenses"]:
            name = notice["file"]
            if Path(name).name != name or wdk.digest(directory / "licenses" / name) != notice["sha256"]:
                raise ValueError("changed Microsoft target license")
        targets.append((directory, target))
    if seen != set(TARGETS):
        raise ValueError("all x86, x64 and ARM64 Microsoft target libraries are required")
    for directory, target in targets:
        for library in target["libraries"]:
            name = library["name"] + ".h"
            shutil.copy2(directory / name, output / name)
    (output / "licenses").mkdir(exist_ok=True)
    for notice in common["licenses"]:
        shutil.copy2(targets[0][0] / "licenses" / notice["file"],
                     output / "licenses" / notice["file"])
    record = dict(common, schema=1,
                  architecture="all", libraries=libraries, target_packages=target_packages,
                  targets=[target for _, target in targets])
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def generate(output: Path, cache: Path, neverd: str, clang: str,
             architecture: str = "arm64", *,
             include_ucrt: bool = False, readobj: str = "llvm-readobj",
             ucrt_compatibility: bool = False, legacy_sdk: bool = False,
             legacy_profile: dict | None = None) -> None:
    if ucrt_compatibility and not include_ucrt:
        raise ValueError("UCRT compatibility requires the official UCRT provider")
    if legacy_sdk and legacy_profile is None:
        output.mkdir(parents=True, exist_ok=True)
        cache.mkdir(parents=True, exist_ok=True)
        (output / "manifest.json").unlink(missing_ok=True)
        legacy_profile = flat.legacy_headers(output, cache)
    if architecture == "all":
        output.mkdir(parents=True, exist_ok=True)
        (output / "manifest.json").unlink(missing_ok=True)
        directories = []
        for arch in TARGETS:
            directory = output / arch
            generate(directory, cache, neverd, clang, arch,
                     include_ucrt=include_ucrt, readobj=readobj,
                     ucrt_compatibility=ucrt_compatibility, legacy_sdk=legacy_sdk,
                     legacy_profile=legacy_profile)
            directories.append(directory)
        combine_targets(output, directories)
        return
    suffix, triple, model, macro = TARGETS[architecture]
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    # A failed update must never certify earlier or partially replaced inputs.
    manifest.unlink(missing_ok=True)
    inventory = wdk.declarations()
    packages = [wdk.download(cache, *record) for record in inventory["DOWNLOAD"]]
    kit = output / "microsoft-kit"
    selected = [f"c/Include/{VERSION}/{part}/"
                for part in ("km", "shared", "um", "winrt")]
    selected += ["LICENSE.txt", "Microsoft.Windows.WDK.x64.nuspec",
                 "Microsoft.Windows.SDK.CPP.nuspec",
                 f"c/build/{VERSION}/WindowsDriver.Common.props"]
    inputs = wdk.extract(packages, kit, selected)
    licenses = output / "licenses"
    licenses.mkdir(exist_ok=True)
    (licenses / "WDK-LICENSE.txt").write_bytes((kit / "LICENSE.txt").read_bytes())
    for record in inventory["LICENSE"]:
        source = wdk.download(cache, *record)
        (licenses / source.name).write_bytes(source.read_bytes())
    overlay = output / "headers-vfs.json"
    overlay.write_text(json.dumps({
        "version": 0, "case-sensitive": False,
        "roots": [wdk.overlay_directory(kit / "c/Include", True)],
    }, indent=2) + "\n", encoding="utf-8")
    include = kit / f"c/Include/{VERSION}"
    # The WDK package's x64 name identifies its host tools/libraries. Its
    # architecture-conditional headers are compiled for the requested target;
    # no host architecture definition or structure is substituted.
    common = [clang, "--target=" + triple, "-ffreestanding",
              "-fms-extensions", "-fms-compatibility", "-Wno-invalid-token-paste",
              "-Wno-ignored-pragma-intrinsic", "-Wno-ignored-attributes",
              "-Wno-nonportable-include-path", "-Wno-pragma-pack",
              "-Wno-microsoft-anon-tag", "-D" + macro + "=1",
              "-D_WIN32_WINNT=0x0A00", "-DNTDDI_VERSION=0x0A00000C",
              "-ivfsoverlay", str(overlay)]
    built, runtime_packages, target_packages = [], [], []
    for base, (headers, directories, defines) in LIBRARIES.items():
        name = base + suffix
        source = output / (name + ".c")
        source.write_text(include_headers(headers), encoding="utf-8")
        defaults, convention = compiler_defaults(kit, architecture, base)
        arguments = common + defines + defaults + [item for part in directories
                                        for item in ("-I", str(include / part))]
        preprocessed = output / (name + ".pre.c")
        command = arguments + ["-E", str(source)]
        run(command, output / (name + ".pre.log"), preprocessed)
        parser_convention(preprocessed, convention)
        read = checked_types(neverd, preprocessed, output / (name + ".parse.log"), model)
        generated = output / (name + ".h")
        with tempfile.TemporaryDirectory(dir=output, prefix="render-") as scratch:
            temporary = Path(scratch) / generated.name
            render = [neverd, "types", "--data-model", model, "--record-abi",
                      "microsoft", "--declare", str(preprocessed), "--all",
                      "--prototypes", "--spelling", "windows", "--no-offsets",
                      "-o", str(temporary)]
            run(render, output / (name + ".render.log"))
            body = temporary.read_text(encoding="utf-8")
            temporary.write_text(
                "// Local declarations generated from Microsoft SDK/WDK.\n"
                "// Source packages, commands and hashes: manifest.json.\n"
                "// Original Microsoft notices: licenses/.\n" + body,
                encoding="utf-8")
            reread = checked_types(neverd, temporary, output / (name + ".roundtrip.log"), model)
            if (set(read["types"]) - set(reread["types"]) or
                    set(read["prototypes"]) - set(reread["prototypes"])):
                raise ValueError(f"declarations disappeared while rendering {name}")
            # Independently compare the actual Microsoft headers with the
            # emitted type declarations under the target compiler ABI. Function
            # prototypes are excluded: target CRT builtin spellings need not
            # equal the renderer's fixed-width aliases.
            layouts = output / (name + ".layouts.h")
            run([neverd, "types", "--data-model", model, "--record-abi",
                 "microsoft", "--declare", str(temporary), "--all", "--asserts",
                 "--spelling", "windows", "--no-offsets", "-o", str(layouts)],
                output / (name + ".layouts-render.log"))
            dump = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]
            original_layouts = run(arguments + dump + [str(source)],
                                   output / (name + ".official-clang.log"),
                                   output / (name + ".official-layouts.log"))
            generated_layouts = run(common + dump + ["-x", "c", str(layouts)],
                                    output / (name + ".generated-clang.log"),
                                    output / (name + ".generated-layouts.log"))
            count = verify_layouts(original_layouts, generated_layouts)
            temporary.replace(generated)
        runtime, compatibility = None, None
        if include_ucrt and base == "WindowsUser":
            sdk = next(path for path in packages if path.name.startswith("microsoft.windows.sdk.cpp."))
            runtime = ucrt.prepare(output, cache, sdk, include, common, architecture,
                                   VERSION, readobj, neverd, model, run, checked_types,
                                   verify_layouts)
            runtime_packages = runtime["packages"]
            target_packages = runtime["target_packages"]
            if ucrt_compatibility:
                compatibility = ucrt.prepare_compatibility(runtime, output, cache)
        gdiplus_imports, flat_units = None, []
        if base == "WindowsUser":
            gdiplus_imports = gdiplus.import_library(output, cache, architecture, readobj)
            if gdiplus_imports["package"] not in target_packages:
                target_packages.append(gdiplus_imports["package"])
            flat_units, archives = flat.source_units(output, cache, architecture, readobj,
                                                     arguments, legacy_profile)
            target_packages.extend(item for item in archives if item not in target_packages)
            if legacy_profile:
                runtime_packages.extend(item for item in legacy_profile["packages"]
                                        if item not in runtime_packages)
                for notice in legacy_profile["notices"]:
                    shutil.copy2(notice, licenses / notice.name)
        providers = merge_providers(base, generated, output, arguments, common,
                                     neverd, model, original_layouts, runtime, compatibility,
                                     convention, gdiplus_imports, flat_units)
        final = checked_types(neverd, generated, output / (name + ".final.log"), model)
        built.append({"name": name, "sha256": wdk.digest(generated),
                      "types": len(final["types"]),
                      "prototypes": len(final["prototypes"]),
                      "verified_records": count, "providers": providers,
                      "default_calling_convention": convention,
                      "commands": [command, render]})
        print(f"Generated {name}: {len(final['types'])} types, "
              f"{len(final['prototypes'])} prototypes", flush=True)
    record = {
        "provider": COMPATIBLE_PROVIDER if ucrt_compatibility else OFFICIAL_PROVIDER,
        "architecture": architecture,
        "schema": 1, "libraries": built,
        "compiler": subprocess.check_output([clang, "--version"], text=True),
        "packages": [{"file": name, "url": url, "sha256": digest}
                     for name, url, digest in inventory["DOWNLOAD"]] + runtime_packages,
        "target_packages": target_packages,
        "profiles": dict({"ucrt": wdk.digest(ucrt.INPUTS)} if include_ucrt else {},
                         wdk_exports=wdk.digest(Path(__file__)),
                         com_registration=wdk.digest(include / "um" / "olectl.h"),
                         rdbss=wdk.digest(Path(rdbss.__file__)),
                         gdiplus=wdk.digest(Path(gdiplus.__file__)),
                         flat_imports=wdk.digest(Path(flat.__file__)),
                         **({"legacy_sdk": wdk.digest(flat.LEGACY_INPUTS)} if legacy_sdk else {})),
        "inputs": inputs,
        "licenses": [{"file": path.name, "sha256": wdk.digest(path)}
                     for path in sorted(licenses.iterdir())],
    }
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--ucrt", action="store_true",
                        help="include exact UCRT exports from pinned SDK/VC inputs")
    parser.add_argument("--legacy-sdk", action="store_true",
                        help="include retired AllJoyn and Peer APIs from Microsoft's archived SDK 22621")
    parser.add_argument("--ucrt-compatibility", action="store_true",
                        help="supplement missing UCRT declarations from pinned Wine sources (requires --ucrt)")
    parser.add_argument("--llvm-readobj", default="llvm-readobj",
                        help="COFF import reader with explicit Export name support")
    parser.add_argument("--architecture", choices=[*TARGETS, "all"], default="all")
    arguments = parser.parse_args()
    generate(arguments.output.resolve(), arguments.cache.resolve(),
             str(Path(arguments.neverd).resolve()), arguments.clang,
             arguments.architecture, include_ucrt=arguments.ucrt,
             readobj=arguments.llvm_readobj, ucrt_compatibility=arguments.ucrt_compatibility,
             legacy_sdk=arguments.legacy_sdk)


if __name__ == "__main__":
    main()
