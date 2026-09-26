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

## Closed in the public SEH corpus

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
