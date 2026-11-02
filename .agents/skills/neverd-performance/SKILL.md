---
name: neverd-performance
description: >
  Keep NeverD fast without changing what it computes. Use when writing or
  reviewing code on a hot path (loaders, decode/lift, Low/Med/High IR passes,
  signature matching, symbolic/MBA proofs, emulation, worker/GUI latency),
  choosing a container or index, adding a parallel loop, SIMD, cache, arena or
  threshold, evaluating a performance library, or claiming or reviewing a
  speedup or regression. Use when the user runs /neverd-performance.
---

# NeverD performance

NeverD is semantics-first and must also be fast. A fast path computes exactly
what the portable path computes, and every speed claim is measured.

Basis: `origin/dev` 159588db82 for the inventory and 92b7f8dab1 for the
profiles (2026-10-27). `dev` moves quickly; re-check a path before relying on
a line number.

- What exists today: [references/current-state.md](references/current-state.md)
- What to do next, and what was rejected: [references/remaining-work.md](references/remaining-work.md)

This skill routes; these documents own the rules:

| Topic | Owner |
| --- | --- |
| Acceleration libraries, allocator, build options | `CONTRIBUTING.md` § Process allocator, § Performance libraries |
| Thresholds, budgets, block sizes, cache retention | `CONTRIBUTING.md` § Limits, defaults and configuration ownership |
| Where measurements live | `CONTRIBUTING.md` § Documentation and local artifacts |
| Acceleration and allocator validation | `docs/testing.md` § Acceleration libraries, § Allocator validation |
| No just-in-case machinery | `docs/development-principles.md` § Speculative defenses |
| Library evaluation example | `docs/bbtree-evaluation.md` |

## Rules for new code

1. **Complexity before constants.** Do not scan all functions, blocks, calls or
   instructions per query. Build an index once per phase, function or image,
   for example blocks by id (`e37aec14d1`). In loops over functions, calls or
   registers, do not rebuild a derived structure per call: build it once and
   key it on every input it depends on (`7d192005b5` keys on target and
   format). Before an expensive build or proof, rule out the functions it
   cannot change with a cheap precondition (`772f5b2283`, `71208d60e9`). For
   example, `buildHighSourceFlowGraph` builds a whole guarded graph each time a
   pass calls it.
2. **Skip unread work; reuse proven facts.** Defer reconstruction until a
   consumer asks (`41fe5c5f96`, `a23c299f2a`), stop a rewrite loop that makes no
   progress (`1d0ce93dc2`), and reuse validated results across selections
   (`592434ba3f`). Recompute when a mutation can invalidate them, for example
   symbol edits.
3. **Choose containers by operation.**

   | Access pattern | Use |
   | --- | --- |
   | Small sequence, worklist | `llvm::SmallVector` |
   | Exact-key lookup | `llvm::DenseMap` / `DenseSet`; reserve when the count is known (`739fc32a3c`) |
   | Unsigned 64-bit keys, built once, queried many times | sorted `std::vector<uint64_t>` plus `neverd::sorted::lowerBound64` / `contains64` |
   | Ordered mutation interleaved with ordered queries | `std::map` / `std::set` |
   | Spatial query over rectangles | the worker's BVH pattern in `tools/neverd-worker/GraphSnapshot.cpp` |

   Hashed and parallel results never decide output order. Sort, or merge by
   index, before emitting.
4. **Reuse the existing fast primitives** (table below) before writing a new
   one. A new primitive or library needs an evaluation like
   `docs/bbtree-evaluation.md` first: fit, alternatives, cost model,
   measurement scope and validation boundaries.
5. **SIMD goes through Highway** with `HWY_DYNAMIC_DISPATCH`. Raw intrinsics are
   only for operations Highway lacks (SHA extensions, for example), behind a
   host feature check with a portable fallback (`lib/support/SHA256.cpp`).
   - Keep short inputs on an inline scalar path.
   - Never read outside the input: no padding reads and no over-reads at EOF.
   - Add no software prefetch, retained state or locks without measured
     evidence.
   - Test every Highway target, `NEVERD_ENABLE_HIGHWAY=OFF`, unaligned input
     and input ending at an inaccessible page.
   - When the design depends on branch-free code, check the disassembly. Clang's
     `__builtin_unpredictable` must appear in the conditional expression itself.
6. **Parallelism uses `neverd::parallelForEach`** or `parallelForEachWeighted`.
   Use the weighted form, heaviest first, when element costs are skewed.
   - Each worker owns the slots it writes. Shared containers are sized before
     the batch and never resized inside it.
   - Keep small batches serial. Their thresholds belong in the owning limits
     (`limits::kMinParallelIRWorkItems`, `SignatureLimits.def`).
   - Do not call oneTBB or start raw `std::thread` workers. Respect
     `workerThreadCount()` and `NEVERD_THREADS`.
   - Nested batches are legal but drain on a fresh native stack, so avoid them
     on hot inner paths.
   - Do not add whole-image or whole-selection work on the caller's thread.
     Per-function work belongs inside the existing `parallelForEachWeighted`
     batches. Interprocedural work needs a stated parallel plan, or a stated
     reason it cannot have one. Half or more of a profiled run's wall time
     already passes with one or two busy threads
     (`references/remaining-work.md`).
7. **Allocation.** Reserve when sizes are known, avoid per-element heap
   allocation in hot loops, and use a phase-scoped arena when lifetimes match
   (`std::pmr::monotonic_buffer_resource` in
   `lib/ir/low/jumptable/JumpTableResolverSlice.cpp`). Allocator replacement
   stays in executables: the SDK never imposes one.
8. **I/O and serialization.**
   - Map inputs with `RequiresNullTerminator=false` (`caf7af4f5c`).
   - Stream large JSON instead of building a whole DOM (`d75ead04bc`).
   - Batch repeated lookups (`a89f54a3d3`).
   - Read typed rows from the validated simdjson tape (`a5f0527ba5`).
9. **Policy values live in the owning limits header**, including
   compiler-specific values. The `SortedSearch` block in
   `include/neverd/support/Limits.h` is the model: each value carries a comment
   giving its reason.
10. **Same output under every configuration**: any thread count, acceleration
    on or off, either allocator. A fast path that cannot decide falls back to
    the exact path. It never approximates or guesses.

## Which existing facility

| Need | Use | Where |
| --- | --- | --- |
| Lower bound or membership on sorted u64 keys | `neverd::sorted::lowerBound64`, `contains64` | `include/neverd/support/SortedSearch.h` |
| Length of an ASCII text run | `neverd::strings::detail::textASCIIPrefix` | `include/neverd/support/ByteScan.h` |
| Whole-buffer UTF-8 fast path | `neverd::knownValidUTF8`; `false` means "take the exact path", not "invalid" | `include/neverd/support/ByteScan.h` |
| Parallel independent items | `parallelForEach`, `parallelForEachWeighted` | `include/neverd/support/Parallel.h` |
| Transport or MCP JSON | `parseJson` | `tools/common/transport/Json.h` |
| Patch JSON rows | simdjson DOM tape reader | `lib/patch/PatchJSON.cpp` |
| Content hash for a cache key | `llvm::xxh3_64bits` | `lib/sigs/SignatureCache.cpp` |
| SHA-256 digest | `neverd::sha256` | `include/neverd/support/SHA256.h` |
| Input bytes | `llvm::MemoryBuffer::getFile(..., /*RequiresNullTerminator=*/false)` | `lib/loader/Loader.cpp` |

## Measuring

1. **Profile first.** Run a sampling profiler on a warm Release run and
   optimize what the profile shows. Where `perf` is unavailable, use
   `tools/neverd-bench/sigprof` (see its README); `NEVERD_NATIVE_PHASES=1`
   prints each pipeline stage's elapsed time.
   - Record the hotspot's CPU share **and** its serial wall share: the time,
     by phase, during which at most two threads were busy (`report.py` prints
     it).
   - A phase with a small CPU share that runs on one thread can still take a
     large part of the wall time on a many-core host.
2. **Same-base A/B.** Build baseline and candidate from the same `origin/dev`
   commit. Use Release, the same compiler, the same dependency snapshot
   (`neverd-dependency-inputs.json`), the same allocator and the same
   `NEVERD_THREADS`.
3. **Alternate A and B runs** over many repetitions, and fix CPU affinity where
   possible. Report user+sys CPU and wall time separately. On a shared host,
   CPU time is the primary signal for compute work; wall time is the signal for
   latency and scaling.
4. **Hash all non-timing output** from both binaries. Any difference blocks the
   change.
5. **Measure both levels.** Measure the primitive and the whole request. A
   primitive win that does not reach the request is not a claim.
6. **Cover the configurations** the change touches: default and constrained
   threads (`NEVERD_THREADS=1`), `NEVERD_ENABLE_{HIGHWAY,SIMDUTF,SIMDJSON,TBB}=OFF`,
   and `NEVERD_ENABLE_MIMALLOC=OFF`.
7. **Keep raw results local.** Put raw samples, traces and summaries in ignored
   build directories. In the PR, give the base commit, commands, input
   hashes, host and paired CPU deltas. Do not commit timing tables.

```bash
# Whole pipeline, bounded; repeat alternately for A and B.
/usr/bin/time -f '%U user %S sys %e wall %M KiB' \
  build-a/bin/neverd decompile IMAGE --max-func=250 -o build-a/out.c
sha256sum build-a/out.c build-b/out.c
```

Primitive benchmarks are opt-in (`EXCLUDE_FROM_ALL`) targets:
- `neverd-acceleration-bench`, and `neverd-sorted-search-bench` with modes
  `std`, `simd`, `std-contains`, `contains` and `owner`. Both are in
  `unittests/acceleration`.
- `neverd-signature-bench`, `neverd-med-bench`, `neverd-cpu-bench` and
  `neverd-solver-bench` in `tools/neverd-bench`.
- `neverd-viewport-bench` and `pseudocode_latency_bench.py` in
  `tools/neverd-gui/benchmarks`.

Correctness owners are `NeverDAccelerationTests`, plus the owners listed in
`docs/testing.md` § Acceleration libraries.

After merging someone else's PR, re-check performance the same way: compare
`dev` with the merge reverted against `dev` with the merge, on the same base.
Push follow-up fixes when the merge regresses.

## Review checklist

- [ ] The profile names the hotspot; the A/B uses the same base; output hashes match.
- [ ] No per-query scan or per-call rebuild inside loops over functions, blocks or calls.
- [ ] No new whole-image or whole-selection work on the caller's thread; the serial wall share did not grow.
- [ ] The scalar/portable path remains and is tested with acceleration off; SIMD covers every Highway target and inaccessible tails.
- [ ] Every new threshold or budget lives in its owning limits definition, with a reason.
- [ ] Output is identical across thread counts; hashed and parallel results are ordered before emission.
- [ ] Each cache is keyed on all its inputs, invalidated on mutation, and has bounded retention in limits.
- [ ] No speculative caches, prefetches, retries or fallbacks.
- [ ] A new library or primitive has an evaluation document, a NeverSight fork if vendored, notices, and a `NEVERD_ENABLE_*` opt-out.
