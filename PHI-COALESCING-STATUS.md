# PHI coalescing: branch status

This branch is work in progress and is not ready for `dev`.

## What the pass does

`coalescePhiCopies` (`lib/ir/high/pass/HighVarRename.cpp`) runs in
`MedToHighConverter::convert` right after `structureControlFlow`, on the flat
HighIR, before `simplifyControlFlow`. It is skipped when exception dispatch can
enter a block (`mayEnterByException`).

The analysis is `highSourcePhiCleanup` (`lib/ir/high/HighSourceFlow.cpp`,
`phiCleanup`). It uses the unrefined source-flow graph, resolves shared
statement addresses the way `coalesceBranchEntryStatements` groups branch
entries, and then:

1. Finds PHI copies whose destination is not strongly live after the copy
   (a copy reads its source only for a live destination, so copy cycles die)
   and erases them. Copies a jump enters stay and keep reading their source.
   Copies of computed values are erasable only when `discardableIntegerValue`
   holds.
2. Merges the source of each remaining scalar local copy into the
   destination's name when the two classes do not interfere (every write
   against every candidate live after it, except the source of a plain copy).
   Entry register values, frame and link registers, address-taken locals,
   byte-insert webs, and pairs of classes without a computed value do not
   merge. Representatives are always PHI copy destinations.

Expressions built from MedIR after the pass (`inlineGotoReturns`,
`ensureTrailingReturn`) go through `renameCoalescedVars`, keyed by
`highSourceLocalIdentity`.

## Measured effect (ntoskrnl 19041, against the same dev build)

- gotos -1.8%, emitted lines -6.8%, compile failures unchanged.
- Semantic suite and lift parity unchanged.
- Definite-assignment reads in the final HighIR: +755 (32026 vs 31271).
  547 functions have reads of computed values that occur only with the pass
  enabled.

## Acceptance gate

With the pass on and off, run the definite-assignment probe over the corpus
and compare the final HighIR (see "Differential" below). Required: no read of
a computed value at a real statement that occurs only with the pass on, or
each remaining one explained as a dead assignment of an already undefined
value. Entry-register reads and address-0 synthetic returns are excluded.

## Differential

The probe is a temporary patch in `convert()`: honour `NEVERD_NO_COALESCE`
around the pass, and with `NEVERD_DA_DETAIL` print every
`HighSourceFlowIssue::DefiniteAssignment` item of `analyzeHighSourceFlow` on
the final HighIR as `NDDA-final <function> <statement> <expression>`. Run the
corpus harness twice (on and off) and diff per function, ignoring statement
address 0 and unversioned register names.

## Open causes

- SourceFlow's `PhiCopy` flag excludes copies whose value is `Undef`, so an
  unread `x = undef` copy is not a dead-copy candidate unless
  `discardableIntegerValue` admits it.
- Structuring treats `x = undef` as droppable clutter in many places
  (`HighCFSimplifyIfElse.cpp`: `stmtIsSkipResidue`, `invertSkipGotosIn`,
  `destBeforeJoinGoto`, `prefixIsJoinSkip`, `countStmtTree`, the join-default
  scans; `HighCFSimplify.cpp` near the entry-label checks). Once merged names
  leave an arm with only `undef` copies, the arm is dropped and a later read
  has no assignment on that path. Example: `PsQueryProcessAttributesByToken`
  (0x140601050) loses `R8.2 = undef` after the first call.
- Call-site arity inflation (callers pass registers the callee never reads)
  creates most of the garbage register webs involved. Fixing it upstream of
  this pass should remove most remaining cases.
