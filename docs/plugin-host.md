# NPH: NeverD Plugin Host

NPH is the Qt-independent owner and task runtime for external process plugins.
The C++ interfaces in `include/neverd/plugin` are internal host interfaces, not
a stable binary plugin ABI. Existing native and embedded Python engine plugins
keep their current loaders. This first layer supplies the shared runtime;
consumer adapters are added separately.

## Boundaries

| Target | Responsibility | Dependencies |
| --- | --- | --- |
| `neverd_plugin_host` | Owner tree, staged activation, exact registration identities, invocation leases, tasks, process pipes and child process shutdown | C++20, Threads and OS process APIs |
| `neverd_plugin_protocol` | Manifest and parameter validation, action dispatch, framing, artifacts and receipts | NPH, nlohmann JSON, OS crypto (OpenSSL Crypto on Linux) |

Neither library loads the analysis engine, Qt or LLVM. Binary semantics and
project mutations remain owned by the SDK and worker. Protocol validation
accepts text, table and graph views plus artifact and analysis-section
candidates. It does not install a candidate into the input image.

## Ownership and activation

A host creates project, component, task and view owners. Registrations require
an owner and remain staged until activation. Failed activation retires the
component and removes its staged registrations. Exact registration tokens keep
late cleanup from removing a newer binding with the same textual name.

Closing an owner rejects new registrations and invocations, cancels its tasks
and runs cleanup. Completion is asynchronous: active invocation leases and
cleanup callbacks must finish before the owner becomes closed. Cleanup failure
is recorded without skipping other resources. The injected dispatcher returns
completion to the host control sequence; it does not start a second scheduler.

Tasks capture their project context, parameters and configuration at admission.
Queue admission and retained history share the limits in `Config.h`. Accepted
work has a terminal state and completion receipt. A process provider exchanges
bounded length-prefixed UTF-8 JSON frames; artifact descriptors bind relative paths
and SHA-256 digests to that task's output directory.

## Configuration

The shared resolver applies plugin defaults, user preferences, project profile
and explicit invocation overrides in that order, then validates the complete
effective configuration. Running tasks retain their captured configuration.

## Build and verify

The normal tools build includes both libraries. A small standalone build avoids
loading the engine for owner and task tests:

```sh
cmake -S lib/plugin -B build-plugin-host -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
python3 scripts/build.py build-plugin-host --parallel 4
ctest --test-dir build-plugin-host --output-on-failure
```

`NeverDPluginHost.*` covers activation failure, retirement, registration
replacement, cleanup errors, separate projects, dispatch, asynchronous close,
late cleanup, task completion, retirement, receipt failures and configuration.

## Shared services and events

`Services` and `Events` register under NPH owners. Services have an ID, exact
major version, context consistency policy, effect and optional payload schemas.
Registration validates the schema; invocation checks that exact registration,
its owner and captured context before acquiring a lease. Host and project
domains prevent cross-project calls. Payload semantics remain with the service
owner. `HostServices` bridges `project.context@1` and the existing
`analysis.<query-name>@1` read operations in `Services.def`.

Events bind exact subscription identities and the same project domains.
Unsubscription, owner retirement and provider replacement invalidate old
bindings. Reentrant callbacks may unregister resources without deleting a
replacement. Events notify consumers; they do not refresh a running task's
captured analysis context.

## Shared process actions and installation catalog

`ProcessActions` launches providers and owns framing, deadlines, cancellation,
artifact verification and receipt completion. GUI and headless consumers supply
their project query/command adapters instead of duplicating this machinery.
Actions preserve captured parameters and context. Cancellation during a starting
notification is rechecked before provider launch.

`PluginCatalog` keeps saved installation intent separate from discovery,
compatibility and activation. An enabled record can remain unavailable with a
diagnostic. Importing the same active manifest reuses its registration; changing
contributions requires retiring the existing component. Removal forgets saved
intent and retires runtime resources; it does not erase provider files or undo
accepted project changes. Late retirement cannot remove a newer generation.

`NeverDPluginServices.*` tests service versions, context, lifetimes, reentrant
callbacks, event subscriptions, replacement and the host bridge.
`NeverDPluginCatalog.*` tests intent, repair, generations, portable installation
records, cleanup and queue ordering. All run in the standalone NPH build above.

## Run the same independent provider in CLI and GUI

Prepare the existing Workbench Explorer example, which has no TVM dependency:

```powershell
python pluginsdk/gui/examples/workbench-explorer/prepare.py E:/plugins/workbench-explorer
neverd-plugin-host list E:/plugins/workbench-explorer/plugin-developer.json
neverd-plugin-host run E:/plugins/workbench-explorer/plugin-developer.json context
neverd-plugin-host run E:/plugins/workbench-explorer/plugin-developer.json background
```

The `background` action demonstrates progress, effective parameters and a
SHA-256-bound report artifact. Run a real function report through the existing
analysis worker with:

```powershell
neverd-plugin-host run E:/plugins/workbench-explorer/plugin.json report `
  --worker E:/neverd/bin/neverd-worker.exe --input E:/samples/example.exe `
  --function 0x140001000 --address 0x140001000 --output E:/reports/function-1
```

Use an actual function address from the input. `--output` must be a new directory.
Without it the CLI creates a new task directory under `neverd-plugin-results`.
The bridge opens the input read-only, forwards declared reads with captured
project/revision identity and preserves worker errors. Image publication remains an explicit host operation. Without a worker/input, only actions
whose required context is `none` can run.

stdout contains one JSON record. Progress, diagnostics and the receipt path go
to stderr. Exit codes are 0 for success, 1 for a failed/timed-out provider task,
2 for configuration/startup errors, and 130 for cancellation. Ctrl+C requests
cancellation and drains the native child transport before exiting.

Import the same manifest through the GUI's plugin action. Its command schema,
effective parameters, wire validation and task completion rules are identical.
GUI views render the structured result that the CLI returns as JSON.

## Headless catalog and Tools consumer

```powershell
neverd-plugin-host plugins E:/plugins/catalog.json import E:/plugins/workbench-explorer/plugin.json
neverd-plugin-host plugins E:/plugins/catalog.json list
neverd-plugin-host plugins E:/plugins/catalog.json explain workbench-explorer
neverd-plugin-host plugins E:/plugins/catalog.json disable workbench-explorer
neverd-plugin-host plugins E:/plugins/catalog.json enable workbench-explorer
neverd-plugin-host plugins E:/plugins/catalog.json remove workbench-explorer
```

Catalog reads start no provider. Failed enablement saves the user's requested
intent and retains its diagnostic. The shared atomic file writer persists the
portable `neverd.plugin-catalog.v1` projection. Removal retains provider files.

`neverd-plugin-host tools <plugin.json> <tools.json> --output <new-directory>`
accepts JSON-lines resolve, call, inspect, cancel and release operations.
See [Tools](tools-layer.md) and the independent Tool Roundtrip caller for the
complete protocol. Project writes require the explicit
`--allow-project-writes` option and declared commands. The initial adapter
supports rename through the existing worker revision and history contract.

## Standalone CLI build

```sh
cmake -S tools/neverd-plugin-host -B build-nph -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
python3 scripts/build.py build-nph --parallel 4
ctest --test-dir build-nph --build-config Release --output-on-failure
```

This configuration does not find Qt or build the analysis engine. Windows uses
BCrypt, macOS CommonCrypto and Linux OpenSSL Crypto for artifact digests.
`NeverDPluginCLI` launches actual providers and tests Unicode paths, framing,
artifacts, progress, cancellation, failures, deadlines and persisted catalog
intent. `NeverDToolsConsumer` checks the JSON-lines consumer. Builds with an
actual worker and `NEVERD_WORKER_REAL_ENGINE_TESTS` also register
`NeverDPluginWorker` and `NeverDToolsWorker` for analysis and project readback.
