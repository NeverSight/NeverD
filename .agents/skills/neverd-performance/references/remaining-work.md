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
- **Build.** ThinLTO and PGO are unevaluated. Entry: a whole-request A/B on
  Linux, macOS and Windows that also reports build time and binary size. Keep
  the generic ISA baseline; runtime dispatch covers SIMD.
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

## Rejected

`docs/bbtree-evaluation.md` gives the reasons:

- The bbtree Rust container, and any FFI boundary to it.
- Wholesale replacement of LLVM containers.
- GPU offload for single queries.
- Software prefetch, retained caches or per-query allocation in sorted search.
- SSE2-SSE4 SIMD for sorted search.
- The variable-tail search prototype: it regressed random queries.
