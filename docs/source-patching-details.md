[← Source and native object patches](source-patching.md)

# Source patch contracts and validation history

NeverD can compile a replacement with a selected compiler, bind its exact entry
symbol to an original function address, and install it in a new binary. The
compiler owns the replacement's language semantics. NeverD owns address binding,
placement, relocation, unwind registration, entry redirection and publication.
This route does not decompile and rebuild the rest of the program.

## Compiler selection

```sh
# Clang C, through LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.patched

# NeverC ordinary C compilation, preserving the produced native machine code
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC uses native objects by default in the CLI
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/x86_64-linux-gnu-gcc \
  --patch-report -o app.gcc

# An independently compiled native object uses the same installer
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

Addresses above are examples; use authenticated function entries from the input.
The replacement declaration must match the original machine ABI, including
argument/return conventions and structure layouts. A stripped binary generally
cannot prove a complete C++ type declaration. An address is target selection,
not proof of that declaration.

Use `--language=c++ --compiler=clang --compiler-path=/path/to/clang++` for C++.
Use `--compiler=gcc --compiler-path=/path/to/g++ --language=c++` for G++ native
objects. An `extern "C"` entry makes naming explicit; otherwise pass the exact
mangled symbol. Overloads and multiple externally visible definitions are not
guessed. A replacement can have additional helper functions without replacing
original functions with similar names.

`--from-c` retains the C-only compatibility entry. Its nonzero `--func` address
now actually selects the original function. Requests that formerly reported
success after installing zero entry jumps now fail. New requests should specify
both `--entry` and `--func`. New APIs represent address presence separately from
address zero. Legacy name matching remains centralized for requests without
explicit mappings; native objects require explicit mappings.

## Current profiles

| Producer | Artifact | Target and language verified |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64 C++ FH3 |
| Clang 22.1.6 | Native object | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | Native object | ELF x64/ARM64 C/C++ with a matching Itanium ABI runtime |
| NeverC 3389.1.5 ordinary mode | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 ordinary mode | Native object | ELF x64 C |

These are fixed profiles, not compatibility promises for every compiler version,
flag or binary. The checked target, actual compiler path/version, arguments,
artifact, requested bindings and installed addresses appear in the JSON report.
Native objects additionally have a SHA-256 digest. An external object has an
unknown producer; its file name does not establish compiler identity.
The native object `target` field is selected from the loaded image ABI by
`sourcePatchTargetTriple`, for both compiled and supplied objects. It preserves
ARM ELF float flags and the distinct iOS/watchOS frontend contracts.

The IR route uses NeverD's LLVM code generator. Native objects retain producer
machine instructions and section contents, changing only admitted relocation
fields and the original entries. NeverC's `-fpatchable-function-entry=16` has been
checked through this route: every non-relocation text byte, including the
compiler's added entry padding, survived installation. Other MIR/obfuscation
transformations need their own relocation and runtime verification.

NeverC is optional and accepts pure C only. NeverD refuses NeverC with C++
source before starting the compiler. C++ replacements use Clang or the verified
ELF GCC object profile. Ordinary C compilation does not enable dyncode. Hosted
`--compile-mode=dyncode` requires its producer's `-fdyncode-hosted` capability,
one native code section, one explicit entry and no original bindings. It
verifies the kept object and final payload digests and compares linked code
bytes/entry layout before publication. The compiler retains actual unwind
records and disables implicit runtime lowering and mutable/TLS state.
Hosted C frames retain the unwind information needed for a surrounding C++
image's exception to propagate through them; NeverC does not compile C++.
Clang source IR admits x64/ARM64 PE FH3 with bounded compiler xdata and exact
funclet/FuncInfo ownership. GCC cannot
produce the LLVM IR artifact. Native ELF, COFF and Mach-O objects are accepted
on all four native architectures under the bounded producer contracts below.
Additional architectures are outside the runtime-verified source profiles;
matching a target triple alone does not establish runtime support. Additional
Windows EH encodings need their own contracts and runtime profiles. The existing
lifted rewrite path retains its existing rules.

## References and storage

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Bindings name external declarations. `function` requires an authenticated
original entry or import stub. `data` requires writable original storage;
`const` requires a const declaration on the IR route. Data extents cannot cross
a section/segment boundary or promise writable access to RELRO. Object data
bindings require an explicit nonzero size; IR bindings can infer a declaration's
allocation size. New writable globals and zero-initialized state occupy a
separate RW allocation, and code remains RX. ELF absolute data/function pointers
are registered with the dynamic loader, retaining the original dynamic symbol
indices and their version/dependency relationships.

A callable import stub, a GOT/IAT pointer slot and the pointed-to address have
different meanings. The resolver preserves that distinction. Only existing
imports are reused; it does not add arbitrary libraries or import-table entries.
Missing runtime symbols fail, including stack-protection and exception symbols.
The installer does not disable those requirements to make a patch fit.

An explicit binding to the original body of a replaced function is rejected.
There is no saved-prologue callback trampoline in this profile. Calls through the
original entry reach the replacement after installation.

Mach-O writable storage currently needs enough unused load-command header space
for the new segments. Test inputs reserve it with `-Wl,-headerpad,0x400` or more.
Insufficient space fails before publication. Existing code is not moved to
manufacture that space.

PE x86/x64/ARM32/ARM64 source patches put new code in RX storage, mutable state in RW storage,
and merged runtime/base-relocation tables in read-only storage. Original runtime
rows and base relocations remain intact. The image must have unused section-header
space. Active CFG, EH continuation protection, XFG and other guard modes require
additional compiler/installer contracts and are rejected without changing flags.
Informational CRT instrumentation flags alone do not enable these modes.

The native ELF reader retains complete COMDAT/link-order members on x86, x64,
ARM32 and ARM64, handles the admitted REL/RELA calls, GOT/data and pointer
encodings, and authenticates FDE or EHABI coverage against bounded function
symbols. ARM descriptors placed outside the original extab are authenticated
against exact installed producer bytes before the public index is retargeted.
Known static ARM unwinders with a baked exidx registry are refused because
changing PT_ARM_EXIDX alone cannot change their private lookup boundaries.

Native COFF installation uses exact function auxiliary lengths or a singleton
function section (`-ffunction-sections`) and cross-checks generated runtime rows
against those extents. It keeps HIGHLOW/DIR64/MOV32T base relocations separate
from the IR backend's private MC fixup evidence. Mach-O uses linker atom,
compact-record and decoded FDE evidence, preserves section-difference pairs,
and can register native compact records in unused mapped TEXT/header capacity.
The watchOS compiler adapter retains leaf frames to obtain genuine unwind
records; externally supplied objects need equivalent producer coverage.
Neither native reader fabricates MC function owners or recompiles IR.
Native COFF/Mach-O language-handler graphs are not yet a verified object
profile; use the documented IR profiles for C++ exception replacements.

Unknown relocations, malformed symbols, overlapping writers, TLS, IFUNC,
common symbols, LTO, archives, unresolved weak references and runtime
initializers/finalizers fail clearly. ELF `.data.rel.ro` needs RELRO registration;
Mach-O absolute pointers need a dyld registration contract and remain refused.
CET/IBT inputs require an ENDBR-preserving entry scheme and are refused while
retaining their protection flags. ARM/Thumb replacement entries use relative
branches so the original text has no new absolute address to rebase.

## Exception behavior

ELF uses the compiler's FDE/LSDA records and a rebuilt registered search table.
Mach-O uses compiler-generated compact-unwind/DWARF data. The checked runtime
cases cover normal returns, internal catch, a new throw caught by an original
caller, an original callee throwing through the replacement, exactly-once RAII
cleanup, rethrow, noexcept termination and untouched original functions.

PE uses the compiler's new x64 unwind programs and C++ FH3 state/handler tables.
Compiler function/funclet ranges authenticate these new records; original lifted
EH identities are neither copied nor fabricated. The checked producer is Clang
with the MSVC ABI and an existing compatible runtime. Type-info vtables and shared
data may need explicit bindings, for example
`--bind=const:??_7type_info@@6B@=0x140003008:8` (an example address, not a default).
The private type descriptors emitted by the compiler receive writable storage
and load-time base relocations. FH4, custom personalities and SEH source profiles
are not admitted by this path.

The original call site must already permit exception propagation. A compiler may
remove catches or cleanups around an original function it proves cannot throw;
installing a new throwing function does not recreate those removed caller paths.
The fixed C++ fixtures therefore use an originally throwing target. The original
runtime must provide matching personality functions, type identities and any
stack-protection dependencies. Symbol-name equality alone is not a general ABI
compatibility proof.

Linux x64 runtime evidence on the development Mac uses Docker and the Linux
dynamic loader (`/lib64/ld-linux-x86-64.so.2`). This exercises the Linux exception
runtime but is not evidence of direct launch on x64 hardware. Some emulators
provide an incorrect `AT_PHDR` for a moved program-header table; explicit loader
launch was used for those environments. The native Linux test suite also runs
ordinary launches when executing on Linux x64.

PE runtime evidence currently uses Wine 11's Windows exception runtime in an
x64 Docker environment. Its seven C++ scenarios are also available as an optional
test on a Windows x64 host/runner. This is not a claim of Windows hardware
coverage or forced-ASLR execution. On the development emulator, Wine required
its preloader to be disabled and the Linux ELF loader to be invoked explicitly.

### LLVM dependency correction

The address-model backend in the r3 and pinned r4 LLVM releases interprets the
x64 PLT specifier as an AArch64 page modifier. Late compiler calls such as
`_Unwind_Resume` can therefore receive a wrong displacement. NeverD checks these
encoded source-patch references and refuses an invalid image.

The scoped backend correction is included in
[`cmake/patches/llvm-aarch64-specifier-scope.patch`](../cmake/patches/llvm-aarch64-specifier-scope.patch).
The integrated build applies this correction to a build-directory copy of the
single LLVM source file. It preserves the submodule checkout and revision, and
accepts an already corrected source. An unrecognized source fails configuration
instead of silently skipping the correction. Use an integrated build:

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target neverd NeverDSourcePatchTests \
  NeverDNativeExceptionRewriteContractTests --parallel 4
```

Point the test compiler setting at a compatible Clang (22.1.6 was verified),
which may differ from the system compiler used to build NeverD itself.
The shared prebuilt cache is not rewritten. Its r3/r4 backend remains subject to
the displacement check and may refuse an IR patch. The native object route does
not recompile through this LLVM MC path. The standalone patch can also be applied
when building the LLVM fork outside NeverD.

## SDK and transaction behavior

The append-only C API is `neverd_patch_source` with
`neverd_source_patch_options_v1`; `neverd_patch_source_report` returns the JSON
receipt. The caller supplies byte length explicitly for object input. The legacy
`neverd_patch_from_c` symbol remains available.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

Inputs are snapshotted; the candidate is staged beside the destination, checked
against every requested installation, reloaded, and published exclusively only
after its exact entry bytes and executable destinations agree with the receipt.
Any missing mapping fails the whole request. Existing output files and symlink
aliases are not overwritten. Mach-O ad-hoc signatures use the existing strict
resigning/verification path; signatures requiring a separate signing workflow
are refused. Signed PE input is not silently stripped.

`--mode=inplace` is explicitly rejected for these inputs. Session LLVM IR
transforms apply to IR artifacts; they cannot be requested for native objects.
Compiler options are argument arrays, with target/output modes owned by each
adapter. Selected compilers never silently fall back to another executable.
Response-file tokens (`@file`) are refused in every compiler argument, including
values following separate options such as `-I`, `-include` or `-D`; drivers would
otherwise expand those tokens before the adapter-approved options are parsed.

## Validation record (2026-10-12)

Before integration with the refreshed `dev`, the focused
source/native-exception/COFF suites ran 313 checks: 311 passed and
two Linux-only checks were skipped on the development Mac. The integrated build
with the corrected LLVM dependency ran 29,577 registered checks: 29,480 passed,
31 were skipped, 64 failed and two timed out. All 66 failures/timeouts were also
reproduced against the original baseline; no new failure was found in this run.
The 3,216 composed semantic cases across four architectures and three formats
passed. These semantic checks do not extend the runtime-verified profile table.

The Python SDK checks against the built shared library ran 154 tests: 109 passed
and 45 lacked the required platform or fixture. The ABI audit and mypy passed.
Runtime evidence and its platform limits are described above; skipped coverage
and reproduced baseline failures are not counted as passes.

The integration checkpoint at `ce298f758` uses the pinned r4 LLVM with the
build-directory correction. Its source, native-exception, COFF and
plugin suites ran 475 registered test executions: 473 passed and two Linux-only
checks were skipped on the Mac. There are 330 distinct test names; shared
checks run in both the lift and focused exception binaries. The required exact
PLT producer check passed with `NEVERD_TEST_REQUIRE_FIXED_LLVM=1`. Another 364
format and shared IR checks passed, including the newly integrated return-proof
and cleanup regressions.

All 3,216 composed semantic cases across four architectures and three formats
passed at this checkpoint, with no skips. These checks cover the shared rewrite
pipeline and do not extend the runtime-verified source profiles.

After incorporating the later C-output changes through `a5f9115d3`, the final
build repeated the 475 focused executions with the same 473 passes and two
Linux-only skips. All five incoming image-table and signed-comparison tests
also passed. The composed matrix above was run at the earlier checkpoint.
The subsequent integration through `1a131e7dd` repeated the same focused result
and both seven-scenario Linux exception runs. Its computed-goto regression
passed; the ARM32 literal-pool runtime test failed because the macOS linker
cannot link `armv4t`, another host-profile limitation.

The final CLI's Clang IR and GCC object replacements each passed seven Linux
C++ exception scenarios in Docker. The GCC input retains stack protection.
The PE FH3 runtime test passed its seven scenarios under Wine. These remain
emulated/runtime evidence with the platform limits above. The final shared
library's Python SDK run passed 110 of 165 tests, with 55 explicit skips. ABI
audit, mypy, formatting and documentation checks passed.

Two additional upstream source tests could not pass in this local profile:
`AArch64_HighCExecution.FMaxAndFMinPropagateANaN` needs a Clang supporting
`__builtin_elementwise_minimum/maximum`; the configured system Clang lacks
them. `HighCPointerAddresses.AnX87StoreWritesItsTenBytes` tries to compile x87
assembly with the ARM64 host compiler. These environment failures are recorded
separately and are not counted as passes.

## Platform qualification update (2026-10-12)

The source-specific matrix now covers four architectures (x86, x64, ARM32,
ARM64) and three formats (ELF, PE, Mach-O). Its 48 checks compile/install source,
install externally supplied IR, preserve the native-object admission rule and
reject dyncode without publication. Six combinations execute their patched
entries in Unicorn; the other six exercise explicit unsupported-target refusals.
The positive object case executes ELF x64 producer bytes. These are isolated
function checks, not an assertion that every target has an OS runtime profile.

The current integrated build ran 363 checks in freshly rebuilt source, matrix,
native exception and COFF binaries: 361 passed and two Linux-only exception
checks were skipped on the Mac. Linux replay separately passed seven Clang IR
and seven GCC object exception scenarios, including the stack-protected GCC
original. Windows PE passed all seven source tests and its seven FH3 scenarios
under Wine 11. Wine's first launch timed out during its 32-bit setup helper;
removing that stalled helper in the disposable container and replaying the
unchanged test passed. That environment failure is not counted as a pass.

On the ARM64 development Mac, 27 additional C API/CLI/state/compiler/exception
checks passed with x64 Mach-O originals executing through Rosetta 2. The seven
exception scenarios include typed internal catch, propagation through original
callers/callees, RAII, rethrow and noexcept. NeverC ordinary C also executed in
this x64 profile. The default ARM64 Mach-O profile passed in the focused suite.
Simple freestanding C replacements additionally executed under native Linux
ARM64 in a container and Linux i386 through QEMU user mode. Their data-reference
probes, and an ARM64 C++ import probe, were refused for relocation contracts
outside the certified runtime profiles; those refusals do not certify the
requested data or exception behavior.

The x64 Mach-O typed-catch probe exposed a four-byte error in LLVM's GOT data
fixup base. LLDB showed the unwinder reading the original type-info GOT slot at
slot + 4. Mach-O's non-PC-relative `FK_Data_4` GOTPCREL relocation becomes
relative to the end of the word at object writing; the final-address backend
had used its beginning. The scoped build copy now applies the data-only bias,
while instructions keep their existing PC bias. An independent field regression
and the seven real exception scenarios passed. Source publication also checks
these encoded data fields, so the uncorrected r3/r4 package is refused instead
of publishing the bad reference. The recorded LLVM checkout and shared package
cache remain unchanged.

Intel Mach-O fixtures need room for both load-command headers and compact-unwind
page growth. `-Wl,-headerpad,0x1000` reserves the former;
`-Wl,-segalign,0x4000` reserves adequate adjacent file and virtual padding for
these small fixtures. Default 4 KiB Intel layouts were refused for insufficient
unwind space. These flags are fixture inputs, not instructions to silently move
or re-link an arbitrary original binary.

The dedicated [Windows qualification workflow](../.github/workflows/source-patch-windows.yml)
requires the checked Clang producer, native MSVC/SDK inputs and actual Windows
execution, and rejects skipped qualification cases. Its branch run
[38190882893](https://github.com/NeverSight/NeverD/actions/runs/38190882893)
was still queued when this record was written; configured CI is not native
Windows execution evidence. No Windows hardware coverage is claimed here.

The canonical guide, index links, architecture ownership and testing entry
points are available in English and all ten supported locales. The documentation
checker validates reviewed source hashes, executable examples, target
dispositions and contract tokens. The complete 310-file matrix and eleven
focused documentation/build regressions passed.

## Native Windows x64 qualification (2026-10-12)

The dedicated [Windows run 38200205814](https://github.com/NeverSight/NeverD/actions/runs/38200205814)
completed successfully at `96cf7cd5936f6c7d2f83f8e1bcc9fee6e1253485`.
Its downloaded XML reports contain all 15 focused PE/object/trampoline checks
and all 48 source-target matrix checks, with zero failures, disabled cases or
skips. The optional NeverC object producer is covered separately and is not
selected by this Clang qualification. The workflow checks both inventories
before accepting a run.

The native PE cases executed normal C calls and mutable pointers with base
relocations. The FH3 case executed all seven C++ scenarios: normal return,
internal typed catch, propagation to an original caller, an original callee
throwing through the replacement, rethrow, noexcept termination and an
untouched original function. Cleanup counters verify exactly-once RAII for
the propagation and rethrow cases. Clang 22.1.6, MSVC and the Windows SDK were
configured on the Windows x64 runner; this is actual Windows execution evidence.
The 48-case matrix still certifies only its isolated CPU checks and explicit
target refusals.

The earlier native runs stopped at DLL linking. `MedSyscallConvention.h`
forward-declared `BinaryImage` as a class while the owning model defines a
struct, producing incompatible MSVC names across include orders. The declaration
now agrees with the model, and the Med component treats C4099 as an error. The
real translation-unit mismatch check failed before the correction and passed
after it; the successful Windows build and runtime run verify the correction.

After integration through `0be80a553`, the freshly rebuilt local source, matrix,
native exception and COFF suites ran 363 checks: 361 passed and two Linux-only
checks were skipped on the Mac. An earlier replay encountered the stopped
owned Wine container; restarting that disposable container and repeating the
unchanged FH3 case passed. The final complete run passed all available cases.
The shared library's Python SDK run passed 110 of 165 checks, with 55 explicit
platform/fixture skips. English and ten localized source guides include the
native Windows result.

A separate local Windows 11 ARM64 VM experiment did not produce a native patch
runtime result: guest command sessions timed out and were later refused by
Parallels Tools. That attempt is not counted as passing coverage. No VM
configuration was changed; the VM was observed stopped later and was not
restarted. Native Windows evidence above comes from the qualified x64 runner.

## All native architecture qualification

The current CPU inventory is 143 passing checks over thirteen profiles:
x86/x64/ARM32/ARM64 × ELF/PE/Mach-O, with separate ARMv7k watchOS and ARMv7
iOS profiles. Every source, supplied-IR and native-object target requires
installation, reload and exact execution. Additional cases cover mutable and
readonly state, original floating-point/stack-argument bindings and entry
execution at two nonzero base slides. Legacy Mach-O containers use actual
compiler instructions and relocated unwind metadata; these CPU checks do not
certify unavailable legacy OS runtimes or additional C++ unwinders.

GCC 13 C object replacements on ELF x86, ARM32 and ARM64 were loaded and executed through Linux QEMU user mode. ELF32 GNU property alignment and ARM driver instruction-state selection have dedicated corrections; x64 GCC/G++ retains the separately verified seven C++ cases.

### Local integration checks at `170e3ad4c`

The integrated build passed all 143 source/IR/object CPU checks, 379 native
exception contracts, 37 COFF exception contracts and 16 native SDK checks.
The source suite passed 45 of 47 cases on the Mac, including NeverC ordinary
mode and the seven PE FH3 scenarios under Wine. Its two Linux-only C++ cases
were checked separately: Clang IR and GCC object replacements each passed all
seven scenarios through the Linux loader in Docker. The GCC default profile
passed; an additional `-O0` probe correctly refused the new unresolved
`__stack_chk_fail` import absent from that original binary.

After updating two old EHABI assertions for the shared decoder, all 25 ARM
EHABI patch checks passed. Valid complete programs can use the implicit finish
specified by EHABI32; truncated opcode operands still fail without changing
the input. The unchanged binary-repair route passed 84 checks across the twelve
architecture/format combinations and six native Darwin runtime checks.
Python SDK integration passed 110 of 165 checks, with 55 missing-platform or
fixture skips; ABI and mypy audits passed.

The wider 7,314-execution run passed 7,208 cases, skipped 51 and failed 55.
Two of those failures were the EHABI assertions corrected above. The remaining
53 are not counted as passing coverage: 52 report compilation, linking or
other C-output/toolchain errors, while
`NativePointerRelocationBoundary.ScalarSelectIndexCycleRemainsNumericTableOffset`
also fails with the private CLI built before the all-architecture change.
These broader results do not replace the source-specific qualification.

The all-architecture Windows run
[38289348605](https://github.com/NeverSight/NeverD/actions/runs/38289348605)
completed compilation but failed to link the matrix test's private
`sourcePatchTargetTriple` dependency. No runtime cases executed in that run.
The matrix target now links the owning codegen library on Windows; internal
C++ APIs remain outside the DLL's public C interface. The successful replay
after that test-target correction is recorded below.

### Native Windows replay and final integration (2026-10-13)

[Windows run 38307461805](https://github.com/NeverSight/NeverD/actions/runs/38307461805)
completed successfully at `2ed7e78fc8e2549cb9e7d0f7d81a4404cdfbc128`.
The downloaded XML reports contain exactly 16 focused PE/object/trampoline
checks and 143 CPU matrix checks, with no failures, errors, disabled cases or
skips. The run built integrated LLVM with MSVC and used Clang 22.1.6 plus the
native Windows SDK/CRT. The FH3 case executed all seven C++ runtime scenarios
under Windows. The matrix uses Unicorn and retains the OS-runtime limits above.

The same commit's freshly rebuilt Mac binaries passed 143 matrix checks,
234 focused native exception contracts and 37 COFF exception contracts.
The source suite passed 45 of 47 checks, including ordinary NeverC and all
seven PE FH3 scenarios under Wine; its two Linux-only object exception cases
were skipped on the Mac. These four binaries ran 461 checks: 459 passed and
two skipped. This count is for these focused binaries, without duplicate
contracts from the broader lift suite. The separate Linux runtime evidence and
the broader unresolved failures remain as recorded above.

After integrating `dev` through `40632e259`, the freshly rebuilt CLI and these
four test binaries repeated the same 459 passes and two Linux-only skips.
The final CLI's Clang IR and GCC object replacements each also passed all seven
Linux C++ runtime scenarios through the Docker Linux loader. This local replay
does not change the exact commit qualified by the Windows run above.

English and all ten localized source guides now link this successful native
Windows run and distinguish its native PE execution from the CPU matrix.

### Producer and process verification update (2026-10-13)

The ordinary NeverC matrix at `a4a5ae3cb` uses Clang originals as independent
ABI inputs. NeverC 3389.1.5 passed all 66 x64/ARM64 checks across ELF, PE and
Mach-O, including IR and native objects, state, original FP/stack arguments,
rebasing and refusal contracts. The default Clang matrix passed all 143 checks.
NeverC's current frontend rejects x86 and ARM32. Those explicit producer errors
do not reduce NeverD's installer coverage and never cause a fallback to Clang.

NeverC automatically added `/DEFAULTLIB` directives to ordinary PE objects.
The adapter now requests `-fno-autolink`, so source objects resolve against the
original image without requesting another CRT. Independently supplied COFF
objects with `.drectve` remain rejected. Their refusal is checked on all four
PE architectures, with no publication or input modification.

The new PE process checker prepares three independent originals and 27 patches
for x86/x64/ARM64. Each architecture has nine replacements: nonleaf calls,
writable pointer state and floating-point/stack arguments through source IR,
source objects and supplied objects. Each original and patch executes three
times, requiring 30 matching process results per architecture. Reports bind
commit, file digest, machine and complete case inventory. Failed replay removes
an earlier success report. Ten checker regressions validate these evidence
rules, including independent expected results and native-host requirements.

Clang-produced x64 images passed all 30 process observations under Wine 11.
NeverC-produced x64 images separately passed all 30 observations through the
same runner; ARM64 images were prepared and validated but were not executed by
Wine. The container could not start even the original x86 image because its
32-bit Windows prefix lacked `kernel32.dll`; no x86 Wine pass is claimed.
The Windows workflow now requires x86/x64 execution on Windows x64 and ARM64
execution on `windows-11-arm`, with the exact prepared image artifact. These C
process cases do not qualify ARM64 C++ exceptions or an ARM64 engine build.
Check the completed CI evidence before claiming these new native results.

The broader old failures were investigated without weakening their assertions.
An old scalar-index fixture discarded all loaded values, so optimization could
remove the indexed loads it intended to inspect. The fixture now checks the
independently computed checksum 536, preserves its nonvolatile register and
executes rebuilt LLVM at both O0 and O2. This regression passed. The old
AArch64 min/max test also passed with compatible Clang 22, instead of the
system compiler lacking its builtins.

A separate replay of 33 preserved generated-C inputs compiled and executed
31 successfully with Linux Clang 22.1.8, multilib and explicit QEMU user-mode
runners. Two packed-float struct-return inputs still fail with extra incoming
vector parameters; these are genuine pre-existing C-recovery defects, not
host-toolchain passes. A scalar-to-vector return experiment did not resolve the
parameter contract and was reverted. The replay is historical artifact evidence,
not a fresh pass of the entire lift suite. The broader unresolved results above
remain separate from the source-patch qualification.

### Native PE process completion (2026-10-13)

[Windows run 38350079817](https://github.com/NeverSight/NeverD/actions/runs/38350079817)
completed successfully at `a4a5ae3cb118d561fd814e1a6b48960760081b21`.
Its Windows x64 job passed all 16 focused PE/object/trampoline tests and all
143 Unicorn target-matrix tests without skips. The FH3 case executed its seven
x64 C++ runtime scenarios with Clang 22.1.6, native MSVC/SDK inputs and integrated
LLVM.

The separate process profile passed all 90 observations, 30 per architecture.
x86 and x64 programs executed on Windows x64; ARM64 programs executed on
`windows-11-arm`. Each architecture used one independent original and nine
replacements, each run three times. Replacements cover nonleaf calls, mutable
pointer state and floating-point/stack arguments through source IR, compiled
objects and supplied objects. Downloaded artifacts were independently checked
for the exact commit, matching manifests, file SHA-256 and PE machine values,
successful patch receipts, expected native hosts, the complete case inventory
and all observed exit codes. The ARM64 job consumed the same hashed images
prepared by the x64 job.

This is native Windows process evidence for the three C profiles. PE ARM32
retains CPU-matrix coverage; the new process profile does not certify ARM64
C++ exceptions or an ARM64 NeverD engine build. NeverC's separate 66-check
x64/ARM64 matrix remains CPU execution evidence, rather than this Clang-produced
native Windows process result.

At the local integration `fa6d21fd18be3cd8e044217aaf84c6d590134bd9`, fresh
binaries passed all 143 Clang matrix checks, 66 NeverC matrix checks, 234 native
exception contracts and 37 COFF exception contracts. The source suite passed
45 of 47 cases with two Linux-only skips; 43 of 46 focused lifting checks passed
with three Linux-only skips. Clang IR and GCC object replacements each passed
all seven Linux C++ runtime scenarios through the actual Linux loader. Binary
repair passed 84 cross-architecture checks and six native Darwin runtime
checks, without skips. Python SDK validation passed 110 of 165 cases, with 55
explicit platform or fixture skips; ABI auditing and mypy also passed. These
focused results do not turn the historical broader C-recovery defects above
into passing coverage.

### Native object receipt identity correction (2026-10-13)

A subsequent review found that native object receipts still reported
`x86_64-unknown-linux-gnu` regardless of the loaded architecture and format.
The installed PE bytes, machine headers and native process observations above
were checked independently, but this receipt field was incorrect. Both compiled
and supplied objects now derive it from the loader-owned image ABI through
`sourcePatchTargetTriple`.

The unchanged 143-check Clang and 66-check NeverC inventories passed after the
correction. Their compiled-object and valid supplied-object cases also verify
receipt identity against independent expected triples for all thirteen target
profiles. The malformed-object cases require successful installation and CPU
execution of their unmodified object as a positive control. The PE fixture
preparer now rejects a wrong receipt target; its thirteen evidence-rule tests
passed, including object and versioned MSVC IR targets.

The freshly built engine also reinstalled the nine supplied PE objects from
[Windows run 38350079817](https://github.com/NeverSight/NeverD/actions/runs/38350079817).
x86/x64/ARM64 each covered nonleaf calls, mutable pointers and FP/stack arguments.
All nine output files matched the corresponding native-qualified images byte
for byte, with correct new target receipts and unchanged original files.
Windows process evidence remains attached to that exact run and its image hashes.
