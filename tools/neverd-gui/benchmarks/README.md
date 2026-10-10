# NeverD GUI performance evidence harness

These tools separate **real engine IPC** from **synthetic Qt viewport work**.
They do not turn the architecture design's proposed P0 targets into accepted
performance guarantees. The viewport executable is a small independent harness,
not the production workbench.

Keep machine-local reports, raw traces and timing summaries in ignored
`build-bench/` or `results/` directories. Commit reusable harnesses and regression
tests; do not upload local measurement reports to the repository.

## Large-function pseudocode latency

Measure fixed function entries in fresh worker processes, retaining the first
page latency, complete source latency, full source hash, phase timings and peak
worker RSS. The Linux profile selects the supplied engine through
`LD_LIBRARY_PATH`; use matching worker and engine builds. An optional activated
idalib Python measures the same entries after fresh IDA auto-analysis.

```sh
python3 tools/neverd-gui/benchmarks/pseudocode_latency_bench.py \
  --worker build-gui/bin/neverd-worker --engine build/bin/libneverd.so \
  --entry 0x19b24 --entry 0x34901 --entry 0x309d0 --entry 0x54174 \
  --threads 4 --samples 3 --timeout 180 \
  --output build-bench/rust-parallel.json /absolute/path/to/rust-eh-fixture
```

Use `--ida-python /path/to/activated/python` to include the comparison. Repeat
with `--threads 1` and compare every complete source hash to check deterministic
publication. Matching source text is a reproducibility check, not a proof of
semantic equivalence. Keep builds and tests out of the timing window. OS caches
are warm, host activity remains uncontrolled, and these worker measurements do
not include Qt painting. IDA auto-analysis and decompilation are recorded
separately; NeverD's first page includes IPC and source generation.

Add `--baseline-engine /absolute/path/to/previous/libneverd.so` to compare two
compatible engines with the same worker. When worker code also changes, add
`--baseline-worker /absolute/path/to/previous/neverd-worker`; both worker hashes
are recorded. Each engine receives its own fresh
process and input copy. Successive repetitions alternate the order; the JSON
records both source hashes, timings and host load before/after each run.
On hybrid CPUs, run the driver under `taskset -c <cpu-list>` so both engines
inherit the same set of cores. The report records the inherited CPU affinity;
pinning does not reserve those cores or eliminate unrelated host activity.

Measure return-to-function reuse separately from first decompilation:

```sh
taskset -c 0,2,4,6 python3 tools/neverd-gui/benchmarks/source_revisit_bench.py \
  --worker build-gui/bin/neverd-worker --engine build/bin/libneverd.so \
  --entry 0x19b24 --switch-entry 0x54174 --threads 4 \
  --output build-bench/rust-revisit.json /absolute/path/to/rust-eh-fixture
```

This Linux profile reads every source page for A, A again, B, then A again in
one worker. It disables background analysis as the GUI's analysis replicas do,
checks the loaded engine path and rejects changed source hashes within the
sequence. It does not include the GUI's own response cache or snapshot restore.
Run both frozen and current worker/engine pairs; a faster revisit is not evidence
of a faster first decompilation.

Generate versioned fixture descriptors:

```sh
python3 tools/neverd-gui/benchmarks/generate_datasets.py build-bench/datasets
```

The manifest fixes seed 3389, 100,000 and 1,000,000 synthetic function rows,
10,000,000 logical text rows, and 1,000/10,000 grid graph nodes. Rows and visible
graph geometry are generated on demand; no giant JSON arrays are created. It
also writes benign arithmetic EVM bytecode and a small C source fixture with
SHA-256 hashes. Synthetic row counts are not recovered functions in these tiny
fixtures. Compile the C source with the desired platform compiler to test a real
PE/ELF/Mach-O, recording compiler options with the report.

Measure the real worker (five warmup requests, then 100 samples per operation):

```sh
python3 tools/neverd-gui/benchmarks/worker_ipc_bench.py \
  --worker /absolute/path/to/neverd-worker \
  --samples 100 --output build-bench/worker-ipc.json
```

Supply `--binary /path/to/fixture` for another real image. The default tiny EVM
fixture keeps the loader and pipeline smoke reproducible. The report records
startup-to-hello, open-to-metadata, first analysis, warmed operation round-trip
p50/p95/p99, response bytes, engine version and worker RSS. A worker reply is not
a displayed GUI frame. First analysis and later query latency remain separate.

## Production GUI startup

Measure the full workbench with its normal Qt Widgets windows, docking,
transport and real `libneverd` worker. The default fixture is the benign 536-byte, named-function
ELF from `tests/analysis_probe_test.py`; it is analyzed, never executed.

```sh
python3 tools/neverd-gui/benchmarks/startup_bench.py \
  --gui /absolute/path/to/neverd-gui \
  --worker /absolute/path/to/neverd-worker \
  --engine /absolute/path/to/libneverd.dylib \
  --warmup 1 --samples 8 --output build-bench/gui-startup.json
```

Use the actual library selected by the worker's dynamic loader for `--engine`
(`libneverd.so`/`neverd.dll` on other platforms); this argument records identity
and does not change loader resolution. The JSON retains SHA-256 and byte sizes
for all three build artifacts and the input, raw per-launch GUI reports,
diagnostics, Qt/backend/DPI/window details, selected environment variables, and
nearest-rank p50/p95/p99 for each milestone. With eight measured samples,
p95 and p99 are both the largest observation; this is exploratory evidence.
The runner rejects changed build artifacts and missing/inconsistent milestones.

Every sample launches fresh GUI and worker processes with a fresh docking layout
and temporary default QSettings. Benchmark mode neither reads nor writes the
user's application preferences. One discarded warmup launch is followed by the
requested sample count. OS file caches, Qt caches and graphics caches are not
flushed: this is **warm repeated process startup**, not a cold-cache result.
Other system activity is not controlled. Native runs require a real desktop
session. Explicit offscreen/software runs remain separate diagnostics and must
not establish native display performance.

Schema 3 GUI milestones begin at `main()` entry, after dynamic loading. They
separate application and docking setup, service construction, window
creation, the first window paint, worker hello, metadata of the opened file,
the first paint of its disassembly listing (`useful_frame_ms`) and the end of
that event-loop pass, when the backing store has been flushed
(`useful_frame_flushed_ms`). Without an input file the report ends after the
first paint. Neither timestamp independently measures hardware presentation.
The runner's separate process wall time includes dynamic loading, report
writing and shutdown.

The opt-in `--startup-benchmark <report.json>` GUI flag is available in production
builds. `--startup-benchmark-timeout <milliseconds>` defaults to 30000; input and
worker failures produce unsuccessful reports and exit immediately. The runner
accepts `--timeout <seconds>` and stops on the first failed launch. Empty or
unsupported representations cannot be reported as successful useful frames.
The in-process deadline is observed when the GUI event loop runs; the runner's
additional hard process deadline covers blocked GUI initialization.
Schema 1 and 2 reports measured the earlier Qt Quick workbench; keep them as
historical evidence rather than comparing them quantitatively with schema 3.

### Constructor measurements

For repeatable constructor comparisons across source revisions, build the small
optional probe against the real MCP client:

```sh
cmake -S tools/neverd-gui/benchmarks -B build-bench/mcp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt \
  -DNEVERD_BUILD_MCP_STARTUP_PROBE=ON
cmake --build build-bench/mcp --target neverd-mcp-startup-probe -j 2
build-bench/mcp/neverd-mcp-startup-probe --samples 3 \
  --output /tmp/neverd-mcp-constructors.json
```

The target links Qt Core and Network only. Its six fixed cases run in separate
child processes and retain raw constructor times, preinitialization times,
diagnostics, and executable/client-source hashes. Save one report per revision;
there are no pass/fail timing thresholds. Without `--output`, JSON goes to stdout,
so samples are not written into the source tree. This standalone probe uses
`QCoreApplication`; record the application environment when comparing timings.

## Production GUI interaction

`--interaction-benchmark <report.json>` opens the given file in the production
workbench, waits for its first painted listing and then measures, each first
over content the worker has not served yet and then once more:

- listing scrolling: 300 wheel steps of three lines, one every 16 ms, down and
  back up, from the step to the backing-store flush of its frame (a step
  without a frame before the next one counts as missed);
- jumps to up to 24 functions spread over the function list, until the
  listing has painted the jump's lines;
- control-flow graphs of the same functions, until the complete graph painted;
- pseudocode of the same functions, until the decompiled text painted.

The report keeps nearest-rank p50/p95/p99, maximum and mean per phase, and the
result or failure of every sampled function. Like the startup benchmark it uses
temporary settings and a fresh layout. `--interaction-benchmark-timeout`
defaults to 600000 ms.

## Against IDA

Two runners measure the workbench against an installed IDA on the same
inputs, on the same machine, one after the other. Neither ships IDA or reads
anything from it but its own output; both need a local installation.

`ida_compare_bench.py` compares the engine work behind the window. The
workbench side drives the real worker as the GUI does when it opens a file:
open, the first page of the function list and of the listing (browsable),
then the reference index, which an explicit cross-reference query finishes
on demand (analysis complete), then the first page of each sampled
function's pseudocode, which F5 decompiles on demand. The IDA side runs IDA
as a library (idalib) in the Python that has its `idapro` package: one
process loads the file without analysis (browsable), another opens it with
auto-analysis and waits for it (analysis complete), then decompiles the same
functions and prints each as text. The functions are a seeded sample of the
entries both sides found. Every sample starts fresh processes on a fresh copy
of the input, so no database or decompiled function is reused:

```sh
python3 tools/neverd-gui/benchmarks/ida_compare_bench.py \
  --worker /path/to/neverd-worker --ida-python /path/to/python-with-idapro \
  --samples 3 --decompile 100 --output build-bench/vs-ida.json \
  --markdown build-bench/vs-ida.md /path/to/binary...
```

`gui_vs_ida_bench.py` times both windows from launch on a private X server
(Xvfb, through Qt's xcb plugin), with default preferences: the workbench
with temporary settings and a fresh layout, IDA with a scratch user
directory holding a copy of the license and configuration but no plug-ins.
The workbench reports its first frame and the first painted listing of the
file through `--startup-benchmark`, whose report records its clock origin on
the monotonic clock; IDA runs a script once the file is open in its window
(`-S`), which records that moment and the end of auto-analysis:

```sh
python3 tools/neverd-gui/benchmarks/gui_vs_ida_bench.py \
  --gui /path/to/neverd-gui --worker /path/to/neverd-worker \
  --ida /path/to/ida --samples 3 --warmup 1 \
  --output build-bench/gui-vs-ida.json /path/to/binary...
```

Both keep every raw sample beside nearest-rank percentiles. OS file caches
stay warm, and other activity on the machine is not controlled: the GUI
runner records the load average.

## Synthetic viewport harness

Build and run the Qt harness independently:

```sh
cmake -S tools/neverd-gui/benchmarks -B build-bench/qt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-bench/qt
build-bench/qt/neverd-viewport-bench --samples 120 --output build-bench/qt-native.json
```

On macOS the sample environment uses `-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt`.
For a separate software/headless diagnostic run:

```sh
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  build-bench/qt/neverd-viewport-bench --samples 120 --output build-bench/qt-offscreen.json
```

Each case runs 20 warmup frames and the requested number of measured frames in a
1100×700 viewport. Function cases expose the full sparse `QAbstractItemModel`
row count and jump through deterministic positions, with visible QML delegates
reused by `ListView`. The 10M-line case keeps a 512-row local model window, with
global addresses formed from uint64 in C++. The graph cases use an analytical
grid index to create only intersecting overview rectangles and outgoing edges;
the measured detail level excludes text labels and general CFG layout.

The Qt report records actual Qt version, platform plugin, graphics API, rendering
thread, device pixel ratio, font, Release/Debug configuration, RSS, sparse model
rows, delegate/scene-node peaks, Qt `frameSwapped` intervals, model updates,
queued event delivery, render callback span and request-to-frame signal timing.
`frameSwapped` is not an independently measured hardware presentation timestamp.
Queued event delivery is not keyboard/IME response. Render callback span is not
GPU execution time. Offscreen frame intervals cannot establish display targets.
RSS is whole-process resident memory, excluding a concurrent worker, plugins,
private/PSS attribution and GPU allocations.

## ADR evidence still required

The workbench now uses Qt Widgets with custom-painted analysis views and the
same C ABI worker. The viewport harness measures the Qt Quick prototype only
and does not establish production performance. Before P0 acceptance:

- Freeze hardware, OS/driver, Qt version, viewport, DPI, font and rendering loop
  on Windows x64, Linux x64 and macOS arm64; repeat runs and retain raw results.
- Measure real PE/ELF/Mach-O end-to-end fixtures at the intended size bands,
  including first analysis and process-tree memory. Report EVM/SBF separately.
- Capture actual displayed frame timing, GPU time and dropped frames against
  the proposed 16.7 ms goal.
- Measure cached/uncached input-to-display jumps, real keyboard/IME and text
  selection, and interaction while the engine performs useful background work.
- Exercise general CFG layout/full text/LOD, docking, floating windows, high DPI
  changes, accessibility, RTL and language switching in the real workbench.
- Record cancellation acknowledgement and actual stop times separately. The
  synchronous engine currently needs completion or worker restart to stop.

Only compare an alternative framework with the same data, detail level, input
behavior and backend conditions. No benchmark here claims that these product
acceptance requirements have been completed.
