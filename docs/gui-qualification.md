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
- All 11 bundled languages: 226 translated source keys, placeholder/newline
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

The macOS packaging helper audits every Mach-O dependency and symlink, derives
the minimum OS, records dependency hashes, includes license texts and creates
an ad-hoc signed app. Clean-environment native launch and packaged real-engine
MCP queries have passed. The app has not been notarized or publicly released.

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
