# bbtree evaluation

The reviewed repository is
[XiangpengHao/bbtree at 8adae97a](https://github.com/XiangpengHao/bbtree/tree/8adae97a0ed97e5c98fa2790312eb0aec9719846).
Its README's Rust container comparisons are not measurements of NeverD.

| Idea | Fit for NeverD |
| --- | --- |
| Dense keys, independent comparisons and rank counting | Applicable to the immutable address vectors in `ExecutableCodeOwnerIndex`. No new index allocation is needed. |
| Inline string prefixes | Potentially useful for large symbol-name indexes, but prefixes require full-key tie checks. No measured NeverD win yet. |
| Interleaved batch traversal | Worth evaluating where callers already have independent query batches. Current single-address ownership queries do not provide that interface. |
| Mutable B-tree replacement | The Rust API is not a C++ container replacement. An FFI boundary, ownership changes and extra dependency are not justified by the current evidence. |
| Read-only mapped index files | Requires a persisted-format and lifetime contract. It is not a transparent replacement for publicly mutable `BinaryImage` metadata. |

The relevant source discussions are the
[cost model](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/06-cost-model.md),
[inline-key design](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/04-inline-key.md), and
[batch API](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/12-batch-api.md).
The implementation is original C++ using NeverD's existing Highway dependency;
no bbtree source or Rust runtime is incorporated. bbtree is dual MIT/Apache-2.0
licensed. No LLVM fork or dependency revision change is required.

## Scope inside LLVM

Replacing LLVM containers wholesale with bbtree is not justified. The
[LLVM container guide](https://llvm.org/docs/ProgrammersManual.html#picking-the-right-data-structure-for-a-task)
selects containers by required operations and access patterns. In the checked
LLVM fork (`06788fb1`), `GVN.cpp` uses `DenseMap` for block availability and
`SmallVector` for worklists. An ordered tree adds traversal and maintenance
cost where hashing or contiguous append/iteration already fits the operation.

LLVM already has a compact B+ tree:
[`IntervalMap`](https://llvm.org/doxygen/IntervalMap_8h.html) stores small maps
inline, separates key/value columns and coalesces equal-valued intervals.
`CodeGen/LiveIntervalUnion.h` uses it for live segments. A generic key/value
tree does not replace its interval operations or iterator contract.

An LLVM integration would need a profiled, sufficiently large ordered index,
matching mutation/iterator semantics, and total workload measurements that
include construction, updates, memory and destruction on supported hosts.
No such LLVM hotspot or end-to-end gain has been established by this work.
The NeverD primitive operates on existing sorted arrays without building a
tree, and remains independent of LLVM.

## Cost model and implementation

`neverd::sorted::lowerBound64` keeps small arrays on the inline standard-library
path. For larger arrays it narrows a sorted interval, then counts unsigned keys
below the query in a bounded, contiguous tail using CPU SIMD. The fixed tail
replaces dependent probes with independent comparisons. Runtime dispatch and
the extra comparisons still cost time, so a vector instruction alone is not
evidence of an improvement.

Clang narrowing retains the pivot on the right with a query-independent interval
length. Other compilers retain standard bisection so ordered inputs can benefit
from predictable branches. A complete tail is extended right or shifted left at EOF; sorted order
preserves the absolute rank. All loads stay within the original array. Scalable
vectors use a scalar remainder when their lane count does not divide the tail.
Duplicates, empty input, unaligned arrays and the complete unsigned address
range retain standard lower-bound semantics.

The first variable-tail prototype regressed random queries. Disassembly showed
key-dependent branches. Clang's
[`__builtin_unpredictable`](https://clang.llvm.org/docs/LanguageExtensions.html#builtin-unpredictable)
must appear directly in the conditional expression for this implementation to
retain conditional moves; assigning the hinted value to a boolean temporary
did not change the generated x86 code. A fixed tail then removed the variable
loop/remainder overhead. Rejected prototypes are not production paths.

The policy values live in `support/Limits.h`; Highway owns ISA feature dispatch.
Clang admits the SIMD path from 64 keys; other compilers use a conservative 4096
key threshold to keep medium ordered queries on the scalar path. x86 targets
with fewer than four 64-bit SIMD lanes retain standard lower bound: SSE2,
SSSE3 and SSE4 experiments did not show consistent gains across query orders.
There are no prefetches, retained caches, locks or per-query allocations. The
foundation target is independent of LLVM and the loader, avoiding a dependency
cycle through `NeverDSupport`.

Membership queries have their own SIMD reduction: OR equality masks and test
whether any lane matches, instead of counting a rank and reloading that element.
GCC retains the previous rank-based membership through the owning limits policy;
the direct path changed small scalar caller loop generation unfavorably. This
choice preserves the existing query path even when its data is below the SIMD
threshold. The benchmark's `contains` mode includes this caller-level effect.
Both operations share one narrowing implementation. Membership narrows to
strictly fewer than the tail's keys before loading the fixed tail, because the
lower-bound candidate may equal the narrowed interval's exclusive end. This
includes that candidate without reading beyond the original array, even when
duplicates straddle a pivot or the original input ends at a protected page.

## Measurement scope

The benchmark validates every result against a standard-library reference and
reports wall and process CPU time. Parsing, reference computation, allocation
and index construction are untimed. Exercise small, medium and large arrays,
plus defined dynamic function addresses extracted from a real ELF shared
library; record the input library's hash with the local evidence. The example
below alternates exact addresses and address-plus-one probes, in seeded random
and sorted order. These are synthetic query traces over real addresses, not a
recorded decompilation trace.

Use Release builds with matching compiler settings and dependency revisions,
fixed CPU affinity and alternating process order. Keep builds, tests and
profilers outside timing runs and record any external contention. Small control
cases help expose scheduling and frequency noise. Compare the previous and
candidate `owner` binaries for integration evidence, and their `contains`
modes for the primitive; `std-contains` supplies a standard-library reference.
These component measurements do not establish whole-CLI/GUI or IDA/radare2
superiority. Keep raw samples, source/binary/input hashes, build receipts and
machine-specific summaries in ignored build directories; summarize results in
the pull request, as required by [the artifact policy](../CONTRIBUTING.md#documentation-and-local-artifacts).

GPU offload has no demonstrated benefit for this single-query path. A future
experiment would need a resident batch workload and must include transfer,
submission and synchronization cost. This change uses CPU SIMD and adds no GPU
runtime dependency.

See [acceleration validation](testing.md#acceleration-libraries) for the
standalone tests, optional benchmark target and native/scalar configurations.

The local input generation is reproducible with Python's `random.Random` seed
20261025. Extract `nm -D --defined-only -n LLVM_LIBRARY` into
`build-sorted-search/llvm-symbols.txt`,
then run:

```python
from pathlib import Path
import random

out = Path("build-sorted-search/inputs")
out.mkdir(parents=True, exist_ok=True)
real = sorted({int(p[0], 16)
               for line in Path("build-sorted-search/llvm-symbols.txt").read_text().splitlines()
               if len(p := line.split()) >= 3 and p[1] in "TtWwiI"})
def write(name, values):
    (out / name).write_text("".join(f"{value:x}\n" for value in values))
for name, keys in [("small", [(1 << 63) + i * 32 for i in range(16)]),
                   ("medium", [(1 << 63) + i * 32 for i in range(256)]),
                   ("large", [(1 << 63) + i * 32 for i in range(4096)]),
                   ("llvm", real)]:
    rng = random.Random(20261025)
    queries = [keys[rng.randrange(len(keys))] + (i % 2)
               for i in range(1048576)]
    write(name + "-keys.hex", keys)
    write(name + "-mixed-queries.hex", queries)
    write(name + "-ordered-queries.hex", sorted(queries))
```

## Validation boundaries

Run the acceleration tests with Clang and GCC, ASan/UBSan, and Highway disabled;
run the code-owner integration owner in the full build. Include native x64 and
ARM64 jobs for Linux, macOS and Windows. ARM64 emulation can additionally cover
NEON and 384-bit SVE/SVE2, but does not qualify native speed. Use the test XML
inventory to distinguish executed checks from skipped or unavailable checks.

The benchmark uses `GetProcessTimes` on Windows because [Microsoft documents
that CRT `clock()` measures wall time](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/clock).
On Unix it uses the process CPU clock exposed by `std::clock()`. The empty-array
contract follows [C++20 pointer arithmetic](https://timsong-cpp.github.io/cppwp/n4861/expr.add)
and is covered by explicit null-input and sanitizer checks.
