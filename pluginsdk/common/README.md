# Shared process-plugin consumers

The model-independent [Tools layer](../../docs/tools-layer.md) now supplies
discovery, exact invocation, cancellation, retained receipts and explicit
follow-up calls. [Tool Roundtrip](examples/tool-roundtrip/README.md) is an
independent external example with a deterministic caller: real disassembly →
report → reviewed naming choice → actual project receipt → persisted readback.
Prepare it with `python pluginsdk/common/examples/tool-roundtrip/prepare.py
E:/plugins/tool-roundtrip`. The optional `neverd_tools_plugin.py` helper uses the
same framed process protocol, with structured output and declared command
extensions; it does not import an Agent or model client.

The first NPH implementation uses the existing `neverd.gui-plugin.v1`
declarations and [standalone Python helper](../gui/neverd_gui_plugin.py).
The format's name remains unchanged for compatibility. It is accepted by both
the GUI and the Qt-independent `neverd-plugin-host` executable.

The independent [Workbench Explorer example](../gui/examples/workbench-explorer/)
is the shared runnable example: it supplies function reports, progress,
parameters and artifacts. Prepare it without modifying the ND build:

```powershell
python pluginsdk/gui/examples/workbench-explorer/prepare.py E:/plugins/explorer
neverd-plugin-host run E:/plugins/explorer/plugin-developer.json background
```

Import that manifest into the GUI to run the same action. The function-report
actions query an opened project; the CLI uses `--worker`, `--input`, `--address`
and `--function` to supply that context. See the [NPH guide](../../docs/plugin-host.md)
for concrete commands, configuration precedence, receipts and tests.

Any language can implement the framed protocol and run as `program`, including
a separately built C++ executable. This directory does not introduce a new
native C ABI or a LowIR/MedIR/HighIR pass SDK. Those remain separate additions.

## Shared host services and events

`neverd_host_plugin.py` exposes the shared process `Task` helper for GUI and
headless hosts. See [Host Observer](examples/host-observer/README.md) for a
standalone discovery/service/event example and [the host contract](../../docs/plugin-host.md#services-and-events).
The prepare script packages both the neutral entry point and the historical
framing helper; no embedded Python SDK or Qt is needed by the provider.
