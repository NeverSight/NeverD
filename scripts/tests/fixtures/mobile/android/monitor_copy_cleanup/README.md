# Split monitor cleanup

Independently authored smali inputs retain a Gate with unprotected register copies and
three catch-all landing pads. Each landing pad copies only dead local values,
then reaches the same monitor-exit and rethrows the same captured object.
Narrow/wide adapters and source/SDK Payload declarations are covered. The
ordinary Java Gate and an owned observer Payload provide the independently
compiled JVM reference; no compiler original or DEX is rewritten. The smali is
not executed as a JVM classfile. Native original-CFG tests additionally check
all copy widths, range spellings, cleanup convergence and rejected boundaries.

Frozen callers run before recovery, then against every recompiled Java source.
They observe concurrent exclusion, reentrancy, null locks, partial allocation,
all three failing stages, Exception/Error identity and actual release to a new
thread. Five independently wrong Java programs must fail the same observer.
Complete original entry inventories, metadata and Exceptions are compared.

The backward-cleanup profile places the final landing pad after the common
release and joins it through an acyclic backward edge. Its protected release
keeps the same captured exception and lock. Additional controls reject cycles,
normal handler entry, bypassed release and a changed lock object.

The release-only-handler profile adds an independently authored catch-all pad
referenced only by the normal monitor-exit range. It captures the original
exception and joins the same owned release. A separate outer handler spans the
shared release and following rethrow, then rethrows that identical object while
unlocked. The frozen
concurrency and identity oracle is unchanged; additional controls reject
normal handler entry, a changed lock, bypassed release and normal recapture.

The release-exception-copy profile forwards every captured object through a
separate reference register before the shared release and rethrow. The same
outer rethrow boundary and frozen oracle remain. Native controls additionally
reject overwritten aliases, scalar copies, wrong throw registers and joins
that bypass or re-enter lock ownership.
