# NeverD GUI performance evidence harness

These tools separate **real engine IPC** from **synthetic Qt viewport work**.
They do not turn the architecture design's proposed P0 targets into accepted
performance guarantees. The viewport executable is a small independent harness,
not the production workbench.

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

Measure the full workbench with its normal QML, docking, transport and real
`libneverd` worker. The default fixture is the benign 536-byte, named-function
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

Schema 2 GUI milestones begin at `main()` entry, after dynamic loading. They
separate application and docking setup, MCP and broker construction, font
lookup, QML load, worker hello, metadata, instructions, representation,
first frame, and first useful frame. A useful frame is eligible only after
`afterSynchronizing` observes both analysis datasets; its timestamp is captured
at the corresponding `frameSwapped` emission on the rendering thread. The
queued GUI delivery time is recorded separately. This follows Qt's documented
[scene graph synchronization and frame signals](https://doc.qt.io/qt-6/qquickwindow.html#afterSynchronizing)
and prevents an older queued swap from being attributed to new analysis data.
Neither timestamp independently measures hardware presentation. The runner's
separate process wall time includes dynamic loading, report writing and shutdown.

The opt-in `--startup-benchmark <report.json>` GUI flag is available in production
builds. `--startup-benchmark-timeout <milliseconds>` defaults to 30000; input and
worker failures produce unsuccessful reports and exit immediately. The runner
accepts `--timeout <seconds>` and stops on the first failed launch. Empty or
unsupported representations cannot be reported as successful useful frames.
The in-process deadline is observed when the GUI event loop runs; the runner's
additional hard process deadline covers blocked GUI initialization.
Early schema 1 measurements used queued frame delivery and inherited user
preferences; retain them as provisional evidence rather than using them for a
quantitative startup speedup comparison with schema 2.

### Historical Window2 GUI validation: macOS arm64, 2026-09-10

The [published startup summary](results/macos-arm64-20260910-gui-startup-summary.json)
retains all eight measured GUI reports and the discarded warmup report, including
their original milestones, Qt/backend/DPI/viewport details, fixture and build
hashes. It is a redacted public summary, not the complete local evidence: process
arguments, diagnostic logs, personal paths and host-specific environment values
are omitted. Hashes identify the retained local evidence files.

This Release validation build enabled `BUILD_TESTING`, included the application
test probes and linked Qt Test; it is not the probe-free distribution
configuration. It ran on macOS 15.6.1 arm64 with Qt 6.11.1, Cocoa, Metal on a
separate render thread, a 1500×950 window and device pixel ratio 2. The runner
passed a controlled environment with `QT_QPA_PLATFORM=cocoa` and `C.UTF-8`
locale, without overriding the rendering backend, render loop or scale. After
one discarded warmup, eight fresh process launches analyzed the same benign
536-byte named-function ELF with temporary default settings and warm caches.

| Measured milestone or phase | p50 | p95 / p99 |
|---|---:|---:|
| First Qt frame | 407.528 ms | 593.003 ms |
| First useful Qt frame, with instructions and representation synchronized | 527.610 ms | 758.660 ms |
| GUI MCP construction phase | 0.117 ms | 0.149 ms |

The MCP phase is `mcp_created_ms - services_started_ms`, about 0.12 ms at p50.
The separate `QCoreApplication` constructor probe completed three fresh process
samples per case: MCP construction p50 was 0.176 ms and the empty
`QSslConfiguration` control was 133.556 ms. Its raw constructor samples are in
the same public summary. These are separate measurements from the earlier
`QGuiApplication` experiment below.

The complete isolated GUI/worker CTest suite passed **20/20, with zero skips**;
the native Cocoa analysis probe also passed and captured the real C, LowIR and
CFG views. Source hashes stayed unchanged throughout the validation window;
GUI, worker, engine and test executable hashes stayed unchanged after the
build. This validates the earlier Workbench facade against the private SDK
whose library SHA-256 begins `e0a838dd1b70`, not the finalized SDK build whose
hash begins `72`. The later PaneController integration is covered separately by the final
Window8 record below.

The GUI and worker versions, environment and system load differed from earlier
exploratory runs. Build and test activity preceded these samples, and background
load was not instrumented. The total timing difference cannot be attributed
entirely to deferred TLS initialization. This eight-sample run establishes no
cold-cache result, hardware presentation timestamp, large-image performance,
cross-platform guarantee or world ranking.

### Final Window8 GUI validation: Qt 6.11.1, macOS arm64, 2026-09-10

The [final startup summary](results/macos-arm64-20260910-gui-startup-final-summary.json)
records the final integrated GUI with the finalized shared SDK, full
artifact/fixture SHA-256 values, all eight measured reports and the discarded
warmup. It is a redacted public summary; complete local logs and process
environment are not reproduced. The historical Window2 summary remains intact.

| Measured milestone | p50 | p95 / p99 |
|---|---:|---:|
| First Qt frame | 388.427 ms | 422.217 ms |
| First useful Qt frame | 517.180 ms | 548.225 ms |

This Release build enables BUILD_TESTING and includes application probes and Qt
Test. It passed 22/22 CTest tests with zero failures/skips, 31 required child PASS
records and all 12 validation steps, including four separate Cocoa checks:
docking, shortcuts, named-ELF analysis and stripped-ELF analysis. Source,
dependency and post-build artifact hashes remained stable. Startup used macOS
15.6.1 arm64, Qt 6.11.1, Cocoa/Metal on a separate render thread, 1500×950 and
DPR 2, with temporary default settings.

One warmup preceded eight fresh process samples of the benign 536-byte named
ARM64 ELF. Caches were warm, milestones begin at main() entry after dynamic
loading, and frameSwapped is not hardware presentation. With eight samples,
nearest-rank p95/p99 equal the maximum. These results do not characterize a
probe-free distribution package, large images, a world ranking or a speedup
attributable to one change.

### Separate Window7 Qt 6.8.3 validation

The same frozen source and shared SDK passed 22/22 CTest tests, zero
failures/skips, 31 required child PASS records and all 12 steps, including the
four Cocoa checks, with source/dependency and built artifacts unchanged.
The [final summary](results/macos-arm64-20260910-gui-startup-final-summary.json)
includes a separate Qt 6.8.3 qualification with its own 8+1 raw reports and
artifact hashes.

| Measured milestone | p50 | p95 / p99 |
|---|---:|---:|
| First Qt frame | 373.776 ms | 408.116 ms |
| First useful Qt frame | 497.921 ms | 542.860 ms |

The same fixture, viewport, warm-cache and probe-build limitations apply.
These are independent small-sample observations; their differences do not
establish a Qt-version performance improvement. The four added main/floating,
editable/read-only focus variants passed both final suites without a recorded
pre-fix run of those new cases.

### Earlier constructor investigation

An exploratory macOS/Qt 6.11.1 phase profile found about 289 ms in the MCP
client constructor. An isolated native probe then measured each member in fresh
processes, after `QGuiApplication` initialization (three samples per case):

| Constructor or control | Median constructor time |
|---|---:|
| MCP client | 243.921 ms |
| `QNetworkAccessManager` | 0.122 ms |
| Empty `QSslConfiguration` | 246.628 ms |
| `QProcess` | 0.009 ms |
| MCP after constructing `QNetworkAccessManager` | 248.067 ms |
| MCP after constructing `QSslConfiguration` | 0.211 ms |

The controls identify eager SSL configuration construction as the dominant cost
on this machine. The client now constructs that configuration when the user
explicitly connects HTTP, using the same Qt default configuration and additive
CA certificate handling. The network manager remains unchanged. These are
constructor measurements; they do not establish an end-to-end GUI speedup, a
different platform's behavior, or a hardware presentation result. The rejected
experiment moving worker launch ahead of QML load had no observed median benefit
in its separate eight-sample native runs, so the original ordering is retained.

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
`QCoreApplication`; the earlier exploratory table used `QGuiApplication` before
timing. Keep those environments distinct when comparing absolute timings.

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

## Recorded exploratory macOS run

The checked-in `results/` reports were measured on the available macOS arm64
machine using Qt 6.11.1 and Release builds. They are one exploratory run, not a
cross-platform or statistically stabilized acceptance result. Native Cocoa used
Metal on a separate render thread; the offscreen run used software rendering on
the GUI thread.

| Native Metal scenario | Qt frame interval p95 | Model update p95 | Peak row delegates / graph scene nodes | Ending GUI RSS |
|---|---:|---:|---:|---:|
| 100k sparse functions | 17.045 ms | 1.024 ms | 33 / 0 | 105.55 MiB |
| 1M sparse functions | 17.259 ms | 0.943 ms | 33 / 0 | 106.53 MiB |
| 10M logical text lines | 17.327 ms | 1.543 ms | 41 / 0 | 109.05 MiB |
| 1k graph overview | 17.516 ms | 0.032 ms | 0 / 168 | 109.27 MiB |
| 10k graph overview | 17.411 ms | 0.039 ms | 0 / 168 | 109.36 MiB |

The graph had at most 56 visible graph nodes; the 168 scene nodes include their
overview rectangles and outgoing edge rectangles. Native p95 frame intervals in
this run exceed the design's proposed 16.7 ms threshold. Object counts stayed
bounded as logical cardinalities increased, but this does not establish the
full workbench's scrolling, text selection, docking or large real-image budget.

The warmed tiny-EVM worker report observed operation round-trip p95 from about
0.051 ms to 0.104 ms. This measures a small result through local pipes, with
analysis already complete; it does not predict 100 MB/1 GB/5 GB image analysis or
uncached viewport latency.

## ADR evidence still required

The current choice remains Qt Quick + a C ABI worker. This harness supports
testing bounded visible work, but does not justify a framework comparison or
final performance sign-off. Before P0 acceptance:

- Freeze hardware, OS/driver, Qt version, viewport, DPI, font and rendering loop
  on Windows x64, Linux x64 and macOS arm64; repeat runs and retain raw results.
- Measure real PE/ELF/Mach-O end-to-end fixtures at the intended size bands,
  including first analysis and process-tree memory. Report EVM/SBF separately.
- Capture actual displayed frame timing, GPU time and dropped frames; investigate
  the native 17 ms p95 result against the proposed 16.7 ms goal.
- Measure cached/uncached input-to-display jumps, real keyboard/IME and text
  selection, and interaction while the engine performs useful background work.
- Exercise general CFG layout/full text/LOD, docking, floating windows, high DPI
  changes, accessibility, RTL and language switching in the real workbench.
- Record cancellation acknowledgement and actual stop times separately. The
  synchronous engine currently needs completion or worker restart to stop.

Only compare an alternative framework with the same data, detail level, input
behavior and backend conditions. No benchmark here claims that these product
acceptance requirements have been completed.
