# Local Microsoft Windows type libraries

Windows x86, x64 and ARM64 user and kernel libraries can be generated from Microsoft's
SDK/WDK, with the bundled ReactOS, mingw-w64 and phnt declarations supplementing
missing names. Microsoft's declarations win every name collision. Program debug
types and explicit user types still take precedence over either system source.
That priority also applies when a typedef and a function share a spelling:
they occupy the same C namespace. A compatibility prototype cannot replace an
official typedef, and a rejected alias cannot remain as a dangling pointer or
callback dependency. Plain record tags may still coexist with function names.

Microsoft's unqualified `__int8` retains plain `char` identity; explicitly
signed and unsigned forms retain their distinct types. Regenerate libraries
produced by an older parser before rebuilding: changing the parser does not
repair an already-rendered `signed char` declaration in a stored library.

Generate the libraries outside the repository using an already-built NeverD:

```sh
python3 scripts/generate_microsoft_windows_types.py \
  --architecture all \
  --cache /path/to/package-cache --output /path/to/microsoft-windows \
  --neverd build-release/bin/neverd
cmake -S . -B build-release \
  -DNEVERD_WINDOWS_TYPES_DIRECTORY=/path/to/microsoft-windows
python3 scripts/build.py build-release --target neverd neverd_shared --parallel
```

The default `--architecture all` produces six libraries and a combined manifest.
Use `--architecture x86`, `x64` or `arm64` to replace only that target's two
libraries. The combined manifest requires all three targets, matching source
packages and compiler versions, and unchanged header and license hashes.

The generator verifies the official NuGet package hashes already owned by
`scripts/WDKDriverFixtures.def`. It preprocesses architecture-conditional headers
for `i686-pc-windows-msvc`, `x86_64-pc-windows-msvc` or
`aarch64-pc-windows-msvc`, with the corresponding target macro and data model.
The WDK package's `x64` host-tool label does not select AMD64 declarations. It
uses Microsoft's CRT headers, never the host CRT.
The selected kernel and user providers are listed in the generator. These are
not an inventory of every Windows export, every SDK header or every OS version.

Add `--ucrt` to include the target's UCRT declarations. The additional Microsoft
VC headers, SDK import libraries and license inputs are pinned in
`scripts/MicrosoftUCRTInputs.json`. `llvm-readobj --coff-imports` must expose
explicit export names: decorated linker symbols do not establish the API name.
The three `ucrt`, `ucrt.osmode` and `ucrt.osmode_permissive` import libraries,
plus the actual `ucrtbase.dll` redistributed in the pinned SDK, select
declarations from the official headers, including the documented
[`_o_` OS-state counterparts](https://learn.microsoft.com/cpp/c-runtime-library/global-state).
An alias is added only when that exact export exists and its base declaration
is available. Compiler intrinsics without a matching export are excluded.
The DLL inventory considers named RVAs inside executable section extents,
recording other exports separately. For ARM64X it reads only the default
[native ARM64 view](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe),
excluding LLVM's transformed ARM64EC view. Coverage refers to these pinned
inputs, not every UCRT DLL version; an export does not establish its ABI.

The C++ header bridge extracts only reviewed C-linkage declarations and trivial,
standard-layout records. Clang checks both original C++ records against the C
bridge for each target. It also compares the compiler's LLVM declarations for
every bridged function, retaining symbol decoration, calling convention and
parameter/return ABI attributes. EH/RTTI pointers to compiler-owned types stay
opaque; C++ methods and unverified record layouts are not converted into C APIs.
A small set of declarations absent from the headers
comes from Microsoft's SDK-shipped UCRT sources or explicit Microsoft Learn
prototypes; the manifest records the source hash, line and declaration. It
does not copy implementation bodies into the generated header.

Add `--ucrt-compatibility` together with `--ucrt` for reviewed declarations
that Microsoft has not made available in these inputs. This separate provider
currently reads pinned Wine sources for `_except1` and
`__std_type_info_destroy_list`, selecting only still-unresolved exports and
their verified `_o_` counterparts. Microsoft's declarations retain priority.
The original source notices, license and revision are retained locally, and
the generated header and manifest identify the supplement explicitly. The
combined manifest rejects mismatched profiles, provider policies and target
packages. Unresolved exports remain recorded; a name alone never supplies a
guessed calling convention or parameter list.

Use `scripts/check_windows_ucrt_calls.py --generated /path/to/microsoft-windows
--output /path/to/results --neverd build-release/bin/neverd` to build original
import-only fixtures from the retained generation inputs. It checks HighC and
LLVM-C against the original declarations on x86, x64 and ARM64, including
OS-state aliases, record arguments/results, callbacks and the compatibility
declarations. `--architecture` and `--function` restrict a regression run.
The JSON report retains commands and input hashes. A successful compile checks
declarations and argument shape; it does not certify native execution, and
declaration mismatches must be distinguished from lost machine values.
The full probe currently records open failures for x86 floating calls, ARM64
HFA records and several source qualifiers, tags and variadic signatures. It is
a gap audit, not an all-green coverage certificate. Force-including the original
headers can also expose duplicate emitted definitions; those diagnostics do not
by themselves prove a calling-convention mismatch.

The x86 kernel unit also reads the WDK's `WindowsDriver.Common.props` and
preserves its default `StdCall` compiler setting. Preprocessing cannot encode
that setting in declarations that omit an explicit convention. The generator
therefore supplies `#pragma neverd_default_calling_convention(__stdcall)` to
NeverD's parser and the matching compiler option to the independent Clang
check. Explicit conventions take precedence, variadic functions use `cdecl`,
and rendered callbacks and prototypes carry their conventions explicitly.
An empty pragma resets the source default; it never leaks into another parse.
User libraries and 64-bit targets retain their own defaults.
Independent kernel provider units receive the same default before parsing or
prototype selection, including their callback typedefs. Microsoft's legacy
`_declspec` and `_inline` spellings are also accepted; malformed attributes
still reject the declaration.

WinHTTP and HTTP Server are separate official declaration units. WinHTTP and
WinINet can use the same `URL_COMPONENTS` name for different string pointees;
equal record sizes do not make these types interchangeable. The generator
imports only missing prototypes and their dependency closure from each unit.
Identical types are shared, while conflicting types and their affected
dependents receive a provider prefix. This includes callbacks, arrays,
anonymous records and recursive references. Existing prototypes retain
priority. Provider parse errors prevent that entire provider from joining.

Additional independent units cover ESENT, Configuration Manager and device
installation, DNS, Windows Installer, failover clustering, TAPI/TSPI, and
OpenGL/GLU. Each unit declares its own preprocessor settings. TAPI uses
`WIN32=1`, as Microsoft's metadata partition does, because its headers test
that macro's value. Both preprocessing and the independent compiler check use
the same settings, which are recorded in the manifest.

Kernel units additionally cover NMR/WSK, Kernel Streaming, storage class, TDI,
VideoPort, ATA, battery, HID port and SmartCard declarations. User units add
ETW providers, PathCch, WinRT strings, RPC proxy/marshalling, credentials,
device queries, application model/features, property variants, performance
data and biometrics. Inclusion of a header does not establish coverage of
private exports or other Windows versions.

`scripts/check_microsoft_provider_abis.py --generated /path/to/microsoft-windows
--output /path/to/abi-results --neverd build-release/bin/neverd` compares
representative functions from those 19 units against the original compiler
declarations on all three targets. It retains decoration, calling convention,
parameter/return attributes and by-value record alignment, excluding only DLL
visibility. Each published prototype is selected with its complete dependent
types; unrelated compiler intrinsics are not compiled as replacement builtins.

`scripts/check_windows_provider_calls.py --generated /path/to/microsoft-windows
--cache /path/to/package-cache --output /path/to/call-results
--neverd build-release/bin/neverd` builds import-only ETW, PathCch and WinRT
fixtures with the retained SDK headers and pinned target import libraries.
Both C routes must compile, retain each API call without unknown operands,
and match its original compiler ABI. These checks do not execute the Windows
API or a driver. The generation must include `--ucrt` to retain the target
SDK package identities used by this check.

DNS also has a Unicode profile. Its official `PDNS_RECORD` alias changes with
`UNICODE`, including in declarations with an explicit `_W` name. An exact
selection imports the reviewed Unicode entry points first; the ANSI profile
then supplies the remaining declarations with its own type identities. The
selection is explicit, not a rule that guesses encoding from arbitrary export
names. It covers the four record-taking Unicode interfaces, `DnsQueryEx`, and
`DnsServiceBrowse`. See Microsoft's [DnsQuery documentation](https://learn.microsoft.com/windows/win32/api/windns/nf-windns-dnsquery_w)
and [asynchronous DNS sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/DNSAsyncQuery).

The standalone `neverd types --merge-prototypes` command exposes this operation
through the C API. Its first `--declare` file is the primary library; subsequent
files parse independently, with their file stems used as prefixes. The JSON
reply and generation manifest record imported prototypes, original dependency
names and renames. The generator checks required provider records against the
original Clang layouts under those imported names. Unused provider records do
not count toward this check. All merging happens during generation, without
adding a provider merge to the decompilation path.
With exactly two files, repeat `--import-prototype=Name` to select prototypes
from the second provider. The C API's per-declaration `importPrototypes` array
offers the same selection with `importPrefix`; missing names fail before any
mutation, an empty array imports nothing, and omission imports all missing
prototypes. Selection retains the complete dependent types and callbacks.
Forward-declared structs and unions are reported as declarations and survive
the round-trip inventory. Their unknown record size remains unknown; pointer
parameters retain the target's pointer width. These opaque records are not
counted as verified complete layouts.

The decompiler preserves calling conventions on callback parameters and
callback return values. Both C emitters share the declarator formatter; a
stdcall callback returned by a cdecl function must not turn the outer function
into stdcall. PDB callback types use the same source convention identity.
An unsupported callback convention is not substituted with the ordinary C ABI.
Callback pointees retain borrowed record identities without recursively
reconstructing their members. For admitted portable prototypes, source
declarations and call conversions share parameter spellings from the same
type graph, preserving nested qualifiers even though the machine carrier
does not encode them. The admitted x86 `__cdecl` and `__stdcall` keywords
retain nested callback qualifiers without requiring an SDK typedef.
Recursive record back references also remain separate borrowed identities,
so releasing an analysis does not retain a shared ownership cycle. Member
access can still complete those identities from the selected type library.

Legacy Windows/kernel and ISO `swprintf` declarations have different fixed
prefixes. A PE image alone cannot select between them: name-only variadic and
`va_list` forwarding rules decline that ambiguity, leaving the selected
declaration authoritative. Underscored bounded formatters retain their capacity
argument, while count-only `_scprintf`/`_scwprintf` take only the format prefix.

Generation rejects parser errors, missing declarations on round trip and named
record layout differences against Clang reading the original Microsoft headers.
The emitted records also compile with size and offset assertions. Fixed-width
`__ptr32` and `__ptr64` pointers survive C/C++ round trips; Rust, Go and Python
represent foreign-width addresses as integers. These checks establish type
layouts, not native ARM64 driver execution or ARM64EC/ARM64X qualification.
Microsoft required alignment survives intermediate records, unions and arrays,
including a later packed parent and a rendered declaration round trip.
Microsoft C anonymous members include references to existing struct/union tags
(`struct Header;` inside another record), in addition to inline definitions and
typedef names. These retain their storage and promoted members; incomplete
anonymous record fields are refused. C++ and non-Microsoft C retain their own
tag declaration behavior. See Microsoft's [anonymous class types](https://learn.microsoft.com/cpp/cpp/anonymous-class-types)
and Clang's [language extensions](https://clang.llvm.org/docs/LanguageExtensions.html).

The primary library and its compatibility supplement are embedded at build
time and parsed once on first use. There is no SDK directory scan during
analysis. `mergeHeaderTypes` supplies only missing names, leaves records with
incompatible by-value dependencies opaque, and omits prototypes whose named
dependencies or by-value size/alignment no longer agree. Official public PEB
and TEB definitions retain their reserved fields; private supplements do not
replace them with larger definitions.

The build validates each generated header's hash against `manifest.json` and
records the compatibility headers' hashes in `neverd-windows-types-inputs.json`.
The build receipt records that file's hash. Package, input header, compiler,
command and license information remains available in the manifest. Original
Microsoft headers and generated declarations are local inputs under Microsoft's
terms and must not be committed as redistributable compatibility headers.

Without `NEVERD_WINDOWS_TYPES_DIRECTORY`, builds use the bundled compatibility
libraries. A single-target manifest changes only its two libraries; a combined
manifest changes all six. Other targets retain their existing sources.

Focused checks:

```sh
python3 -m unittest discover -s scripts/tests -p 'test_generate*windows_types.py'
python3 scripts/build.py build-release --target NeverDTypesTests \
  NeverDSessionCAPITests NeverDAAPCS64CallContractTests --parallel
build-release/bin/NeverDTypesTests
build-release/bin/NeverDSessionCAPITests --gtest_filter='SessionCAPITest.*Imports*'
build-release/bin/NeverDAAPCS64CallContractTests
```

Run the type suite both with and without the optional official input. The
public ARM64 driver fixture exercises the NDIS import contracts through both
HighC and LLVMC. Export names alone never supply parameter types.
