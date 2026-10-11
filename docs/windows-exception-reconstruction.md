**Languages**: [English](windows-exception-reconstruction.md) | [简体中文](zh-CN/windows-exception-reconstruction.md) | [繁體中文](zh-TW/windows-exception-reconstruction.md) | [日本語](ja/windows-exception-reconstruction.md) | [한국어](ko/windows-exception-reconstruction.md) | [Français](fr/windows-exception-reconstruction.md) | [Deutsch](de/windows-exception-reconstruction.md) | [Español](es/windows-exception-reconstruction.md) | [Italiano](it/windows-exception-reconstruction.md) | [Русский](ru/windows-exception-reconstruction.md) | [العربية](ar/windows-exception-reconstruction.md)

# Windows Exception Reconstruction

[← Documentation Index](README.md)

NeverD carries Windows exception information through loading,
lifting, decompilation, and binary rewriting. Exception metadata is part of a
function's executable contract: a rewrite is rejected when NeverD cannot prove
that the generated code, runtime-function records, language tables, and guard
tables remain mutually consistent.

This document distinguishes three levels of support:

- **Analysis** means the native representation is decoded into checked,
  normalized records and exposed to the IR pipeline.
- **Decompilation** means reducible protected regions are represented as
  explicit HighIR exception nodes; other shapes retain deterministic native
  annotations rather than losing handlers or state transitions.
- **Native reconstruction** means patch mode can ask LLVM to emit a complete
  replacement exception contract and install it in the final PE image.

Analysis support does not imply native reconstruction support.

## Support matrix

| Native form | Lift and analysis | High-level output | Patch mode |
|-------------|-------------------|-------------------|------------|
| x64 unwind v1/v2 | Complete checked unwind records, operations, chains, handler data, and provenance | Frame/unwind summary plus structured language regions where applicable | Supported for complete primary records; generated `.pdata` and `.xdata` replace the superseded closure |
| x64 unwind v3/APX | Dedicated version-3 payload, epilog, and operation accounting | Explicit v3 annotations | Analysis only; a touched function is rejected |
| ARM32/ARM64 packed unwind | Function ranges, packed fields, primary/fragment identity | Frame/unwind summary | Supported only for complete non-language primary records when the image has no independently addressable fragments |
| ARM32/ARM64 unpacked unwind | Checked xdata header/code extent, handler association, and fragments | Frame/unwind summary | Supported only for complete non-language primary records when the image has no independently addressable fragments |
| `__C_specific_handler` | Scope ranges, filters, finally targets, handlers, and continuation targets | Reducible regions become `__try`/`__except`/`__finally`; incomplete or irreducible regions remain annotated | Native x64 reconstruction for complete, representable scope graphs |
| `__CxxFrameHandler3` | Unwind map, try map, catches, catch-object/frame offsets, continuations, and IP-to-state map | Reducible state intervals become explicit C++ HighIR with C-compatible typed annotations | Native x64 reconstruction for the deliberately narrow, verifier-clean subset described below |
| `__CxxFrameHandler4` | Bounded variable-length decoding into the common C++ graph, including action kinds and object offsets | Same HighIR graph with FH4 provenance | Analysis only; a touched function is rejected |
| `__GSHandlerCheck_SEH/EH/EH4` | Wrapped personality plus checked GS cookie provenance | Base-language graph and wrapper annotation | Analysis only; a touched function is rejected rather than downgraded |
| x86 registration-chain SEH3 | Checked scope graph, actual FS:[0] administration, callback roots and CFG-derived reaching try levels | Reducible, unambiguous regions become explicit EH nodes; other state flow retains native annotations | Native PE32 reconstruction for the checked fixed-frame, caller-cleanup subset below |
| x86 registration-chain SEH4 | Checked cookie expressions, encoded scope pointer and CFG-derived state flow | Structured EH where reducible; lossless annotations otherwise | Native PE32 reconstruction for the authenticated direct-frame subset below, including initialized EH/GS cookies |
| x86 registration-chain C++ EH | Absolute-pointer FuncInfo, cleanup/object contracts and CFG-derived state flow | Structured EH where reducible; lossless annotations otherwise | Native PE32 reconstruction for the checked synchronous try and scalar or unbound catch subset below |

Malformed records are never treated as ordinary complete records. A partially
decoded record remains useful for inspection, but cannot authorize native
metadata generation. If an ARM xdata header still proves a bounded executable
fragment range but its trailing unwind body is malformed, the range remains
available to disassembly while the record is marked malformed and is not
promoted to a patchable function.

## Normalized model

`ExceptionInfo` is owned by `BinaryImage`. Each `ExceptionFunction` contains:

- a checked half-open code range;
- primary, chained, or fragment identity;
- the native unwind encoding and exact runtime/unwind provenance;
- normalized unwind operations and epilogs, with opaque operand bytes retained
  for operations that are not semantically understood;
- the exact personality identity and its handler data;
- optional SEH scopes, C++ state maps, and GS cookie data;
- `Complete`, `Partial`, or `Malformed` status and deterministic diagnostics.

The loader never exposes raw file pointers through this model. Native RVAs are
retained for diagnostics and patch replacement, while IR consumers operate on
validated virtual addresses and ranges.

The image-wide index permits overlapping chained and fragment records and
returns the most specific function covering an address. Any corrupt directory,
range, pointer, count, state transition, compressed integer, chain cycle, or
decode-budget exhaustion lowers the relevant parse status.

Language-table limits are enforced both per native table and across the whole
normalized graph for one function. Reusing one handler map from many try-map
entries therefore cannot multiply parser work beyond the aggregate budget.
FH3 records that share one `FuncInfo` and personality are decoded as a bounded
function group, so the parent's IP-to-state map may legally name its catch
funclets without admitting addresses from unrelated runtime functions.

### x86 registration state

x86 has no runtime-function directory. The loader follows registration
prologues to EH3/EH4 scope tables or C++ FuncInfo. SafeSEH tables must be mapped,
strictly sorted and executable; a malformed table cannot become an absent one.
C++ map counts share one aggregate decode budget.

`analyzeRegistrationStates` is the shared LowIR owner of reaching try levels.
It follows CFG predecessors and backedges, unions levels at joins and applies a
recovered state store only after its exact decoded instruction completes.
The loader retains byte, word and dword store widths. A narrow immediate
replaces only those low bits in every reaching whole state; unknown high bytes,
invalid resulting levels and disagreement with the lifted width fail closed.
Unreachable stores cannot change a reachable block's state. Runtime-entered
filters and cleanup callbacks are kept outside the parent's lexical interval;
catch and except entries use their runtime state transfer. MedIR carries these
derived facts separately from the authenticated loader descriptor. HighIR
requires every reaching state to agree about region membership before emitting
a structured region. Unknown transitions retain annotations rather than
inventing an IP-to-state map.
C++ catches have a separate runtime context. The active catch guard first
searches tries inside that catch; an unmatched exception continues through
enclosing guards and the parent registration. LowIR records each possible
search target and the number of exited catch guards. Selecting an enclosing
try discards those invocations and restores their captured SavedESP before
entering its catch. A catch returns a continuation code pointer to the runtime.
LowIR requires an exact decoded return and saved stack value,
decodes the target within the same function, and replays the restored context.
Each active catch retains the saved-stack snapshot taken before runtime
dispatch. A direct-frame catch may overwrite SavedESP: the continuation
restores the captured pointer to that cell before restoring ESP. LowIR and
MedIR bind this implicit writeback to the exact catch return; HighIR and native
LLVM materialize it in the recovered source frame. The installer independently
checks the writeback's address, value and order against fresh source analysis.
A missing pre-dispatch snapshot still cannot be invented by a catch write.
For a checked realigned frame, LowIR separately models the callback's private
stack, initialized spill cells and restored entry EBP. A catch can
resume only after balancing its private ESP and recovering runtime EBP. Checked
calls may borrow initialized parent objects using the captured parent stack
bound; the saved entry EBP remains protected. MedIR preserves distinct runtime
root definitions; HighIR and LLVM express the aligned parent coordinate from
the original entry ESP without inventing a constant stack displacement. Exact
no-return call receipts remove ordinary fallthrough before SSA while preserving
exceptional entries. HighIR restores the captured SavedESP through that same
aligned coordinate before transferring to a checked continuation. A checked
runtime-only catch CFG becomes an explicit clause, including its private ESP
input and the complete callback body. The input is captured at callback entry;
HighC names it with an explicit runtime-ABI intrinsic in its analysis view.
Explicit C retains the callback as a labelled native entry in the parent,
skipped by ordinary fallthrough; C++ pseudocode includes it in the catch clause.
Neither view implements a standalone exception dispatcher. Checked helper calls
use their retained callee ABI and current SSA arguments, so callback spills do
not become extra call arguments. Explicit FS accesses remain visible when
aligned, byte-addressed registration stores depend on them.
Ordinary incoming edges, incomplete roots, changed return receipts or a body
that cannot move as one region retain handler and continuation annotations.
Native reconstruction supports the checked scalar and unbound catch subset
below, including its separate invocation stack.
An unproven continuation or conflicting return retains annotations and
withdraws native authority. These facts remain separate from the source
FuncInfo and from the parent's scalar return value.
Only callback-pointer fields authenticated by the FuncInfo parser are excluded
from ordinary indirect-entry discovery. Another reference to the same target
retains its independent ordinary-entry role. MedIR gives runtime-only catch and
cleanup roots the established parent EBP, keeps their private callback ESP
distinct, and gives a checked continuation its saved parent ESP. The checked
catch projection captures its own ESP; a cleanup still needs its own callback
ABI proof before its body can be embedded. Stack-offset
proofs, HighIR and LLVM use the same coordinates. Malformed root carriers and
conflicting saved-stack values cannot acquire those guarantees.
Structured catches keep an explicit transfer to the checked continuation;
unstructured annotations retain the catch-object offset, parent-frame offset
and continuation list while leaving the native handler out of line.
A try whose checked address intervals surround an out-of-line catch can form
one HighIR region when removing that runtime-only callback leaves a contiguous
protected body. Ordinary predecessors, intervening unprotected statements and
ambiguous state flow prevent the move. The transformation commits the body and
catch together. A separately converted PE32 callback still needs a parent-frame
projection before its body can be copied into a clause; its native target is
retained when that proof is absent.
Direct MSVC prologues must prove the actual FS:[0] write and the registration
and state-field offsets; matching integer sequences in locals are insufficient.

The checked LLVM fixed C++ prologue places the node at source EBP-24 and state
at EBP-16. Runtime EBP is source EBP-12. Ordinary source accesses retain their
entry-EBP coordinate; runtime catch homes, cleanup objects and callback roots
use the checked displacement. Catch entry and continuation code must restore
their own EBP explicitly. SavedESP is written back at source EBP-28. Private
callback stacks and saved-register exclusion require independent dataflow
proofs; recognizing the prologue alone does not authorize reconstruction.

For the checked LLVM realigned C++ prologue, ordinary LowIR propagation keeps
entry EBP and runtime-establisher offsets separate. It replays stack alignment,
allocation, the ESI anchor and all registration fields before accepting the
actual FS:[0] installation. Accesses that could cross between the two frames
fail closed. A generated catch's independent callback stack and runtime return
protocol still require separate proofs, so these coordinates alone do not
enable native reconstruction of a realigned source.

Native SEH3/EH4 reconstruction additionally proves a fixed private source frame,
balanced FS:[0] administration and one active state at each ordinary CFG block.
LLVM owns the new physical registration. Outlined filters and termination
callbacks recover source EBP/ESP and exception pointers through its escaped
frame; source chain operations are replaced only at authenticated occurrences.
Every active interval has explicit asynchronous scope boundaries, including
nested handler entry into its outer scope. Original functions and preserved
direct callees must have checked caller-cleanup stack behavior. Indirect or
unproved cleanup conventions remain rejected.

Incoming cdecl stack slots are projected onto the real caller frame at their
original memory-operation occurrences, including reads and writes in outlined
callbacks. Preserved callees need a closed, frame-private call graph: stack
reads must be initialized, accesses must stay within their live allocation,
and callee-saved registers and SP must be restored. Frame provenance survives
flags, vector aliases, spills and calls. A return-address observer may record
the regenerated call site for an external observer; reloading that value inside
the source/callee closure is rejected. Opaque imports other than authenticated
`RaiseException`, and memory intrinsics without a checked access contract, are
outside this native subset.

The compiler emits indexed scope rows with exact table extent, enclosing state,
filter/handler targets and source semantic receipts. The PE transaction checks
their physical bytes and DIR32 fixups, merges SafeSEH and HIGHLOW relocations,
and reparses the installed image. A new Guard CF/EH continuation table pointer
also receives its own base relocation. Both `section` and `inplace` patch modes
use this complete transaction for registration functions.

SEH3 requires an argument-preserving veneer to the known CRT import. EH4
requires an exact forwarding wrapper: all four dispatcher arguments, the
load-config cookie address, the executable cookie checker and the CRT common
handler import must agree. A handler name alone never authorizes rewriting.
The shared frame domain proves the encoded scope pointer and every cookie
expression before the registration becomes visible. Source and preserved
callees cannot change the image cookie or scope table. Synthetic cookie values
cannot escape the private frame. LLVM derives the generated cookie offsets
from its physical registration record, including the runtime's virtual frame
base; the installer compares those offsets with the exact emitted table bytes.
A directly initialized GS slot gets a compiler-owned stack protector. GS encoding
and exit checks use that same virtual base, including with stack realignment.
The checker keeps the exact fastcall ABI and the original wrapper's code identity;
a similarly named function or import cannot substitute for it.
Its checked success path compares ECX with the load-config cookie and returns
without touching stack storage or other registers. An escaped argument copy
stays in the recovered local frame when LLVM realigns the stack.

An explicit source GS check requires an exact full-width ECX cookie expression
at the decoded call occurrence and the authenticated checker identity. Native
lowering replaces only that proved call with an indexed execution event; LLVM
emits the physical stack-protector check. The public installer independently
replays the source proof and rejects removed, duplicated, unmarked or reordered
events. Checks inside outlined callbacks remain outside this subset.

This path needs the LLVM fork's `LLVM_NEVERD_X86_REGISTRATION_EH` contract;
EH4 also requires `LLVM_NEVERD_X86_REGISTRATION_COOKIES`; GS initialization
requires `LLVM_NEVERD_X86_REGISTRATION_GS`. The older published r3 package
rejects native installation.

Native x86 C++ reconstruction supports synchronous parent try groups with
ordered catches, with at most 128 source unwind states. A catch may bind a checked scalar
by value or reference, omit its local object, or be `catch(...)`. An unbound
typed catch retains its exact RTTI and adjectives; catch-all retains null RTTI
and its native catch-all adjective. An absent object home requires complete
source proofs with no runtime-object accesses. It cannot supply an implicit
initialization write to the recovered parent frame.
The source uses a checked direct MSVC registration frame, the LLVM fixed frame
with saved EBX/EDI/ESI above the node, or the bounded LLVM ESI-anchored aligned
frame, with `FuncInfo` magic `0x19930522` and no GS wrapper.
Every preserved call, throw type and cleanup relay needs an independently
checked ABI. Source object
borrows must be bounded, initialized and separate from registration storage;
reference accesses retain the CRT-provided object identity through catch return.
Reads and writes must retain the original image storage identity.
Checked incoming words retain their physical caller locations across parent
code and catch execution, including writes observed by the caller. C++ parent
entries support cdecl, stdcall, thiscall and fastcall with 32-bit physical words:
zero, one (ECX), or two (ECX/EDX) register parameters precede contiguous stack
parameters. LowIR authenticates each reachable parent RET against the source
bytes independently of callback RETs. Its checked cleanup count also preserves
unused callee-popped slots; callback-only reads can recover caller-owned slots.
Unknown or conflicting returns, variadic entries and incomplete frame proofs
remain rejected. LLVM emission and independent installation agree on the exact
convention and parameter attributes, including both fastcall `inreg` words.
Synchronous regions may leave through a checked normal return or shared runtime
resume tail. A moved fallthrough becomes an explicit transfer to that same tail;
the tail and callback bodies retain their own entry identities.
SEH and C++ share the transactional caller-frame projection; installation
independently checks its entry initialization, escape, offsets, access widths,
occurrences and calling convention. Private-frame pointers cannot escape into
caller storage.
For an aligned source, the synthetic allocation proves the parent coordinate's
alignment and extent. Catch objects, cleanup borrows and SavedESP writeback use
that same projection. A catch has a separately bounded scratch allocation;
its address may enter the parent only through SavedESP. Installation replays
the source contract and independently checks actual LLVM alignment, bounds,
initialization at every catch entry, callback lifetime and the final writeback.
A frame-layout descriptor alone cannot authorize reconstruction.

Each clause retains its own catchpad, object home, scratch stack and exact
continuation. Dispatch order must match the source HandlerMap. A sibling catch
cannot borrow another clause's implicit object initialization or callback stack.
Try groups may be disjoint, nested in a parent try, or nested inside a checked
catch, with at most 64 tries and 128 source states. Catch entry immediately
follows the protected state interval; its catch interval may contain further
tries. The shared source projection proves the complete inner-to-outer
search chain; independent installation checks preserve every try's protected
state set, emitted HandlerMap, search order and unwind edges. A checked private
throw inside a catch can continue the search through an enclosing parent try.
Its invoke retains the active catch token and targets the outer dispatch;
independent IR validation replays that transfer and its restored stack.
Private rethrow helpers are distinguished from helpers constructing a new
exception. The helper must pass two known null arguments to the authenticated
CRT import, and its source caller must have a proved live catch invocation.
The runtime retains the current exception's type and object, including changes
made through a reference catch. A rethrow supplies no new ThrowInfo or implicit
object initialization. A direct rethrow additionally proves both initialized
null argument words in the current source stack coordinate. Native lowering
emits the CRT's two-pointer x86 stdcall ABI; independent installation checks
reject changed arguments, calling conventions or catch context.
Direct scalar throws use the same CRT entry ABI, with a separate contract for
each call occurrence. The actual table argument must select a checked immutable
ThrowInfo and the object must occupy initialized, pointer-free bytes in the
parent frame or the active callback's private stack. Candidate type tables alone
do not authorize a call. Current LLVM object arguments are retained; installation
independently checks their address, lifetime and initialization, the exact original
table identity and its relocation. A function can use this entry for different
scalar types and for a rethrow without conflating those operations.
Runtime parameter attributes and optimizer effect promises are independently
checked; adding `inreg`, `nonnull` or `memory(none)` cannot change the CRT ABI.
For checked fundamental types, C++ output reads the current object bytes with
the exact type, including floating-point bits. Unknown types retain the runtime
call instead of inventing a value or default constructor.
For a try inside a catch, LowIR retains the suspended invocation's private
stack cells and initialized bytes. The inner catch may access a still-live
outer scalar reference object; it must not access an exited object or borrow
another invocation's bounds. On return, the exact saved callback ESP and
outer context are restored. MedIR owns callback membership and nesting for
both source and native consumers. LLVM catchswitch parents and resumed stack
definitions must match that proof. The installer independently checks each
resume seed's parent, target, offset, allocation, dominance and SavedESP
writeback. Checked cleanup relays may run while the outer reference stays live.
The source solver requires every object borrow to be initialized and excludes
cleanup writes overlapping the saved reference. Native cleanup pads retain
their active parent catch and exact ordered calls, including multiple
local objects destroyed by one compiler-combined action. Missing,
reordered or redirected calls, changed effect attributes and skipped unwind
edges reject installation. General object lifetimes and shared callbacks remain
unsupported.
HighIR can gather terminal branches of synchronous tries even when runtime
resume blocks interrupt their address order or merge different post-catch
states. An inner try stays intact with independently checked callback bodies
and exact continuation targets. It requires complete call and
state receipts. Checked returning leaves contain no calls or C++ throws and can
precede the first state store; unknown calls, unprotected throws and asynchronous
faults cannot acquire a new handler through this projection. C and C++ output
retain explicit native object homes and load snapshots instead of assuming a mutable catch object is an
immutable source expression.

LLVM recreates the physical registration, an object home only when required,
ordered cleanup dispatch, complete FuncInfo and private handler. The installer
checks that absent object/RTTI fields are literal zero with no overlapping
fixup, so rebasing cannot turn them into pointers. Public installation requires
`LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS`,
`LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS` and
`LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS`, then independently replays edited IR and
checks actual emitted code, tables, SafeSEH and all absolute relocations. Entry
patches may not overwrite preserved helper or CRT instructions. The current
runtime fixtures cover integer value/reference catches, unbound typed catches,
catch-all with signed and unsigned throws, caller argument reads and writes,
and nested destruction, including
forced relocation. Fixed MSVC-style frames use an independent assembly fixture;
LLVM fixed and realigned frames use compiler-generated parents. Fixed-frame
coverage includes both short and full-width stack allocations. Ordered-catch
fixtures combine signed value, unsigned reference and catch-all clauses in one
function and check all three continuations and reference writes across four
caller stack layouts. Both installation modes and forced rebasing participate
in the same-file runtime matrix. Additional Clang `-O0` and `-O1` fixtures
exercise inner reference catches, outer value/catch-all search, all three
continuations, secondary throws and both helper and direct rethrows from a
catch. A further three-try/four-catch fixture throws inside a reference catch,
modifies the outer object from the inner catch, then resumes the outer catch.
Its cleanup variant constructs two local objects and independently records
reverse-order destruction. Clang O0 retains two actions; O1 combines their
calls in one action. Both forms require the same ordered runtime trace.
Negative controls and forced rebasing cover each profile. The matrix uses the captured
Microsoft x86 CRT DLL, including under Wine. Wine's built-in CRT is not the
authority for catch-guard stack restoration;
the same original and reconstructed files are also replayed on Windows.
Runtime bytes and provider identity are bound into the evidence. The `-O0`
loader proof also checks the exact adjacent ESP-to-EAX save and the personality thunk's four
argument reads. Mutating those bytes, an IR dispatch edge or generated try and
unwind rows must reject reconstruction. All fixtures
link the captured MSVC CRT libraries. The CI replay executes the identical PE files on Windows. Other
try/catch graphs, unproved object types or entry ABIs, unproved dynamic frames
and GS or asynchronous C++ remain available for analysis and are rejected for
native installation.

Generated PE32 C++ entries can be loaded, lifted and reconstructed again when
the complete source proof succeeds. The loader checks the exact FS:[0] store,
realigned frame and SafeSEH pointer roles from image bytes. Installation can
patch an authenticated entry in an earlier generated executable section; its
virtual and raw storage must be unique and match the analyzed bytes. The
cdecl/stdcall/thiscall/fastcall fixtures cover two rewrite generations, both
patch modes and forced rebasing, with the Microsoft x86 CRT. Each generation
requires fresh LowIR and LLVM proofs; a previous receipt grants no authority.
Generated cleanup relays and broader object lifetimes still require additional
support.

## IR contract

Canonical Windows EH metadata schema 11 and semantic-token schema 2 bind the
primary code range and every disjoint callback range. Previously saved LLVM IR
must be lifted again before native reconstruction; older receipts cannot
authenticate the expanded ownership contract.

Exception metadata is attached at every representation level without changing
the meaning of the ordinary CFG:

- LowIR splits blocks at protected-range boundaries, state transitions,
  filters, handlers, cleanup actions, and continuation targets.
- Exceptional successors and predecessors are separate from ordinary
  successors and predecessors. Existing dominator and structuring algorithms
  therefore do not mistake a runtime dispatch edge for a machine branch.
- MedIR retains the normalized function descriptor and stable exceptional
  edges.
- HighIR uses distinct `SEHTry` and `CxxTry` statements. Clause descriptors
  preserve native target VAs, type descriptors, adjectives, catch-object and
  parent-frame offsets, cleanup action kinds and object offsets, states, and
  continuation VAs.

For an x64 in-function SEH handler that needs RSP, MedIR proves the established
frame before initializing the exceptional root. Normalized fixed unwind
allocations must match decoded prologue instructions and converted SP effects;
all ordinary paths into the protected scope must retain that SP. Independent
entries, prologue backedges, dynamic SP changes, frame-register or chained
unwind contracts, and incomplete evidence are rejected explicitly. Conversion
failure reaches the pipeline caller without aborting its process or publishing
a partial function. HighC and LLVM-derived C consume the same explicit SP
adjustment; the C renderer does not apply another frame-size correction.

The HighIR structurer is interval-conservative. It only moves one contiguous
statement slice whose addresses are wholly contained by a complete protected
range. Nested regions are processed inner-first. Crossing regions, partial
graphs, address-less ambiguous boundaries, and out-of-line funclets remain in
their original control-flow form and increment the function's unstructured-EH
count.

The C backend emits MSVC SEH syntax for a reducible single-clause SEH region.
It emits deterministic C-compatible comments for C++ catches and cleanup
states because HighC is a C backend and must not claim to produce compilable
C++ source. Out-of-line native funclets retain their exact addresses.

## LLVM metadata schema

Every parsed exception function associated with an emitted lifted function
receives lossless LLVM metadata, even when it is not eligible for native WinEH
lowering:

- function attachment: `neverd.windows.eh`;
- native-lowering marker: `neverd.windows.eh.native`;
- module table: `neverd.windows.eh.functions`;
- current schema version: `9`.

The fixed function record carries parse status, encoding, code range, native
runtime/unwind RVAs, runtime-record kind and chain provenance, packed-unwind
word, frame description, canonical and resolved personality names, handler
data, exact native unwind bytes, normalized operations (including native slot
counts) and epilogs, SEH scopes, C++ header/maps, GS data, diagnostics, and a
regeneration flag. Patch validation requires the exact schema version and an
exact range match with the loaded image. A module cannot silently omit the
attachment from an auto-named lifted function that has an exception contract.

Native x64 SEH lowering uses LLVM WinEH constructs and emits verifier-clean
`invoke`/funclet control flow only when the full scope graph is representable.
Native FH3 lowering is intentionally narrower and requires all of the
following:

- x64 COFF, unwind v1/v2, complete metadata, valid synchronous FH3 state graph;
- no `noexcept`, asynchronous, separated-funclet, GS-wrapper, FH4, or unknown
  flag semantics;
- nested or disjoint protected intervals, never crossing intervals;
- no destructor/unwind action, catch-object construction, or parent-frame
  dependency;
- a handler represented by an ordinary predecessor-free, call-free block in
  the lifted function;
- every protected operation that may unwind represented by an LLVM `invoke`.

If one condition is false, the lifted LLVM remains analyzable and retains the
lossless metadata, but patch planning rejects native language-table replacement.
The PE entry point, TLS callbacks, and CRT callback roots remain preservation
boundaries rather than ordinary ABI rewrite candidates.

## Patch transaction

For a supported rewrite, NeverD treats exception reconstruction as one PE
transaction:

1. Validate every touched function against the loaded exception graph and the
   LLVM metadata attachment.
2. Compile replacement code while retaining emitted section identity,
   alignment, allocation flags, code/data traits, and semantic symbol-index
   references. A locally modeled Windows personality is externalized before
   code generation so emitted xdata binds to the proven original executable
   handler instead of recompiling a private copy of that ABI routine.
3. Preserve untouched runtime-function entries and remove the complete native
   closure superseded by each touched primary function, including associated
   chained records.
4. Relocate generated code and xdata, merge generated and retained pdata, sort
   by begin RVA, reject overlaps, prove that every redirected language-EH
   entry is covered by a generated runtime-function record carrying the same
   personality class, and install one replacement PE exception directory.
5. Preserve the input CFG instrumentation mode, resolve generated `.gfids`
   semantic references, and merge those targets plus redirected entries with
   the original Guard CF table. Resolve `.gehcont` semantic references into
   generated executable VAs, merge them with the original Guard EH continuation
   table, and update load-config pointers and counts while preserving the
   advertised guard flags. Unresolved CFG dispatch/check helpers abort the
   transaction. Guard modes that require a different code-generation contract
   (CFW, return-flow guard, retpolines, or XFG) remain analysis-only and reject
   rewriting instead of advertising protection the generated code cannot prove.
6. Reparse the completed byte image before writing it to disk.

The LLVM fork extension is deliberately generic. Its final-image writer keeps
section traits and semantic symbol-index references that would otherwise be
lost when object sections are flattened. PE parsing, MSVC language-table
decoding, policy, directory merging, load-config updates, and final validation
remain in NeverD.

Original Guard CF and Guard EH continuation entries are retained because the
original entry trampolines remain valid indirect targets. Generated targets
must point into emitted code. All resulting tables must be strictly RVA-sorted.

## Final-image validation

A patched PE is rejected unless all of these checks pass:

- LLVM accepts the bytes as a COFF object and the PE machine, class, section
  table, optional-header directory bounds, image base, and image extent agree;
- every section's raw and virtual extent is in bounds and section ranges do not
  overlap;
- the exception-directory extent is file-backed and inside the image;
- runtime-function entries are sorted, nonempty, non-overlapping, and wholly
  executable;
- x64 unwind RVAs are aligned, headers and code arrays are file-backed,
  versions and flags are supported, handler targets are executable, and
  chained records are acyclic with a depth limit;
- final imports, exports, and COFF symbols are rebuilt in memory so known SEH
  and FH3 personalities can be resolved and their scope/state tables parsed
  again from the completed bytes;
- ARM runtime entries and xdata identify a valid supported version and range;
- load-config Guard CF and Guard EH continuation fields are present when their
  flags advertise a table;
- guard pointers/counts/strides remain inside both the PE image and the file,
  and every table entry is strictly sorted and executable.

Failure aborts the patch operation. NeverD does not write a best-effort image
after validation has failed.

## Focused verification

Build the lift suite and run the Windows EH model, parser, IR, codegen, and PE
integration cases:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

The guarded x64 fixture is cross-assembled and linked with `/guard:cf` and
`/guard:ehcont`. The integration test loads its SEH scopes and guard tables,
checks structured HighC output, patches the image, reloads it, and verifies the
updated table counts, ordering, and executable targets.

A separate linked x64 FH3 fixture exercises the supported C++ closure through
the same full transaction. It verifies the original fixed tables, HighC state
annotations, preserved personality binding, regenerated try/catch graph, and
IP-to-state map after reloading the patched PE.

For parser changes, also run the existing ARM format cases because ARM packed
and unpacked xdata share the normalized model and final runtime-entry checks.

The focused registration-state suite and PE32 runtime baseline are:

```bash
cmake --build build-release --target NeverDRegistrationStateTests \
  NeverDRegistrationEHTests NeverDWindowsRegistrationFrameTests --parallel 4
build-release/bin/NeverDRegistrationStateTests
build-release/bin/NeverDRegistrationEHTests
NEVERD_REGISTRATION_RUNTIME_OBJECT=/tmp/neverd-frame.obj \
  build-release/bin/NeverDWindowsRegistrationFrameTests
python3 -m unittest scripts.tests.test_check_windows_registration_eh \
  scripts.tests.test_check_windows_registration_frame \
  scripts.tests.test_check_windows_registration_cookie -v
python3 scripts/check_windows_registration_eh.py --output build-registration/evidence
python3 scripts/check_windows_registration_frame.py --object /tmp/neverd-frame.obj \
  --output build-registration/callback-runtime
```

The runner executes the pinned MSVC x86 SEH and C++ probes with `/GS` on/off and
at O0/O2, using Wine on Linux or the native loader on Windows. Its default
report is `original-runtime` evidence. `--patched-root` requires all eight
rewritten counterparts, rejects byte-identical copies and compares outcomes;
that mode reports `changed-image-runtime` evidence. This establishes runtime
equivalence for changed images; native reconstruction additionally needs a bound
patch receipt and evidence that the replaced EH entries executed. Missing
runtimes or images fail the run. The callback-frame suite checks PE32 filter
and finally recovery, existing escape indices, bounded exception-pointer cells,
private callback stacks, atomic rejection, and actual i386 COFF scope-table
code generation. The frame runner links the emitted object with SafeSEH checks
enabled, executes 16 real exceptions, and requires the exact filter count and
successful outcome. Its report is `generated-x86-callback-abi` evidence. These
checks alone do not authorize a native patch. The source reconstruction runner
executes original, manually installed, public COFF, symbol-collision, CLI
section and CLI inplace variants at preferred and forced relocation bases.
The strict EH4 oracle additionally requires valid execution and pre-dispatch
rejection after separately corrupting EH and GS cookies. Wine's common EH4
dispatcher does not validate cookies, so a small fixture supplies that check
before forwarding dispatch to the runtime.
The CI `windows_eh_only` dispatch profile additionally checks
ARM32 cross-target PE generation and reconstruction. It does not claim execution
on Windows ARM32. When supplied an exact LLVM artifact build, a dependent
Windows job replays the same hashed PE32 images with the native Windows CRT.

## Extending native support

New native reconstruction support must include all of the following in the
same change:

- a complete, bounded parser and normalized-model invariants;
- HighIR and LLVM metadata round-trip coverage;
- verifier-clean native IR for every newly accepted graph shape;
- emitted-section and semantic-reference retention where required;
- linked PE fixtures for the exact architecture/personality/version;
- exception-directory, load-config, and final-image structural validation;
- explicit rejection tests for the nearest unsupported shapes.

Do not broaden an allow-list solely because a new record can be decoded. The
acceptance criterion is preservation of runtime exception behavior in the
final linked image.
