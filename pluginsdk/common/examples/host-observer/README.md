# Host Observer

An independent process plugin for both GUI and CLI. It lists actual host
services and declared versions, reads the current context, subscribes to a task
event, emits progress, receives that notification and explicitly unsubscribes.
When an analysis adapter and project are available it also reads metadata.
The GUI shows the service table and report; the CLI writes the same JSON artifact.

Prepare a copy outside the source tree:

```powershell
python pluginsdk/common/examples/host-observer/prepare.py E:/plugins/host-observer
```

Import its `plugin.json` using **Plugins → Plugin manager…**, then run
**Plugins → Host Observer → Inspect host services and events**. Headless:

```powershell
neverd-plugin-host run E:/plugins/host-observer/plugin.json inspect --output E:/reports/host-observer-new
```

The output directory must be new. No analysis engine is required for context,
discovery or task events. With `--worker <exe> --input <file>`, the same action
also reports available analysis services and metadata. A missing engine is
shown as missing services, not as a failed plugin task.

Subscriptions belong to the task owner and end when it finishes or unloads.
The service/event bindings stay fixed for that invocation. `project.context`
explicitly reads current host state; it does not update the task's captured
analysis revision. Process plugins declare the services and events they use in
their manifest. The wire contract is documented in `docs/plugin-host.md`.
