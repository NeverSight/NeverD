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
Worker/GUI clients can consume this C API; no new GUI workflow is claimed.

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

Transcendental and partial-remainder results can be processor-dependent.
Concrete execution refuses them in strict mode; explicit native-reference mode
labels its observations approximate. They cannot supply portable exact or
checked-refinement evidence. Emitting a native instruction also does not
qualify processor-independent numerical results.

The portable concrete slice below is explicitly selected and instruction scoped.
There is still no qualified x87 cross-architecture translation profile; mixed
supported/unsupported programs retain their refusal gates. LLVM and both C
output paths must acquire an explicit portable runtime contract before
publication on other hosts. No approximate reduced-precision mode is introduced.

## Failure and future portable admission

Unknown incoming architectural state remains unknown. Waiting boundaries refuse
pending unmasked exceptions before address evaluation. Memory operands require
authenticated full ranges and permissions, including stores suppressed by an
x87 fault. Invalid control fields, missing payload provenance and unimplemented
effects cannot produce a plausible numerical value. Refusal must leave state,
output and memory unchanged.

The first portable slice must name its exact encodings, finite/nonfinite input
domain, rounding points, exception masks and environment policy. Raw m80
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
Arithmetic uses canonical finite operands with all exception masks set, all
three valid PC settings and all four RC settings. Raw m80/register transport
preserves all ten bytes, including NaN payloads and unsupported encodings.
Using such a payload in finite arithmetic remains an explicit refusal.

The existing FXSAVE64 context remains authoritative. TOP changes rotate logical
slots while preserving physical payloads, physical tags and the physical
known-payload mask. A pop retains the empty register's bits. FNINIT resets the
environment and tags but never invents payload provenance. An instruction only
requires known payloads for the operands it reads; unrelated unknown slots do
not become known. Waiting pending exceptions precede address evaluation.
Invalid controls, unsupported forms/values, unmasked arithmetic controls, stack
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
