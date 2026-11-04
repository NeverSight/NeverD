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

Provider processes retain cumulative stdout and diagnostic limits per task.
The headless analysis worker persists across tasks, so its transport instead
bounds bytes awaiting callback delivery on each stream. Delivered bytes release
that charge; a stalled consumer still fails at the same buffered-byte limit.
The worker's separate per-frame size limit remains unchanged.

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
`NeverDPluginProcess.*` covers cumulative provider limits, persistent output on
both streams, queued-output limits, and callback disposal after transport exit.

## Services and events

`Services` and `Events` in `include/neverd/plugin` are Qt-independent host
registries. `HostServices` adapts project context and existing query operations
for GUI, ordinary CLI and Tools process callers. They use the same NPH owners,
registration identities and invocation leases as actions. These C++ interfaces
are internal host extension interfaces; the native plugin ABI remains separate.

Services have a stable ID, an exact positive integer major version, a description,
consistency policy, effect and optional input/output payload schemas. A null
schema delegates payload validation to the semantic service owner; the analysis
bridge uses the existing SDK/worker validators. A supplied schema uses the same
supported JSON-schema subset as Tools, validated at registration and invocation.
Replies carry `status`, a successful `payload` or structured `error`, and the
captured request `context`. SDK replies retain their existing revision fields.

`Services::add(provider, domain, ...)` stages registrations under a provider
owner. The domain is a host or project ancestor. Discovery and invocation only
see active bindings within the caller's domain. Different projects cannot call
one another's service tokens, even with matching context JSON. Versions coexist;
resolution never falls back to a different major. Duplicate ID/version pairs in
one domain are refused. Resolving an ambiguous host/project ID is also refused.

Consistency policies are `none`, `project` (loaded/project ID/epoch), and
`revision` (also exact revision). Existing analysis services use `revision`.
Their adapter and SDK/worker still validate the state at actual query execution;
NPH admission does not replace existing pagination or discovery-generation rules.
An event does not update the captured analysis input. `project.context@1` is an
explicit current-state read, available even when no input file is loaded.

The built-in query mapping is `analysis.<query-name>@1`, covering the names in
`Services.def`, such as `analysis.functions`, `analysis.disasm`,
`analysis.metadata` and `analysis.cfg`. An analysis adapter must
actually be present: the headless host without `--worker/--input` only offers
`project.context`. Legacy `Task.query()` routes through the same captured service
bindings when running in these hosts. Project writes continue through declared
command/publication adapters and their real receipt rules; the new process
service-call endpoint currently exposes read services.

Async handlers retain their reply callback until completion or explicit
detachment. A provider invocation stays leased until then. Caller retirement
removes the consumer callback, so no result invokes disposed plugin code.
Provider retirement rejects new requests and waits for outstanding handlers.
Duplicate replies are ignored. Synchronous handlers also retain a stack lease
through their return, including when a reply reenters host cleanup.

Events have an ID, exact major version, description and optional payload schema.
`Events::add(provider, domain, ...)` registers a source;
`Events::subscribe(subscriberOwner, sourceToken, callback, ...)` creates an owned
subscription. A staged source/subscriber cannot deliver notifications before
activation. Publishing queues immutable context/payload envelopes with a
monotonic sequence and exact source identity on the host control dispatcher.
Delivery rechecks both registrations and holds their invocation leases.
Unsubscribing or retiring either side drops queued notifications. A replacement
with the same name does not inherit the old subscriptions. Notifications have
no persistence, replay or delivery acknowledgement contract.

Built-in sources, all at version 1, are:

| Event | Payload / origin |
|---|---|
| `project.context_changed` | Previous/current host context, including selection and revision |
| `project.revision_changed` | Previous/current revision from the project adapter |
| `selection.changed` | Previous/current context when address or function changes |
| `task.changed` | Task ID, plugin/action, phase, progress and captured input context, from ProcessActions |
| `plugin.installation_changed` | GUI catalog record/runtime notification and installation snapshot |

Sources belong to the bound project. Closing/replacing the project retires its
subscriptions; a process task does not remain subscribed across projects or
after its result/exit. Other host/plugin components can register their own
service and event definitions through these same registries. Long-lived remote
service providers are not added by this per-task process extension.

### Process capability extension

The existing v1 manifest accepts optional `services` and `events` declarations:

```json
{
  "services": [{"id": "project.context", "version": 1}],
  "events": [{"id": "task.changed", "version": 1}]
}
```

These extend a complete manifest; they are not a standalone manifest. Entries
require exactly `id` and `version`; duplicate pairs and nonpositive/noninteger
versions are rejected. Declarations permit calls/subscriptions, not OS sandbox
permissions. Missing versions are reported without disabling unrelated actions.
Legacy `queries` also permit the matching `analysis.*@1` service.

Start frames advertise `host_services` and `host_events` in `extensions`.
Providers send `type:host` with their task/request ID, operation and object
payload; responses use `type:host_reply` with the same IDs and a structured
`reply`. Existing length framing, protocol version, outstanding-request limits
and result-after-replies ordering apply.

| Operation | Payload | Successful reply payload |
|---|---|---|
| `services.list` | `{}` | `services` descriptors and `missing` declared versions |
| `services.call` | `registration` string, `payload` value | Service result payload |
| `events.list` | `{}` | `events` descriptors and `missing` declared versions |
| `events.subscribe` | `registration` string | `subscription` string |
| `events.unsubscribe` | `subscription` string | Subscription ID and `active:false`; idempotent |

Discovery shows `declared` separately from availability. Bindings are captured
once when a task is admitted, and returned registration IDs are opaque strings.
Repeated discovery rechecks availability without rebinding a vanished service
to a replacement. Invoking an undeclared/fabricated/stale token returns a
structured refusal. Each task may hold at most 32 live event subscriptions.

Notifications use `type:event`, `task_id`, `subscription`, and `event` containing
`id`, `version`, `sequence`, `registration`, `context` and `payload`. Sequence and
registration are opaque decimal strings. Events may interleave with query,
command and host replies; consumers must demultiplex by frame type and identity.
In-flight notifications already written before unsubscribe can precede its
reply; the Python helper discards buffered notifications for that subscription
once unsubscribe returns.

The Python `Task` exposes `services()`, `service(id, version=1, **payload)`,
`events()`, `subscribe(id, version=1)`, `next_event()` and `unsubscribe(id)`.
`next_event()` pumps the protocol and waits under the action's existing deadline.
Optional handlers run while the provider reads replies/events; they return
before making another host request. No Python background thread is created.
Providers can import the frontend-neutral `neverd_host_plugin` entry point;
the historical helper module continues to own framing and compatibility.

The standalone [Host Observer example](../pluginsdk/common/examples/host-observer/README.md)
demonstrates discovery, context reads, event delivery, explicit unsubscribe,
metadata when an engine is present, GUI views and an exported report. Its prepare
script copies all helpers outside the repository. The same prepared manifest
works in the GUI and ordinary CLI.

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

## GUI adapters and manager

The workbench uses the same NPH catalog, actions, tasks, services and events as
the headless consumer. `Session` owns `PluginServices`; it adapts persistent
installation intent, captured query subscriptions and accepted image-publication
receipts. `PluginJobs` projects the shared runtime into Qt notifications.
`AnalysisPlugins` renders commands and per-plugin task results, while
`PluginManagerDialog` exposes import, enable, disable, retry, uninstall and
structured diagnostics. Closing the task pane does not destroy the runtime.

Menu placement is presentation state: each command can appear directly in the
context menu with its icon or under Plugins → plugin name. The manager persists
requested enablement independently of actual activation. Installation and menu
restoration do not launch provider processes. The task pane starts hidden and
opens when work is invoked.

Importing an analysis-section candidate captures the task revision, verifies
input/artifact digests and submits the existing SDK command through Session.
An owner invocation lease keeps accepted publication alive until its receipt
arrives even if the plugin is retiring. A Session destroyed before a reply
records an unknown outcome; cleanup does not undo an accepted project edit.

See [GUI process plugins](gui-plugins.md) for manifests, menus, images and manual
acceptance steps. `NeverDGuiPlugins` covers actual external providers through
the Qt adapter, grouping, settings, parameters, query lifetime, manager state,
captured revisions, cancellation and malformed protocol. The CLI owns a
separate headless consumer; neither adapter links analysis semantics into NPH.

## Current implementation limits

This series implements external process-plugin hosting and the model-independent
Tools library. The C++ interfaces remain internal. Existing native and embedded
Python engine plugins keep their current loaders. Native package negotiation,
LowIR/MedIR/HighIR pass registration, persistent project providers, and agent
adapters remain separate work. The GUI manages process manifests and does not
yet discover Tools catalogs. No external source-IR renderer or F5 source
provider is part of this series.

