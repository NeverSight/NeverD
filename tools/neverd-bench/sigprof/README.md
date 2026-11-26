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
cost of moving it. [Apple's dyld loader](https://github.com/apple-oss-distributions/dyld/blob/dyld-1285.19/dyld/Loader.cpp#L1535-L1555)

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
