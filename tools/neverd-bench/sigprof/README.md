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

The comparison exits nonzero and suppresses timing ratios if any command fails,
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
