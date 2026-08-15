**Languages**: [English](architecture.md) | [简体中文](architecture.zh-CN.md) | [繁體中文](architecture.zh-TW.md) | [日本語](architecture.ja.md) | [한국어](architecture.ko.md) | [Français](architecture.fr.md) | [Deutsch](architecture.de.md) | [Español](architecture.es.md) | [Italiano](architecture.it.md) | [Русский](architecture.ru.md) | [العربية](architecture.ar.md)

[← Documentation Index](README.md)

# NeverD Architecture

This guide describes the production boundaries a contributor needs in order to
change NeverD safely. It intentionally covers NeverD-owned code only; the LLVM,
Capstone, and Unicorn submodules keep their own internal architecture.

## System boundary

```mermaid
flowchart LR
  CLI["tools/neverd CLI"] --> CAPI["libneverd C API"]
  SDKUser["SDK user or plugin"] --> CAPI
  CAPI --> Session["sdk::Session"]
  Session --> Loader["format loader"]
  Loader --> Image["BinaryImage"]
  Image --> Pipeline["Pipeline"]
  Pipeline --> Low["LowIR"]
  Low --> Med["MedIR"]
  Med --> High["HighIR"]
  High --> HighC["structured C"]
  Med --> LLVM["LLVM IR"]
  LLVM --> LLVMOut["LLVM IR or LLVM-derived C"]
  LLVM --> Codegen["target codegen"]
  Codegen --> Rewriter["PE / ELF / Mach-O rewriter"]
  Rewriter --> Patched["patched binary"]
```

NeverD has four IR representations, but they are not one mandatory four-hop
sequence. `LowIR -> MedIR` is shared. Structured decompilation then uses
`MedIR -> HighIR -> C`; `lift`, `decompile --llvm`, and `patch` use the direct
`MedIR -> LLVM IR` route. In particular, patch and lift modes deliberately skip
HighIR.

The CLI parses commands in `tools/neverd`, creates a `neverd_session_t`, and
calls the public API in `include/neverd/sdk/NeverDCAPI.h`. Engine state lives in
`lib/sdk/SessionImpl.h`; `neverd_session_load` selects a loader and builds a
`BinaryImage`, while IR-backed operations run `lib/pipeline/Pipeline.cpp`
lazily. The `neverd` executable links `neverd_shared`; the component archives
and their LLVM/Capstone dependencies are private implementation details of
that shared library. The CLI uses LLVM Support for its command-line UI,
but it does not bypass the C API to drive the engine.

## IR representations and routes

| Representation | Purpose | Primary definitions and transformations |
|----------------|---------|-----------------------------------------|
| LowIR | Architecture-neutral `NdOp` operations, basic blocks, CFG, and jump-table metadata | `include/neverd/ir/low`, `lib/ir/low`, produced by `lib/decode` + `lib/lift` |
| MedIR | Types, ABI/calling conventions, memory and stack model, flags, calls, and SSA-like data flow | `include/neverd/ir/med`, `lib/ir/med` |
| HighIR | Structured expressions and control flow for readable C | `include/neverd/ir/high`, `lib/ir/high`, emitted by `lib/backend/c/HighC` |
| LLVM IR | Optimization, LLVM-derived C, target code generation, and binary rewrite input | `lib/backend/llvm`, optimized/orchestrated by `lib/pipeline` |

| User route | Representation path | Exit |
|------------|---------------------|------|
| Low/Med dump | Binary -> LowIR, optionally -> MedIR | Diagnostic text |
| High dump or `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR or structured C |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | LLVM-derived C |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | Rewritten binary |

`lib/pipeline/Pipeline.cpp` is the source of truth for route selection. Keep
representation-specific logic in its owning IR or backend library; the pipeline
should orchestrate those components rather than absorb their algorithms.

## Cross-architecture translation contract

`include/neverd/translate` defines a contract layer, not an
execution backend. `GuestState` models architecture-neutral, machine-visible
state for `x86_32`, `x86_64`, `AArch64`, and `ARM32`. Its canonical version-1
serialization uses fixed-width little-endian fields, stable register IDs,
sorted collections, and fail-closed validation, so persisted state never
depends on the host C++ layout.

The `GuestState` wire-v1 baseline is permanently frozen. Modeled state outside
that baseline must use an extension-register ID in the extension range with a
canonical lower-case name, or move to a new wire version with an explicit
upgrader; changing the v1 baseline in place is forbidden.

For an `ARM32` guest, `ExecutionMode` is the authoritative decode mode and must
agree with `CPSR.T`. The stored PC is always the canonical instruction address
with bit 0 cleared; ARM mode additionally requires word alignment.

The pair-policy contract defines `x86_64 -> AArch64`,
`AArch64 -> x86_64`, `x86_32 -> AArch64/ARM32`, and
`ARM32 -> x86_32/x86_64`. `ContractDefined` means that a request can be
validated and persisted; it does not mean that code can be translated or
executed. JIT policy accepts only the native process host, while AOT policy
requires an explicit host architecture and target triple; a selected CPU or
feature set is explicit as well.

`ResolvedHostTarget` makes that selection concrete. `Native` resolution derives
the process triple, CPU, and enabled/disabled feature set; `Explicit` resolution
validates and normalizes the caller-provided architecture, triple, CPU, and
features and rejects conflicts. Its versioned cache identity is built from the
normalized target inputs in deterministic byte order and contains no process
addresses or locale-dependent text.

A versioned `TranslationExit` records a stable stop reason and the matching
typed payload for syscalls, exceptions or signals, breakpoints, unsupported
instructions, self-modification, resource budgets, external calls, memory
faults, and other terminal conditions. Consumers therefore do not have to
reinterpret an untyped integer according to the stop reason.

For every stop other than the matching `BudgetExhausted` case, the reported
instruction, block, and generated-code counts must not exceed the corresponding
non-zero request budget. Instruction and block exhaustion stop exactly at the
limit. Generated-object size is an indivisible post-codegen measurement, so its
exhausted result may report `Observed > Limit`; that rejected object is never
linked, published, or executed. Every `BudgetExhausted` payload identifies the
exact requested limit, never a derived or implementation-private threshold.

The backend-private `RuntimeControlBlockV1` contract is exactly
128 bytes, aligned to 8 bytes, and guarded by fixed v1 magic, version, size,
field offsets, zeroed reserved fields, and coherent typed exits. It contains no
C++ containers, host pointers, or guest-address aliases. It is neither the C++
layout nor the wire format of `GuestState`; a backend implementing this contract
must convert state into this record explicitly.

The fixed v1 generated-code call surface contains exactly eight helpers:
`nvd_rt_v1_load8_le`, `nvd_rt_v1_load16_le`, `nvd_rt_v1_load32_le`,
`nvd_rt_v1_load64_le`, `nvd_rt_v1_store8_le`, `nvd_rt_v1_store16_le`,
`nvd_rt_v1_store32_le`, and `nvd_rt_v1_store64_le`. Their names, signatures,
and pointer provenance must match exactly; a backend binds this finite table
explicitly and never falls back to ambient symbol resolution. Executable-
generation validation and budget/cancellation polling are trusted-dispatcher-
only operations: `nvd_rt_v1_validate_generation` and `nvd_rt_v1_poll` are not
generated-code helpers. The trusted host dispatcher also owns block selection
and is not callable from generated IR; translated blocks return a typed exit
code instead. Generated IR may directly read only the declared scalar-result
runtime slot.

`RuntimeSymbolRegistryV1` turns that helper table into a closed host-side
registry. Construction validates the complete ABI-v1 set, exact canonical
names, helper classes, signatures, and one non-null class-matching function
pointer per entry. Lookup is exact-name only, never consults ambient process or
dynamic-loader symbols, and supplies the same sorted names as the artifact
verifier allowlist. Its versioned identity covers names, helper classes, and ABI
shape but deliberately excludes native addresses, making it stable across
ASLR.

`RuntimeCodeMemory` owns page-isolated generated-code storage with a one-way
`RW -> RX` publication transition. It is never writable and executable at the
same time, cannot be reopened for writes, bounds-checks writes and entry
offsets, and invalidates the host instruction cache when published. The native
smoke test executes only a tiny host instruction sequence after publication;
that proves this W^X memory boundary, not a translation engine.

`GuestMemoryRuntime` is isolated from logical `GuestState`: construction first
validates the state and copies region bytes and metadata into a sorted private
index. Guest virtual addresses are lookup keys and are never converted to host
pointers. Checked scalar access reports typed width, alignment, overflow,
mapping, cross-region, permission, executable-write, generation-overflow,
generation-mismatch, and policy faults. Instruction/block budgets,
cancellation, generation tracking, and the `RejectExecutableWrites`,
`InvalidateOnExecutableWrite`, and `ValidateBeforeDispatch` code-write policies
also produce coherent typed records rather than implicit host-side behavior.

`TranslationObjectCompilerV1` is the verified LLVM-IR-to-object boundary. It
validates a const input module, clones it before applying any transformation,
composes proof-gated semantic simplification with LLVM optimization at `O0`
through `O3`, validates the final IR again, and emits relocatable ELF, COFF, or
Mach-O objects for the four contract host architectures. It canonicalizes the exact
target-mangled block and runtime-symbol manifests, audits every emitted object,
and returns the runtime-registry identity plus versioned request and artifact
cache keys. A non-zero generated-byte budget bounds which object may proceed to
artifact verification. LLVM first emits into a private buffer to measure the
exact indivisible object; an oversized object is rejected before publication
and artifact audit, with typed telemetry retaining the observed size and exact
requested limit. Zero means unlimited by caller policy. The compiler stops at
audited relocatable bytes: it does not link, publish, dispatch, or execute them,
and it does not supply guest instruction lowering.

The post-codegen verifier audits relocatable ELF, COFF, and Mach-O
objects as a closed set. Format and architecture must match the selected host;
undefined symbols require exact membership in the finite helper allowlist and
dynamic symbols are forbidden. Relocations are explicit direct whitelists with
checked encoding, width, alignment, offset, loadable destination, and an
object-local non-preemptible or exactly allowlisted target. The verifier rejects
W+X, unwind/exception and initializer metadata, TLS, IFUNC, GOT and ordinary
PLT indirection, dynamic relocations, weak/preemptible or selectable
definitions, unknown allocated sections, and linker directives. LLVM's hidden
x86-64 ELF `R_X86_64_PLT32` spelling is accepted only when the v1 policy proves
it is a sealed direct branch to an exact runtime helper; it does not authorize a
PLT or GOT path. ELF `ET_REL` artifacts must contain no program headers or
segments. Mach-O load commands use a positive list: exactly one width-matching
segment and at most one symbol table, dynamic-symbol table, platform-version,
and data-in-code command, with their dependencies checked; linker options and
every other command are rejected.

`TranslationObjectRequestV1` is the first public, deliberately narrow
guest-byte-to-object slice built on these contracts. Within the published
version-1 fail-closed x86-64 scalar-register subset, it accepts only canonical
encodings without legacy prefixes: REX.W full-width GPR `MOV`, `ADD`/`SUB`, and
`AND`/`OR`/`XOR` forms over the supported register/immediate LowIR shapes.
Schema 9 also accepts full-width register-only `CMP` encodings `39/3B`,
register/immediate `CMP` encodings `81/7`, `83/7`, and `3D`, full-width
register-only `TEST` encoding `85`, and register/immediate `TEST` encodings
`F7/0` and `A9`.
Arithmetic forms retain their scalar flag computations; logical and `TEST`
forms compute their architecturally defined flags while preserving `AF` in the
NeverD state model. Canonical `C3` `RET`
and `C2 iw` `RET imm16` terminate return blocks; canonical `EB cb` and `E9 cd`
direct-relative `JMP` encodings terminate direct-branch blocks. The published
lowering schema is 9. Canonical, legacy-prefix-free traditional Jcc branches
terminate blocks only in these forms: `JO`/`JNO` short `70/71 cb` or near
`0F 80/81 cd`; `JB`/`JAE` short `72/73 cb` or near `0F 82/83 cd`; `JE`/`JNE`
short `74/75 cb` or near `0F 84/85 cd`; `JBE`/`JA` short `76/77 cb` or near
`0F 86/87 cd`; `JS`/`JNS` short `78/79 cb` or near `0F 88/89 cd`; `JP`/`JNP`
short `7A/7B cb` or near `0F 8A/8B cd`; `JL`/`JGE` short `7C/7D cb` or near
`0F 8C/8D cd`; and `JLE`/`JG` short `7E/7F cb` or near `0F 8E/8F cd`.
`JRCXZ`/`JECXZ`/`JCXZ` and `LOOP`/`LOOPE`/`LOOPNE` remain unpublished and
fail closed. Reserved `F7 /1`, guest-memory and partial-register forms, legacy
prefixes, and semantically redundant REX extension bits also fail closed.
It emits only an audited,
little-endian AArch64 ELF or Mach-O relocatable object. Ordinary guest-memory
operations, partial-register forms, any instruction or encoding outside that
exact subset, all other control flow, and every LowIR operation not implemented
by the lowerer are rejected before object emission.
The checked return-address read required by `RET` is internal to its terminator
contract and does not publish general guest-memory lowering. The request
rebuilds and validates the block descriptor, uses one resolved target machine
for lowering and object emission, and combines proof-gated semantic
simplification with LLVM's default `O2` optimization pipeline. This slice is
not coverage for other x86-64 instructions, other guest/host pairs, or the
reverse AArch64-to-x86-64 direction.

The public C entry point
`neverd_translate_x86_64_block_to_aarch64_object_v1`, the Python ctypes wrapper
`translate_x86_64_block_to_aarch64_object`, and the
`neverd translate-object` command expose that same object-only boundary. Python
uses `TranslationObjectFormat.ELF` or `.MACHO`. Native translation failures
raise a typed `TranslationError` carrying `TranslationErrorCode`; local
argument validation instead raises `TypeError` or `ValueError`. Success returns
an immutable, Python-owned result. The C result owns its object bytes, stable
cache identities, and optimization telemetry; the CLI writes only the selected
ELF or Mach-O object. These C, Python, and CLI object surfaces stop before
linking, loading, dispatch, execution, and debugging; they are not execution
session interfaces.

`verifyTranslationLinkGraphV1` adds a second, pre-allocation audit. It builds
an ephemeral LLVM JITLink graph from an accepted AArch64 ELF or Mach-O object
and checks its target, section permissions, block/runtime symbol manifests,
external-symbol closure, and edge kinds and targets. The graph is destroyed
after the address-free audit result is produced. Passing this audit does not
link, allocate, resolve, load, publish, dispatch, or execute code.

`linkTranslationObjectV1` is the separate native linking boundary. It re-audits
the trusted descriptor, raw object, and JITLink graph before and after pruning,
allocation, symbol resolution, and fixup. Runtime symbols come only from the
sealed registry. A dispatcher credential binds the one manifest entry to its
session, block identity, guest entry PC, cache generation, and code epoch;
invocation also requires the runtime guest `RIP` to match that entry. Successful
finalization publishes executable memory with final protections, and unload
revokes new invocations and waits for an active invocation before releasing the
allocation. A credential-free overload remains audit-only and cannot invoke.

`NativeTranslationSessionV1` composes those pieces into the experimental C++
x86-64-to-native-AArch64 execution boundary. On a little-endian AArch64 ELF or
Mach-O process it preserves one checked guest-memory runtime and fixed guest
state across a compile-link-validate-invoke-unload dispatcher loop. A canonical
direct jump continues at its exact static target. A published canonical Jcc
branch continues only at the taken or fallthrough
successor declared by the block manifest; the dispatcher rejects every other
selected PC. A return
terminates. Global instruction, block, and generated-object-byte accounting
remains exact across blocks, and successful guest stops commit executed state
and authoritative memory together. Cancellation is linearized against that
final commit.

This is an executable vertical slice, not a complete translator. It does not
yet cover ordinary guest-memory instructions, partial registers, conditional
control flow outside the exact schema-9 traditional-Jcc slice above—including
`JRCXZ`/`JECXZ`/`JCXZ` and `LOOP`/`LOOPE`/`LOOPNE`—indirect control flow, calls,
floating-point, SIMD, x87, atomics, system instructions, general exception
propagation, block caching, other guest/host pairs, or the reverse
AArch64-to-x86-64 direction. The execution session has no C, Python, CLI, or
JSON surface yet, and debugging remains separate and unsupported. The object
APIs above remain useful without opting into native execution.

The generated-IR contract requires every translated block governed by it to be
hidden and non-preemptible with the C ABI `i32 (ptr state, ptr runtime)`.
Blocks are discoverable only through a private registry, never through ambient
process symbol lookup; direct block-to-block calls are forbidden.

The IR verifier also caps integer widths at the host scalar-register width to
avoid known compiler-runtime libcalls introduced during legalization. That
check is necessary, not sufficient: any execution backend implementing this
contract must perform an exact audit of post-codegen control transfers,
`MachineIR`, and target-object relocations against the same finite
runtime-symbol allowlist.

Direct TranslationIR loads and stores, and values held by private constants,
may contain only one scalar integer no wider than the host scalar-register
width. Aggregates must be scalarized before the verifier boundary so compact IR
cannot trigger unbounded backend expansion.

The generated-code ABI is defined only for scalar integers. Floating-point,
SIMD, x87, atomics, and system instructions are outside this contract. The
`ProvenSemanticAndLLVM` policy requires any implementation that selects it to
run NeverD's proof-gated semantic simplification to a joint fixed point with
LLVM optimization; the policy does not supply an executable translation
backend.

## Exception-rewrite boundaries

Mach-O compact unwind has a strict parser for original `__unwind_info`, a
fixup-aware parser for generated `__LD,__compact_unwind` records, an exact
original/generated range merge, a deterministic regular-page encoder, and a
transactional final-section installer. The installer rewrites an existing
file-backed `__TEXT,__unwind_info` only when the encoded table fits its declared
capacity; it revalidates the architecture, layout, and byte preimage, clears
the unused tail, reparses the result, and proves semantic equivalence before
the enclosing Mach-O transaction commits once. Generated records are
authenticated by an exact compiler-recorded IR source-function to target MC
owner-symbol mapping (including private definitions, with no object-format
prefix or mangling guesses), opaque nonzero range IDs, and exact half-open
fragment ranges. Every generated FDE must match exactly one authenticated
fragment, and every required fragment must match exactly one FDE installed by
that transaction unless an exact, strictly validated non-DWARF compact row
covers it. Adjacent or disjoint fragments owned by the same function may reuse
one source recipe, while missing, duplicate, dangling, cross-owner, or
boundary-mismatched identities fail before mutation. The injected RX segment
is committed only after a unique terminal `__LINKEDIT`, checked offset
relocation, and a strict replay of the final file and virtual layout have been
proved. When the final section is absent, generated compact rows are not
installed and the transaction may proceed only through the exact authenticated
DWARF-FDE closure above; an existing but undersized or malformed final section
still fails closed. A linked native throw/catch proof is still pending.

For ARM32 compact unwind, encoded stack adjustment and GPR layout have
`Complete` semantic status. D-register pattern selectors 0 through 3 are also
`Complete`; selectors 4 through 7 are `Partial` because the compact word alone
cannot prove every runtime-aligned CFA-relative slot. `Partial` entries retain
proven register identities for analysis, but every rewrite path rejects them
fail-closed. Each EH-frame install receipt binds the exact target architecture,
pointer width, and byte order, and
compact-unwind DWARF binding rejects any receipt mismatch.

The top-level ARM32 section transaction is narrower than the compact-unwind
decoder. It is enabled only when the Mach-O header is exactly
`CPU_SUBTYPE_ARM_V7K` and the original symbol table's `N_ARM_THUMB_DEF` bits
positively authenticate every required function as Thumb code. The exact
`thumbv7k-apple-watchos` triple and Thumb mode then remain bound throughout
code generation, whose input feature requirements may not exceed the
Cortex-A7 ceiling. Unflagged or unknown functions, generic non-v7k subtypes,
ARM mode, mixed or unknown external-code targets, the ARM Mach-O in-place
entry point, and C-source ARM Mach-O patching all fail closed before output
mutation. Stripped inputs whose only function discovery source is
`LC_FUNCTION_STARTS` are not yet supported.

PE, ELF, and Mach-O each have format-specific exception components, but NeverD
does not yet publish an all-formats, all-exception-types end-to-end rewrite
pipeline. Unsupported encodings or unresolved registration/layout requirements
must fail before output mutation; existing partial format support must not be
described as full exception closure.

Recognizing an Ada or D Itanium personality is not Ada or D exception support.
GNAT, GDC, DMD, and LDC address-form LSDAs are parseable; type-table slots stay
opaque (`Exception_Id` / `Exception_Data` for GNAT, `ClassInfo` for D) and are
never followed as `std::type_info`. Native reconstruction emits LLVM
`personality` plus address-form `invoke`/`landingpad` clauses. Corpus-proven
status is a separate claim and is not implied by personality recognition or
native lowering.

## Component map

Every component is a static archive created by `add_neverd_component_library`.
The table lists important NeverD dependencies, not the common LLVM and Capstone
libraries supplied by the CMake helper.

| Directory | Responsibility | Important dependencies |
|-----------|----------------|------------------------|
| `lib/loader` | Format detection, PE/COFF, ELF, and Mach-O loading; normalized `BinaryImage`; function discovery | LLVM Object APIs |
| `lib/lift` | Hand-written x86/i386, AArch64, and ARM32 instruction semantics | IR data types |
| `lib/decode` | Capstone/native decode and dispatch into the architecture lifters | `NeverDIR`, `NeverDLift` |
| `lib/ir` | Common types plus LowIR, MedIR, HighIR, and intrinsic definitions/transforms | Its four IR subcomponents |
| `lib/pipeline` | Function detection and Low/Med/High/LLVM route orchestration | IR, decode, lift, LLVM backend, debug info, IR passes |
| `lib/backend/c` | HighIR-to-C and LLVM-IR-to-C rendering | IR |
| `lib/backend/llvm` | MedIR-to-LLVM lowering | IR |
| `lib/backend/codegen` | Target code generation plus PE/ELF/Mach-O patch and in-place rewrite | IR, loader |
| `lib/sdk` | Public C ABI, session lifecycle, queries, persistence, plugins, lift/decompile/patch entry points | Aggregates the engine components into `libneverd` |
| `lib/pass` | LLVM IR obfuscation passes and MIR pass runner | IR |
| `lib/debug` | DWARF, PDB, and linker-map debug contexts | IR |
| `lib/sigs` | Signature parsing, databases, and matching | Loader |
| `lib/libc` | Known libc names and call-model support | Standalone component |
| `lib/support` | Shared binary-loading helpers | Loader |
| `lib/translate` | Versioned guest state/policy/exits, fixed runtime ABI, checked guest memory, generated-IR/object/LinkGraph audits, sealed native linking, and the experimental x86-64-to-AArch64 C++ dispatcher | IR, LLVM, LLVM Object, and JITLink contracts |

Public headers mirror these areas under `include/neverd`. Avoid making an
internal C++ class part of the SDK by accident: stable external operations
belong in the pure C header and one of the focused `lib/sdk/NeverDCAPI*.cpp`
files.

## Strict lifting contract

`Decoder` and every architecture lifter start in strict mode. If Capstone can
decode an instruction but the selected lifter has no implementation, the
lifter throws `UnliftedInstruction`. The exception records the instruction
address, mnemonic, and operand string; unsupported semantics must therefore
fail visibly instead of being omitted or guessed.

The internal non-strict path emits `NdOp::NOP`, but it is a diagnostic escape
hatch, not an acceptable implementation of an instruction. Contributor and CI
tests should keep strict mode enabled. When a strict failure appears:

1. Reproduce it with the smallest architecture-specific fixture.
2. Add the missing semantics in `lib/lift/<ISA>`.
3. Assert the expected LowIR shape in `unittests/lift`.
4. Add a Unicorn differential roundtrip in `unittests/semantic` when the
   instruction has observable behavior.

Do not catch `UnliftedInstruction` merely to make a pipeline continue. A new
intentional approximation would need an explicit contract and tests; it must
not masquerade as 1:1 lifting.

## Format and ISA ownership

Input format logic and output rewrite logic are deliberately separate:

| Format | Load, metadata, and input relocations | Patch and output relocations |
|--------|---------------------------------------|------------------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

Architecture lifters live in `lib/lift/X86`, `lib/lift/AArch64`, and
`lib/lift/ARM`. The corresponding public lifter/register declarations live in
`include/neverd/lift`. Target-specific LLVM emission and code generation live
under `lib/backend/llvm/<ISA>` and `lib/backend/codegen/CodeGen<ISA>.cpp`.

<a id="support-and-test-depth"></a>

### Support and test depth

The root support matrix means that each cell is implemented. It does not mean
that every opcode, ABI edge case, binary producer, or operating-system version
has been exhaustively tested. Strict mode fails closed when instruction
semantics are outside the implemented lifter coverage.

All 12 format-by-architecture cells have semantic rewrite-backend coverage in
`unittests/semantic/PatchFullSubstRTTests.cpp`. Integration depth is more
specific:

| Format | x86-64 | i386 | AArch64 | ARM32 |
|--------|--------|------|---------|-------|
| PE/COFF | Linked fixture | Backend grid | Linked fixture | Linked Thumb fixture |
| ELF | Linked fixture + semantic roundtrip | Object pipeline + semantic roundtrip | Linked fixture + semantic roundtrip | Linked fixture + semantic roundtrip |
| Mach-O | Linked fixture\* | PIC/no-PIC object pipeline\* | Linked fixture\* | Backend grid |

- **Linked fixture** exercises a linked executable through loader/pipeline and
  patch behavior for representative programs.
- **Object pipeline** exercises loading, all IR stages, and decompilation of a
  relocatable object, but not host linking and execution of a patched binary.
- **Backend grid** compiles representative IR through the exact rewrite
  code-generation path and compares behavior in Unicorn; it does not exercise
  that format's loader on a linked executable.
- `*` Mach-O linked fixtures depend on a host toolchain that can produce the
  requested target. Modern macOS cannot link historical i386 executables, so
  i386 coverage uses both PIC and no-PIC thin objects plus the rewrite grid.

Treat linked-fixture cells as the strongest format-integration
evidence for those representative programs. Object-pipeline and backend-grid
cells have partial format-integration coverage. No cell is “fully tested”
without that qualification, and none claims exhaustive ISA coverage.

The principal evidence is
[`PatchFormatTests.cpp`](../unittests/lift/format/PatchFormatTests.cpp) for linked ELF
and PE fixtures,
[`COFFARMFormatTests.cpp`](../unittests/lift/format/COFFARMFormatTests.cpp) for Windows
ARM loading/decompilation,
[`MachOI386RelocationTests.cpp`](../unittests/lift/format/MachOI386RelocationTests.cpp)
for i386 thin objects,
[`X86_64_PipelineE2ETests.cpp`](../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp)
and
[`AArch64_PipelineE2ETests.cpp`](../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)
for linked Mach-O, and
[`PatchFullSubstRTTests.cpp`](../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)
for the 12-cell backend grid. See the [testing guide](testing.md) for commands.

## Where to edit

| Change | Start here | Minimum focused verification |
|--------|------------|------------------------------|
| Add or fix an instruction | Matching files in `lib/lift/X86`, `AArch64`, or `ARM`; public lifter header if dispatch changes | Architecture test in `unittests/lift`; semantic roundtrip in `unittests/semantic` |
| Add an `NdOp` | `include/neverd/ir/NdOps.h`, then audit Low-to-Med, emitters/renderers, verifier/emulator, and dumps | `NeverDLiftTests` + relevant `NeverDSemanticTests` cases |
| Change CFG or function discovery | `lib/ir/low`, `lib/loader/FunctionDiscovery*.cpp`, `lib/pipeline/PipelineFuncDetect.cpp` | Lift CFG/jump-table tests and focused semantic transform suite |
| Add a PE input relocation or unwind rule | `lib/loader/COFF` | `COFFARMFormatTests` or a new focused loader fixture |
| Add a PE output relocation or patch rule | `lib/backend/codegen/COFF` | `PatchFormatTests`, `RewriteCodegenRTTests`, and the PE backend grid |
| Change ELF or Mach-O format behavior | Matching `lib/loader/<Format>` and/or `lib/backend/codegen/<Format>` directory | Matching format tests plus rewrite grid |
| Change MedIR/ABI recovery | `lib/ir/med` | Calling-convention lift tests + cross-ISA semantic roundtrips |
| Change structured control-flow recovery | `lib/ir/high` | `NeverDCFGLoopXformTests` and structured-C tests |
| Add an LLVM transform | `lib/pass/ir`, public header in `include/neverd/pass/ir`, pipeline toggle if exposed | Focused transform suite + `NeverDPatchFullTests` when patch output changes |
| Add a C API operation | `include/neverd/sdk/NeverDCAPI.h`, focused `lib/sdk/NeverDCAPI*.cpp`, `SessionImpl.h` only for state | SDK/CLI semantic tests; preserve `neverd_last_error` and allocation conventions |
| Add a CLI command | `tools/neverd/NeverDCLIOptions.cpp`, `NeverDCLI.h`, a focused `NeverDCmd*.cpp`, and dispatch in `neverd.cpp` | `unittests/semantic/CLIEndToEndTests.cpp` and direct CLI smoke test |
| Add a semantic regression | Focused `unittests/semantic/*Tests.cpp`; register a new file in `unittests/semantic/CMakeLists.txt` | Build its test binary, then use `ctest -R` for the named case |

Keep edits narrow. Files that define a representation may change with their
transforms, but unrelated loaders, lifters, and backends should not be modified
solely to make a broad refactor appear uniform.
