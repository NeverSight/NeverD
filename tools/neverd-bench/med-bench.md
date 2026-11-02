# LowIR to MedIR conversion benchmark

`neverd-med-bench` decodes an image once, then repeatedly converts the same
LowIR functions through the standalone `LowToMedConverter` API on one thread.
It measures conversion, including SSA construction and the converter's other
passes. Loading, decoding, verification, textual dumps and output destruction
are outside the measured interval. One initial conversion is discarded as a
warmup.

```bash
python3 scripts/build.py build-release --target neverd-med-bench --parallel 4
build-release/bin/neverd-med-bench /path/to/image --repetitions=7
```

Use `--max-functions=N` to bound the decoded function selection; zero selects
all functions. The JSON output includes function/block/operation counts, wall
and process CPU times, and a SHA-256 of the canonical MedIR dump. Conversion
errors, skipped SSA, verifier failures and differing dumps across repetitions
fail the run. The dump hash is an output parity check, not a semantic oracle.

For interleaved comparisons without repeating image preparation, pass
`--interleave`. The tool writes a JSON line with `ready: true` after warmup,
then waits for `run` followed by a newline on stdin before each sample. It
verifies the result and releases the conversion and dump before writing that
sample as a JSON line and waiting again.
After the requested repetitions it writes the same aggregate JSON as normal
mode and exits. EOF or any other command before the last sample fails the run.
A driver can keep baseline and candidate processes ready and alternate batches;
stdin waits and JSON output remain outside the measured intervals.
After a process's last sample, drain its aggregate result and wait for it to
exit before starting the other process's next batch, so final image teardown
does not overlap that batch.

This exercises standalone conversion with the loaded image's layout and ABI.
It does not install pipeline-owned interprocedural summaries or source hints,
and is not an end-to-end CLI/GUI benchmark. Use the existing CLI/worker
benchmarks to establish user-visible latency changes separately.

For a comparison, keep the input bytes, function selection, build profile,
compiler and dependency receipts identical. Preserve both executables and
their runtime dependencies, alternate baseline/candidate process order, and
check counts and dump hashes before comparing paired timings. Stop other
local builds, tests and profilers during measurement; record unavoidable host
contention. Keep raw outputs and timing summaries in an ignored build or
`benchmark-results` directory.
