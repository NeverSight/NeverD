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
