# CPU-time stack sampling

`sigprof.cpp` profiles a NeverD process where `perf` is unavailable, for
example when `kernel.perf_event_paranoid` forbids it. It is an `LD_PRELOAD`
library for Linux x86-64 and AArch64: `ITIMER_PROF` advances with the
process's CPU time, so each thread is sampled in proportion to the CPU it
uses. `report.py` symbolizes the samples with `nm` and `readelf`. `ab.py`
compares two configurations in alternating runs. None of these is part of the
product build.

## Build and record

```sh
c++ -std=c++20 -O2 -fPIC -shared -o build-prof/sigprof.so \
  tools/neverd-bench/sigprof/sigprof.cpp
NEVERD_SIGPROF_OUT=build-prof/sqlite NEVERD_NATIVE_PHASES=1 \
  LD_PRELOAD="$PWD/build-prof/sigprof.so" \
  build-release/bin/neverd decompile IMAGE --max-func=250 -o build-prof/out.c
```

The process writes `build-prof/sqlite.<pid>.bin` and `.maps` when it exits.
`NEVERD_SIGPROF_HZ` changes the rate (default in `../ResourceLimits.h`).
`NEVERD_NATIVE_PHASES=1` independently prints each pipeline stage's elapsed
time to stderr. Profile a Release build; it keeps the symbol table that the
report needs. Keep samples and reports in ignored build directories.

## Read the report

```sh
python3 tools/neverd-bench/sigprof/report.py build-prof/sqlite.<pid>.bin
python3 tools/neverd-bench/sigprof/report.py build-prof/sqlite.<pid>.bin \
  --focus 'ResolverGraphCache::acquire'
```

- **self** and **inclusive** are shares of CPU samples.
- **phases** attributes each sample to its outermost pipeline phase and
  counts the threads that ran it.
- **wall time with <= 2 sampled threads** is the time the host's other cores
  sat idle, by phase. On a many-core host a phase with a small CPU share can
  dominate this list; check it before optimizing CPU hotspots.
- `--focus REGEX` lists the callers and callees of the innermost matching
  frame, and `--timeline` prints every bucket.

Demangled lambda and template frames spell out the parameter types of their
enclosing function, so an unanchored pattern such as `X87CallGraphCache` also
matches a frame that merely receives one. Anchor class patterns at the start
of the name, for example `--focus '^neverd::X87CallEffectIndex::'`.

Frames come from the unwinder, so code without unwind tables ends a stack.
libc exports only some symbols; its internal functions, such as the `mem*`
and `str*` implementations, appear under the nearest export or as
`<libc.so.6+address>`. Use `--focus` to find who calls them.

## Compare two configurations

```sh
python3 tools/neverd-bench/sigprof/ab.py --runs 7 \
  --a build-a/bin/neverd --b 'MIMALLOC_ALLOW_LARGE_OS_PAGES=1 build-b/bin/neverd' \
  -- decompile IMAGE --max-func=250 -o '{out}'
```

`ab.py` alternates the order of each pair and reports median CPU (user+sys)
and wall time with the per-pair range. Every run's output hash must agree;
otherwise the timing comparison is meaningless.

For a short command, use `--show-pairs` to retain each A and B observation
in milliseconds and its execution order. The signed B−A summary takes the
median of differences within pairs. A positive wall delta means B took longer.
For a suspected submillisecond regression, repeat the same experiment with two
copies of one binary and library. When both versions share a launcher, keep
its path fixed and verify which SDK the loader selected for each side. If
identical SDK copies in different directories give a persistent offset, repeat
with both SDKs loaded from the same pathname. For warm-file-cache measurements,
prime each copy first. On macOS the loaded image UUID identifies the version.
Measure process start to exit separately from time inside the decompile command.

For a macOS pre-main tail, use a fresh process for every launch. Compare the
first executable mapping of a fresh SDK copy, a first mapping after reading
its bytes, and a repeat launch using the same file. Check residency with
`mincore` before timing and verify the file hash afterwards. At the first
statement of `main`, a probe can timestamp entry and block on a pipe. Once
the child is blocked, the parent can read its completed runnable interval
with `proc_pid_rusage`. For a single-threaded launch, runnable time minus
CPU time estimates time waiting for a CPU. Record the remaining elapsed
time separately. An SDK-free process timing `dlopen` isolates library load.

The comparison exits nonzero and suppresses paired results if any command fails,
any output is missing, or any full SHA-256 differs (including between repeated
runs of the same side). A positive `--runs` count is required. CPU ratios are
unavailable when a baseline CPU measurement is zero. Output identity is a
necessary comparison gate, not proof of semantic correctness or full coverage.
This helper does not retain raw command logs or measure peak RSS; record those
separately when qualifying an optimization.

Run the helper's offline regression checks with:

```sh
python3 -m unittest discover -s tools/neverd-bench/tests -p test_sigprof_ab.py -v
```

## Locate a warm macOS launch tail

Use one Release Ninja build of the CLI and two signed SDKs built with the same
configuration. Supply the linked input and function used for the original
timing. The harness uses one HighC thread and disables the signature cache.
The included n32 short function can be assembled with an LLVM toolchain
that supports MIPS.

```sh
clang --target=mips64-linux-gnuabin32 -mabi=n32 -march=mips64r2 -mabicalls -fPIC \
  -Wa,-L -c tools/neverd-bench/sigprof/warm-route.s -o build-release/warm-route.o
ld.lld -m elf32btsmipn32 -shared build-release/warm-route.o -o build-release/warm-route.elf
python3 tools/neverd-bench/sigprof/macos_warm_launch.py \
  --build build-release --baseline-sdk before/libneverd.dylib \
  --candidate-sdk after/libneverd.dylib --image build-release/warm-route.elf \
  --function route_value --jsonl build-release/warm.jsonl
python3 tools/neverd-bench/sigprof/macos_warm_report.py \
  build-release/warm.jsonl --json build-release/warm-report.json
```

Each round interleaves the stock CLI, a privately relinked CLI that timestamps
entry to and return from `main`, and an A/A control. The control uses two
additional baseline copies so both primary SDK inodes see the same number of launches.
The probe writes both timestamps after `main` returns. The parent reads kernel
birth and exit times before reaping the child. This separates startup, work
inside `main`, teardown and the delay before the parent observes exit. Both
executables must load the selected SDK and emit byte-identical C. Switch the
primed SDKs by renaming their containing directories. Creating or removing a
hard link changes the file's metadata and can cause library validation again.

On a dedicated host, add `--control-load` to alternate ambient rounds with two
CPU workers per logical CPU. A quiet round uses at most 35% CPU in both
ambient windows and at most 70% during the measurements. A busy round uses at
least 85% in both windows and at least 90% during the measurements. Other
rounds appear as `unclassified`. Compare the difference between the two P95s.
The interval resamples rounds and consecutive pairs. To attribute the tail,
rank complete launches by wall time and compare their component times on those
same launches. The pre-main breakdown uses the kernel's process birth timestamp
to separate time before the child exists from time spent reaching `main`.

When background work occupies the measurement host, add `--quiet-wait 100`
to wait for three consecutive
three-second quiet windows before each unpressured round and observe a fixed
seven-second window after it. A timeout keeps the round in the raw data.
The report requires a successful wait to label that round quiet. Report measurements
from a different machine in a separate group.

Add `--entry-counters` to investigate the interval from process birth to
`main`. The private executable samples CPU and runnable time at entry and
blocks its initial thread on a socket. The parent reads the process's scheduling
and VM counters, then acknowledges the entry marker. The report ranks these launches by
birth-to-entry time and reports bounds on the excess CPU time, runnable time
spent waiting for a CPU, and remaining wall time. A delayed observation widens
the bounds. The remaining time includes waits such as page faults and process
setup. Process-wide fault, page-in and disk-read counts describe the same
launches. Specific SDK operations require a separate loader measurement.
The stock CLI provides the full-command P95 in both modes. The accounting
probe requires a single thread throughout initialization.

A separate `DYLD_PRINT_SEGMENTS` trace can time the SDK's segment mappings.
In Apple's dyld, the SDK's `Mapping` line follows library validation and each
segment line follows its mapping. Verbose logging changes launch time, so
measure full-command P95 with logging disabled. [dyld segment mapping](https://github.com/apple-oss-distributions/dyld/blob/dyld-1286.10/dyld/Loader.cpp#L1541-L1655)

### Warm launch qualification

The stock HighC measurements compare the SDK at `b185e8f` with `035a081`.
The M4 Max used the original signed images. The M1 VM ran the same public
revisions built on a separate machine. Both used the same n32 machine
instructions and emitted identical C. These runs shared the A/B baseline inode
with the A/A control, giving the baseline additional warm launches.

| Host and measured load | A/B pairs | Before P95 (ms) | After P95 (ms) | 95% interval for change (ms) | Extra time before `main` (ms) | Extra time inside `main` (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M4 Max, busy | 512 | 265.00 | 267.78 | -30.23 to +33.15 | 218.22 | 38.17 |
| M1 VM, quiet | 768 | 33.52 | 33.59 | -0.96 to +2.27 | 7.86 | -1.89 |
| M1 VM, busy | 768 | 37.37 | 38.52 | -0.93 to +2.41 | 10.09 | 0.41 |

The P95 interval resamples whole rounds and blocks of consecutive pairs.
Each host and load has its own baseline. The last two columns compare the
slowest 5% of marked candidate launches with the middle 45th–55th percentile
of those launches. The raw observations also record child teardown and parent
wakeup. [Isolated measurement run](https://github.com/NeverSight/NeverD/actions/runs/45855229077)

## Isolate first SDK load on macOS

On macOS 13 or later with the Xcode command line tools installed, run the
native loader probe against the signed Release SDK. Use a scratch directory
on the SDK's APFS volume.

```sh
python3 tools/neverd-bench/sigprof/macos_sdk_load.py build-release/bin/libneverd.dylib \
  --scratch-dir build-release --pairs 8 --json build-release/sdk-load.json
```

Every launch uses a new process. Each pair alternates the order of direct and
staged loads on separate file clones. `fresh` measures the first mapping of
a new clone, `pre_read` reads its bytes before the first mapping, and
`repeat` maps the same file again. The probe records page residency before
each load, verifies the device and inode of the loaded SDK, and checks the
clone's SHA-256 after both launches. These conditions describe the SDK file
cache on a running host. A reboot is a separate experiment.

Compare direct `dlopen` with `staged_total` on the same file state. The latter
includes native Mach-O slice inspection, registration of its embedded signature,
library validation, and the subsequent ordinary `dlopen`. Moving validation
earlier can make that last call appear much faster. The total includes the
cost of moving it. [Apple's dyld loader](https://github.com/apple-oss-distributions/dyld/blob/dyld-1286.10/dyld/Loader.cpp#L1535-L1555)

The probe records wall time for each operation. A separate process samples
the single loader thread's process counters while the thread waits on a pipe.
`ready` estimates runnable time waiting for a CPU. `other` is elapsed time
outside the runnable interval and can include blocking inside validation.
The `empty` phase shows the sampling resolution. Very short phases can have
small negative residuals. The summary reports medians and nearest-rank wall
P95 within each cell. The JSON keeps paired samples for comparisons across
builds. Time the full HighC command separately to measure user-visible latency.

For a smaller SDK, check the unstripped images before timing. Compare local
Mach-O symbols by name, type, section, flags and multiplicity. Compare dyld
exports separately. Alternate builds at the same load path and use separate
new clones for the probe and the complete command. Running one on the other's
clone warms library validation for the second measurement.

```sh
python3 -m unittest discover -s tools/neverd-bench/tests -p test_macos_sdk_load.py -v
```
