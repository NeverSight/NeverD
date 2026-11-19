# Performance: remaining work

Basis: `origin/dev` 159588db82 for the inventory and 92b7f8dab1 for the
profiles, both 2026-10-27; paths re-checked at 03e7784a40. Each item names its
entry condition. Do not start an item until that condition is met with a
profile or same-base A/B; see `docs/development-principles.md` § Speculative
defenses.

## Profiled hotspots

The profiled command was `neverd decompile IMAGE --max-func=250`, on a Release
build sampled with `tools/neverd-bench/sigprof`. Inputs:
- x64 and AArch64 libstdc++;
- a switch-heavy PE x64 DLL (Capstone built at O2);
- x64 and Mach-O arm64 Rust test binaries.

Items 1 and 7 also cite a whole-image `neverd decompile` of Ubuntu's x64
libsqlite3.so.0.8.6, profiled at `origin/dev` 4c991c2416 with the return-slot
fix that lets it complete; their paths are checked there.

On a 20-core host, half or more of each run's wall time passed with one or
two busy threads, so the serial stretches matter as much as the CPU hotspots.
These items meet their entry condition. Each change still needs a same-base
A/B with identical output.

1. **C emission's unit preparation is serial.** `writeFunctionsInParallel`
   (`lib/backend/c/HighC/HighCEmitter.cpp`) writes a unit's function
   definitions on worker threads, each with a copy of the writer `writeAll`
   prepared, joined in order. Its text is byte-identical to the serial loop:
   a same-binary A/B over the regression inputs, 2 to 20 threads, and a
   serial/parallel sweep of the Windows EH corpus. Wall time fell 17% to 39%
   on the Rust and whole-image runs. What stays on the caller's thread is
   everything `writeAll` does before the definitions: `prepareRecordDefinitions`,
   `prepareFunctionIdentifiers`, `collectImageObjects`, `collectMemoryTypes`,
   `writeIncludes` and `writeForwardDecls`. On the whole-image libsqlite3 run
   that was about a third of `writeAll`'s CPU samples.
   - Those passes walk every expression of every function. Collecting per
     function on workers and merging in function order must reproduce the
     serial walk, including the allocation order of
     `GlobalIdentifierAllocator`.
   - Renderings with a source map, recorded positions or source names (GUI,
     SDK dialect output) still write definitions serially:
     `SourceRecorder->positionDefinition(..., OS.tell())` and the per-function
     source-map updates would need rebasing and an ordered merge.
   - Reading CPU time on the 6P+8E host: the same definitions take about 2.3x
     their serial CPU time on 20 threads (E-cores, hyperthreads, a lower
     all-core clock) and equal it on 2. So whole-image A/B CPU rose 12% to 18%
     while no work was added; the writer copies cost about 0.5 s over 20
     threads on whole-image libstdc++. Check a CPU rise at 2 threads before
     calling it overhead.
2. **Restricted runs read omitted callees serially.**
   `omittedDirectCalleeSignatures` (`lib/pipeline/PipelineCallAbi.cpp:655`) is
   called under `recoverModuleCallAbi` (`:1388`). It lifts and converts the
   callees outside the selection one at a time on the caller's thread.
   - Scope: it fires only for restricted runs (`--max-func`, `--func`,
     `OnlyFunctionEntries`, GUI and SDK single-function views). Full-image
     runs have no omitted callees.
   - It held one thread for a large part of the PE run.
   - Ordering constraint: the breadth-first order decides which callees fit
     `kMaxCallEffectExtraLifts` (`include/neverd/Limits.h:444`) and the
     operation budget (loop at `lib/pipeline/PipelineCallAbi.cpp:710`). Lift
     each level concurrently, then consume the results in queue order.
   - `CalleeReader` (`:478`) shares one `CFGBuilder`, decoder and
     `X87CallGraphCache` (`:490`); each worker needs its own.
3. **x87 call-effect proofs are heavy on x64 inputs.** For each call, these
   frames prove whether the callee leaves x87 state, by building bounded
   callee graphs: `X87CallEffectIndex`
   (`lib/ir/low/X86/CFGBuilderX86Fpu.cpp:1040`), `X87CallGraphCache` and
   `fixupFpuStack`.
   - Their inclusive share of CPU samples was about a third on the PE and
     about a seventh on the x64 Rust binary. It was small on x64 libstdc++ and
     absent on arm64.
   - Where the time goes depends on the input:
     - PE: mostly the jump-table resolution of those callee graphs (item 4),
       under `usesContext` (`:1608`) and `X87CallGraphCache::refusesCall`;
     - Rust: `X87CallEffectIndex::contract`
       (`lib/ir/low/X86/CFGBuilderX86Fpu.cpp:1721`).
   - x64 code rarely uses x87; MSVC's x64 `long double` is `double`. Look for
     either:
     - an exact, cheap image-level precondition that settles every query, for
       example no x87 or MMX operation in any lifted function and no declared
       x87 floating import (`declaredX87Imports`,
       `lib/pipeline/PipelineLowIR.cpp:4511`);
     - reuse of the callee graphs that the main lift already builds.
   - A per-function test is not enough: a returning wrapper can carry x87
     state with no local floating instruction (`CFGBuilderX86Fpu.cpp:2136`).
   - Measured on capstone LLVM C (2026-11-18, a throwaway log in
     `X87CallEffectIndex::graph`): 396 callee graph builds take 80-92 s of
     CPU, 26.6% of all samples, and 97% of that is `resolveJumpTable` under
     the second lift. The 10 slowest builds hold 85% of it: five large
     decoder functions each built twice. The second build of each comes from
     the jump-table rebuild round (`RebuildGraphs`, `PipelineLowIR.cpp`),
     whose cache is new because its protected relocation slots and unsafe
     jump-table branches grew. On capstone that round is the module-wide
     fallback: the module arbitration runs out of
     `limits::kMaxJumpTableEvidenceWork` (4096) at its physical-envelope
     step, so all 325 code-pointer relocation slots become protected and all
     689 functions are lifted again (logged 2026-11-18; the unsafe set stays
     empty). Raising that budget alone is not enough: with it at 1,048,576
     (a private build), the module address-use inventory stops at its own
     cap (`scan-cap`: all lifted operations of the module must fit
     `limits::kMaxSSAFunctionOps`). Letting capstone's arbitration finish
     would remove the second lift and the second graph of every callee, but
     it needs both limits raised and changes which slots are protected, so
     it is a policy decision whose output effect is not measured yet. Builds that end in `MissingTerminator` (an
     indirect branch the resolver could not settle) hold 56% of the time,
     and only a complete CFG shows that rejection.
   - On x64 the raw-stack flag of a graph build does not change its CFG
     (`independentCallPop` and `usesAnalysisRecovery` are 32-bit x86 only);
     a graph build differs from the callee's own build by the no-return
     depth (one deeper) and the context sets.
   - Landed (`c41b8090fd`): an x64 whole-image batch large enough to run in
     parallel first builds every function without the fixup, each build
     keeping its own graph for its callers (valid while the prover answers
     its questions one level deeper as before); a second pass keeps each
     body the fixup leaves unchanged. Of capstone's graph-build time, 95%
     had started while the callee's own build was still running. Capstone
     LLVM C CPU -12%, wall -11.5%; pe64-capstone -4.8%/-8.6%; x64 Rust
     -5.2%/-9.7%; libsqlite3 -1.8%; libstdc++ -0.6%; identical output on
     those, ntoskrnl and the x64 Windows EH corpus. Selected runs keep one
     pass: their cache keeps call-register bodies, which the left-out callee
     graph builds supply.
   - The fallback rebuild, and with it that second half of the graph builds,
     no longer runs on images whose arbitration has nothing to decide
     (`3533bc3bee`): no recovered table requests a relocation slot and no
     table's storage may be written. That held for every performance input
     with tables and for 123 of the 126 EH-corpus binaries with tables (the
     other three are Go arm64 executables with absolute or writable tables).
     The inventory had run out of the per-occurrence and per-function limits
     it borrows on every large image before; on Rust its fixed point also
     never settled, and on libsqlite3, libstdc++ and ntoskrnl a call window
     with two cells at one coordinate refused the module (both fixed in
     `671a79251b`). Six alternating pairs, whole images: capstone LLVM C CPU
     -44%, wall -42%; pe64-capstone -18%/-23%; x64 Rust -21%/-33%; Mach-O Rust
     -6%/-21%; libsqlite3 -6%/-4%; libstdc++ -5%/-5%; ntoskrnl -2%/-2%.
     250-function selections, which took the fallback too: capstone LLVM C
     -48%/-30%, pe64-capstone -31%/-30%, x64 Rust -27%/-34%; identical output.
     LLVM C over the 176 x64 corpus binaries changes only the six Rust
     x86_64-pc-windows-msvc outputs that HighC changes.
   - On ntoskrnl the fallback had left KiServiceInternal's argument-copy
     dispatch unproved (a trap), and with it the 247 `Zw` stubs showed no
     arguments; now the dispatch resolves and each stub shows the four
     registers it forwards, which over-approximates services with fewer
     (`ZwFlushInstructionCache` gains a fourth parameter beyond its declared
     three, the policy that already extends nine declared prototypes there).
     The cause was the lost-table verdict, fixed in `a3e5abd7ba`: a build
     resolves again after computed-branch recovery, a rebuild is seeded with
     the branches its function published, and each resolve judged a published
     table without targets lost at its own end. The rebuild's first resolve
     ran before the recovery that republishes this dispatch
     (`sub r11, rax; jmp r11`), so the final graph kept all 16 targets and
     still published the branch unsafe. Judged once, on the final graph, the
     full fallback keeps the switch; no corpus output changes. (The commit
     message of `3533bc3bee` blames the protected set's size; that is wrong.)
   - Limits decided (`3a1260673d`), after scoping (`a55d84570b`). EH
     continuation discovery now solves only catch funclets for returned
     continuations and runs the local-unwind inventory only where the image
     imports `_local_unwind`; it completes on every input, ntoskrnl and Go
     Windows included, with no inventory there and identical output (Rust
     windows-msvc probe wall -30%, CPU -8%). The arbitration inventory has no
     input cap and may make 65536 visits or 2 per operation it reads,
     transfers sixteen times that; its solves run in parallel, exactly. A
     census of every inventory, run on every image with the nothing-to-decide
     shortcut off, found the work where the limits bit: frame pointers already
     imprecise and possibly outside the frame gaining one offset a round
     (up to 170 visits a block on Go's arm64 runtime) though nothing reads
     them. Widening those at once cut arbitration visits 70% (6.2 to 0.73 per
     operation at most), all 548 outputs identical. Only three Go arm64
     arbitrations need more than the floor (0.31, 0.32, 0.73 visits per
     operation); they no longer take the fallback: linux-arm64 pie proves 8
     more switches (unproved traps 16 to 8), and both darwin images pass the
     live registers to `runtime_goStatusToTraceGoStatus`'s panic and throw
     calls instead of traps. Its signature grows to 8 register parameters
     where Go declares 2: the forwarding over-approximation the `Zw` stubs
     show, not recovered arguments (the message of `3a1260673d` says the
     darwin images keep the function's eight arguments; that overstates it).
     Six alternating pairs
     on 12 threads against their fallback runs: CPU +0.6%, -0.5%, +1.9%; wall
     -5.7%, -2.8%, +1.3%. Without the widening the symbol-table image needed
     2.5 million visits, and a factor of 16 to cover it had cost 61% more CPU.
   - Exact levers, each a resolver-level change:
     - reuse a first-round graph in the rebuild round. Recording the
       membership reads is easy: the unsafe branches are read only by
       `count` of the build's own instructions and by a walk that keeps
       those. The sizes are not: `orderedLookupWork` charges the protected
       slots' size at about 12 sites, and `JumpTableResolverGroup::work` adds
       both sets' sizes, so a grown set can exhaust a budget the first build
       did not. Reuse needs a proof that every such budget had the slack, or
       a decision to charge by the queries made instead of the set size
       (a budget policy change, the user's call);
     - track the resolver's stage-loop dependencies (item 4).
4. **Jump-table resolution rebuilds its flow graph.**
   - On the switch-heavy PE, about half of all CPU samples were in
     `buildResolverFlowGraph`
     (`lib/ir/low/jumptable/JumpTableResolverSlice.cpp:484`). They came under
     `ResolverGraphCache::acquire` (`:3246`), called from
     `CFGBuilder::tableValuesMatchAtUses`.
   - The builder retains one graph per metering mode (`CachedResolverGraph`,
     `CachedUnmeteredResolverGraph`; `include/neverd/ir/low/CFGBuilder.h:1154`),
     matched by `(Snapshot, ProofRoots)`.
   - The same functions form the LowIR tail, in which a few large functions
     each hold a thread.
   - Measure first how many distinct `(Snapshot, ProofRoots)` keys recur per
     function. The answer decides between a small keyed cache and one lease
     shared across a jump table's queries.
   - The graph indexes an address-sorted snapshot with red-black trees:
     `std::pmr::map<std::pair<va_t, int>, ...>` and `std::pmr::map<va_t, ...>`.
     A sorted-vector index fits that shape and would be the first production
     caller of `neverd::sorted::lowerBound64`.
   - The cost is small on inputs with few switches, so measure both kinds. On
     the PE, half of these samples come from item 3's callee graphs.
5. **Ordered containers, allocation and type erasure.**
   - Across all runs, the largest self-time categories were red-black tree
     operations, `operator new`/mimalloc, and libc copies.
   - `std::function` dispatch was visible on the Rust runs; Capstone decoding
     was negligible.
   - Work one profiled site at a time:
     - hot integer-keyed `std::map`/`std::set` that is built once and then
       queried: sorted vectors or `DenseMap`;
     - non-escaping callbacks: `llvm::function_ref`;
     - matching lifetimes: phase-scoped arenas.
   - Keep iteration order wherever output depends on it.
6. **HighIR and LowIR tails.** After the batches drain, the largest functions
   each hold one thread.
   - LowIR batches are weighted by code bytes
     (`lib/pipeline/PipelineLowIR.cpp:5027`), HighIR batches by operation count
     (`lib/pipeline/PipelineHighIR.cpp:358`). Neither weight counts jump
     tables or x87 proofs, which drive LowIR cost (items 3 and 4).
   - Items 3 and 4 also shorten the LowIR tail, and item 7 the HighIR tail.
7. **Late HighIR cleanup reruns all of dead-code elimination.**
   `reduceLateGotos` (`lib/ir/high/lower/MedToHigh.cpp:1252`) runs the whole
   `eliminateDeadStmts` (`lib/ir/high/pass/HighDCE.cpp:749`, about thirty
   passes) after each of its six phases that rewrote, and once more at the
   end.
   - On the whole-image libsqlite3 run at 4c991c2416, these reruns took about
     two thirds of the samples under `eliminateDeadStmts` and about a tenth of
     all CPU. At 6361fc7c9b all of `eliminateDeadStmts` was 1.7% of that run's
     samples, so re-profile before starting. What remains is the tail: one
     x64 libstdc++ function (`time_get::_M_extract_via_format`, 401 blocks)
     spent 4.8 s of its 5.2 s MedToHigh in DCE (`NEVERD_HIGHIR_DETAIL=1`
     splits it into pre, iterative and post).
   - The phase comment asks only that dead copies between a jump and its label
     be cleared. Find what each later phase needs from the cleanup; a
     narrower pass must still give byte-identical output on the regression
     inputs.
8. **Serial loading of small inputs.** `Loader::loadWithProgress` held one
   thread for a visible part of the short libstdc++ runs. Entry: a latency
   target for opening small images.

## x87 call proofs (capstone LLVM C, 2026-11-18)

About a quarter of a whole-unit `decompile --llvm` of capstone (x64 ELF,
about 36 s) is `X87CallEffectIndex::graph` under `fixupFpuStack`: each
callee is lifted again (SkipX87StackFixup, no-return depth + 1) to prove an
x87 contract, though capstone has no x87 code. `796c513988` keeps proved
contracts for the session with their no-return dependencies (-3.3%,
identical output). A decode scan that skips callees whose call closure has no
x87 instruction (Tarjan components over direct calls) was exact but gained
0.8%: 189 of about 570 scanned functions met a register-indirect transfer
(switch tables, function pointers) and 78 an indexed or non-import slot.
Borrowing the primary build's jump tables is not exact: the graph build's
no-return depth and jump-table settings differ, so it can reach code the
primary build cut. A function's own build is exact when it is configured as
a callee graph build at its own depth and the prover answers its questions
one level deeper as before: `c41b8090fd` builds an x64 whole-image batch so
first (item 3). The fallback rebuild's graph builds, the other half, went
with the fallback wherever arbitration has nothing to decide (`3533bc3bee`,
item 3).

## Default-off and unused code: verdicts

| Code | Verdict |
| --- | --- |
| mimalloc large OS pages (`MIMALLOC_ALLOW_LARGE_OS_PAGES=1`) | A small CPU change at most, not separable from host noise in single runs; peak RSS unchanged. Re-measure on a quiet host before changing the default. |
| VQSort (Highway contrib) and `tbb::parallel_sort` | Sorting appeared in no profile. Do not enable them. |
| `neverd::sorted::lowerBound64` | No production caller yet. Bound searches are inlined and do not show in profiles; adopt it with item 4's index. `contains64` already serves the code-owner index. |
| GCC-only sorted-search policy (`include/neverd/support/Limits.h:72-83`) | Applies to GCC builds only. Windows releases use clang-cl, which takes the Clang path. |
| `PipelineOptions::DeferHighIR` / `DeferMedIR` | SDK sessions already select them by depth (`lib/sdk/capi/SessionImpl.h:1088`). Whole-output CLI runs need every stage. |
| Signature cache | Already on by default; `NEVERD_SIGNATURE_CACHE=off` disables it. |
| `kMedToHighSemanticThreadLimit = 1` (`include/neverd/ir/Limits.h:53`) | Intentional. Function batches already run in parallel, and a nested batch drains on one fresh native stack (`lib/support/Parallel.cpp:118`), so raising the limit adds no parallelism. |

## Retained decisions to re-measure

| Decision | Where | Revisit when |
| --- | --- | --- |
| GCC keeps rank-based membership | `include/neverd/support/Limits.h:77-83` | GCC caller measurements show direct membership no longer slows small scalar loops |
| Non-Clang compilers enter SIMD search at 4096 keys | `include/neverd/support/Limits.h:72-75` | A GCC or MSVC A/B shows medium ordered queries gain |
| x86 targets with fewer than four u64 lanes stay scalar | `include/neverd/support/Limits.h:84-85` | SSE2-SSE4 gain consistently across query orders |

## Candidates from `docs/bbtree-evaluation.md`

- **Inline string prefixes** for large symbol-name indexes. Entry: name-index
  lookups appear in a profile. Prefix matches still require a full-key tie
  check.
- **Interleaved batch traversal.** Entry: a caller that already holds
  independent query batches shows lookup cost, for example the batched runtime
  entry-point name lookups (`a89f54a3d3`).
- **Read-only mapped index files.** Entry: a persisted-format and lifetime
  contract. This cannot transparently replace mutable `BinaryImage` metadata.

## Unexplored; profile first

- **Interval-shaped address data.** Evaluate `llvm::IntervalMap` (no uses
  yet). Entry: interval lookups or coalescing appear in a profile.
- **Build.** Profile-guided builds are an opt-in
  (`cmake/NeverDProfileGuided.cmake`). ThinLTO was measured on Linux only and
  left out (§ Rejected). Entry for another look: a whole-request A/B on macOS
  and Windows that also reports build time and binary size. Keep the generic
  ISA baseline; runtime dispatch covers SIMD.
- **JSON.** Transport keeps the nlohmann DOM, and simdjson On-Demand is unused.
  Decompile runs did not show JSON. Entry: JSON parsing or DOM copying is
  significant in a worker or MCP request profile.
- **`std::regex`** in `tools/neverd/mobile/*` and
  `tools/neverd-worker/CodeEdits.cpp`. Entry: a regex runs on a per-item path
  in a profile.
- **A regression guard.** Extend the opt-in benchmark targets and the bounded
  `neverd decompile ... --max-func` run (see `SKILL.md` § Measuring) into a
  pinned-corpus run. Entry: a runner with stable hardware counters. Until
  then, the same-base A/B is the guard.

### Workbench, after an edit

Found while the listing learned to show an edit's lines first (`ffb77c8063`);
none is started.

- **The repaint that follows a dialog.** When a dialog closes, the window takes
  its activation back and repaints its title bar and every item view
  (`QAbstractItemView` updates its viewport on each activation change): 598 k
  pixels, 3 to 10 ms on the GUI thread, of which the functions list is 1.7 to
  5. Where it is still unpainted when the edit's answer lands, the edit's row
  waits behind it. The theme draws an inactive list as an active one, so the
  list's repaint buys nothing; a change needs a test that the two paint the
  same pixels in every theme, high contrast included, and figures taken under
  a window system, where activation always moves.
- **A command for the worker's own writes.** Every edit is followed, 300 ms
  later, by a read of the project's files on the GUI thread and a
  `project_sync` command that answers `changed: false`. While it waits, a
  rename is not read ahead. The worker could say what it wrote, or a read
  could ask before a command does.
- **The listing's rename leaves late.** Its dialog runs inside the delivery of
  the read that asked the address's name, and the rename is sent when that
  delivery returns: 0.4 to 0.5 ms after the dialog is accepted.
- **Lines carried by a worker that slept.** 1.2 ms where the warm path takes
  0.2. The prepare request a pseudocode rename sends as its dialog opens has
  no counterpart for the listing's lines.

## Rejected

`docs/bbtree-evaluation.md` gives the reasons:

- The bbtree Rust container, and any FFI boundary to it.
- Wholesale replacement of LLVM containers.
- GPU offload for single queries.
- Software prefetch, retained caches or per-query allocation in sorted search.
- SSE2-SSE4 SIMD for sorted search.
- The variable-tail search prototype: it regressed random queries.

Measured on whole requests between 2026-11-04 and 2026-11-10, with identical
output, and left out. Each needs a new reason before it is tried again:

- **ThinLTO with lld, `-fno-semantic-interposition` and
  `-Bsymbolic-functions` on top of a profile-guided build** (Linux): 3% more
  CPU and no gain in the median pseudocode request.
- **A jemalloc worker.** One round cut the first-visit total by 15% to 27%
  and left the median flat. After library records were shared per session
  (`4b28263e42`) it was level with mimalloc.
- **Huge pages for the engine's text.** No median gain in a four-round A/B.
- **Warming call-signature probes ahead of requests.** Probe misses fell from
  52.7 to 39.7 per request; the first-visit time did not move.
- **A lower cap on the synthesis search's retained behaviours.** The present
  cap was the fastest of five settings. With no retention CPU rose 4% to 6%,
  and a smaller cap regrows the array (`9edc47b6bb`, `88d388842c`).
- **Keeping the worker thread spinning between spaced edits.** No round-trip
  gain, because the client's own wake-ups dominate, and 2 ms stalls under load
  when the spinner took a core the client needed. A mimalloc purge delay made
  no difference to the same case.
