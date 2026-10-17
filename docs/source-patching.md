**Languages**: [English](source-patching.md) | [简体中文](zh-CN/source-patching.md) | [繁體中文](zh-TW/source-patching.md) | [日本語](ja/source-patching.md) | [한국어](ko/source-patching.md) | [Français](fr/source-patching.md) | [Deutsch](de/source-patching.md) | [Español](es/source-patching.md) | [Italiano](it/source-patching.md) | [Русский](ru/source-patching.md) | [العربية](ar/source-patching.md)

[← Documentation Index](README.md)

# Source and native object patches

NeverD compiles a replacement with the selected compiler and installs it at an explicit original function entry. The compiler owns C/C++ semantics; NeverD owns address binding, relocation, storage, unwind records and publication. Binary repair and lifted rewrites retain their own pipeline. The rest of the program is not rebuilt.

<!-- i18n-section: compiler -->
## Compiler selection

Choose `--compiler=clang`, `--compiler=gcc` or `--compiler=neverc`. Clang and ordinary NeverC can produce LLVM IR; the `object` artifact preserves native producer instructions. The CLI selects objects for GCC. The Python SDK also selects objects for GCC. A selected compiler never silently falls back.

Use `--compiler-arg` or SDK `compiler_args` for includes, macro definitions and supported source options. Target/output overrides and response-file tokens (`@file`) are refused in all positions, including separate option values. Ordinary `-I` paths and `-D` definitions remain supported.

Use authenticated entries for the example addresses. The replacement declaration must match the original machine ABI, including calling convention and structure layouts; an address cannot prove that declaration. C++ needs `--language=c++` and the correct Clang++/G++ executable. Prefer an `extern "C"` entry or pass the exact mangled symbol. Extra helpers are allowed. Both `--entry` and `--func` select the replacement explicitly. The legacy `--from-c` address is also honored.

You can also select `artifact="object"` explicitly.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Verified profiles and limits

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C and hosted DynCode**

NeverC accepts pure C source on x64/ARM64 ELF, PE and Mach-O. NeverD rejects NeverC with `--language=c++` before starting the compiler; it does not translate C++ or choose another compiler implicitly. Select Clang for C++ and exception replacements, or a matching GCC driver for the verified ELF object profile. NeverC's hosted C frames can carry an exception thrown and caught by the surrounding C++ image; this requires real unwind records and does not add C++ language support to NeverC.

`dyncode`: `--compile-mode=dyncode` selects NeverC’s `-fdyncode-hosted` profile. It requires source, one explicit entry, a native object and no original bindings. CLI and Python default to the object artifact for this mode and GCC. The adapter verifies the kept object and final payload digests; the installer compares every linked code byte and the native entry offset before publishing. Real compiler CFI/SEH or complete native compact records provide unwind coverage. It rejects mutable/TLS globals, implicit syscall/PEB/heap/string runtime lowering, unresolved imports and changed/reordered code. Data and callback pointers can be passed as parameters.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

The Clang LLVM-IR route also admits Windows ARM64 C++ FH3, checking compiler-owned xdata, parent-frame ABI and exact funclet/FuncInfo groups. Native object language handlers remain unsupported. `check_source_patch_cpp_runtime.py` prepares x64/ARM64 images and runs nine patched exception scenarios on matching native Windows hosts; CPU execution and OS unwinder qualification remain separate evidence.

| Producer | Artifact | Verified OS runtime profile |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | Native object | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | Native object | ELF x64/ARM64 C/C++ with a compatible Itanium runtime; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 ordinary mode | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 ordinary mode | Native object | ELF x64 C |
| NeverC 3389.1.5 ordinary C | LLVM IR / native object | ELF / PE / Mach-O x64/ARM64: C source |
| NeverC 3389.1.5 `hosted-v1` | Native object + sealed payload | ELF / PE / Mach-O x64/ARM64: C source and exception propagation through C frames |

These are checked OS runtime profiles, not promises for every compiler version or option. The native installers accept all four architectures in ELF, PE and Mach-O. GCC requires a matching ELF driver and cannot produce LLVM IR. Unknown relocations, TLS, load-time global constructors, LTO, common/IFUNC symbols and unresolved imports fail clearly. The JSON receipt records compiler identity, arguments, bindings and installed addresses; native objects also record SHA-256.

The `target` field of a native object receipt comes from the loaded image ABI, including ARM ELF floating-point flags and the distinct iOS/watchOS platforms. Compiler-produced and supplied objects use the same identity rule.

The CPU matrix requires successful source, supplied-IR and native-object installation on all twelve combinations. It also covers ARMv7k watchOS and ARMv7 iOS separately, mutable/readonly state, original floating-point and stack-argument bindings, and entry execution after image rebasing. CPU execution does not certify an OS loader, C++ unwinder or arbitrary ABI; use the OS profiles above for those claims.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` means source/IR/object installation, reload and exact function execution. ARM ELF selects EABI5, ARM/Thumb state and the floating-point ABI from the image and registers a complete sorted `EHABI` index. PE preserves x64/ARM runtime rows and `HIGHLOW`/`DIR64`/`MOV32T` base relocations; x86 uses compiler registration code. Mach-O selects the image's CPU subtype and preserves the distinct watchOS DWARF and iOS `SjLj` contracts. Source ARM/Thumb entry branches are relative and survive rebasing.

Native objects use their own bounded function symbols, auxiliary/section definitions and unwind records; they never receive invented MC owner IDs or an IR recompilation. COFF function extents require exact auxiliary sizes, singleton function sections (`-ffunction-sections`), or decoded native runtime-function rows; the next symbol is never used to guess a size. The watchOS compiler adapter retains leaf frames to obtain real unwind rows. Native COFF/Mach-O language handlers remain outside the verified object profile; use the verified IR route for C++ exceptions. A statically linked ARM unwinder with a baked exidx registry is refused, as are unregistered absolute pointers in Mach-O and unsupported RELRO storage.

GCC C objects were also installed and executed on ELF x86, ARM32 and ARM64 through Linux QEMU user mode. The adapter selects ARM/Thumb from the image, and GNU property records use the ELF pointer-width alignment.

<!-- i18n-section: references -->
## Original callbacks, layout growth and dependencies

Bindings name external declarations. `function` authenticates an original function or import stub; `data` requires writable storage; `const` requires a const IR declaration. Object data bindings need a nonzero extent. Extents cannot cross storage boundaries or grant writes to RELRO. Use `original` explicitly to call the saved body of a function being replaced. The public entry still invokes the replacement; the saved entry executes a relocated whole-instruction prefix and continues in the original body. Source, supplied IR and native objects share this contract, including floating-point and stack arguments and multiple callbacks.

Callbacks require authenticated function bounds, instruction mode, continuation and unwind evidence. Unknown instructions, unsafe PC-relative references, branches into the copied prefix, unsupported loader relocations, overlapping callbacks and unmodeled handler-bearing prologues fail before publication. A matching declaration remains the caller's responsibility. The receipt records `original_callbacks` and the saved entry addresses.

PE32/PE32+ can grow a full section-header table by moving file-backed ranges and updating the modeled debug, symbol, certificate and overlay pointers while preserving original RVAs. Mach-O x64/ARM64 PIE images can obtain command and unwind storage from a checked header-prefix extension when an adjacent page-zero guard and terminal linkedit segment permit it; original code/data addresses remain fixed. This profile does not require `-Wl,-headerpad,0x400` or adjacent spare unwind pages. Explicit header-address references, unmodeled load commands, mixed code/data ownership, malformed ranges and unsupported layouts are refused. Header space and unwind capacity are planned together before installation.

New code stays RX and mutable state stays RW. Explicit `--dependency` requests add function or data imports through ELF dynamic metadata, PE import descriptors/IAT, or Mach-O dylib/bind metadata. Existing import identities, ordinals and version records remain stable; slots and callable stubs keep distinct meanings. Choose the library name/path expected by the target OS loader. The example below uses an ELF shared-library name. Missing libraries or symbols produce an actual loader error at execution. TLS, IFUNC, new symbol-version contracts, load-time constructors and unmodeled relocation forms remain unsupported; ELF `.data.rel.ro` and CET/IBT still require additional contracts. For native COFF objects, declare new function/data imports with `__declspec(dllimport)` so their relocations identify the IAT. Plain direct external references are refused.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

The append-only, size-gated dependency options apply to source, supplied IR and native-object requests. Receipts include `added_imports`; all callbacks, imports, storage permissions and installed bytes are checked again after reloading the candidate. A failure preserves the input and any existing destination.

<!-- i18n-section: examples -->
## Runnable examples

[Complete Windows/macOS/Linux examples](../examples/source-patch/README.md) include the original, replacement and dependency library sources, build commands, and a real hosted Python SDK plugin. Run from the repository root with a Python-enabled NeverD build; use a new output directory.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

Both paths run by default; add `--api cli` or `--api sdk` to choose one. The target address is discovered automatically. With input `7`, the original prints `8` and both patches print `36`. The driver checks execution, receipts and actual missing-library failures; full build and patch commands are saved in `commands.json`.

<!-- i18n-section: exceptions -->
## C++ exceptions

ELF uses compiler FDE/LSDA data; Mach-O uses compact unwind/DWARF. PE x64/ARM64 admits compiler FH3 tables with a compatible existing MSVC runtime and type identities. Type-info data may need explicit bindings. FH4, custom personalities, SEH source and active CFG/EHCont/XFG modes are refused; original protection flags are preserved.

ELF x86/x64 originals with IBT requirements are currently refused: the source installer does not yet provide an ENDBR-preserving trampoline.

Runtime cases cover normal return, internal catch, a new throw caught by an original caller, an original callee throwing through the replacement, exactly-once RAII cleanup, rethrow, noexcept termination and untouched original functions. The original caller must already allow propagation: a patch cannot restore catches that the original compiler eliminated around a proven no-throw function.

<!-- i18n-section: publication -->
## SDK and publication

The append-only C API is `neverd_patch_source` with `neverd_source_patch_options_v1`; `neverd_patch_source_report` returns the receipt. Object input supplies its byte length. The legacy `neverd_patch_from_c` remains available.

Inputs are snapshotted. The candidate is staged beside the destination, reloaded and checked against every requested entry before exclusive publication. Existing files and symlink aliases are not overwritten. One missing mapping fails the entire request. `--mode=inplace` is refused. Session IR transforms cannot be applied to native objects. Mach-O uses strict signature verification/resigning; signed PE is not silently stripped.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## Building and verification

Six selected native OS/ISA jobs passed with zero failed or skipped cases: 3669 GoogleTest executions, 1008 native process observations (504 functional and 504 C++ exception observations), 12 real loader rejections, 96 images and 84 publication receipts.

| Host | Source | Harness | CI job | Tests | Observations / refusals |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

These artifacts qualify exactly the source and harness commits listed below. Later dev integration and documentation commits have separate local and documentation checks. Earlier failed or cancelled jobs remain failures. The historical 216 supplementary observations, PE ARM32 and older 32-bit Apple CPU/compatibility results do not establish new native hardware or C++ exception coverage.

The Windows ARM64 qualification uses a native ARM64 Unicorn C DLL built from the pinned submodule with the verified LLVM-MinGW toolchain, an MSVC import library and an explicit CMake target override. Its isolated build copy authenticates three host fixes: the non-SEH jump pair, an ARM64 page-commit callback with instruction-cache synchronization, and 64-bit immediate masks for Windows LLP64. The submodule source and revision stay unchanged. This qualifies the test host for the CPU matrix; it does not establish default MSVC ARM64 JIT support. The five host-recovery checks are separate from native patched-process and C++ exception evidence.

Use a compatible source producer through `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 was checked). Integrated LLVM receives a scoped correction in a build-directory copy; the submodule and shared prebuilt cache remain unchanged. The r3/r4 prebuilt x64 PLT bug can cause an IR patch to be refused; native objects avoid that MC path. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` requires the exact corrected PLT producer.

`NeverDSourcePatchTests` contains native C/API/object/EH cases; `NeverDSourcePatchMatrixTests` requires 283 checks across fourteen target profiles through Unicorn. `SourcePatchPE` runs seven C++ scenarios when `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` and `NEVERD_TEST_WINDOWS_SDK_LIB` are supplied. Non-Windows hosts also need `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` makes missing runtime inputs fail. The dedicated Windows workflow requires native execution and rejects skipped cases.

Linux evidence from the development Mac uses Docker and an explicit Linux loader; PE evidence uses Wine 11. Mach-O ARM64 runs on the Mac itself. Native Windows CI results must be checked separately; a configured job is not a passing run. Historical counts, baseline failures and emulator limits are in [the detailed contracts and validation record](source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

The scoped LLVM copy also corrects Mach-O x64 GOT data references: Clang’s typed-catch table is relative to the end of its four-byte relocation word. The r3/r4 prebuilt misanchors that reference by four bytes. Publication validates the encoded field and refuses the uncorrected producer. On the development Mac, x64 Mach-O runtime cases use Rosetta 2; ARM64 cases execute directly.

Native Windows x64 qualification at [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) passed all 16 focused PE/object/trampoline checks and all 143 CPU target-matrix checks without skips. The FH3 case executed seven C++ runtime scenarios; the target matrix uses Unicorn. This run used Clang 22.1.6 with native MSVC and Windows SDK inputs and integrated LLVM.

The Windows workflow also prepares 27 PE replacements across x86, x64 and ARM64. `scripts/check_source_patch_pe_runtime.py` checks nonleaf calls, mutable pointer state and floating-point/stack arguments through source IR, compiled objects and supplied objects. It runs each original and patch three times: 30 process observations per architecture. x86/x64 run on Windows x64; ARM64 runs on `windows-11-arm`. The verifier requires the expected native host, matching commit and file digests, and the complete case inventory; external runners are recorded separately. This process profile does not establish ARM64 C++ exception support.

The optional matrix producer is selected by `NEVERD_TEST_MATRIX_NEVERC`. NeverC 3389.1.5 passed all 66 checks for x64/ARM64 across ELF, PE and Mach-O, including IR and native-object installation and CPU execution. This compiler version does not support x86 or ARM32; use Clang for those targets, or a matching GCC driver for ELF. For NeverC PE compilation, the adapter adds `-fno-autolink`: patches resolve against the existing image without linking another CRT. Supplied objects with `.drectve` linker contracts remain rejected.

Native Windows process qualification at [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) passed all 90 observations: 30 each for x86, x64 and ARM64, covering source IR, compiled objects and supplied objects. x86/x64 executed on Windows x64; ARM64 executed on `windows-11-arm`, using the same hashed images. The same run passed all 16 focused PE/object/trampoline checks and all 143 Unicorn target-matrix checks without skips. The FH3 case retained its seven x64 C++ runtime scenarios.

Native producer qualification pins NeverC revision `182fd2c5dc`, independent originals and the current NeverD installer. `check_source_patch_hosted_pe_runtime.py` requires 36 native Windows observations per architecture for C IR/object/hosted results and a throwing callback through two hosted C frames. `check_source_patch_elf_runtime.py` requires 120 native Linux observations per architecture for C IR/objects/hosted, hosted propagation and nine Clang C++ exception scenarios. Receipts, artifact hashes and the actual host architecture must match. These checks install real C producer outputs; they do not claim a NeverC executable ran on the CI host. The Linux inventory includes nine exception scenarios for each of Clang IR, Clang native objects and GCC native objects on x64 and ARM64. NeverC remains C-only.

Historical qualification [21ac20341](https://github.com/NeverSight/NeverD/actions/runs/38431902780) recorded 204 observations under the former translation profile; it is not evidence for the current C-only compiler contract. The current fixtures use C for all three NeverC routes and keep Clang C++ exceptions separate. Native Clang FH3 qualification [75b6079e6](https://github.com/NeverSight/NeverD/actions/runs/38413109780) passed nine exception scenarios plus controls, 36 observations each on Windows x64/ARM64. On the development Apple Silicon Mac, macOS x64 execution uses Rosetta 2; CPU emulators do not establish native OS or unwinder coverage.

AArch64 instruction relocations require explicit RELA addends. Implicit REL instruction addends are rejected before publication; their instruction bitfields are not interpreted as plain data. Supported PREL64 data relocations retain their full signed addends in both REL and RELA.

C-only producer qualification [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) passed all 312 native process observations: 36 each on Windows x64 and ARM64, and 120 each on Linux x64 and ARM64. The Linux checks cover nine exception scenarios separately for Clang IR, Clang native objects and GCC native objects; the NeverC inputs use C for IR, objects and hosted DynCode.

Fresh installer qualification [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) passed 19 focused PE/object checks, all 143 CPU matrix cases and 474 native process observations on Windows and Linux. Windows x86 processes ran on Windows x64; x64 and ARM64 used matching hosts. Separate [Linux qualification](https://github.com/NeverSight/NeverD/actions/runs/38585759996) passed the original 108 cases plus three LLVM-C and three Clang/GCC C++ checks, with zero failures and zero skips.

Integration qualification [0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) reverified all 312 process observations on native Windows/Linux x64 and ARM64. All 30 final images match the previously qualified bytes; source digests and C-only publication receipts were refreshed and independently checked.

Expanded [Linux qualification at 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) passed 429 executed checks with zero failures or skips: the original 108 cases, 55 complete Swift checks, 143 CPU target checks, 117 native session checks, three LLVM-C checks and three Clang/GCC C++ checks. The local full binary patch suite also passed all 22,961 cases at this integration. Native process counts and CPU emulation coverage remain separate.

The rebuilt [Windows installer qualification at 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) passed all 182 focused checks and 474 native process observations, with zero failures or skips. An independent artifact audit checked every image hash, architecture, scenario and repetition. After the subsequent ABI/unwind integration at `e1cac949f`, the full binary patch suite again passed all 22,961 cases with zero failures or skips. These results retain the C-only NeverC contract; C++ replacements use Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## macOS and 32-bit process qualification

The process-image workflow requires native macOS ARM64 (`macos-15`) and Intel x64 (`macos-15-intel`) execution. Each architecture executes 96 observations covering Clang C source/object installation, writable pointer state, floating-point/stack arguments, nine Clang C++ exception scenarios, and existing NeverC pure C IR/object/hosted output. Mach-O signatures are checked strictly before execution. Hosted code must match the producer payload byte for byte.

The same workflow requires 30 Windows x86 observations on Windows x64, 48 Linux x86 observations on Linux x64, and 96 Linux ARM32 observations through QEMU user mode, split equally between ARM and Thumb. The Linux checks include matching GCC objects. Every scenario repeats three times. Reports distinguish `native`, `compatibility`, `rosetta` and `qemu-user`, and retain both target and actual host architectures. Rosetta cannot satisfy a native Intel requirement.

All 14 CPU target profiles remain covered by the 283-case source/IR/object matrix. PE ARM32, Mach-O i386, ARMv7 iOS and ARMv7k watchOS currently have CPU qualification, without a native OS loader or C++ unwinder claim. NeverC remains pure C on x64/ARM64; Clang and matching ELF GCC drivers cover the 32-bit producers. The verifier does not build or modify NeverC.

Windows x64 and ARM64 each also execute 36 Clang C++ FH3 observations.

Mach-O writable-pointer cases initialize the pointer during execution; static address initializers still require unsupported load-time relocation registration and are covered by an explicit refusal test. ELF zero-sized COMDAT functions are accepted only when the signature is the sole function at the start of its executable section and CFI matches the complete section exactly. Interior, shared, padded, ungrouped and missing-CFI cases remain rejected.

ELF ARM and Thumb have separate profiles. Source producers receive explicit `-marm` or `-mthumb`, and native function bindings preserve the original instruction-state bit. The 32-bit ELF processes use PIE images with real dynamic relocations. ARM32 QEMU execution records an explicit glibc `--loader` and sysroot: QEMU’s AUXV calculation does not follow relocated program headers as the Linux kernel does. This route verifies glibc loading and execution under QEMU; it is not physical ARM32 or native kernel-loader evidence.

For these patched ELF images, the Linux x86 and ARM32 `qemu-user` routes both require an explicit glibc `--loader` from the matching target sysroot. Linux x86 `compatibility` uses the Linux x64 host kernel's 32-bit support and is recorded separately from QEMU execution; it does not establish execution on a native 32-bit x86 host.

The [process qualification](https://github.com/NeverSight/NeverD/actions/runs/38694434082) at `fcee81c9a5e2f20a71090ade23ae833175f64c59` passed all 750 observations: 576 on matching native hosts, 78 in x86 compatibility mode and 96 ARM/Thumb observations through QEMU user mode. Independent artifact audits verified all 126 image hashes, execution identities and complete scenario inventories. The [Windows rebuild](https://github.com/NeverSight/NeverD/actions/runs/38694628242) passed 194 focused checks and 474 Windows/Linux process observations. The [Linux rebuild](https://github.com/NeverSight/NeverD/actions/runs/38694638358) passed 444 checks, including every one of the original 62 failures and 46 skips, with zero failures or skips. These results retain the hardware and loader limits above; NeverC remains pure C and its repository was not changed.

Additional [ARM/Thumb qualification](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) passed all 96 observations (48 ARM, 48 Thumb) for the 32 C test images from `fcee81c9a5e2f20a71090ade23ae833175f64c59` on `ubuntu-24.04-arm`. The Linux kernel loaded each image directly through `/lib/ld-linux-armhf.so.3`, without QEMU or an explicit loader command. This is `compatibility` on an ARM64 host, not a native 32-bit host. The harness commit `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` and image commit are recorded separately. Independent artifact audits verified image hashes, all 30 publication receipts, the complete scenario/repetition inventory and the absence of matching `binfmt_misc` interpreter rules.

Separate [hosted runner probes](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) found that the provided Windows 11 ARM64 runner rejects ARM32 programs with `WinError 216`. On macOS 15.7.9 with Xcode 16.4, the supplied SDKs could not link macOS i386 or iOS ARMv7 programs. A watchOS ARMv7k program linked successfully, but no physical device execution was established. Successful probe jobs do not qualify these platforms; their native hardware coverage remains open. NeverC was neither built nor changed.

Further [personal Actions Linux checks](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) passed 96 ARM/Thumb observations inside a complete QEMU TCG ARM32 guest with a 32-bit Linux kernel and glibc, plus 30 PE ARM32 observations through Wine in that guest. The guest kernel loads the ELF images directly; this is system emulation, distinct from the explicit-loader `qemu-user` route. Wine registry installation on the ARM64 host's compatibility layer is recorded separately and contributes zero patch observations. Guest Wine initialization and every measured launch must succeed within the original deadlines.

The [Apple compatibility checks](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) passed 30 Mach-O i386 observations under Darling on Linux and 60 ARMv7/ARMv7k observations under touchHLE on Intel macOS. Each original and each source IR, compiled-object or supplied-object replacement executes three times. Independent audits verified all 72 images, 66 publication receipts and 216 observations across these supplementary runs, including GitHub artifact sizes, SHA256 digests, exact commits and complete case inventories. Linux inputs remain pinned to `fcee81c9a5`; PE inputs were generated at `a542f97ba`, and Mach-O inputs at `8f5f06762`. Harness commits are recorded separately in the linked runs.

The Mach-O fixes authenticate selected Thumb functions independently of mixed ARM/Thumb image summaries, canonicalize proven Thumb export pointers, and normalize ARM32 unwind code addresses without changing data pointers or range lengths. The integrated LLVM source build passed all 154 CPU matrix checks and 258 exception-contract checks locally, with no failures or skips. Legacy Mach-O probes use a documented open-source linker and reserve unwind capacity; i386 has a correctly aligned startup entry and a 64 KiB page-zero segment.

These results establish C execution under the named emulators and compatibility layers. Wine is not Microsoft Windows, Darling is not macOS, and ARMv7k execution inside the iOS HLE does not qualify watchOS, WatchKit or an Apple device. No legacy Apple SDK or native 32-bit hardware coverage is inferred, and these probes do not qualify C++ exception unwinding. Previous native and compatibility evidence remains separate. NeverC was neither built nor changed.
