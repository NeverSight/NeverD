# Workbench Explorer

The developer manifest also includes **Create analysis function**. With an x64
image loaded, select a free virtual address and a returned integer; the provider
emits a real `mov eax, imm32; ret` artifact. Click **Import code into image**, then
use the normal CFG/F5 views. This demonstrates the generic `image_addition`
contract, without depending on TVM or claiming to recover an existing function.
The SDK validates overlap and function extents; save the project to keep it.

A standalone GUI plugin example covering project context, read-only engine
queries, menus, context actions, command palette, parameter forms, docking,
text/table/graph results, navigation, background progress, cancellation,
diagnostics and artifact export. It contains no TVM code or test-binary
addresses and does not execute or rewrite the open binary.

## Prepare and install

From the repository root:

```powershell
python pluginsdk/gui/examples/workbench-explorer/prepare.py E:/plugins/workbench-explorer
```

On Unix replace the destination with a writable external directory. The copy
contains the provider, helper, two manifests, an `icons/` directory and this walkthrough. The manifests
pin the interpreter used for preparation. Open the copied `plugin.json` through
**Plugins → Import process plugin…**. The source `plugin.json` can also run
directly when Python 3.8+ is on PATH; it finds the helper in the SDK tree without
requiring `PYTHONPATH`.

Use a GUI build supporting `imports`, `resolve` and manifest icons. Earlier
example hosts may refuse the updated manifest with `Invalid process plugin
manifest` or `Unsupported or repeated plugin query`. Start the matching rebuilt
GUI before importing the updated manifest; replacing the provider alone cannot
update the host's contract.

## Choose the context menu layout

Commands initially appear under **Plugins → Workbench Explorer → command**.
Other installed plugins get their own named submenus.

Open **Plugins → Context menu settings…** (also **Menu settings…** in the task
pane). Set **Analyze current function** to **Direct context menu** and keep the
other commands in **Plugins submenu** to try both forms together. Change the
icon through **Browse…**, use a workbench name such as `builtin:analyze`, or clear
it. Defaults include a bundled SVG report icon and built-in instruction/graph
icons. Local PNG, ICO and SVG files are supported.

OK applies and remembers choices; Cancel leaves them unchanged. **Restore
Defaults**, followed by OK, restores grouping and manifest icons. Unload and
reimport to verify that user choices survive. Moving a command does not change
its availability, result or task behavior, and it remains in the top Plugins
menu and command palette.

## Walk through the GUI

1. Open a binary and choose **Browse largest functions**. The sortable table
   ranks up to 512 catalogue entries by size without analyzing their bodies.
   Activate a row with Enter/double-click to navigate to its address.
2. Choose **Analyze current function** from Plugins, the analysis view's
   right-click Plugins submenu, or Ctrl+Shift+P. Inspect the summary, call sites,
   references and instruction tabs. Direct calls use ND reference information;
   imported calls show the module/API and IAT slot. Unknown indirect targets
   retain their operands. Callee bodies are not analyzed.
3. Use **Export artifact…** to save the JSON report or Markdown summary to a new
   destination. JSON includes the collected instructions, calls, references,
   coverage notes and optional CFG; exported bytes are checked against SHA256.
4. Run **Function report with CFG**. Pan by dragging; `+`/`-` zoom;
   double-click or select a node and press Enter to follow its address.
5. Select an instruction and run **Inspect selected instructions**. Inspect the
   next 32 instructions, 64 bytes and captured selection in readable tabs.
6. Float/redock the pane, switch workbench colors/fonts, and navigate its
   controls by keyboard. Unload the plugin; its commands disappear and pending
   jobs stop. Reimport to update it. Registration is currently per window.

The result tabs take priority over task history. Logs are retained while hidden;
use **Show log** to reveal them and drag splitter handles to resize each area.

The function report scans at most 512 linear instructions inside ND's declared
function extent. If the extent is unknown, it explicitly reports a 64-instruction
window. Linear instruction counts do not establish reachability. Reference
queries cover at most 32 instruction sites and imported names use the first 512
import slots. Missing optional engine information is recorded in coverage notes;
core disassembly, stale-session and protocol errors still fail clearly.

The CFG tab previews at most 128 blocks and 512 edges. JSON retains the full CFG
returned by ND. An unavailable or over-budget CFG leaves the other report tabs
usable with the actual diagnostic in the summary.

## Exercise the developer controls

Unload the default manifest, then import `plugin-developer.json`. It uses the
same plugin ID and adds three explicitly named demonstrations. Intentional
failure is not part of the default menu.

1. With or without a file, run **Show captured context**. Project/address/function
   commands are disabled until their context exists.
2. Run **Run background demonstration…**. Exercise integer, text, choice and
   boolean fields; Cancel the dialog once, then run it. Values are remembered.
   Observe the initial unknown-total phase, completed-step counts, elapsed time
   and log while continuing to browse the binary.
3. Enable report output. After success, use **Export artifact…** and select a
   new destination. Inspect the JSON context and message; `<b>text</b>` remains
   literal text in the result pane.
4. Increase the delay and steps; start two jobs. Cancel a queued job and then a
   running job. Close/reopen the task dock to observe that hiding it does not
   stop work. Switch projects during another run and inspect its stale state.
5. Exercise all failure modes: reported error, exit code 17, malformed result,
   and a deadline timeout. Verify that the main analysis worker remains usable.

See the [full protocol, limits and lifecycle](../../../../docs/gui-plugins.md).
The example's sleep loop demonstrates task mechanics, not a performance
benchmark or an analysis algorithm. Its graph uses queried CFG relationships;
layout and node summaries are deliberately compact.
