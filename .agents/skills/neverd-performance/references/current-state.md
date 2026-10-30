# Performance: current state

Basis: `origin/dev` 159588db82 (2026-10-27). Line numbers drift; search for the
symbol when one no longer matches.

## Build

- Release is the measured profile. There is no LTO, PGO or `-march` baseline;
  Highway and the SHA-256 code select ISA features at run time.
- Two targets force `-O3 -fomit-frame-pointer` (MSVC `/O2`) outside Debug:
  Capstone's decoder (`CMakeLists.txt:127`) and NeverDDecode's AArch64 operand
  decode (`lib/decode/CMakeLists.txt:31`).
- Highway, simdutf, simdjson, oneTBB and mimalloc follow the default branches
  of the NeverSight forks in `third_party/forks.json`. Each build records the
  commits it used in `neverd-dependency-inputs.json`.

## Measurement tools

- `tools/neverd-bench/sigprof`: an `LD_PRELOAD` CPU-time stack sampler for
  hosts without `perf`. Its `report.py` prints self and inclusive shares,
  phases and serial wall time, and its `ab.py` runs paired comparisons.
- `NEVERD_NATIVE_PHASES=1` prints each pipeline stage's elapsed time
  (`lib/pipeline/Pipeline.cpp:136`).
- The opt-in benchmark targets are listed in `SKILL.md` § Measuring.

## Libraries

| Library | Option | Used by | Shape |
| --- | --- | --- | --- |
| Highway | `NEVERD_ENABLE_HIGHWAY` | `lib/support/ByteScan.cpp:28` (dispatch `:62`), `lib/support/SortedSearch.cpp:28` | Runtime dispatch. Contrib, including VQSort, is not built (`cmake/NeverDPerformance.cmake:150`). |
| simdutf | `NEVERD_ENABLE_SIMDUTF` | `lib/support/Unicode.cpp:21`, used by `lib/support/TextEncoding.cpp:27` | Text of 64 bytes or more (`include/neverd/support/Limits.h:88`) that validates whole skips escaping. |
| simdjson | `NEVERD_ENABLE_SIMDJSON` | `tools/common/transport/Json.cpp:120` (thread-local parser `:142`), `lib/patch/PatchJSON.cpp:207` | DOM tape. Integer-only frames copy directly; other input takes the original path. |
| oneTBB | `NEVERD_ENABLE_TBB` | `lib/support/Parallel.cpp` | Scheduler only: task arena, isolated task group, checked 8 MiB stacks. Linked statically with private symbols. No `tbbmalloc`, TBB algorithms or concurrent containers. |
| mimalloc | `NEVERD_ENABLE_MIMALLOC` | `cmake/NeverDMimalloc.cmake`, `tools/neverd-worker/main.cpp:405-424` | Executables only. The worker tunes purge delay and collects. |
| LLVM ADT/Support | none | everywhere | `SmallVector`, `DenseMap`/`DenseSet`, `StringMap`, `BitVector`, `xxh3_64bits`, `MemoryBuffer` |

## In-house fast paths

- **C declaration lexer**: punctuation retains literal lengths and rejects a
  different first character before comparing its spelling. Only known object
  macros enter recursive expansion; ordinary identifiers append directly.
  Expansion depth, token ownership, longest matching and diagnostic positions
  retain their parser contracts. This reduces first-use header parsing in the
  worker as well as direct type-library imports; it adds no retained cache.
- **Ordered u64 search**: `neverd::sorted::lowerBound64` and `contains64`
  (`include/neverd/support/SortedSearch.h:30`, `:36`;
  `lib/support/SortedSearch.cpp`).
  - Applies to existing sorted arrays the dense-key comparison idea assessed
    in `docs/bbtree-evaluation.md`. Landed in PR #1760 (`257455ec80`), with
    direct SIMD membership following in `611d2a856b`.
  - Narrows the interval without branches on Clang (`__builtin_unpredictable`,
    `:58`), then counts a fixed 16-key tail with SIMD. It never reads padding.
  - Policy: `include/neverd/support/Limits.h:68-85`. Callers:
    `lib/loader/ExecutableCodeOwnerIndex.cpp:233-245`.
- **ASCII runs**: `textASCIIPrefix` (`include/neverd/support/ByteScan.h:34`)
  probes 16 bytes inline and dispatches to Highway from 32 bytes
  (`include/neverd/support/Limits.h:66`). Called by string scanning in
  `lib/support/StringScan.cpp`.
- **Signature index**: `SignatureMatcher::HashIndex`
  (`include/neverd/sigs/SignatureMatcher.h:88`).
  - A decision trie over a module's leading bytes; each node holds sorted
    exact edges and one wildcard edge.
  - Built with stable 257-bucket counting partitions
    (`lib/sigs/SignatureMatcher.cpp:496`), a shared-prefix skip using
    `countr_zero` (`:473`), parallel prefix extraction (`:361`) and a
    heaviest-first parallel build of the subtrees.
  - Lookup uses `lower_bound` (`:597`). Policy:
    `include/neverd/sigs/SignatureLimits.def:38-39`.
- **Pattern text**: SSE2 hex decoding in `lib/sigs/PatternParser.cpp:250`.
- **SHA-256**: ARMv8 SHA2 or x86 SHA-NI after host feature detection
  (`lib/support/SHA256.cpp:164`); otherwise LLVM's implementation (`:370`).
- **Cache keys**: `llvm::xxh3_64bits` in `lib/sigs/SignatureCache.cpp:170`.
- **Parallel loops**: `include/neverd/support/Parallel.h`.
  - `parallelForEach` (`:121`) claims indices from an atomic counter.
  - `parallelForEachWeighted` dispatches the heaviest items first.
  - `defaultWorkerThreadCount` (`:60`) prefers performance cores on Apple
    Silicon; `NEVERD_THREADS` (`:103`) overrides the count.
- **Worker viewport**: a bounding-volume hierarchy in
  `tools/neverd-worker/GraphSnapshot.cpp:66-96`. It splits at the median with
  `nth_element`, keeps at most 16 boxes per leaf, and returns only the visible
  nodes and edges.
- **Arena**: `lib/ir/low/jumptable/JumpTableResolverSlice.cpp:373`.
- **Mapped input**: `lib/loader/Loader.cpp:83` and `:169`
  (`RequiresNullTerminator=false`, `caf7af4f5c`).

## Recurring patterns in the history

| Pattern | Example commits |
| --- | --- |
| An index instead of a scan per query | `e37aec14d1`, `83175809ee`, `cc35ba05a9`, `b5b9f3e1e6` |
| Built once per thread or phase, keyed by invariant inputs | `7d192005b5`, `d9f4afdede` |
| Deferred or skipped unread work | `41fe5c5f96`, `a23c299f2a`, `c3aef8bc81`, `008052010d` |
| Reused validated results | `592434ba3f`, `0c3c246305`, `8d7d8ec9ab` |
| Stopping at no progress or a proven minimum | `1d0ce93dc2`, `0866786a7a` |
| Independent proofs in parallel | `fc3b82528e`, `3a565e2b3b` |
| Fewer allocations, reservation, streaming, batching | `13cd3b9a5a`, `6e75924551`, `d75ead04bc`, `a89f54a3d3` |
| Parsing once from the validated tape | `a5f0527ba5` |
| Worker and GUI latency: retained pages, deferred encoding | `3fd1fe135c`, `2497ae3c62`, `aa3cf127ab` |
| Symbolic/MBA: packed keys, dense lookups | `53516fa07b`, `66db685f89` |
| Decoder dispatch and stack frames | `47af7133db`, `dfc2a225a6` |
| Graph algorithms (loops via SCCs) | `e20fad37c5` |
| Acceleration infrastructure | `8c6b37a00c`, `90999fc74f`, `4e4e2ada9b`, `385d1b34dd`, `855ae6c5aa` |

More: `git log origin/dev -i -E --grep='^(perf|\[perf\])' --oneline`.

## Baseline facts

Counts are occurrences in `include`, `lib`, `tools`, `plugins` and `pluginsdk`
at the basis commit.

- Ordered containers: 3974 `std::map<` and 3626 `std::set<`, about half keyed
  by integers or addresses.
- Sorting: 650 `std::sort`/`llvm::sort` and 64 stable sorts, all from the
  standard library. `llvm::sort` shuffles only under `EXPENSIVE_CHECKS`.
- Bound searches: 282 `lower_bound`/`upper_bound`/`partition_point` calls.
- No `llvm::IntervalMap`, and no `BumpPtrAllocator` in NeverD code.
- Matches for `radixsort`, `bsearch` and `memmem` are libc declarations in
  `lib/types/headers`, not calls. Most `_mm_*` names model decompiled
  intrinsics; they are not NeverD's own SIMD.
