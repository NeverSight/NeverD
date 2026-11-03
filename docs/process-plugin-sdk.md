# External process-plugin SDK

External providers are standalone programs. The Python helper in
`pluginsdk/gui/neverd_gui_plugin.py` requires Python 3.8+ and only the standard
library. The manifest name `neverd.gui-plugin.v1` is retained for compatibility;
the process protocol is independent of Qt and embedded Python. Any language,
including a separately built C++ executable, can implement the same framing.

## Provider lifecycle

The host launches the manifest's executable and argument list without a shell.
Frames have a four-byte big-endian length followed by UTF-8 JSON nested at most
64 levels deep. The provider first sends `hello`; the host sends one captured
`start` invocation. Progress, logs, queries and their replies carry the exact
task identity. The provider sends one `result` and exits. `run(handler)` owns
this exchange. Reserve stdout for protocol frames. Cancellation terminates the
provider process tree.

`Task` exposes context, validated parameters, output directory, progress,
logs, declared queries, services and event subscriptions. Query failures retain
their structured status and error code. Required queries should propagate
failures. Optional sections may report an explicit coverage limitation.

Results contain text, table or graph views and artifact descriptors. Artifacts
are resolved within the task output directory, use relative paths and bind the
file's SHA-256 digest. Root and sibling escapes, symlink escapes and reserved
receipt paths are refused. This containment also works with Python 3.8.
Analysis-section candidates remain explicit host write operations, using the
existing SDK contract rather than provider-side input mutation.

## Independent examples

- [Workbench Explorer](../pluginsdk/gui/examples/workbench-explorer/README.md)
  produces function reports, instruction tables, CFG views and exportable
  artifacts. Its separate developer manifest demonstrates parameters,
  background progress, failure handling and image-addition candidates.
- [Host Observer](../pluginsdk/common/examples/host-observer/README.md)
  discovers and invokes host services and observes project-context events.
- [Tool Roundtrip](../pluginsdk/common/examples/tool-roundtrip/README.md)
  provides action declarations and a deterministic caller for the Tools
  consumer. That consumer is supplied by the separate Tools and CLI layers.

Each prepare script copies its complete provider package and records the exact
Python executable, so it can run outside the source tree without PYTHONPATH or
an embedded SDK install. Workbench report queries require a loaded project.
The worker and GUI/CLI adapters supply that project; the provider does not link
the analysis engine.

## Verification

```sh
python -m unittest discover -s pluginsdk/gui/tests -v
```

`NeverDProcessPluginSDK` registers the same suite in the NPH test build. Run it
with every supported interpreter being qualified. The suite exercises fragmented
framing, task identities, errors, services/events, artifact containment and
example report construction without loading the analysis engine.
