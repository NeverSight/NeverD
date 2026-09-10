# NeverD desktop workbench

The desktop workbench uses Qt Quick/QML and a separate `neverd-worker` process.
The worker links the same `libneverd` shared library used by the CLI through its
public C ABI. Analysis, function discovery and decompilation remain in that
library; Qt owns presentation and asynchronous request coordination. Keeping the
worker separate lets analysis run without blocking or crashing the UI. The GUI
executable does not link LLVM or the CLI, and no model service is required to
browse binaries.

## Build

The normal engine/CLI configuration is unchanged. Qt is optional and is searched
only when `NEVERD_BUILD_GUI=ON`. To add the workbench to an engine build, use:

```sh
cmake -S . -B build -DNEVERD_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.1/platform
cmake --build build --target neverd-gui
```

For fast desktop development, build against a matching existing shared engine:

```sh
cmake -S tools/neverd-gui -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.1/platform \
  -DNEVERD_ENGINE_LIBRARY=/path/to/libneverd.dylib
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
build-gui/bin/neverd-gui /absolute/path/to/binary
```

Requires C++20, CMake 3.24+, Qt 6.8+ Core/Gui/Qml/Quick/QuickControls2/Network/Svg/
LinguistTools, GuiPrivate/QuickPrivate for the pinned KDDockWidgets frontend (Qt Test for tests), and Python 3.10+ for tests and the MCP adapter.
The first configure downloads nlohmann/json 3.11.3 and KDDockWidgets 2.4.1 using pinned SHA-256 digests. Ship the exact Qt build used to compile its private headers.
On Windows set `NEVERD_ENGINE_LIBRARY` to the runtime DLL and
`NEVERD_ENGINE_IMPLIB` to its matching import `.lib`; make runtime dependencies
available alongside the worker. On Linux use the matching `.so` file.

The worker can also be built without Qt, either with `NEVERD_BUILD_WORKER=ON` in
the root build or by configuring `tools/neverd-worker` standalone. The shipped
worker never links the test engine. Existing CLI commands remain available;
sidecar write failures now propagate as errors instead of reporting success.

## Use

Open a native binary or supported bytecode file. Select a function, or enter a
symbol or hexadecimal **virtual address** in the navigation bar. Addresses stay
64-bit in C++ and cross JSON/QML as strings. The left list supports filtered,
paged reads; the central pane shows disassembly, bytes or CFG; the adjacent pane
shows C, LowIR, MedIR, HighIR or LLVM IR. Code pages retain exact engine output.
Use the load-more controls for additional rows. Code text supports selection and
copy; instruction selection and references use actual addresses.

Native LowIR and MedIR rows carry retained instruction anchors for linked
selection when the engine provides the additive mapped-page API. Headers and
synthetic operations have no invented address. C, HighIR, LLVM IR and VM source
mapping remain explicitly unsupported; their text is still available. An anchor
identifies an originating instruction, not every contributor to a transformed
expression.

The CFG uses a worker layout and indexed viewport queries. A view receives at
most 256 nodes and 512 edges, with visible counts and an explicit zoom/truncation
indication; it does not create an object for every offscreen block. Snapshots
support up to 20,000 nodes and 100,000 edges within the backend result budget.
Only the legacy `cfg` preview request retains the 500-node/2,000-edge cap.

The engine's current decompilation API may first analyze the whole image. The
window stays responsive while this runs in the worker. Cancel can remove queued
work; a synchronous active engine call continues until it returns or the worker
is terminated. Restart terminates the worker, reopens the binary and reloads saved edits.
Opening another file, restarting, reloading or quitting with staged edits offers Save, Discard and Cancel before the session changes. Errors, unavailable results and
budget limits remain visible instead of being presented as empty successful
analysis.

Annotations use the existing `.neverd-annotations.json` sidecar and explicit Save.
Renames use the existing `.neverd-renames.json` sidecar and are saved atomically.
One worker owns a writable input through an operating-system advisory lock;
headless MCP opens read-only. Current CLI/C ABI sidecar writes use the same lock
and fail while another writer owns the input. Older binaries and external editors
can bypass the lock, so saves also check for foreign edits. Reload discards
staged notes; a missing sidecar is an empty saved state. Failed sidecar writes
preserve the previous file through atomic replacement.

The UI starts in English and bundles all 11 project languages. Settings switches
language without reloading the analysis; Arabic mirrors UI chrome while code and
addresses stay left-to-right. Floating and tabbed dock layouts, window size and language preference are saved per user. Pin keeps a representation on one function while the instruction pane navigates independently.

## Keyboard and reading workflow

The workbench retains Dark+ and adds the common IDA navigation defaults. These
shortcuts operate on analysis content; typing in a search field or annotation
dialog keeps normal text editing behavior. A read-only C/IR view remains an
analysis view, so navigation, rename and comment commands stay available there.

| Key | Action |
| --- | --- |
| G | Focus the address/symbol field; Enter navigates, Esc returns to disassembly. |
| Esc / Ctrl+Enter | Previous / next navigation position. Platform Back/Forward also work. |
| Space | Toggle linear disassembly and CFG while reading the machine view. |
| F5 | Show recovered C and focus its text. |
| Tab | Move between machine and C/IR content; ordinary controls retain Tab traversal. |
| X | Show and focus incoming references. |
| Ctrl+P | Focus function search. |
| N / ; | Rename the selected function / edit the selected address comment. |
| F6 / Shift+F6 | Cycle open panes in either direction. |
| Platform Save | Save annotations. |

Function, instruction and reference lists support arrows, Home/End and
PageUp/PageDown. Enter activates the current row; platform Copy copies an
instruction or reference as plain text. Current keyboard rows remain visible,
and linked instruction selections reveal their address without loading the full
function catalog. Holding an arrow key updates selection immediately while
coalescing comment/reference queries; each pane has at most one derived request
in flight, with comment and reference reads dispatched serially.

Instruction columns follow the configured font metrics, with horizontal
scrolling where needed. Narrow references use two lines so both 64-bit addresses
remain readable. View → Focus Panel temporarily expands a pane; selecting the
same command again, or opening another pane, restores the saved arrangement.

The shortcuts follow the [official IDA usage guide](https://docs.hex-rays.com/8.5/getting-started/basic-usage)
and [shortcut reference](https://docs.hex-rays.com/user-guide/configuration/shortcuts).
NeverD's multi-representation and pane navigation add to those familiar actions.

## History and extensions

Annotation and rename commands have bounded undo/redo history, persisted alongside
the binary in `.neverd-history.json`. The history is bound to the input hash and
sidecar contents. A write-ahead journal recovers an interrupted save; foreign
edits disable replay instead of silently applying commands to another state.
Save or reload staged annotations before undoing a durable rename.

The Extensions panel imports versioned JSON manifests with namespaced read-only
query contributions. Imported labels and results are plain text; manifests
cannot run scripts or register arbitrary engine operations. The registry lasts
for the worker session. See the manifest schema in the worker protocol.

## MCP

The Connections panel starts connections only on request. It supports local stdio
programs with an explicit argument list and Streamable HTTP with TLS verification,
optional bearer credentials and a custom CA file. Tool schemas, arguments and
results are inspectable. The bounded call history retains parameters, results and cancellation states; large server results use immutable paged resources. MCP is an interoperability client, not a model provider.

The standalone, Qt-free adapter uses MCP **2025-11-25** newline JSON-RPC:

```sh
tools/neverd-mcp/neverd-mcp --worker /absolute/path/to/neverd-worker \
  --file /absolute/path/to/binary
```

For the current GUI project, enable session sharing in Connections and copy its
credential-file path. Configure the external client as:

```sh
tools/neverd-mcp/neverd-mcp --attach /absolute/path/to/credentials.json
```

Attachment queries the same worker and revision, never starts another project
writer, and ends when sharing or the GUI closes, or the active project changes. The credential file is private
to the user. The adapter exposes bounded analysis queries; it does not expose
arbitrary core/plugin execution. See [MCP details](../tools/neverd-mcp/README.md)
and [worker protocol](../tools/neverd-worker/PROTOCOL.md).

## Packaging and qualification

`cmake --install build-gui --prefix dist` invokes Qt's QML deployment script.
The matching engine and its dependencies must be included in a distributable
package. macOS can create a new ad-hoc signed development bundle with:

```sh
python3 tools/neverd-gui/package_macos.py --build-dir build-gui \
  --engine /absolute/path/to/libneverd.dylib \
  --qt-dir /path/to/Qt/6.11.1/macos --output dist/NeverD.app
```

This produces an ad-hoc signed development application, not a notarized release.
Test native dialogs, IME, accessibility, mixed-DPI screens and platform packaging
on each target platform before a public release. Automated offscreen tests do not
prove those properties. Performance measurements and unresolved issue #108
acceptance items are recorded in the implementation plan and benchmark report;
proposed latency targets must not be described as measured results.

See the [packaging guide](../tools/neverd-gui/PACKAGING.md) for dependency and license inputs, and [qualification record](gui-qualification.md) for measured evidence and remaining issue #108 criteria.
