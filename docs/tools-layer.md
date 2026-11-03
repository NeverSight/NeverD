# Tools: model-independent plugin calls

`neverd_tools` is an implemented C++20 library above NPH. It discovers tools,
freezes a consumer step, invokes exact action registrations, retains operation
records and routes explicit follow-up calls. It does not depend on Qt, an Agent,
a model client or the analysis engine. The executable consumer is
`neverd-plugin-host tools`.

Its process actions also use NPH's [versioned services and owned events](plugin-host.md#services-and-events).
The same manifest declarations and Python helper calls work in the ordinary CLI
and GUI. Event frames can interleave with command receipts; the shared helper
demultiplexes them without replacing Tools' operation ledger or command adapter.

```text
headless JSON-lines caller / future GUI, Agent or MCP consumer
                         ↓
                 neverd_tools
       frozen sets, policy, call IDs, continuations
                         ↓
              NPH Actions / ProcessActions
       owner registrations, leases, Tasks, Process, Protocol
                         ↓
             external plugin and project adapter
                         ↓
         existing ND worker / SDK revision and journal
```

NPH never links to Tools or a model SDK. Tool algorithms live in actions, not in
the consumer. Long operations use the existing NPH task controller; Tools does
not create another scheduler. Original v1 GUI process plugins retain their
existing protocol behavior.

Process tools are action contributions in the shared NPH `PluginCatalog`.
Their output/effect/command declarations extend manifest actions, and Tools
registrations reference the same staged action tokens. GUI, ordinary CLI and
Tools execution all use `Actions` / `ProcessActions`. Installation metadata,
saved intent, activation and cleanup diagnostics use NPH installation snapshots.

## Implemented surfaces

| Surface | Current implementation |
|---|---|
| Tool registration | NPH owner resource and exact action token; component activation publishes all registrations together |
| Discovery | Action description, input/output schemas, resolved defaults, context/services, effect, headless eligibility and unavailable reason |
| Execution | Synchronous or asynchronous action, progress notifications, inspection, cooperative cancellation, validated business data and artifacts |
| Frozen consumer steps | Exact tool/action generations, parameter defaults, configuration, project/input/epoch/revision/profile context |
| Continuations | Explicit prior action binding plus a host-retained successful parent operation; original context checked again |
| Deduplication | One live-runtime operation per consumer session/step/call ID; changed input or binding conflicts |
| External provider | Optional tools.json catalog referencing ordinary manifest actions; structured data and declared command extensions |
| Project adapter | CLI reads through the existing worker; explicitly enabled rename commands use the existing revision/journal implementation |
| Future consumers | The same library can be embedded in GUI or exposed through an Agent/MCP adapter |

The GUI task pane does not yet discover Tools catalogs or provide a Tools
manager. Agent/model clients, prompt construction, model authorization UI and
full model-context archival are separate work. The C++ headers are host library
interfaces, not a frozen native plugin DLL ABI. Existing native and embedded
Python loaders are unchanged.

## Registration and execution

The owning interfaces are:

- [plugin/Actions.h](../include/neverd/plugin/Actions.h): callable NPH action
  declarations, exact registration, structured completion and cancellation.
- [plugin/ProcessActions.h](../include/neverd/plugin/ProcessActions.h): adapter
  from external providers to Actions, reusing Tasks/Process/Protocol.
- [tools/ToolRegistry.h](../include/neverd/tools/ToolRegistry.h): discovery,
  resolved steps, deduplication, explicit continuations and operation records.
- [tools/ProcessTools.h](../include/neverd/tools/ProcessTools.h): atomic catalog
  installation into one component owner.

Register handlers with `Actions::add(owner, definition, handler, error)`.
Register tools with `ToolRegistry::add({toolId, actionToken, continuationOf},
error)`. Tools inherit the action owner, parameter fields, schemas and execution
entry. Activating components stage definitions; consumers cannot discover or
invoke them until activation succeeds. `failActivation` retires that component's
registrations while unrelated components stay active.

`resolve(session, step, user, profile, error)` captures one immutable tool set.
Configuration objects are keyed by plugin ID, then tool ID. The existing NPH
resolver applies defaults → user preferences → project parameters → explicit
call parameters. Every layer is checked; later overrides do not hide invalid
earlier values. Stored settings are unchanged. Discovery publishes effective
defaults and structured unavailable reasons.

Process inputs currently use existing v1 boolean, integer, string and choice
fields, not arbitrary nested JSON-schema inputs. Structured outputs support
`type` (object, array, string, integer, number, boolean, null), `description`,
`enum`, numeric `minimum`/`maximum`, object
`properties`/`required`/`additionalProperties:false`, and array `items`/`maxItems`.
Unsupported keywords fail registration. Input-schema defaults in discovery come
from the v1 resolver, not the output-schema validator.

`call(set, exactToolToken, callId, parameters, parentOperation, error)` checks
current policy, required services, owner and captured context before NPH invoke.
Policy is an injected consumer function, called during resolution and again at
admission. A model adapter must supply its policy; the host does not infer model
approval from plugin text. Tokens include host identity. Same-name replacement
cannot retarget an old set, and task tools expire when their owner closes.
Releasing a set prevents new calls from every copy without cancelling existing
operations.

All mutations and callbacks run on the NPH control sequence. Post completions
from other threads through its dispatcher. Execution leases span both the
synchronous handler stack and later asynchronous completion. An observer can
request cancellation during the first progress notification.

## Operations, cancellation and callbacks

The key `(consumer session, step ID, call ID)` is recorded before provider code
runs. Repeating it with the same exact tool, effective parameters, parent
operation and captured configuration/context returns the existing operation.
Different data returns `call_conflict`. A new key starts a new operation even
with identical parameters. Rechecking an existing call after a revision change
does not replay it.

Operation IDs include a fresh runtime identity. The headless protocol requires
that identity on every request, so old numeric set/registration IDs cannot bind
to a restarted runtime. Missing records return `outcome_unknown`. No uncertain
write is automatically retried; deduplication covers the live runtime, not
crashes or a rebuilt ledger.

A continuation declares an exact prior action in the same component. The call
supplies a known parent operation ID; the host supplies its report data,
artifacts, receipts and original context. The parent must have succeeded in the
same consumer session and original action generation. Its original context and
configuration must match the current step. A report at r0 remains stale after
an edit to r1, even when the caller resolves a fresh set at r1. After the original
task process exits, a valid component continuation starts a fresh process.

Cancellation is a request. `cancelling` stays nonterminal until the provider
exits and accepted command replies arrive. Set release and observer detachment
do not delete operation records. A cancelled process may have a successful
rename receipt; both facts are preserved. No rollback is invented.

Analysis context stays fixed. Each process job has a separate command cursor,
advanced only from that job's successful same-project receipt. A second
sequential command uses its first command's new revision, without adopting
unrelated edits from another operation. The worker remains authoritative for
revision conflicts and durable commits.

`ProcessActions::shutdown(done)` is asynchronous. Completion waits for provider
exit and accepted commands. Destroying its facade initiates cancellation;
pending command callbacks retain the required control state. Keep the NPH host,
dispatcher and service adapter alive until close completes. A dead worker must
finish pending callbacks with an explicit failure or unknown outcome, not
abandon them. Provider data cannot forge host command receipts.

## External plugin and headless example

The independent [Tool Roundtrip](../pluginsdk/common/examples/tool-roundtrip/README.md)
package inspects a bounded entry instruction window, then applies a reviewed
name to that report's exact entry. It uses real ND disassembly and the worker's
durable rename command, starts a new provider process per call and verifies the
persisted name through a fresh report.

```powershell
python pluginsdk/common/examples/tool-roundtrip/prepare.py E:/plugins/tool-roundtrip
python E:/plugins/tool-roundtrip/caller.py --host <bin>/neverd-plugin-host.exe `
  --worker <bin>/neverd-worker.exe --input E:/samples/test.exe `
  --function 0x140001000 --output E:/reports/tool-roundtrip-1
```

The example caller explicitly enables project writes. Use a disposable input
copy: project names/history are changed, while original machine bytes remain
unchanged. The plugin package is copied outside the repository and is not added
to the ND build.

To embed another caller:

```text
neverd-plugin-host tools plugin.json tools.json --output <new-directory>
  [--worker <exe> --input <binary> --function <address> --address <address>]
  [--allow-project-writes]
```

The first stdout line is `event:ready` with `runtime` and `context`.
Send JSON lines on stdin, each with `id`, that `runtime`, and `op`:

| op | Other request fields | Result |
|---|---|---|
| resolve | session, step, optional user, profile | set, tools[], context |
| plugins | none | installed declarations, desired state, activation and diagnostics |
| call | set, registration, call_id, optional parameters, parent_operation | operation snapshot in value |
| inspect | operation | retained snapshot, or unknown |
| cancel | operation | cancellation requested |
| release | set | set invalidated; existing operations retained |

Notifications use `event:operation` and `value`; replies preserve request `id`.
The sample Consumer distinguishes them. Ctrl+C or stdin EOF stops new input and
cancels providers while draining accepted commands. Output directories must be
new. Operation directories contain verified artifacts and atomic receipt.json.

Providers still use framed protocol v1. The start message advertises optional
`structured_data` and `commands`. Only the new process-action adapter accepts
result `data` or declared `command` messages; replies use `command_reply`.
The common Python helper returns the whole service receipt. The ordinary GUI
path does not implicitly acquire write capabilities.

## Verification

Use the [NPH standalone profile](plugin-host.md#build-and-verification):

```text
python scripts/build.py build-nph --target neverd-plugin-host neverd-tools-tests neverd-tools-process-tests --parallel 4
ctest --test-dir build-nph --build-config Release -R "^(NeverDTools\.|NeverDPluginHost\.|NeverDPluginCLI$)" --output-on-failure --parallel 4
python tools/neverd-plugin-host/tests/tools_worker_test.py <host> <worker> <engine-library>
```

Core tests cover configuration, schemas, activation failure, replacement,
cross-project/host isolation, temporary tools, current policy, stale reports,
deduplication and observer reentrancy. Real provider tests hold command replies
across facade destruction and validate sequential commit cursors and conflicts.
`NeverDToolsWorker` uses the real engine, copied external plugin, actual rename
receipts and reopen/readback; CTest registers it when a real worker is available.
Existing process/GUI SDK suites cover compatibility. Windows is verified;
Linux/macOS paths still need platform qualification.
