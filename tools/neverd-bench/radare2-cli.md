# CLI reads against radare2

`radare2_cli_compare.py` compares fresh CLI processes opening the same image
and returning one complete mapped byte range as JSON. Each result must equal
the independently selected bytes at a file offset. A faster partial result,
wrong address, failed command or timeout cannot qualify.

Prepare a schema 1 manifest. Paths are relative to the manifest directory or
absolute. Supply the actual input SHA-256 and a known file-backed mapping:

```json
{
  "schema": 1,
  "cases": [{
    "name": "text",
    "input": "/path/to/sample.elf",
    "input_sha256": "REPLACE_WITH_ACTUAL_SHA256",
    "address": "0x401000",
    "file_offset": 4096,
    "size": 64
  }]
}
```

Use a Linux Release CLI beside its matching SDK and an installed radare2
prefix containing actual copies of its executable and libraries. The runner
selects each library directory and verifies resolution with `ldd`. radare2
uses built-in plugins, with external plugins and user startup scripts disabled.

```sh
taskset -c 0,2,4,6 python3 tools/neverd-bench/radare2_cli_compare.py \
  --neverd /snapshot/neverd --engine /snapshot/libneverd.so \
  --radare2-prefix /snapshot/radare2 --manifest /path/to/ranges.json \
  --source-revision NEVERD_COMMIT --radare2-revision RADARE2_COMMIT \
  --samples 10 --warmup 1 --threads 4 \
  --output build-bench/radare2-cli
```

Keep builds, tests and profilers idle during measurements. Use `--diagnostic`
when developing or checking the harness during other work. Each pair uses a
fresh input copy; execution order alternates, warmups remain in the raw report
and are excluded from summaries. Complete stdout/stderr, command lines, host
load, CPU affinity, selected loader/allocator environment, file and runtime-library
identities are retained. Artifact, library-resolution or input changes invalidate
the campaign. Output directories must be new.

Wall time includes launch, loading, JSON output and exit. OS file caches remain
warm. CPU time and `wait4` peak RSS are process resource observations, not
summed process-tree RSS/PSS. This operation does not qualify discovery,
decompilation, GUI rendering, cold OS caches or other operating systems.
Record compiler/build settings alongside the local report; revision strings
are supplied by the caller, while hashes identify the measured files.

Run the harness checks without installed analyzers:

```sh
python3 -m unittest discover -s scripts/tests -p test_radare2_cli_compare.py -v
```

For actual workbench observations, use the separate
[GUI startup harness](../neverd-gui/benchmarks/README.md#production-gui-startup).
Worker IPC timings are not GUI rendering measurements.
