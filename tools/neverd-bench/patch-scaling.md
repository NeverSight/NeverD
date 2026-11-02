# Interactive patch scaling

`patch-scaling.py` measures established byte overlays through the public Python
SDK and the real worker. Fixtures are disposable raw x64 images; no guest code
is executed. Each case checks the returned data, exported bytes and unchanged
input file. Failed checks invalidate the run. Run without Python `-O`.

```sh
python3 tools/neverd-bench/patch-scaling.py \
  --library /snapshot/bin/libneverd.so \
  --worker /snapshot/bin/neverd-worker \
  --rows 64 1024 2048 --worker-rows 64 1024 2048 \
  --range-bytes 4096 1048576 --samples 21 \
  --output build-bench/patch-scaling.json
```

Use matching Release SDK/worker snapshots and the same driver for each variant.
The Linux worker benchmark pins `LD_LIBRARY_PATH` and verifies the loaded SDK
through `/proc/<pid>/maps`, so a saved executable's build RUNPATH cannot silently
select another library. Omit `--worker` for SDK-only measurements. Set `TMPDIR`
to the same filesystem for both variants; durable commit latency depends on it.
Run variants sequentially in baseline/candidate/candidate/baseline order. Prefer
an idle host; preserve load and raw samples when shared work prevents that.

The three operations have distinct boundaries:

- `sdk_single_byte_edit`: replace one byte within alternating `bytes`/`nop`
  canonical rows. This includes the Python binding, planning, stale-byte check,
  overlay mutation and SDK invalidation. Reset/load and export verification are
  outside the timed region. Both wall and process CPU time are retained.
- `worker_patch_ranges`: request `{ranges_only:true}` for one contiguous patch.
  Timing includes IPC; returned address and size must be exact. It does not time
  Qt painting or analysis.
- `worker_single_byte_commit`: commit one byte through the writer, including
  history and sidecar persistence. Read back the changed byte, then undo to
  restore the fixture outside timing; export checks the restored overlay after
  the sequence.

An established overlay excludes the first immutable input snapshot. These
fixtures do not measure instruction assembly, source compilation, reanalysis,
cold caches, peak memory or other platforms. SDK row publication and durable
history/sidecar snapshots still scale with total overlay state.

## 2026-10-25 review

Mode: **PR Review**, sampled patch edit, persistence and color-refresh paths.
Generated timing samples account for most of the diff; the production changes
remain within the existing overlay, SDK, worker and session owners.

| Symptom | Source | Consequence | Remedy |
|---|---|---|---|
| A local edit repeatedly visits adjacent runs of different kinds | `PatchOverlay::apply` expands its window through unrelated neighbors and repeatedly searches the touched list | A 2,048-row fixture spends hundreds of milliseconds editing one byte | Locate the write with `lower_bound`; visit overlaps and matching-kind neighbors at its original edges once |
| A small range response encodes every original/current byte | The worker obtains full SDK rows before discarding byte payloads | A 1 MiB patch makes a color-range query expensive | Add an owned-string C/Python range API; retain the full-row fallback for older SDKs |
| Persisting a live edit replays the identical overlay and serializes it for comparisons | SDK load and worker acknowledgement both compare full JSON | Extra copying, validation and hex encoding on every durable edit | Parse the sidecar, compare typed rows with the already-admitted live overlay, and acknowledge exact equality; otherwise retain full replacement admission |
| Existing patch colors disappear during an unrelated rename | `Session::refreshBytePatches` clears before its asynchronous read | Transient incorrect colors and redundant repaint signals | Keep the previous same-project ranges until replacement; emit only on change, retaining epoch/revision fences and project-unload clearing |

The load shortcut compares complete parsed rows, not just a saved digest: an
unsaved live edit must still be replaced by the saved overlay. The regression
explicitly saves one value, changes it in memory, then reloads the unchanged
sidecar. A row with the same address/size but a foreign original byte still fails
admission and preserves the current overlay. Core tests independently check
byte values, kind boundaries, overlapping edits, partial restoration and the
65,536-row limit. Worker tests cover real and
mock SDKs, replicas, stale previews, undo/redo, reopen and exclusive export. GUI
tests keep colors through rename and clear them across project switches; both
X11 and Wayland exercise the full patch-key suite.

### Measurements

Baseline: PR #1763's merged tree, represented by commit
`640418249fe0684ed1a2a9e216603bf6296e18e6`. Candidate: the same source plus these
patch changes, before later `dev` integration. Linux x86-64, i9-13900H, Release,
Clang 23, identical dependency snapshots. Each result below pools 42 samples
from the four sequential runs. Other users' builds were present; no own build
or runtime tests overlapped this paired campaign. See the
[raw evidence](../../docs/evidence/patch-scaling-2026-10-25.json) for artifact and
driver hashes, build options, CPU times, p95, loads and every sample.

| Operation | Before median | After median | Median speedup |
|---|---:|---:|---:|
| SDK one-byte edit, 64 adjacent rows | 0.148 ms | 0.122 ms | 1.2× |
| SDK one-byte edit, 1,024 adjacent rows | 50.580 ms | 0.159 ms | 318× |
| SDK one-byte edit, 2,048 adjacent rows | 381.350 ms | 0.202 ms | 1,884× |
| Worker ranges, 4 KiB changed bytes | 0.384 ms | 0.088 ms | 4.4× |
| Worker ranges, 1 MiB changed bytes | 44.244 ms | 0.094 ms | 472× |
| Worker durable edit, 64 adjacent rows | 19.900 ms | 18.983 ms | 1.0× |
| Worker durable edit, 1,024 adjacent rows | 148.442 ms | 53.176 ms | 2.8× |
| Worker durable edit, 2,048 adjacent rows | 575.882 ms | 95.880 ms | 6.0× |

The large SDK ratio addresses a pathological mixed-kind adjacency chain; it is
not the speedup for every patch. At 2,048 rows, process CPU medians are
381.316 ms → 0.198 ms, supporting an algorithmic improvement despite host load.
The candidate-only row-limit run completed with correct exported bytes at
65,536 rows (1.844 ms wall median, seven diagnostic samples); no old-version
timing at that size is claimed. Small durable edits remain dominated by shared
host and persistence costs, and no significant improvement is claimed for them.

These are NeverD before/after measurements. They do not rank IDA, Binary Ninja,
Ghidra or other products. Native Windows/macOS input and performance remain
unmeasured by this campaign.

### Delivery qualification prerequisites

Integrating current `dev` exposed a Clang 23/libstdc++ 15 failure constructing a
nested Android queue snapshot through zero-argument `optional::emplace`. Explicit
value emplacement preserves the existing field defaults and aggregate API. The
SDK adapter and its four test-fixture call sites use the same form; driver and
Z3 support stay enabled in the delivery build.

The native-pipeline CI audit also rejected the decoder's explicit native-x87
hardware skip on Windows and ARM hosts. It now accepts only that named test,
its exact return-code/reason and declared unsupported runner platforms. Native
x64 POSIX hosts still require execution; every error, unexpected skip, missing
test or duplicate inventory entry remains a failure. The summary reports passes
and unavailable hardware tests separately. The three original Actions JUnit
reports were replayed with additional controls for native-host skips, renamed
tests, wrong reasons, disabled tests and failures hidden inside skipped cases.
The unchanged native oracle also ran on Linux x86-64: all 608 CPU memory-boundary
and access observations passed with no translation.


### Final delivery checks

The integrated Release SDK preserves driver emulation and Z3 support. Its 22
core patch tests, nine worker/GUI CTest groups, 76 Python API tests, 14 ABI tests,
three real SDK/CLI integration tests, 122 Android queue/framework tests and 24
Android JSON tests pass. The integrated read-only-byte and constant-branch
test executables also pass. The delivered GUI and worker in `build-gui/bin` pass
the same nine CTest groups and the full native X11 and private GNOME Wayland
patch suites (78 Qt cases each, including setup/cleanup; zero failures/skips).
These are focused checks rather than a complete release certification. The
qualification source/binary identities are separate from the paired benchmark
snapshots and are recorded in the evidence JSON.

The same remote qualification also exposed an unmarked process-guide heading
and an older stack-projection fixture calling the previous three-argument
interface. The guide keeps its existing localization section inventory, and
the ARM/x64 fixture supplies the matching low-function entry with no x86
registration state. All existing stack-operation assertions remain required.
The pipeline-outcome executable passes all 55 cases, and the complete 134-test
documentation matrix and its CLI validation pass.
