# NeverD GUI process plugin SDK

Create an external program that contributes workbench actions, parameter
dialogs and result views. The host manages its tasks independently of the main
analysis worker. No NeverD engine library, Qt dependency or embedded Python
support is required for the Python example.

Read the [process-plugin SDK contract](../../docs/process-plugin-sdk.md), then run the complete
[Workbench Explorer example](examples/workbench-explorer/README.md).

`neverd_gui_plugin.py` is a Python 3.8+ standard-library helper for protocol v1:

```python
from neverd_gui_plugin import Task, run


def handle(task: Task) -> dict:
    task.progress("Preparing report")
    metadata = task.query("metadata")  # Declare the query in the manifest.
    task.log("Metadata received")
    return {"views": [{"kind": "text", "title": "Architecture",
                       "text": metadata["architecture"]}]}


if __name__ == "__main__":
    raise SystemExit(run(handle))
```

Copy the helper beside your provider or package it with your own application.
Create a manifest with an explicit executable/argument list, declare actions
and queries, and import it through **Plugins → Import process plugin…**.
Other languages can implement the same framed protocol directly.

String parameter declarations can include `maxLength` (0–262144 Unicode code
points). The host validates the default and each invocation against that bound,
and publishes it in tool input schemas. Omitting it preserves the v1 limit of
4096 UTF-16 code units. Parameter titles also become schema descriptions. Use an
explicit larger bound for report text or other multiline inputs; do not assume
that a model consumer knows hidden host limits.

Programmatic NPH actions can additionally declare `type: "object"` with an
object default for service-owned structured payloads. Process-plugin v1
manifests keep scalar form fields and reject this type. Result schemas support
explicit `additionalProperties: true` for heterogeneous payloads; closed
schemas retain rejection of unknown fields. `maxLength` and validated `default`
annotations are supported by the shared schema validator.

`Task` exposes captured `context`, validated `parameters`, `action`, `id`,
`output_dir`, `progress`, `log`, `query` and `artifact`. `run` implements the
handshake, one invocation, one result and error reporting. Do not print to
stdout or launch child work before `start`. A non-success query raises
`QueryError`, a `RuntimeError` subclass exposing `reply`, `status` and `code`.
Required queries should propagate that error. An optional section can catch a
specific unavailable/over-budget response and publish an explicit coverage
note; do not hide stale-session, malformed-request or transport errors.
The example keeps its useful report commands in `plugin.json` and additional
task/failure demonstrations in `plugin-developer.json`.
V1 cancellation terminates the process; flush private diagnostics or
checkpoints as needed, and never rely on a termination callback.

Tests are independent of the embedded Python SDK:

Run the suite with each supported interpreter being qualified. Artifact
containment uses resolved paths and `Path.relative_to`, including on Python 3.8;
it does not require the newer `Path.is_relative_to` API.

```sh
python -m unittest discover -s pluginsdk/gui/tests -v
```
