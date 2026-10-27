# Remaining x64 C-quality work

Use public fixtures and emitter tests as the durable contract. Private binary
analysis and comparison output belong outside the repository.

## Open

Shared MedIR flag folding preserves both subtraction operands even when they
are equal. Treating `A-A` as `A-0` changes branch results and invents incoming
register dependencies. `NeverDSelfCompareFlagTests` compares original machine
code against lifted code, HighC and LLVMC on x86, x64, ARM32 and AArch64;
logical tests and shifted operands remain input-dependent controls. This does
not resolve genuine hidden machine-state inputs at VM continuation boundaries.
The same SSA identity rule now serves compound-condition rewriting and direct
boolean flag propagation. Unknown operands disappear only when the result is
independent of their bits; unspecified occurrences stay distinct.

Captured HFA callees retain their checked source record signature, matching
argument reassembly and result unpacking. The saved function pointer remains
the call target, including when the current import slot can no longer name it.

Whole-image audits must reconcile the detector's final catalog with the input
inventory and retain every failure record. A loader boundary guess refuted by
completed discovery is different from a real function rejected by lifting;
stated symbols and user identities cannot be removed by that reconciliation.
COFF instruction-content flags and page execute permissions are separate.
Full source emission is not proof of executable kernel reconstruction.
Discovery and CFG recovery now share no-return evidence for loaded targets.
A loader import binding or a unique immutable relocated code pointer can
transfer its target's fact; writable, conflicting and user-overridden pointers
do not acquire it. Public fixtures cover both C routes and their continuations.

Mapped LLVM C pages now leave EH tokens without a storage extent instead of
querying DataLayout for an unsized type. The same public exception fixture
checks mapped/unmapped source identity and executes both continuations. SEH
units include their platform declarations, name filter callees and retain
implicit exception-code assignments without colliding with header macros.

Debug-named callback storage remains a loaded target unless loader import
evidence binds its identity. Switching the slot at runtime is covered on
COFF and ELF. HighC call results use only an admitted declaration, including
header-owned record argument adapters; a rejected debug prototype cannot
silently change an integer carrier into a pointer result.
Function-address expressions now resolve their final allocated identifier.
Reconstructed data steps aside from an external function's linker label, and
LLVM C declarations retain the same recovered symbol used for name allocation.
Direct calls, mutable callback slots and address-taking execute against one
external provider even when their source spellings collide.
Deferred unknown copies also preserve a declared void call as an effect and
clear the destination availability. Both source routes execute the call once;
an overwritten result becomes known, while reading an unavailable result
retains the explicit guard.

LLVM C prefetch rendering now checks the full owned assembly contract and
retains computed memory operands and FS/GS overrides. All six hint forms
assemble at O0/O2; baseline read hints execute, while write hints and segment
forms do not assume host features or segment bases. Altered constraints,
dialect, effects, stack alignment and address width remain refused.

CLAC/STAC, XEND and CLZERO now share the validated system-instruction contract in
HighC, native LLVM and LLVMC. Their ordered effects survive unused results and
source optimization; scalar emulation cannot silently claim completion.
Instruction IDs remain unchanged, and native RTM code generation enables its
required feature. The architectural returns — far, interrupt and syscall
returns — now have the terminal state-transfer source boundary described below.
XBEGIN rel32 lifts as a two-exit instruction: the `_xbegin()` status, EAX
written only on abort, and a branch to the encoded fallback, which keeps the
original abort continuation and checkpoint in both C routes. The 16-bit form
truncates its fallback and RSM stay refused.
CLZERO's implicit address no longer depends on Capstone exposing a register
operand. Address width, FS/GS offsets, unchanged flags and native cache-line
effects remain explicit; generated code does not replace the instruction with
an eager fixed-size clear. Native cache-line writes on CLZERO-capable hardware
remain a separate qualification from fault and register-transport checks.

Large LLVM source functions no longer retain redundant entry stores on private
SSA temporaries whose single later definition dominates every read. The proof
runs after exception edges are installed; bypasses, loop-first reads, ordering
and escapes keep their unknown seeds. Public raw-IR regression checks protect
this promotion cost without changing a function's timeout or skipping output.

The source routes now preserve original-image byte-storage identity, finite
bit projections, native byte shuffles and SHA-256 vector operations. Split SEH
scopes no longer depend on LLVM block order. Unsupported scope arrangements
return an emission error instead of aborting the process. Independent SEH
entries can retain an explicitly unknown runtime stack in source LLVM, as
HighC already did; this does not recover that stack or authorize native output.
Bounded callee register summaries repair stack-probe frame proofs. Replaced-PC
tail calls and closed recursive internal-call groups have separate checked
return-slot proofs. Arbitrary changed-stack returns remain outside them.
Own-interior call proofs keep the x64 stack pointer across ordinary calls, end a
path at a direct tail call into a callee proved never to return, and treat a
call inside another function's unwind record, reached through a jump into that
body, as interior to that record. No-return screens skip returns that only a
terminator or a known no-return call precedes and no branch targets; far and
interrupt returns no longer count as returns to the caller. Entry-stack offset
proofs follow long copy/displacement chains without a depth limit, and CALL
return-slot observations accept an upper bound below a dynamic allocation in a
loop. A live-frame jump through a dispatch thunk and a computed jump into an
unrolled copy remain refused.

x86 architectural returns now lift to a terminal, non-returning state-transfer
intrinsic that names every cell the transfer reads. Far and interrupt returns
(RETF/RETFQ, IRET/IRETD/IRETQ) name the stack frame — CS:IP, plus FLAGS and, for
64-bit IRET, SS:RSP; the syscall returns SYSRET/SYSRETQ and SYSEXIT/SYSEXITQ
name their transfer registers — RCX:R11 and RDX:RCX. Both C routes print
`neverd_far_return`/`neverd_interrupt_return`/`neverd_syscall_return`/
`neverd_sysenter_return` and the shared no-return termination contract ends the
block; the intrinsic grants no source ABI or memory contract and reconstructs
no privilege, segment or flag change. An ordinary C return, a bare `iret`
assembly statement, or an opaque call with only a stack-pointer operand still
loses that observable machine state and stays rejected. RSM (whole-processor
restore from SMRAM) has no such named target and keeps the explicit refusal in
permissive decoding and both source routes; old RSM IR cannot bypass it through
direct emission. Native
reconstruction of the architectural returns remains a
separate contract this source boundary does not grant.
Known fixed argument positions now
survive an interior stack-setup gap, including selected LLVMC recovery; missing
values remain explicit rather than becoming zero. Unknown incoming stack-pointer
escapes still prevent a complete stack-argument bound: do not cap calls
by a private routine's name or treat a sparse outgoing slot as proof that
later arguments are absent.

Windows provider declarations now include NDIS, filter manager, NETIO,
storage, USB and HID APIs, with explicit x86/x64 layout and SDK HighC/LLVMC
coverage. User libraries include more networking, security and device APIs.
An export inventory is only a coverage audit; undocumented and ordinal-only
exports still require authenticated declarations. The retained Microsoft
SDK/WDK input supplies x86/x64/ARM64 declarations; the older MinGW DDK input
alone has no ARM64 kernel headers. Input availability is not full ABI coverage.
Runtime Windows kernel type joins now associate official opaque executive
pointer aliases with complete program PDB records when their kernel prefix
matches at offset zero. Both C routes recover the executive members through
the same local types. Existing aliases and incomplete or mismatched records
remain unchanged. WDK/kernel version conflicts stay visible as opaque records;
they do not authorize importing an incompatible layout.

The shared call-read summary excludes register copies proved overwritten before
observation, including CPUID scratch exchanges. Calls, exits, partial writes
and exceptional edges keep their incoming evidence. A VM entry that stores
registers into an escaping machine-state frame can still have genuine inputs
unknown after earlier calls; never erase those inputs or replace them with zero.
Source stack preparation now also discards post-entry scalar spills proved
overwritten before observation. Explicit x86 flags transfers remain emitted;
their frame loads/stores are already separate operations. Unread scratch
restores require a register overwrite before a later call, a checked absence of
callee input observations, or a normal exit;
ABI-preserved/result registers, PHIs and implicit clobber readers keep their
values. This bounded shared proof removes avoidable HighC/LLVMC traps but does
not recover escaping VM state or authorize a dynamic code-address fallback.
The separate call-input proof does not mistake an inferred argument list or a
nested may-write for absence of an observation. It fixes a HighC scratch-save
trap that LLVMC's later cleanup already removed; both routes execute the public
preserving-callee case. Opaque calls and unmodeled entry observers retain state.
Pure POPCOUNT steps used to compute x86 parity no longer prevent an otherwise
complete explicit scratch overwrite proof; trapping operations still do.
Physical CALL return-slot writes remain barriers: they are absent from the C
frame projection. Discarding an old save solely because CALL overwrites its
bytes can turn a later return-slot read into an uninitialized C value. A full
source projection of the new return-address value is required before admitting
that optimization; runtime code-address relocation remains a separate proof.
Shared source preparation now rejects exact x86/x64 loads reached by those
unrepresented physical CALL writes, including when the old frame content is a
known constant. Complete later stores permit the read; partial writes, joins,
loops and unknown call positions retain the diagnostic. Both source routes use
the same bounded check. This closes stale-value success, not the missing
machine-state/continuation source ABI, unresolved aliases or native proof.
The shared stack-address owner also recognizes canonical zero-extended
32-bit memory-address containers. This closes the i386 observation gap without
treating truncated 64-bit pointers or wider incoming registers as frame roots.
Both spill proofs share a checked private-frame extent predicate: extreme
positive offsets cannot wrap into frame storage or Windows home slots.
Post-recovery Win64 scalar leaf borrows now have a shared complete-use proof.
Only unread private bytes can lose their stores; negative offsets, implicit
register inputs and source call home areas retain observations. SSA version
zero alone is not an incoming register. Nested callees, incoming pointer stack
parameters and opaque external effects remain unsupported by this proof. The
ntos `StDmpSinglePageAdd` gap therefore still requires broader authenticated
pointer effects and per-slot incoming argument reads; a leaf fixture is not
evidence that this original function is fixed.
HighIR now keeps the shared direct-call argument boundary: its secondary setup
scan cannot append neighboring stack locals after a complete recovered list or
replace an unavailable interior operand with a stale local. This removes a
route-specific source of extra arguments; it does not prove those interior
operands unread.
Integer call recovery now retains the complete SSA operand prefix published by
callee-read summaries on x64 COFF, ELF and Mach-O. Authenticated fixed scalar
stack declarations now bind their exact-width operands before SSA, using shared
source parameter placement. Predecessor stores survive ordinary calls,
recovered branches and tail jumps through both C routes on x64 COFF/ELF/Mach-O
and ordinary ARM64 COFF/ELF calls. Checked local stack declarations use the same
LLVMC adapter, and changing a parameter width invalidates cached output even
when its argument count stays the same. Apple ARM64 packed stack placement,
x86/ARM32 adapters, nonordinary conventions and general incoming machine-state
projection remain separate gaps. Explicit x64/ARM64 source stack declarations
now initialize their exact entry bytes in the shared MedIR frame, including
floating bit transport. Bounded complete PE/ELF leaves can derive aligned
1/2/4/8-byte integer stack inputs automatically, including restored RBP or x29/x30 frames
and explicit local stack allocation. The shared byte proof checks original
restoration against the current format's preserved bank; saved state, frame
escape, external memory and unaligned carriers cannot become such inputs.
The memo retains each input width, signed/unsigned loads preserve their bits,
and narrow declarations do not initialize adjacent bytes. Mixed prefix and
complete reads share one scalar carrier only when the original LowIR contains
a complete read of that same slot; narrow-only evidence cannot widen it. Every
selected caller must completely write the same outgoing bytes. Selected-only
LLVM exports retain fixed declarations rather than falling back to varargs.
These rules do not supply an arbitrary VM dispatcher with a missing bytecode
cursor or hidden register state. A bounded source-only
proof marks a complete declared stack operand unavailable when any required
private-frame byte is never written on any incoming path. Possible aliasing
writers, prior calls, incomplete graphs and query backedges prevent that proof;
path-dependent initialization and generic partial loads remain outside it.
This does not give native reconstruction an initialized frame. ARM/ARM64 count-only contracts
still require actual setup values. This repairs operand loss; it does not
recover an opaque callee's observable exit-register state. Native reconstruction
still models unproved call clobbers with `freeze undef`.
C source paths retain wholly unknown clobbers and MedIR `Unspecified` values
through a shared opaque marker, so source optimization cannot choose zero.
Mixed integer PHIs carry availability separately in HighC and LLVMC. Copying
an unknown incoming value, including across a loop edge or integer-width view,
does not trap until it is read. `CallExitRegisters` executes guarded reads,
parallel PHI swaps and native BSF/BSR loops with known and clobbered destinations.
Pure local copy cycles now require a rendered observable reader to remain
live. Unused void-call result copies disappear while the calls and their
memory effects remain; returns, call arguments and indirect targets retain
their input chains. A declared void call whose result is genuinely observed
still needs a checked source/result contract.
HighC store/assignment printing no longer overrides unknown expressions with
zero. This exposes missing state; recovering its actual dynamic contract and
partially preserved vector suffixes remains open. Generic FP/vector summary
operands need the same audit, including register-bank and scalar-width
preservation.
Scalar-only summaries now preserve explicit zero vector demand through
deferred ABI recovery. Residual XMM writes and unknown vector PHIs cannot add
an argument when the complete read summary excludes that bank. Unknown demand,
real floating inputs and overlapping register-bank reads stay distinct; this
does not recover genuinely observed clobbers or partially preserved lanes.
Speculative floating arguments now share the integer call-leftover proof:
a PHI containing only whole call clobbers supplies no argument setup. Known
FP demand, mixed joins, partial preservation and possible return fields stay
visible, including before a result-producing block is visited. Both C routes
remove the phantom callback parameter in ntos `vDbgPrintExWithPrefixInternal`.
The original `StDmpSinglePageAdd` still needs the separate byte-observation
proof above. `KiDispatchException` also retains independent LLVMC unknown
machine-state operations; count both unknown-value and unknown-machine-state
markers when reporting residuals.
Bounded call-free x64 callees now publish scalar constants agreed at every
return, before SSA, through the shared register-effect summary. Ordinary ABI
result registers, addresses, exceptional roots and unknown paths remain outside
that proof. Public caller stores execute through both C routes and real PE/ELF
consumers, including cached query order. Complete register-only CPUID leaves
now have a bounded source expansion: the shared converter retains their extra
exit words, proves the written ABI-preserved GPRs unchanged, and emits every
query once with all outputs sharing that query. The original CALL-slot guard
runs first; both C routes use the same projected SSA. Cached order, repeated
calls, high-byte reads and independent native-query oracles cover x64 inputs
across COFF/ELF/Mach-O. Runtime entry dependencies, memory/stack-using leaves,
other hardware effects and general VM state still require a real call-state
contract. Native lifting keeps its original calls. An explicit unsupported
expression does not certify those dynamic values or executable reconstruction.
Complete header and explicit local FP/HFA declarations share the same checked
carrier adapter. ARM64 cached imports retain the original loaded callable
through bounded private-frame proofs. Selected LLVMC output includes the
checked import declaration and its by-value record definitions even when the
callee is reached only through an image pointer mirror. The public record
matrix checks both declaration origins, both C routes, selected-source target
compilation, and independent host/AArch64 execution; it does not establish
integer/void HFA result contracts or native Windows execution.
Scalar stores and speculative integer call arguments now reuse the LLVM
constant address policy before treating low mapped bytes as rebuilt pointers.
Explicit address and loader relocation evidence still permit genuine low data
addresses. Cross-architecture/format boundary checks cover scalar, unknown,
address and data-address occurrences, direct/indirect calls and copies; ARM64
PE/ELF consumers independently execute both C outputs at O0/O2.
Register-only local scalar import declarations now reuse the checked shared
carrier layout in LLVMC. Both C routes retain authenticated portable pointer
qualifiers, including nested const and volatile pointers. Counts, carrier
widths, calling conventions and observed results must agree before using the
source declaration; incompatible calls remain unprojected regardless of query
order. Public PE/ELF workers and independent C providers cover x64 and ARM64,
including void, pointer and narrow integer returns. These source contracts do
not recover missing machine state or certify unsupported calling conventions.
The local type bridge also gives scalar call conversions the prototype's
integer spelling: equal-width `long`, `long long` and fixed-width aliases must
not produce incompatible pointer arguments on a different host data model.
Readonly scalar loads among instruction bytes reuse the immutable code reader's
mapping and relocation checks. Default source cleanup also folds exact integer
idempotence and absorption without symbolic search. Runtime-indexed memory and
unresolved relocatable code identities remain separate recovery requirements.
Same-width signed/unsigned views no longer hide word absorption. Consecutive
scalar offsets combine modulo their declared width; negative-literal spelling
retains scalar provenance and leaves explicit high kernel addresses intact.
This source cleanup does not resolve a VM target that depends on runtime data.
Default MedIR propagation now folds same-width word absorption using SSA
identity before either source route expands expression trees. Changed SSA
versions and narrowing conversions remain distinct; named producers retain
their effects under the existing DCE rules. Public x86/x64/ARM/ARM64 MedIR
contracts cover all three formats, with independent host execution of x64 and
ARM64 PE/ELF C outputs. This reduces repeated dispatcher arithmetic but does
not recover its runtime inputs or prove a unique callable target.

Mapped LLVM C now keeps array and record allocations out of scalar variable
typing, including frames larger than the supported integer width. HighC and
LLVM C annotate image blocks with the shared string scanner's text and encoding
without changing memory accesses. `NeverDCSourcePresentationTests` covers these
contracts, including UTF-16/UTF-32 byte orders. Bounded, call-free stack-byte
observations now show reconstructed terminated text in both C function bodies,
including separate vector writes. Exact alignment guards remain in the code.
Unknown calls, cycles, frame-derived stored values (including some security-cookie
prologues), and arbitrary runtime decoding still need separate proofs.

LLVM C also places recognized text beside ordinary and volatile image reads
inside the function body, which remains visible when the GUI folds image
declarations. Dynamic pointers and disabled comments stay unannotated.
Exact scalar branches now reconnect discontiguous and backward code while
preserving independently known tail calls. Worker navigation uses complete
recovered instruction ownership to select the original function when a user
lands on a fragment; it does not invent a new callable entry or its live-ins.
Saved code items retain this bounded owner lookup on literal GUI navigation.
Manual independent function definitions still take precedence; correcting one
does not erase its saved instruction boundaries or invent missing VM state.
The public constant-branch and worker chunk regressions cover these boundaries.

| Gap | Next check |
|---|---|
| LLVMC Windows EH remains goto form | Keep recovered filters, nested cleanup order, and both normal and unwind edges visible. Compare the public `seh_probe` and C++ EH corpus output with HighC. |
| LLVMC output may retain avoidable frame references or control flow | Reproduce each issue with a public synthetic LLVM fixture. Keep a negative test for any value or edge that must remain printed. |
| Source types and member calls remain partially recovered | Use authenticated PDB/TPI type and method metadata; do not infer a virtual method name from a vtable offset alone. |
| Release performance varies by function and debug input | Time paired fresh `--func` calls and separate load from decompile cost. Profile before changing shared analysis. Preserve byte-identical C and EH semantics. |
| UCRT declarations do not yet imply complete source-call coverage | Use `scripts/check_windows_ucrt_calls.py` with retained official inputs. Pointer qualifiers, variadic prefixes, pointer-only record/union tags, mixed scalar banks and x86 float/double stack arguments now have public contracts. Declared x87 scalar results retain TOP and exact source precision; real extended arithmetic still requires an 80-bit C type. Dense ARM64 HFAs retain their complete source groups in registers and ordinary COFF/ELF stack slots, and all one-to-four float/double result fields. The HFA adapter still requires an FP/HFA result; integer/void-result variants, padded or over-aligned records, Apple stack packing and nonordinary conventions remain outside it. Checked HFA results now retain their register-record shape in HighIR, avoiding artificial 192/256-bit C integers. Both C routes have Windows ARM64 target compilation, host sanitizer execution and freestanding Linux AArch64 execution matrices for import thunks/slots and single/chained calls; native Windows runtime execution remains separate. A wrapper without a source return type or observing caller still cannot infer a source aggregate result merely from residual FP registers. The strict SDK check independently compares anonymous enum values/layout and compiles the unmodified standalone output too; compiler success alone does not certify execution. |
| Toolset coverage varies by hosted runner | Verify each compiler's actual version and path before publishing corpus artifacts. Treat unavailable versions as explicit skips. |
| Interior code-address bits have no portable C or general LLVM blockaddress contract | Preserve precise CLI refusal and existing-file protection at full/selected optimized/unoptimized emission. Other atomic functions must still emit. LLVM verification does not prove bit observation; close opaque transport versus inspected bits at the shared LLVM owner. |
| Explicitly lifting an interior fragment still lacks its caller's machine state | Navigate through authenticated recovered ownership when available. A forced standalone fragment must not invent missing register or stack inputs; executable-section data addresses alone do not establish a relocatable function identity. |
| MinGW x86 call arguments can remain unknown after function boundaries are repaired | Inspect the recovered callee ABI and each caller's actual register/stack evidence. Do not treat residual unknown arguments as evidence that an internal label is still a function. |
| The public x64 `xcpt4` source projection retains unresolved runtime stack values | The analysis matrix now retains the body and explicit unknown entry values. Native reconstruction still needs authenticated effects across opaque imports and cross-funclet runtime activation identities. ABI declarations alone do not preserve saved memory. |
| Native reconstruction of PE32 `xcpt4` nonlocal invocations remains incomplete | Source registration-state closure and local filter recovery are fixed by #920 / #859. CFG, SSA and native emitters still need to consume the source capture/resumption and invocation-exit evidence. A source receipt does not certify a replacement runtime activation. |

The original MSVC nonlocal-finally fixture now includes a second exception
immediately after parent resumption and a guarded caller-owned counter helper.
Its schema 2 oracle checks the x86 chain at each return and at the resume label.
The new reentry oracle permits a nonlocal jump only during ordinary execution;
exception unwinding completes its finally before raising the second exception.
Jumping out of an abnormal finally is MSVC C4532 undefined behavior. The legacy
compiler observations do not certify guard retirement inside the resumed parent.
`check_windows_native_seh_resume.py` is a separate x64 LLVM/CRT protocol
experiment with a physical-stack capture and a parent continuation address.
Its explicit escaped resume token selects the ordinary suffix at the call
continuation. An address-taken join alone allowed O2 to sink suffix effects and
require a predecessor-only PHI at hidden entry. Local Wine O0/O2 observations
are supporting evidence only; native Windows and compiler-visible resumption
still require their own validation. A second ordinary finally call in the same
parent activation checks token reinitialization. Arbitrary live SSA values,
nested activations and EHCONT/CET compatibility are not authenticated. The independent
`check_windows_native_seh_bridge.py` tests a fixed assembly-owned frame and two
native continuation labels, entering optimized LLVM bodies through ordinary ABI
calls. Local Wine O0/O2 covers eight paths and 512 positive calls, plus six
corrupt compiled controls. Raw abnormal bytes, event counts, actual codes and a
persistent unexpected-event flag remain independently observable. Native Windows
is pending; no source-memory, arbitrary nesting, concurrency, EHCONT/CET or
production reconstruction authority follows. LLVMC refuses block addresses and other
unrepresented constants instead of silently substituting zero.
Compiler or native-runtime success alone grants no reconstruction receipt;
conditional memory, invocation lifetime and both C routes remain open.

## CRT and catch-continuation repairs

- Return contracts require the shared no-return proof before interpreting an
  absent ordinary RETURN as void. An external catch/continuation keeps the
  parent result unknown until attachment, so nested rethrows retain the
  catch object's returned value. Signed 32-bit SEH filter casts preserve the
  continue-execution sentinel when the frame carrier is unsigned.

- Source-written Win64 frame formats now retain exact reaching scalar bytes.
  Every predecessor and inspected callee return must agree, and the complete
  format through NUL must remain live outside runtime home/scratch areas and
  return PCs. Image and frame formats share one Windows grammar. Initial
  mutable-image bytes and language handler entries still supply no memory
  authority. #858 source analysis is covered by #969; native reconstruction
  still needs those memory contracts.
- Windows SEH runtime-memory proofs use their own bounded scalar printf
  grammar instead of Darwin/Objective-C argument typing. Positional and
  foreign dialects, dynamic widths, decimal overflow and multibyte formats
  retain refusal. Native reconstruction still needs writable-format and
  exception-activation memory contracts beyond the #858 source projection.
- Object borrows, local-finally writes and catch-object construction share the
  source solver’s administration-boundary check. SEH SavedESP and exception
  pointers, discontiguous EH4 cookies and realigned saved-entry anchors cannot
  become scalar objects, even when no later reload exposes the overwrite.
  Whole and partial overlaps reject; adjacent initialized objects stay valid.
  These boundaries do not close the remaining conditional incoming-object or
  nonlocal-invocation proof.
- COFF parsing and LowIR share EH3/EH4 scope-index and frame-sentinel rules.
  Registration analysis rechecks normalized initial levels, backward enclosing
  links, handler/filter roles and PE32 code-address width before publishing
  facts, even when parser status is complete. Twenty-four malformed-record
  mutations reject without frame facts; initial-active and empty-table controls
  retain valid sentinels. Byte, executable-ownership and frame-installation
  proofs remain separate.
- Single-function source LLVM emission uses the same verified promotion fallback
  as the SDK and sharded source route. Incomplete native EH contracts remain
  explicit and still prevent binary rewriting. Cross-format and architecture
  tests retain the call and contract through both optimization modes.
- PE32 source LLVM retains handler identities authenticated by immutable
  SafeSEH membership, even when a GS wrapper has additional behavior that
  prevents a forwarding-personality proof. Declaration order and sharding do
  not change that identity. Native reconstruction still requires its separate
  language, frame and callback contracts; the MSVC finally fixture matrix
  checks source LLVM independently of original-runtime observations.
- Direct PE32 EH4 publication follows bounded stack allocation, saved registers
  and cookie initialization instead of requiring fixed byte positions. Both
  pushed cookies and explicit disp8/disp32 stores retain their exact header
  offsets. Unallocated, overlapping, saved-register and runtime-administration
  bytes remain rejected; shared LowIR still proves values and lifetime.
- `scripts/check_windows_nonlocal_finally.py` supplies independently authored
  MSVC x86/x64 source/runtime baselines for conditional finally exits and
  returns during normal flow and unwinding. O0/O2 and GS variants retain their
  linker maps and exact images. Altered cleanup traces are negative controls.
  These compact inputs reproduce mixed physical returns and parent-stack
  restoration; successful original execution is not a reconstruction receipt.
- Same-frame x64 local-unwind arguments use one all-predecessor LowIR proof,
  including stable loops, partial writes, released storage and independent
  roots. Both module ownership and SSA consumers retain exact call identities;
  a whole-function dependency digest rejects stale earlier-block evidence.
  Complete decoded callee chains can preserve saved cells when every returning
  path bounds actual writes and retains every active physical return PC.
  Recursion, depth exhaustion and overlapping CALL pushes refuse the proof;
  repeated invocations use their current arguments. Transitive callee digests
  are rechecked by shared SSA. Frame-dataflow work has a separate cumulative
  budget from address-use receipts; exhaustion does not erase that inventory.
  Certified runtime-only continuation roots receive their established SP even
  without an ordinary incoming edge. This does not preserve saved pointers
  through opaque callees or prove cross-funclet frame borrowing.
- Win64 `_controlfp_s` now bounds its real four-byte output for authenticated
  UCRT queries and exception-mask changes. Partial saved-pointer overlaps,
  active return PCs, home storage, escaped frames and unsupported masks still
  refuse. PE32 and x64 share checked WAIT, MXCSR and FP arithmetic effects with
  separate x64 GPR/APX negative controls. This closes two frame-transfer gaps;
  the x64 xcpt4 case still lacks writable-format and runtime-activation proofs.
- SEH filters and finally handlers retain separate entry/search-mode states
  when decoded callback tails are shared. Each must restore its own private
  stack and parent EBP. Source-value projections intersect all activations,
  including scalar and unknown observations, under one cumulative budget.
  Parent/callback bridges still require an explicit invocation-exit contract.
  Exact registration-frame accesses visit overlapping cells only; ambiguous
  addresses retain full-frame accounting and partial pointer writes keep taint.
- The Windows EH analysis matrix requests the structured source view. Explicit
  C has a separate native-callback contract. The matrix reports every case even
  if one input fails; x86 EH4 classification now reaches its actual registration
  frame check instead of rejecting the architecture categorically.
- COFF discovery excludes debug bytes from pointer scans and records validated
  DWARF frame extents before heuristic discovery. Named, unsized functions keep
  their FDE-owned interiors; relocation labels do not create spurious functions.
- Shared MedIR evaluates bounded immutable sentinel scans and LLVM retains all
  independent roots while pruning proven dead control flow. Empty TLS tables
  and static constructor counts no longer force unsupported indirect calls.
  Mutable or incomplete tables still require an explicit supported contract.
- LLVM audits nested pointer recurrences as complete graphs and bounds repeated
  DAG analysis. Role-neutral data addresses cannot masquerade as scalar offsets;
  flat SELECT arms can belong to separately materialized data runs.
- FH3 catch continuations carry exact parent/return evidence into shared SSA.
  Matching unwind and decoded stack effects establish the continuation frame;
  synchronous x64 edges identify the throwing call and sample its register
  state. The public MSVC nested catch retains one genuine parameter and its
  resume jumps. Clang IP markers inside instructions use return-PC state lookup.
- LLVMC accepts proved integer freeze projections, supported indirect vector
  calls and relocatable constant add/subtract expressions. Global `ptrtoint`
  preserves the address; emitted Windows analysis bodies receive their needed
  FP helpers. These changes do not certify native regenerated C++ EH execution.

## Closed in the public SEH corpus

- x86 EH3/EH4 handler roots now recover the proven registration EBP in shared
  MedIR, using CFGBuilder's exceptional predecessors. Modified frame pointers
  and other unproven registers remain unspecified; HighC no longer guesses
  that an incoming EBP equals entry ESP minus four. The public x86 SEH probe
  keeps normal, handler and continuation accesses on the same frame.
- Fixed x64 SEH handlers recover their established SP in shared MedIR from
  matching unwind, decoded prologue and converted SP effects. Normal, handler
  and continuation accesses share the same local bytes on both C routes.
  Unsupported frames return an explicit pipeline error; HighC no longer
  applies a second frame-size adjustment to the entry SP.
- LLVMC preserves exact integer widths for unsigned division, remainder and
  shifts, and sign extension through typed fields and scalar homes. Generated C
  is compiled and executed at both `-O0` and `-O2`; narrowing an address cannot
  authorize forwarding from the original frame slot.
- Canonical x86 REP STOS retains element width, direction and zero-count
  behavior. Unknown assembly contracts fail explicitly. `llvm.localaddress`
  remains an intrinsic binding so target lowering chooses the correct frame
  address; it is never guessed from a C builtin.
- Literal integer guards use LLVM's width semantics. Eliminating a constant
  select preserves unconditional calls and volatile or atomic producers,
  including load ordering and pointer-slot qualifiers.
- Import veneers with authenticated IAT bindings recover the runtime callee
  name on the LLVM route as well as HighC. A null C++ throw object prints a bare
  rethrow, and neither route emits a success return after the throw.
- LLVMC now projects canonical Windows EH SEH scopes, C++ unwind/try/catch/IP
  records, and GS cookie facts as bounded comments. Unsupported or malformed
  metadata is diagnosed explicitly; the executable C remains goto form.
- HighC now keeps fixed slots and indexed accesses to the same frame in one
  byte backing store, including C++ catch funclets and SEH handlers. Public
  buffered C++/SEH cases and a fixed-write/indexed-read regression cover the
  shared bytes; a real second argument remains a parameter.
- LLVMC's fallback projection now lets the final normal try block fall into a
  `seh.try.end` marker when intervening handler blocks are printed later in
  `__except` and the edge needs no PHI copy. The public `seh_probe` regression
  keeps its conditional skip and handler rejoin jumps.
- LLVMC drops that try-end marker's now-unused C label only after the final
  function text confirms no other reference. The same public regression keeps
  the conditional skip and handler join labels that still have printed jumps.
- LLVMC moves a shared SEH continuation after the full `__except` only when
  each protected invoke's normal edge remains explicit or falls through to
  the next printed block. A two-invoke regression keeps both nonadjacent
  normal edges as labeled jumps; the final text requires each new target label
  exactly once without a name collision. Public `seh_probe` passes Windows C syntax.
- A handler-to-continuation PHI copy no longer prevents that continuation from
  moving after `__except`; the copy is emitted on the handler edge and its
  cached constant cannot replace the joined PHI in the continuation. When the
  continuation still cannot move, LLVMC projects only invoke normal edges to
  printed targets in the same fallback `__try`, checks each required C label,
  and rejects a handler jump back into that protected scope. A proven shared
  return epilogue can be inlined in `__except` without such a jump. Public
  synthetic SEH fixtures cover the PHI join, fallback jumps and inlined
  handler return, and unsafe cross-scope case.
- Public SEH sink checks follow the captured result through the exact four-byte
  volatile store into shared image storage. Both normal and handler values must
  reach that common sink after `__except`, including optimized PHI copies; a
  separate scalar global at the interior image address is no longer expected.
- A structured `__finally` omits the cleanup terminator's implicit unwind to
  caller instead of printing a return that suppresses it. The continuation
  fixture uses explicit SEH range markers and verifier-valid unwind edges.

## Closed control-flow regressions

- ARM/Thumb HLT now uses the same resumable ownership checks as BKPT/UDF.
  HighC retains its instruction and code instead of substituting a fatal C
  trap. Sized/unsized boundaries, resumed return values, all narrow HLT codes
  and ARMv8 source recompilation cover both C routes; AArch64 fatal HLT remains
  a separate contract.
- Thumb `__brkdiv0` now shares UDF's classification, immediate and void effect
  instead of becoming BKPT with a synthetic R0 result. Lifting, reachable mode
  discovery and COFF GS-wrapper recognition consume shared alias predicates.
  Boundary/source matrices and exhaustive narrow Thumb and wide ARM/Thumb UDF
  code checks cover compiler aliases as well as ordinary encodings. Wide UDF
  codes use `.w`; O0/O2 cross-compilation checks the actual ARM/Thumb object
  bytes so a source-only spelling check cannot miss an assembler rejection.
  GS-wrapper recovery follows ARM UDF #254 and AArch64 BRK #0xF000
  continuations; conflicting resumed paths prevent personality inference.
- INT1/ICEBP now retains #DB through LLVM and both C routes instead of becoming
  INT3's #BP. GNU/LLVM byte assembly and the MSVC `_emit` spelling also avoid
  unsupported assembler aliases. Native Linux x64 tests compare exception
  vectors, signal codes, event order and live returns at O0/O2; the source
  matrix covers x86/x64 and COFF/ELF/Mach-O. Other hosts skip native execution.
  ARM/Thumb BKPT retains its actual immediate in assembly through both C
  routes, instead of replacing it with the compiler's default debugtrap code.
- Owned NOP suffixes no longer manufacture a tail call after a debugger trap,
  including long runs and decoded x86 prefix/MPX-disabled NOP forms. Real
  continuations, register-changing lookalikes and independently reached suffixes
  remain live; disposable proof peeks consume the existing decode allowance.
  ARM NOP predicates retain this identity without accepting other hints or the
  separate unconditional encoding space.
- Instruction extents, trap peeks and call termination share the exact range-end
  policy. Partial instructions retain decode failures across primary/fragment
  ownership; contiguous owned extents still work. Coarse unwind spans neither
  split instructions nor erase independent symbol/code-range size evidence.
  LLVM source queries now retain the same pipeline rejection reason as HighIR,
  including the failing instruction address.
- Cross-architecture trap padding uses mode-aware ARM/Thumb/AArch64 NOP
  recognition. Malformed or missing owned continuation bytes retain explicit
  failure accounting, and cold-fragment ends bound calls/traps even without a
  separate unwind record. Adjacent owned fragments and real continuations remain
  controls across the shared metadata paths.
- Boolean-result normalization consumes the current void BRK shape and the
  shared code-sensitive termination rule. Actual lifted fatal BRKs qualify;
  debugger BRKs, missing codes, stale outputs and mismatched immutable bytes
  do not. This closes the stale consumer left by the earlier return-register fix.
- Consecutive debugger breaks preserve owned continuations. Bounded lookahead
  distinguishes unsized continuations from trap/NOP padding before independent
  entries. Padding guesses alone do not stop fallthrough, and full/selected
  pipelines absorb guesses at actually decoded instruction boundaries.
- Long and prefixed x86 `int 3` use the short encoding's void, zero-argument
  contract. ARM/Thumb `bkpt` and AArch64 `brk` define no return-register value.
  Tests keep live values across breaks through both C routes at both pipeline
  optimization settings, while service interrupt result contracts remain.
- AArch64 compiler `brk #0xF000` shares one resumable policy across decoding,
  termination analysis and source emission. Fatal BRK codes still propagate
  no-return to callers. LLVM-to-C retains raw ARM UDF/HLT assembly instead of
  silently giving it the stronger non-returning `__builtin_trap` contract.
- Resumable x86 breakpoints at known function boundaries no longer create
  synthetic tail calls to adjacent functions. CFGBuilder shares the existing
  instruction-range ownership check with calls; interior debugger continuations
  and adjacent owned fragments remain intact. Synthetic x86/x64 metadata cases
  and both PE C routes cover the boundary without changing trap semantics.
  The sibling audit also closes missing classification for long `int 3`,
  `int1`, `ud0`/`ud1`, ARM/Thumb `bkpt`/`udf`, and AArch64 `udf`; fast AArch64
  classification shares the same accepted terminator set.
- LLVMC forwards exact private scalar homes through bounded CFG reaching
  definitions. Joins require agreement from every incoming edge; cycles,
  missing definitions and excessive expansion keep explicit snapshots. Shared
  one-call cleanup tails use each edge's actual object. Generated C execution
  covers mutable homes, repeated diamonds and both cleanup paths.
- Repeated field tests remain explicit across calls that may write memory,
  including calls between a load and its first test. Read-only predicates can
  still use the existing simplification. Debug records narrower than a machine
  access keep the original frame backing bytes; a 16-byte copy through an
  8-byte debug view is checked by executing the generated C at O0 and O2.
- Ordinary scalar globals use their declared C objects for exact accesses.
  Different-width copies use the object's address, including the optional
  unaligned-pointer spelling. O0/O2 execution checks full and partial accesses;
  the SEH corpus keeps handler writes inside their recovered exception arms.
- LLVMC now invokes bounded scalar loop recovery in its source clone when no
  image/debug projection applies. Search and cleanup repeat under shared
  budgets, and the exact final body is reproved against the original. Function
  identity, attributes, callers and intrinsic bindings survive publication;
  metadata, external block addresses, collisions and exhausted work retain the
  complete original function. Native ABI and memory projection stay separate.

- Neutral select arms in admitted scalar regions now use conditional updates
  after PHI snapshot scheduling. Changed bases require conditions and terms
  independent of the old destination; unchanged bases need no self-assignment.
  Materialized conditions stay snapshots and narrow truth tests keep their
  normalization. A dominating direct entry edge may combine a returned
  carrier declaration with an immutable leaf seed; branch-dependent seeds
  retain shared scope. Independent O0/O2 checks preserve caller LLVM.

- Admitted scalar loops now name the complete returned PHI group by role.
  Counters move into `for` declarations only after all coalesced members and
  transitive inline uses pass a bounded confinement check; materialized values
  retain their own lifetimes. LoopInfo stays alive through output, and role names reserve called C
  function identifiers. Compound
  add/subtract/bitwise updates and increments follow snapshot scheduling, with
  narrow multiplication, boolean masks and reversed subtraction left explicit.
  O0/O2 regressions cover wrapping counters, escaping values and old-value reads.

- Typed scalar expressions now distinguish LLVM width from the promoted C
  type and operator precedence. Redundant unsigned casts are omitted, while
  narrow wrapping, signed interpretation and widened arithmetic/shift carriers
  remain explicit. Bounded fallback and existing materialization preserve deep
  expressions. Independent LLVM comparisons at O0/O2 run with undefined-behavior
  traps. Debug/image/composed projections and broader source naming still need
  their own evidence and cleanup.

- LLVMC plans complete bounded integer CFGs before printing nested header
  loops and conditionals. Edge PHIs retain simultaneous semantics; liveness
  coalesces only noninterfering carriers, and safe constant-seeded counters
  print as `for` loops. Header effects, shared steps, swaps and live outer
  values remain explicit. Memory, EH, irreducible and multi-exit shapes retain
  the existing fallback. This closes scalar loop rendering, not general
  devirtualization, ABI recovery or source-parameter inference.

- LLVMC keeps the true-arm exit when the false body moves ahead of a shared
  join. The LLVM fixture executes PHI and memory effects at O0/O2; a native
  four-case recovery fixture exercises the same structure through both C routes.

- Source frame admission shares bounded dynamic stack ranges across both C
  routes. Proven disjoint CALL writes no longer block fixed-frame observations;
  unknown displacements and unseeded PHI components still require an explicit
  machine-state contract. Byte histories reuse memory-event prefixes without
  raising the work limit.

- Source-only SEH handlers with a stable frame register may carry several
  protected SP positions without choosing one exceptional-entry value. The
  CALL return-slot check retains every protected and ordinary alternative.
  Live entry-register declarations survive DCE after copy propagation so both
  C routes can still prove unobserved callee-save stores. Guarded unsigned
  counter recurrences bound repeated stack adjustments; unbounded loops and
  unknown interprocedural frame observations still require further proof.

- Fixed scalar import contracts now split wide outgoing stores on i386 in
  shared ABI recovery, including later overwrites of either word. Both C
  routes execute against an independent 32-bit provider. Captured ARM64
  Windows imports retain scalar function-pointer types without rereading a
  changed import slot; native ARM64 execution remains separate coverage.

## Established rules

- Integer record calls in LLVMC bind physical ABI pieces through the shared
  LLVM call-contract metadata. Compacted register banks and reordered stack
  operands retain their byte locations; incomplete or ambiguous carriers
  fail. This does not extend unsupported mixed floating record conventions.

- HighC owns structured `__try` / `__except` / `__finally` and source-like
  statements. LLVMC preserves a goto projection when structure is uncertain.
- Win64 call arguments use Microsoft register and stack rules. Debug types can
  improve names and display, but cannot authorize an unobserved argument.
- Assigned locals have declarations. HighC can print calls with unused results
  as statements; LLVMC retains their result assignments. PHI and register-home
  copies are printed only when their values are used.
- Imports and recognized runtime calls use their actual names and arities.
  Unknown semantics stay explicit; an unsupported operation is never a NOP.
- Integer width, address-taken storage, exception edges, and noreturn behavior
  remain semantic facts through C rendering.
- Public regressions are in `HighCPointerAddresses.*`,
  `LLVMCPointerAddresses.*`, `COFFExceptionIR.*`, and the Windows EH corpus.
  Use `docs/testing.md` to select affected lift, patch, and store-forward checks.

## Workflow

1. Capture the smallest public reproduction and the before/after C output.
2. Change the owning semantic or rendering layer once, then verify both emitters
   when the behavior is shared.
3. Run focused regression filters, affected EH and patch filters, and the
   corpus producer verification when corpus code changes.
4. Keep private binaries, PDBs, decompiler comparisons, addresses, names, and
   timing logs in local scratch storage, outside Git.
