# Remaining x64 C-quality work

Use public fixtures and emitter tests as the durable contract. Private binary
analysis and comparison output belong outside the repository.

## Open

| Gap | Next check |
|---|---|
| LLVMC Windows EH remains goto form | Keep recovered filters, nested cleanup order, and both normal and unwind edges visible. Compare the public `seh_probe` and C++ EH corpus output with HighC. |
| LLVMC fallback can lose a nonadjacent invoke normal edge | Build a two-invoke SEH fixture whose handler-to-join edge needs a PHI copy; preserve the normal successors when the shared continuation cannot move after `__except`. |
| LLVMC output may retain avoidable frame references or control flow | Reproduce each issue with a public synthetic LLVM fixture. Keep a negative test for any value or edge that must remain printed. |
| Source types and member calls remain partially recovered | Use authenticated PDB/TPI type and method metadata; do not infer a virtual method name from a vtable offset alone. |
| Release performance varies by function and debug input | Time paired fresh `--func` calls and separate load from decompile cost. Profile before changing shared analysis. Preserve byte-identical C and EH semantics. |
| Toolset coverage varies by hosted runner | Verify each compiler's actual version and path before publishing corpus artifacts. Treat unavailable versions as explicit skips. |

## Closed in the public SEH corpus

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

## Established rules

- HighC owns structured `__try` / `__except` / `__finally` and source-like
  statements. LLVMC preserves a goto projection when structure is uncertain.
- Win64 call arguments use Microsoft register and stack rules. Debug types can
  improve names and display, but cannot authorize an unobserved argument.
- Assigned locals have declarations; unused call results are statements.
  PHI and register-home copies are printed only when their values are used.
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
