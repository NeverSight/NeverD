# x87 semantic contracts

[Issue #1677](https://github.com/NeverSight/NeverD/issues/1677) separates
instruction recognition, representation, numerical behavior, architectural
state, concrete execution, translation and checked equivalence. The
[generated capability inventory](x87-capabilities.md) records those boundaries.
It does not close the issue's portable execution or qualification gates.

## Query and ownership

`neverd x87-capabilities`, `neverd_x87_capabilities_json(session)` and Python
`Session.x87_capabilities()` return the same versioned JSON. They accept an
unloaded session, do not execute input code or probe a CPU, and work without
optional CPU emulation. C callers free the owned result with
`neverd_free_string`; failure returns NULL with `neverd_last_error(session)`.
The worker advertises `x87_capabilities` when its engine exports the C API.
The empty-object operation returns the report unchanged, before or after a
binary loads, without changing the project revision. Older engines report
the operation as unavailable.

The desktop's **Help → x87 support** opens that report through the normal
read dispatcher. Source mode, ABI, state path, consumer, backend and destination
remain explicit. The table separates implementation, numerical results and
architectural state; details retain the shared profile requirements, refusal
reason, ABI call contract and regression source references. Unlisted cells
remain unsupported, and malformed or newer schemas show an error instead of
retaining earlier support claims. Inspection never executes an input or probes
the host. Regression source references are not passing-run or proof receipts.

`ir/X87Instructions.def` owns semantic coverage metadata.
`ir/X87Capabilities.h` exposes the C++ query and report; malformed selectors,
unknown enum dimensions and unlisted cells fail closed. `ir/X87Context.h`
continues to own encoding/operand-shape admission. The report includes its
short/long operand widths, waiting boundaries and flag/memory effects.
`X87StateBoundaries.def` owns external-state synchronization membership.
The strict concrete evaluator shares the inventory's processor-dependent
classification. A query does not override a lifter, verifier or executor gate.

The report factorizes the matrix without inventing implicit success:

1. `forms` lists each encoding range, typed lifting availability and coverage
   class. `variants` gives the first register/group selector; a memory selector
   retains the original ModRM shifted by `X87ContextOriginalModRMShift`.
   Effective-address computation remains the lifter's responsibility.
2. `source_paths` lists i386/x86-64, System V/Microsoft/MinGW and explicit state
   paths, including unavailable combinations. Convention defaults describe the
   actual calling convention; a file extension does not establish one.
3. `coverage` evaluates every class × mode × state path × consumer × backend ×
   destination. Explicit state-path implementation is ABI-independent; the
   separate ABI call contract still applies. `backend=none` is for analysis;
   generated code uses `native-x86`; concrete execution distinguishes native,
   portable and explicitly selected native-reference modes.
4. `implemented`, `numerical`, `architectural_state`, `profile`, `reason`,
   `requirements` and `test_sources` are separate fields. A source reference
   records a regression location, never a passed run or a proof receipt.

## Existing state paths

The owned x64 context carries raw binary80 payloads, physical register identity,
runtime TOP, tags, CW/SW and saved environment. Its concrete storage uses the
existing 512-byte FXSAVE64 image: payload slots are logical ST order, while its
abridged tag and known-payload masks identify physical registers. A portable
backend must preserve that mapping. It must not create a second stack owner.

The default owned path is Win64; System V x64 and i386 retain typed binary80
values and the shared CFG stack/tag projection. `BinaryImage::X87ContextOverride`
can select the x64 representation explicitly. It does not prove incoming state
or a floating import/return ABI. Calls and independent CFG roots need their
existing export/refresh and stack contracts; backedges retain their context.
MMX addresses physical aliases; EMMS empties tags without initializing payloads.
Owned x64 FEMMS also empties tags, but invalidates every retained MMX/x87
payload. The concrete model retains CW/SW, TOP and saved environment, rejects
pending exceptions before changing tags, and requires subsequent writes to
establish each physical payload again. A raw snapshot or MMX read cannot
resurrect the previous bits. The explicit portable x87 profile and unowned
stack paths still refuse this instruction. Native output executes FEMMS;
FXRSTOR and context steps declare both MMX and x87 register clobbers because
LLVM represents the physical aliases as separate registers. See AMD's
[3DNow! Technology Manual, FEMMS](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/programmer-references/21928.pdf)
for the undefined-payload distinction.

Typed i386 and x64 lifting includes FDECSTP and FFREEP alongside FINCSTP and
FFREE. `X87Stack.h` owns the distinction between rotation and pop: a rotation
changes TOP without defining a payload or changing tags; FFREEP empties both
the named slot and the previous top. CFG joins retain physical register
identity. LLVM, HighC and LLVM-C emit the original instruction with the shared
x87 stack-clobber contract. These are typed projection routes, not full-state
qualification: the typed concrete executor refuses them and symbolic execution
keeps affected state unknown. Portable owned-context admission is unchanged.

Typed FBLD/FBSTP carry raw ten-byte values and an explicit control word through
one shared value contract. CFG admission proves an empty load destination or a
full store source with known payload. The native LLVM/HighC/LLVM-C projection
requires a valid CW with all exception masks and valid FBLD decimal digits;
generated guards trap outside those conditions before converting or storing a
result. The original native instructions provide conversion and status under
each PC/RC, without a host floating type or an intermediate binary64 value.
The conversion samples status in the same assembly block and passes it to the
immediately following synthetic read through the existing FPREM status owner.
HighC uses thread-local storage for that handoff and requires initialized TLS.
Typed concrete execution refuses these operations and symbolic results/status
remain unknown. Unmasked BCD and stack-fault behavior belongs to the separately
tested owned context; typed emission does not qualify those cases.

FNINIT establishes control/status and empty tags, not known retained payloads.
Environment loads cannot authenticate unknown retained register bytes. Full
save/restore, MMX, calls and extended-state instructions remain separate state
boundaries in the inventory. Synchronization alone does not qualify their
faults, memory effects, asynchronous delivery or arbitrary ABI use.

## Precision and profiles

Binary80 has a 64-bit significand and the extended exponent range. PC=00,10,11
selects 24,53,64 significand bits for applicable arithmetic; PC=01 is reserved.
RC=00,01,10,11 selects nearest/even, toward negative infinity, toward positive
infinity and toward zero. PC=53 is not an ordinary binary64 computation: the
exponent range remains extended. A binary80 intermediate followed by a second
rounding can also produce the wrong result near a rounding midpoint.

The current `typed-x87-projection-v1` profile describes a value/stack projection,
not complete architectural state. Ordinary FLOAT arithmetic does not supply an
x87 PC/RC, sticky-status or unmasked-exception model. Typed comparisons project
condition bits. FILD, constant and sign completion can preserve exact numerical
bits under their CFG/tag/control preconditions; that is not complete exception
or environment fidelity. Generated C must satisfy the actual type/runtime
requirements, including binary80 long double for operations that use that type.
Raw-bit completion helpers retain their existing representation.

`owned-x64-context-v1` describes the representation and native code routes.
`native-x87-host-reference-v1` describes concrete results produced by the host's
real x87 instructions. It requires x64 GCC/Clang, FXSAVE/FXRSTOR and SSE3 for
FISTTP. The analysis host must satisfy those prerequisites at execution time;
a static query is not an availability probe. Numerical exactness is conditional
on authenticated operands/state and the selected host instruction behavior.
Complete state/exception qualification remains unknown in the inventory.

FBLD's native numerical scope requires all eighteen packed decimal digits to
be 0 through 9. The hardware does not validate digits A through F; their result
is undefined, including the packed BCD indefinite encoding. The concrete
executor refuses these inputs before changing state or publishing a load,
even in native-reference mode. The sign byte's low seven bits are ignored, and
negative zero retains its sign. See the
[Intel FBLD instruction contract](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf).

Transcendental and partial-remainder results can be processor-dependent.
Concrete execution refuses them in strict mode; explicit native-reference mode
labels its observations approximate. They cannot supply portable exact or
checked-refinement evidence. Emitting a native instruction also does not
qualify processor-independent numerical results.

Typed FSIN/FCOS, F2XM1, FSCALE, FPATAN, FYL2X/FYL2XP1 and FPREM/FPREM1
sample the value and status in one assembly operation. Their synthetic status
read consumes that sample before spills can
change C1 or TOP. Status-only consumers keep the producer alive. This is a
defined-status/value projection, not an architectural stack snapshot.
The scalar concrete tracer refuses unmodelled native value operations even in
lenient mode. Missing status after a call cannot become zero through FNSTSW,
FNCLEX or the partial-remainder evaluator.

`X87Value.h` derives scalar operand counts from the shared value table and
requires raw ten-byte operands/results (two bytes for the synthetic status
read). Low/Med validation and LLVM/HighC publication reject missing, extra,
narrowed or malformed operands instead of duplicating an input or widening a
binary64 substitute. LLVM checks even masked bodies before publication.
The legacy single-result FPTAN/FXTRACT IDs remain reserved but are refused;
their replacement is the guarded two-result contract. Scalar x87 output has
no Swift or non-x86 LLVM runtime contract.

Typed FPTAN, FSINCOS and FXTRACT execute once and return both raw binary80
results plus the sampled status. `X87GuardedValue.h` owns their operand/result
contract alongside BCD; the existing CFG tag owner requires a full source with
known payload and an empty push destination. Runtime guards require canonical
finite operands and a valid, all-masked control word. FPTAN/FSINCOS additionally
require `abs(input) < 2^63`: outside that range hardware leaves ST0 unchanged
and does not push, a runtime TOP change the static projection cannot represent.
Those typed inputs trap explicitly before the operation; owned-context
execution retains the separate conditional-stack behavior. The generated
projection does not split FSINCOS into independently approximated operations.
See the [Intel instruction contracts](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf).

The native generated-code regression separately compares transcendental,
scale/extract and partial-remainder results, conditional stack changes and
status on the same physical CPU. Linux x64 signal fixtures compare selected
pending-invalid, WAIT/non-WAIT and inaccessible-memory boundaries, including
the saved x87 state. Those observations exclude relocated instruction/data
addresses, signal resumption and other OS/CPU contracts; they do not promote
the inventory's architectural-state or formal qualification.

The portable concrete slice below is explicitly selected and instruction scoped.
Generated LLVM, HighC and LLVM-C use separate C++ entry points with an explicit
portable runtime contract. The C++ translation profile targets native Linux
AArch64; it does not widen the existing scalar C/Python/CLI translation ABI.
Mixed supported/unsupported programs retain their refusal gates. Recorded
execution scope appears in [x87 qualification](x87-qualification.md); a static
inventory is not a passing execution record. No reduced-precision mode is offered.

## Failure and portable admission

Unknown incoming architectural state remains unknown. Waiting boundaries refuse
pending unmasked exceptions before address evaluation. Memory operands require
authenticated full ranges and permissions, including stores suppressed by an
x87 fault. Invalid control fields, missing payload provenance and unimplemented
effects cannot produce a plausible numerical value. An uncompleted instruction
leaves its state, output and memory unchanged. Earlier completed instructions
remain committed. A completed self-modifying store retains its store and pop
while reporting invalidation; that status does not mean the store was rolled back.

The portable slice names its exact encodings, finite/nonfinite input domain,
rounding points, exception masks and environment policy. Raw m80
transport, NaN payloads, signaling/quiet behavior, signed zero, subnormals,
unsupported encodings and integer/BCD conversion have instruction-specific
rules; host math-library behavior is insufficient. Applicable PC/RC rounding
must happen directly without narrowing the exponent range. Existing LLVM
APFloat/APInt are candidates, not qualification: APFloat's ordinary binary32/64
semantics and illegal-encoding normalization are not interchangeable with x87.

Every expanded cell needs independent bit/state tests, including all TOP values,
full/empty stacks, sticky status, pending exceptions, rejection atomicity,
unknown payloads, overflow/underflow boundaries and double-rounding counterexamples.
Unimplemented exception delivery or memory-fault partial completion stays
explicitly refused. Native original instructions, generated code, an ARM64
portable run and a symbolic/refinement result are distinct evidence kinds.

Reusable qualification must bind binary/generated hashes, semantic profile,
backend, CPU/ISA, compiler/runtime/options, environment, seeds and assumptions.
Changes invalidate the corresponding evidence. Numerical equivalence, state and
exception equivalence, translation correctness and termination remain separate
properties. Performance claims require measurements for each precision mode.

## Portable numerical building block

`ir/low/X87Arithmetic.h` provides `evaluateFiniteX87Arithmetic` for canonical
finite binary80 ADD/SUB/MUL/DIV operands. It uses existing LLVM APInt integer
arithmetic and exact rational division, followed by one PC/RC rounding with the
extended exponent range. The tininess predicate uses a separately calculated
unbounded rounding; the bounded result is rounded from the original rational.
This follows the numeric-underflow definition in the
[Intel SDM changes, revision 079](https://cdrdv2-public.intel.com/851055/252046-079-sdm-change-document.pdf).
No host floating type, new arithmetic dependency or host x87 instruction is used.

It returns raw result bits, newly raised arithmetic status bits and the numerical
C1 rounding direction. Nonfinite/noncanonical operands and invalid controls
return no result. The output is a masked numerical result, including zero-divide
infinity or the 0/0 indefinite value; an instruction owner must decide whether
any result commits under the actual masks and state. It cannot silently apply
this result to an unmasked exception. Control-word validity is shared with the
existing concrete context through `X87Control.h`.

`X87Binary80.h` owns raw encoding classification shared with the existing
partial-remainder evaluator; decoding preserves unsupported encodings for each
instruction to admit or refuse. This numerical building block does not itself
initialize a stack, authenticate incoming state, or implement calls/environment
operations. Its portable instruction caller is described below.
Tests distinguish fixed mathematical fixtures, LLVM full-precision results,
Unicorn Haswell instruction results and native x64 instruction/state results.
The native test explicitly skips on ARM64. A passing numerical test is not
whole-instruction state or translation evidence.

## Portable owned-context slice

`NdOpEmulator::setX87PortableMode(true)` selects
`intel64-architectural-masked-v1` on a 64-bit analysis host, including ARM64.
It survives reset, requires explicit x64 owned state, and never falls back to
native x87. The inventory's `portable_profile` identifies admitted forms;
`X87PortableOperation` drives the same executor dispatch and capability query.

The initial slice admits FNINIT/FNCLEX, WAIT, FLDCW/FNSTCW/FNSTSW, FLD/FSTP m80,
FLD/FST/FSTP ST(i), FXCH, FILD m16/m32/m64, FABS/FCHS, and the D8/DC/DE
register ADD/SUB/MUL/DIV variants. FILD shares its exact conversion with typed
LowIR; it preserves all 64 integer significand bits regardless of PC/RC.
Finite FCOM/FUCOM register forms, their one/two-pop variants, FTST and the
FCOMI/FUCOMI flag-producing variants share ordered comparison semantics.
They preserve signed-zero equality, set DE for subnormal operands, commit
condition/flag results and pop only after successful comparison. NaNs,
infinities, unsupported encodings and stack faults refuse atomically; this
does not conflate FCOM/FUCOM's unimplemented NaN exception differences.
FIST/FISTP m16/m32 and FISTP m64 convert canonical finite values directly under
RC, independently of PC. The rounded integer range determines masked IE and
the destination-width integer indefinite; otherwise inexact conversion sets PE
and the rounding direction sets C1. Subnormal inputs do not set DE. This follows
the [Intel FIST/FISTP instruction contract](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf#page=458).
FISTTP remains outside this initial profile. Integer stores and their pops
commit together only after the full memory range and state are authenticated.
Arithmetic uses canonical finite operands with all exception masks set, all
three valid PC settings and all four RC settings. Raw m80/register transport
preserves all ten bytes, including NaN payloads and unsupported encodings.
Using such a payload in finite arithmetic remains an explicit refusal.
All data instructions in this initial slice require all six exception masks,
including raw transport, FILD and sign changes. FLDCW admits only all-masked
input; nonwaiting control/status operations retain their separate behavior.

The existing FXSAVE64 context remains authoritative. TOP changes rotate logical
slots while preserving physical payloads, physical tags and the physical
known-payload mask. A pop retains the empty register's bits. FNINIT resets the
environment and tags but never invents payload provenance. An instruction only
requires known payloads for the operands it reads; unrelated unknown slots do
not become known. Waiting pending exceptions precede address evaluation.
`preflightPortableX87Context` owns instruction/profile and environment checks
that an adapter must run before operand access; execution repeats them before
publication. Native and portable paths share the pending-exception predicate.
Preflight success authenticates neither payloads nor guest memory and is not a
reusable execution receipt. Exact binary80 adapters can use the translation
memory owner's transactional byte-span operations rather than split stores;
those operations alone do not select the separate generated or translation
interfaces.
The checked `X87PortableRuntime` C++ adapter uses a separate versioned state
with explicit environment-known and physical-payload-known fields. It requires
an authenticated incoming environment even for FNINIT; it does not synthesize
one from an ABI or initialize unknown retained registers. It validates the full
store operand before evaluation, and publishes memory and x87 state only on a
completed instruction. Self-modifying stores commit their corresponding pop
before reporting invalidation. The caller must bind a generated/translation
profile separately; the adapter does not qualify such a profile by itself.
`neverd_x87_runtime` is the separately linkable C bridge for generated-code
work. `X87PortableABI.def` owns its two exact symbols and declarations; the IR
signature view derives types from those declarations. Status values come from
`X87PortableStatus.def`, and calls return `uint32_t` independently of enum
storage. `nvd_x87_v1_step` takes an explicit opaque execution context, selector
and numeric guest IP/DP. It returns a stable status and retains only that
instruction's diagnostics. `nvd_x87_v1_result` consumes a scalar result once,
after a successful producing step; missing, stale or repeated reads terminate.
A failed or nonproducing step invalidates the previous result.

Trusted C++ embedding code constructs `PortableX87ExecutionV1` from separately
owned state and checked memory. Both owners must outlive the context, remain on
one execution thread, and match the runtime source build. Generated code may
only receive that context, never manufacture or reinterpret its internals.
The bridge catches C++ exceptions as `internal-error` before any instruction
commit. Its closed export list contains only the two C helpers; it does not
publish a stable C++ SDK or independently qualify generated/translation profiles.

The explicit `preparePortableX87Function`/`emitPortableX87` C++ route prepares
an ABI-recovered x64 owned-context integer leaf for this bridge. Source arguments
become numeric guest values, followed by an execution pointer and a `uint64_t *`
source-result argument; the C function returns `uint32_t` status. Each failed
step returns immediately, preserving earlier completed effects and leaving the
source result unchanged. A self-modifying store commits its bytes and pop,
returns invalidation status, and does not continue to a later instruction.
Only complete known 64-bit source return operands can be published. Calls,
external state handoffs, floating source ABIs, unsupported x87 forms and ordinary
guest-memory operations are refused before emission. Private control labels
never become original-address rewrite evidence. This alternate interface does
not change ordinary decompilation or enable translation admission.

Invalid controls, unsupported forms/values, unmasked data-instruction controls, stack
faults and unauthenticated memory refuse without committing state or a store.
Unqualified MMX and external save/restore boundaries also refuse in lenient
tracing mode; selecting the portable profile never turns them into skipped work.
`X87PortableOutcome` provides distinct refusal reasons at the semantic boundary.

The selected Intel 64 architectural contract updates FIP for non-control
instructions and FDP for their memory operands. Masked operations retain FOP;
register operations retain FDP and undefined C0/C2/C3 bits. These retained
undefined fields are not a claim of a particular CPU's bits. Control operations
retain pointers except initialization. See the
[Intel SDM, volume 1, sections 4.9.2 and 8.1.8–8.1.9](https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf)
for exception priority and environment definitions. The capability matrix keeps
complete architectural-state qualification unknown pending broader validation.

Independent references have explicit limits: this repository's Unicorn/QEMU
reference preserves numerical bits, tags/TOP and FIP, but does not fully expose
arithmetic exception flags/C1 and serializes FDP as zero. LLVM APFloat omits
the x87 overflow flag when directed rounding saturates to the largest finite
value. Neither reference supplies those missing observations. Fixed boundary
fixtures and original-instruction replay check the separately defined flags.

`scripts/check_x87_arithmetic_replay.py` freezes a numerical differential run:
24-byte requests contain operation, CW and raw operands; 12-byte responses
contain raw results and SW. All inputs/outputs, driver/source SHA-256 hashes,
declared build commands, backend/CPU metadata, environment, seed and mismatches
remain in the output directory. Rosetta identifies itself separately from a
native x86-64 process. The protocol compares result bits plus IE/DE/ZE/OE/UE/PE
and C1; it is not whole-state, generated-code, termination or formal evidence.
Recorded process time includes startup and file I/O, not application throughput.


## Portable translation and resumption

`TranslationRuntimeProfileV1::PortableX87` is an explicit C++ x64-to-native-Linux-
AArch64 ELF profile. It requires the existing scalar capability plus X87 and
an authenticated `neverd.x87.portable-v1` extension in the logical `GuestState`.
The extension serializes the versioned 528-byte portable image and provenance
in explicit little-endian order. It does not infer state from an ABI, FNINIT,
tags or arbitrary bytes. Import/export is transactional and preserves unrelated
extensions; trusted embedding code owns authentication.

The composed runtime state retains the original 160-byte integer layout and
adds portable x87 state plus the last-step diagnostic. The legacy integer ABI,
scalar memory protocol and default runtime factories remain unchanged. Old
integer invocation cannot enter the larger profile, even when an individual
block contains only integer instructions. The C/Python/CLI translation APIs
continue to publish scalar-only objects and expose no execution session. Their capability
queries, worker and GUI nevertheless report this C++ interface and its limits.

An x87 block ends at its first admitted x87 instruction. Integer effects before
that instruction are flushed, the terminal helper commits at most that one
instruction, and the runtime validates its selector, PCs, completion, state,
memory extent and scalar result before another block can execute. FNSTSW AX
preserves the upper RAX bits. A refused instruction does not advance RIP; a
completed instruction does, including stores that trigger code invalidation.
Budget checkpoints preserve explicit state across resumption. This is a bounded
execution/resumption contract, not a proof that an arbitrary loop terminates.

`isPortableX87RuntimeInstruction` owns the shared selector gate used by capability
queries, generated preparation and translation. Step V1 has no integer-flag
operands: the concrete interpreter's FCOMI/FUCOMI support does not admit those
forms at this boundary. Unsupported x87 instructions, floating source/call ABIs,
MMX and general save/restore operations remain explicit failures. Rejecting an
unsupported later block preserves the last completed prefix.

The object, runtime registry, cache identity and checked invocation bind the
runtime profile, state layout, helper protocol and exact-span memory policy.
Registry shape identity cannot authenticate a forged helper address. Object
publication alone is not permission to invoke: the actual linked object needs
matching code-generation credentials and memory policy. Only
`RejectExecutableWrites` and `InvalidateOnExecutableWrite` are admitted. The
former rejects writable executable mappings during session creation;
`ValidateBeforeDispatch` is refused for this profile. A completed executable
store invalidates its credential even when cancellation takes exit priority.

## Measured workloads

`scripts/benchmark_x87_portable.py` requires passing frozen generated evidence
from the same clean source, host and runtime before loading its authored shared
libraries. It measures the LLVM, HighC and LLVM-C leaf, branch and two-iteration
backedge bodies at source-default/host-O2, both exact-only and with every 32nd
call deliberately refused on a separate qNaN context. Every call checks its
status and source result; final raw state and memory must match frozen execution.
There is no fallback dispatch, and refusals are retained in the measurements.

Raw 80-bit C-bridge load/store pairs and logical state import/export are separate
transport measurements. Native AArch64 additionally measures complete session
creation, compilation, linking, execution, snapshot and unload against the frozen
session result. Defaults are five alternating-order trials, 16,384 calls for
bounded workloads and four requests for complete sessions, pinned to one allowed
logical CPU without changing the governor. CW is 0x037f and TOP is 5. Timing
includes status checks; Linux VmHWM measures the whole process including loaded
libraries and fixtures. These are scoped throughput and memory observations,
not isolated allocator costs, a speedup claim, all-control performance coverage,
or third-party application compatibility. No approximate mode contributes to
exact qualification.
