# Remaining x64 C-quality work

Use public fixtures and emitter tests as the durable contract. Private binary
analysis and comparison output belong outside the repository.

## Open

| Gap | Next check |
|---|---|
| LLVMC Windows EH remains goto form | Keep recovered filters, nested cleanup order, and both normal and unwind edges visible. Compare the public `seh_probe` and C++ EH corpus output with HighC. |
| LLVMC output may retain avoidable frame references or control flow | Reproduce each issue with a public synthetic LLVM fixture. Keep a negative test for any value or edge that must remain printed. |
| Source types and member calls remain partially recovered | Use authenticated PDB/TPI type and method metadata; do not infer a virtual method name from a vtable offset alone. |
| Release performance varies by function and debug input | Time paired fresh `--func` calls and separate load from decompile cost. Profile before changing shared analysis. Preserve byte-identical C and EH semantics. |
| Toolset coverage varies by hosted runner | Verify each compiler's actual version and path before publishing corpus artifacts. Treat unavailable versions as explicit skips. |
| MinGW x86 call arguments can remain unknown after function boundaries are repaired | Inspect the recovered callee ABI and each caller's actual register/stack evidence. Do not treat residual unknown arguments as evidence that an internal label is still a function. |
| The public x64 `xcpt4` SEH analysis matrix still rejects an independent ordinary entry in a protected scope | Bounded decoded leaf writes and certified runtime-only continuations now retain their frame evidence. This case still needs nested/opaque callee effects and cross-funclet runtime activation identities. ABI declarations alone do not preserve saved memory. |
| The public PE32 `xcpt4` mixes ordinary calls into finally bodies with runtime activations and nonlocal stack restoration | Model invocation identity and the actual return PC through normal returns and parent resumes. The current registration solver loses that distinction; neither a zero-pop summary nor an unconditional no-return annotation is valid. |
| Two native out-of-line catch tests match an unused forward declaration as if it were an executed call | Audit declarations separately from emitted function bodies; the historical build reproduces both failures, while the catch body already resumes after the skipped call. |

## CRT and catch-continuation repairs

- Same-frame x64 local-unwind arguments use one all-predecessor LowIR proof,
  including stable loops, partial writes, released storage and independent
  roots. Both module ownership and SSA consumers retain exact call identities;
  a whole-function dependency digest rejects stale earlier-block evidence.
  Complete decoded leaf calls can preserve saved cells when every returning
  path bounds actual writes and retains its physical return PC. Callee digests
  are rechecked by shared SSA. Frame-dataflow work has a separate cumulative
  budget from address-use receipts; exhaustion does not erase that inventory.
  Certified runtime-only continuation roots receive their established SP even
  without an ordinary incoming edge. This does not preserve saved pointers
  through opaque callees or prove cross-funclet frame borrowing.
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

## Established rules

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
