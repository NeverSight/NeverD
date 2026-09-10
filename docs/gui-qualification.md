# Desktop qualification record

This records the first runnable implementation for issue #108. It is not a
claim that every P0–P4 release criterion has been completed.

## Delivered and exercised locally

- Optional Qt Quick application, separate Qt-free C ABI worker and independently
  built CLI. The GUI executable has no engine or LLVM dependency.
- Dark+ dockable/floating workbench, saved layouts, narrow-window arrangement,
  function search, paged instructions, Hex, references and C/Low/Med/High/LLVM.
- Native Low/Med retained instruction anchors, exact hexadecimal addresses,
  revision guards, one-to-many highlighted rows and explicit missing mappings.
  Pin retains a representation while navigating the instruction pane.
- Bounded function model tested at one million logical rows; indexed CFG tested
  at 10,000 blocks. Production graph replies contain at most 256 nodes/512 edges,
  with native scene-graph rendering and a bounded software-renderer fallback.
- Atomic annotations/renames, input-bound undo/redo history, recovery journal,
  shared CLI/worker write lock, and Save/Discard/Cancel for session transitions.
- Namespaced declarative read-only query contributions, with plain-text results.
- Headless MCP, explicitly enabled attachment to the active GUI project, and
  manual GUI stdio/Streamable HTTP connections with bounded history/cancellation.
- All 11 bundled languages: 228 translated source keys, placeholder/newline
  checks, English first-launch default, live switching, and Arabic/LTR checks.

The local reference environment is macOS 15.6.1 arm64, AppleClang 17, Release,
Qt 6.11.1, KDDockWidgets 2.4.1 and the pinned NeverD LLVM 23 r2 package. The
standalone engine used for the bundle disables embedded Python plugins; its
MCP adapter requires Python 3.10+ separately. The application bundle's audited
minimum macOS version is 15.0, inherited from its actual binary dependencies.

Verification includes core session tests, real x86/AArch64 high-address mapped
pages, real EVM queries, cross-process CLI writer ownership, protocol framing,
worker cancellation, history recovery, 10k CFG paging, stale UI requests, dirty
session transitions, MCP transport/broker tests, native graph texture lifecycle,
layout float/redock/restore, and live language switching. Synthetic fixtures are
test inputs and are never packaged as the product engine.

Package3 is a local ad-hoc Qt 6.8.3 development bundle with Qt Test/probes,
built from Window7 GUI/worker inputs and SDK input SHA-256 prefix
`72d00428`. Its dependency audit covered 96 Mach-O images and established
minimum macOS 15.0. All five steps passed in 93 seconds: packaging, relocation,
strict signature verification, clean-environment GUI smoke and first analysis
using the bundled default worker. Signed, relocated and post-run manifests
matched SHA-256
`fa969725257578e88b4080b26e62c923bd35b2e97462cb3175575aa9fadaf7ee`.
SQLite was the only SQL driver retained; QtSql and LocalStorage consumers
remained. The single useful-view smoke is not a performance statistic.
Earlier packaged MCP checks are historical; this package's MCP protocol was
not validated. MCP requires a separately supplied Python interpreter.

## Measured performance

The reproducible [benchmark report](../tools/neverd-gui/benchmarks/README.md)
contains the raw results and limitations. The native Qt prototype tested 100k
and 1M functions, 10M logical text rows and 1k/10k-node graphs. Its 120-frame runs
reported frame-swapped interval p95 of about 17.0–17.5 ms, exceeding the proposed
16.7 ms target. This is not a hardware presentation measurement. UI update work
was approximately 0.03–1.54 ms, with 33–41 row delegates and 56 visible graph
nodes; reported GUI RSS was roughly 106–109 MiB.

Tiny warm EVM worker IPC p95 was about 0.05–0.10 ms. These numbers do not establish
large-image navigation latency, total process-tree memory, first useful view,
uncached engine throughput, or full-workbench input-to-present latency under
concurrent analysis and MCP load. Those measurements remain required.

## 2026-09-10 GUI polish validation — historical Window2

The isolated Release GUI polish build enabled `BUILD_TESTING` and included
application test probes linked with Qt Test. It passed the complete GUI/worker
CTest suite:
**20 tests passed, zero failures and zero skips**. A separate native Cocoa run
of the real analysis probe passed and captured C, LowIR and CFG views from the
benign named-function ELF. The source snapshot remained unchanged during the
validation window, and the GUI, worker, engine and test executable hashes
remained unchanged after the build.

Eight fresh launches of this validation build after one discarded warmup
recorded first Qt
frame p50/p95 of **407.528/593.003 ms** and first useful Qt frame p50/p95 of
**527.610/758.660 ms**. This was macOS arm64, Qt 6.11.1, Cocoa/Metal, a
1500×950 window and device pixel ratio 2, using temporary default settings and
warm caches. The [published startup summary](../tools/neverd-gui/benchmarks/results/macos-arm64-20260910-gui-startup-summary.json)
preserves the original measured reports and artifact hashes while omitting
personal environment values, diagnostic logs and absolute paths. It is a
public summary, not the complete local evidence.

This run used the earlier Workbench facade and the private SDK library with
SHA-256 prefix `e0a838dd1b70`; it does not qualify the finalized SDK build with
prefix `72` or the later PaneController integration. That later integration is qualified separately by Window8 below. GUI/worker revisions, environment and system load changed between
exploratory runs, so the full difference is not attributable to the TLS change.
These measurements do not establish large-image, cold-cache, cross-platform or
hardware presentation performance, or a world ranking.

## Final Qt 6.11.1 GUI polish validation — Window8

The final integrated GUI/worker and finalized shared SDK passed **22/22 CTest
tests, zero failures and zero skips**, including **31 required child PASS
records**. All 12 validation steps succeeded. Four separate Cocoa checks passed:
docking, shortcuts, and real analysis of named and stripped benign ELF fixtures.
Both analysis runs captured C, LowIR and CFG views. Translation validation
covered 11 locales and 228 keys. Frozen source/dependency hashes remained
unchanged, as did all built artifact hashes during tests and native checks.

The [final startup summary](../tools/neverd-gui/benchmarks/results/macos-arm64-20260910-gui-startup-final-summary.json)
retains all eight measured reports, one discarded warmup, fixture identity and
full GUI/worker/SDK hashes. On macOS 15.6.1 arm64 with Qt 6.11.1, Cocoa/Metal,
1500×950 and DPR 2, first Qt frame p50/p95 was **388.427/422.217 ms**; first useful
Qt frame p50/p95 was **517.180/548.225 ms**. With eight samples, nearest-rank p95
and p99 are both the maximum observation.

This is a Release validation build with application test probes and Qt Test,
not a probe-free distribution package. Launches use temporary default settings
and warm caches; milestones start at main() entry after dynamic loading. Qt
frameSwapped does not independently measure hardware presentation. The tiny
536-byte startup fixture establishes no large-input performance, world ranking
or speedup attribution across different builds.

## Qt 6.8.3 compatibility validation — Window7

The same frozen product source and shared SDK also passed a separate Qt 6.8.3
window: **22/22 CTest tests, zero failures and zero skips**, all **31 required
child PASS records**, all 12 validation steps, and the same four Cocoa checks.
Source/dependency and post-build artifact hashes remained stable. The four new
focus cases cover editable and read-only text in main and floating windows;
they have final PASS evidence in both Qt versions, but were not run against the
pre-fix baseline.

Its independent eight samples after one warmup recorded first Qt frame p50/p95
of **373.776/408.116 ms** and first useful Qt frame p50/p95 of
**497.921/542.860 ms**. The additional Qt 6.8.3 section in the
[final summary](../tools/neverd-gui/benchmarks/results/macos-arm64-20260910-gui-startup-final-summary.json)
retains its own raw reports and artifact identities. The same warm-cache,
tiny-fixture, probe-build and Qt-frame timing limits apply. These separate
runs do not establish a performance improvement caused by either Qt version,
nor qualify native Linux/Windows behavior.

## Remaining issue acceptance work

| Area | Remaining qualification or capability |
| --- | --- |
| P0 performance | Production end-to-end latency, total process-tree memory, first useful view and concurrent-load measurements on reference hardware; confirm frame targets. |
| P2 mappings | Full contributor provenance and C/HighIR/LLVM/VM source mappings. Current Low/Med anchors are precise but partial. |
| P2 independent panes | Multiple instances and user-defined synchronization groups beyond the current linked workspace and pinned representation. |
| P3 extensions | Rich declarative menus/panels and persistent imported-manifest preferences beyond session-scoped read-only contributions. |
| P4 platform builds | The three-platform Qt contract workflow is provided; native Linux/Windows packaging and platform validation must be recorded from actual runs. |
| P4 accessibility/input | Native screen-reader, CJK IME composition, mixed-direction copying, mixed-DPI/multi-monitor testing, and Linux X11/Wayland qualification. |
| P4 release | Signing/notarization or platform installer work, corresponding-source distribution and release artifact publication. |

Keep issue #108 open until its remaining acceptance items are verified or the
maintainers explicitly revise its scope. A working macOS GUI is available now;
source portability and offscreen checks alone do not establish release readiness
on every target platform.
