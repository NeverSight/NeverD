**Languages**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows Exception Reconstruction

[← Documentation Index](README.md)

NeverD carries Windows table-based exception information through loading,
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
| x86 registration-chain EH | Kept distinct from table-based EH | Unsupported-form annotation | Not reconstructed |

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

## IR contract

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
- current schema version: `3`.

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
