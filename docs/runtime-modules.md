# Explicit binary modules and debug information

`NeverDModules` owns a format-independent catalogue of caller-supplied binary
and debug files. Official system files and privately built modules use the
same interface. The C++ resource layer, generic instance/mapping transactions,
validated low-level resource stops, persistent workload/stage control and finite
guest invocation are implemented. Module resource C ABI/CLI access is also
implemented. Persistent execution C ABI exposes explicit raw CPU buffers and the
Windows driver environment, with a persistent JSON-lines CLI using that same
public boundary. The Windows driver adapter publishes explicit Native x64 guest
modules through the common transaction. Supported CPU resource stops retain the
driver call/phase while declared lazy modules or caller-supplied missing modules
are mapped. Additional Native x64 guest bindings support exact imports and
bounded export forwarding. Primary-driver/reference instance adoption is also
implemented. Primary private-provider IAT linking uses the same stopped module
transaction. Additional active CFG and C SEH/unwind metadata publish with that
transaction. The driver-lab replay now consumes the public SDK through
[`run_module_driver_replay.py`](../scripts/labs/run_module_driver_replay.py).
Ring3 execution integration is reserved in the design.

The existing Windows driver path uses a private `DriverSessionState` resource
owner, with initialization separated from guest execution and stable lifetimes
for its models, inputs, budget, CPU view and resident frame data. Its initial CPU
transfers once into the shared `ExecutionSession` for hook installation,
instruction accounting, execution and recoverable faults. The existing driver
pump and OS policy remain authoritative. `DriverExecutionEnvironment` now retains
that pump and its phase/callback/exception call tree in private suspended tasks,
yielding CPU actions to `ExecutionRun`. The internal adapter stops after successful
entry finalization and before the first callback drain. The one-shot driver API
uses this adapter for explicitly selected native version, kernel reference,
guest paging, resident-callback draining and lifecycle-event inputs. Ordinary
one-shot execution, observation and direct execution retain their existing
entry path and share the same kernel model. Finite driver calls adopt a generic Win64
frame on a separate stack, use that same pump and validate return ownership,
stack balance and IRQL. Both public factories are described below. Additional
module imports use existing exact model bindings or explicitly bound guest
providers. The public replay runs the original entry, stops at `after-entry`
and makes caller-specified finite calls without rewriting production sources.

## Declare and prepare inputs

```json
{
  "base_directory": "D:/analysis-inputs",
  "module_defaults": {"load": "eager"},
  "modules": [
    {
      "id": "kernel",
      "image": "ntoskrnl.exe",
      "debug": {"kind": "pdb", "path": "ntkrnlmp.pdb"}
    },
    {
      "id": "support",
      "image": "support.sys",
      "load": "lazy"
    }
  ]
}
```

See the [configuration schema](../resources/modules/schema/modules.schema.json).
An explicit absolute base directory resolves relative paths. Absolute input
paths need no base. Registration performs no file access and permits files
that will be supplied later. Unknown fields, duplicate caller IDs, invalid
policies and excessive declarations fail before reading files.

```cpp
#include "neverd/modules/ModuleSet.h"

auto Modules = neverd::modules::ModuleSet::fromJSON(Configuration);
if (!Modules)
  return Modules.takeError();
if (auto E = (*Modules)->prepareEager())
  return E;

// A consumer explicitly asks for a lazy resource when it needs it.
auto Support = (*Modules)->prepare("support");
if (!Support)
  return Support.takeError();
```

`prepareEager()` prepares all eager declarations as one catalogue update. If
any input fails, none of that batch is published. A module's `load` overrides
the default; the default is `eager`. `lookupSymbol(id, name)` prepares that
module on demand. These operations prepare file resources; they do not map a
guest address space or execute an initialization entry point.

`reviseUnprepared(id, image, optionalDebug)` corrects a declaration whose input
has never published an observation. It replaces the image/debug declaration,
retains its ID and load policy, validates paths without file IO, and increments
the catalogue revision only on success. This permits retrying an omitted file
or incorrect companion while an execution retains its resource request.
Prepared inputs stay fixed; `attachDebug` remains the operation for adding
debug information to an existing observation.

`moduleOptionsFromFile(path)` reads an explicitly named, bounded regular JSON
configuration file. An explicit JSON `base_directory` selects its input origin;
otherwise relative module/debug paths use that file's absolute parent directory.
It does not open binary/debug inputs. Direct `moduleOptionsFromJSON` calls keep
their explicit-base conflict check; a file origin is a fallback rather than a
second caller assertion.

## C SDK and module CLI

Include [`NeverDCAPIModules.h`](../include/neverd/sdk/NeverDCAPIModules.h) or the
umbrella C header. `neverd_module_set_t` is a process-local opaque 64-bit identity,
with zero invalid and no reuse during the library's lifetime. It does not require
a decompiler session or CPU/driver emulation. All resource operations consume
the same `ModuleSet` implementation described above.

`neverd_module_set_create_v1` accepts an exact-size versioned structure containing
configuration JSON and an optional explicit absolute base. NULL options create
an empty catalogue. `neverd_module_set_from_file` is the named-configuration
convenience operation with file-origin handling. Create/register/inspect do not
prepare binary/debug files, including eager entries.

The resource operations are `add_json`, `revise_unprepared_json`, `prepare`,
`prepare_eager`, `attach_debug_json`, `inspect_json`, `lookup_symbol_json` and
`destroy`, all prefixed with `neverd_module_set_`. Add/revise accept a single
module object. Revision must omit `load` and retains the original policy; debug
attachment accepts a `kind`/`path` object. Lookup returns static original-image
addresses and a successful `found=false` result when no symbol matches.

Each operation clears its optional `const char **Error` output. Failure returns
zero/NULL and an owned UTF-8 diagnostic; JSON results are also owned. Free both
with `neverd_free_string`. Diagnostics remain within 64 KiB without splitting a
UTF-8 character, and JSON output stays within 64 MiB. Input/error-output storage
must be separate and borrowed inputs must remain readable during the call.
Concurrent operations on one source catalogue report busy rather than racing
mutable parser/query caches. Destroying a handle invalidates future lookups;
an operation that already acquired its resources can finish safely, and copied
reports remain valid after destruction.

```c
#include "neverd/sdk/NeverDCAPI.h"
#include <stdio.h>

int validate_modules(const char *configuration_path) {
  const char *error = NULL;
  neverd_module_set_t set = neverd_module_set_from_file(configuration_path, &error);
  if (!set) {
    fprintf(stderr, "%s\n", error ? error : "module configuration failed");
    neverd_free_string(error);
    return 1;
  }
  int ok = neverd_module_set_prepare_eager(set, &error);
  if (ok) {
    const char *report = neverd_module_set_inspect_json(set, &error);
    ok = report != NULL;
    if (report) {
      puts(report);
      neverd_free_string(report);
    }
  }
  if (error) {
    fprintf(stderr, "%s\n", error);
    neverd_free_string(error);
  }
  neverd_module_set_destroy(set, NULL);
  return ok ? 0 : 1;
}
```

The CLI calls these C functions, including configuration-file reading; it owns
no additional loader, identity or path-resolution policy:

```sh
neverd modules inspect /path/to/modules.json
neverd modules validate /path/to/modules.json
neverd modules lookup /path/to/modules.json --module=kernel --symbol=KdDebuggerDataBlock
```

`inspect` reports declarations without preparing inputs. `validate` prepares
eager inputs and leaves lazy entries unprepared. `lookup` prepares only the
queried source. Exit codes are 0 for success/found, 1 for invalid configuration or
operation failure, and 2 for a successful symbol query with no match. JSON goes
to stdout and diagnostics to stderr. The Python plugin ABI table also declares
the fixed-width handle, exact v1 layout, owned results and owned-error pointers;
the existing `HostAPI` binding can call them without a decompiler session.

## File identity and lifetime

Preparation reads bounded source bytes and retains private, read-only copies
for the existing path-based binary and debug loaders. This provides one input
snapshot for parsing, deferred symbol reads and future execution consumers
without creating another format parser. Replacing the caller's source files
does not change an already prepared observation. The private files are removed
when their last observation is released, including after the catalogue dies.
`preparedImagePath()` and `preparedDebugPath()` are borrowed, read-only paths
whose lifetime is the owning `ModuleImage`; consumers must not modify them.

`ModuleImage` exposes original bytes, the loader's immutable `BinaryImage`,
source SHA256 identities and the debug provider. It owns no guest base, process,
session, CPU or OS objects. `ModuleInstance` supplies execution-specific bindings.
Current operations require caller serialization, as do the underlying debug
provider caches. No host libraries are executed and no files are downloaded.

## Explicit debug files

`debug.kind` selects `pdb`, `dwarf` or `map`. No debug declaration means image
only: the catalogue does not search recorded build paths or neighboring files.
A declared companion is authoritative and an error never falls back to another
format. PDB authentication uses the shared PE/PDB identity loader, including its
linked DBI-age policy. Explicit split DWARF retains companion trust; MAP files
provide names without authenticated ABI or object-layout claims.

`attachDebug(id, input)` validates a new companion and publishes a new observation
sharing the original binary snapshot. Existing holders retain their previous
observation. A failed attachment changes neither the declaration nor its
prepared image, byte accounting or revision. Replacing an already declared
debug input is unsupported.

`lookupSymbol()` returns an original image address and symbol kind, not an
implicitly chosen guest address. Equal-name observations at different addresses
or with incompatible kinds fail as ambiguous. Symbol names and debug presence
do not authorize native execution or make runtime global values known.

## Inspection and bounds

`inspect()` and `moduleSetJSON()` never materialize lazy inputs. Reports include
resolved source paths, effective load policy, prepared status, source bytes,
hashes, format/architecture, original base, image CodeView identity when present,
and the debug provider's authenticated-semantic capabilities. `debug_loaded`
means the selected provider loaded successfully; `debug_identity_verified`
currently applies to matched PDB input. A parsed MAP or unauthenticated split
DWARF companion does not acquire image-identity verification.

Defaults: 256 modules, 256 MiB per image, 512 MiB per debug file, 1 GiB of retained
source bytes, and 1,000,000 symbol observations per query. Optional `limits`
fields can select finite bounds up to the schema ceilings. Source-byte accounting
covers retained file inputs; it is not a bound on all loader caches or guest RAM.
Configuration JSON is limited to 2 MiB; paths are limited to 4096 UTF-8 bytes.

## Execution module instances

`NeverDEmulationModules` provides `ModuleBindings`, `ModuleBinding`,
`ModuleInstance` and the shared resource request/resolver path. This component
depends on Modules, EmulationImage and EmulationRuntime; it has no Windows
dependency. The low-level CPU/runtime components do not depend on module parsing.

A binding names one resource, an instance ID, guest name, scope, address space
and optional base/extent. Declaration reads no file and is visible in `inspect()`
for eager and lazy instances alike. Guest names are exact identities within a
scope/address space; an OS adapter owns case rules and aliases. IDs are unique
within one bindings owner. The same source may have separate instances with
independent RAM at different bases or in different address spaces.

`materialize(batch, builder)` prepares all source observations and mapping plans
before publishing RAM, instance state and namespaced exports together.
`prepareEager(space, builder)` selects eager declarations in that address space
and uses the same transaction path. Lazy instances remain declarations until
explicitly requested and supplied. There is no default OS loader: the required
builder owns relocation, imports/forwarders, privilege, header visibility and
runtime-data knowledge. It must return prepared bytes and final permissions
without modifying the CPU, live memory or OS tables. This boundary does not
grant unsupported native code or disk-initialized kernel globals execution or
runtime-value authority.

An environment may `adopt(batch, validator)` to publish pinned observations over
mappings it already owns. The adapter must provide a pure proof of the existing
source/mapping identities and export ownership. The common layer checks extents,
conflicts, limits and export names, kinds and ordinals. Preexisting external
export addresses may remain opaque metadata; adoption grants no RAM backing or
access permissions. Guest materialization still requires backed forwarder
targets. Adoption requires fresh IDs, one
stopped address space and no pending resource request. It permits a pinned source
whose original catalogue or files no longer exist. Environment metadata patches,
if any, join the same RAM transaction. Without patches, a metadata publication
lease changes no RAM, mapping generation, physical capacity or reservation
witnesses. `prepareMappings` still rejects an empty batch. This internal adapter
entry is not a caller-supplied binding flag or an initialization request.

`lookupSymbol(instance, name)` projects the instance's pinned source symbol into
its guest extent. `lookupExport` selects a name or ordinal within that instance.
An export address is metadata and can denote opaque, inaccessible storage;
successful lookup supplies no read/execute permission. Missing instance resources
do not trigger implicit mapping from these query methods. The source observation
is pinned at materialization; later debug attachments preserve old observations
and do not silently replace an instance's execution inputs.

## Resource requests and publication

`requestModule`, `requestDependency` and `requestAddress` produce one pending
request per bindings owner. Address requests retain PC, address, size and access
kind. Only a declared, unmapped range covering the access supplies a module ID;
an unknown address remains an `UnmappedCandidate` without a guessed file name.
Fully mapped memory, including protection failures, cannot be reclassified as a
missing module. Requests record a question; they do not prove a faulted CPU can
resume.

`supply(token, batch, builder)` verifies owner/version/sequence before file IO.
The batch must satisfy the instance or exact dependency identity and target
address space. Address supply checks the whole access across new ranges and
existing pages, including final permissions and holes. OS dependency aliases
must be canonicalized before requesting. Failed supply retains the pending
request and original guest/module state; successfully cached file resources may
remain prepared. Successful supply consumes the token once. Cancellation also
invalidates it. Existing mapped instances cannot be replaced or unloaded.

`resolveModuleRequest` invokes a resolver once while stopped, outside CPU/model
callbacks. It may register omitted resources in the shared `ModuleSet` and return
bindings, request human input or report no match. Found bindings use the same
`supply` path. Resolver recursion and mutation of its bindings owner are rejected.
No callback starts execution, downloads files or searches host directories
implicitly. The execution owner must provide the stopped boundary. Low-level
resource pauses are available through `ExecutionSession` and `ExecutionRun`;
Windows adapter integration remains under development.

An optional `ModulePublicationPreparer` prepares environment metadata from the
candidate instance table. Its `ModulePublication` may expose guest RAM patches
for module lists/IAT-like fields and commits host metadata with a nonthrowing
swap. Preparation must have no visible effects. Its commit cannot allocate,
fail, invoke other callbacks, execute CPUs or mutate memory directly.

`AddressSpace::prepareMappings` owns the common physical reservation and
publication boundary. The transaction holds the physical owner's stopped lease,
copies initial bytes and backing patches, rejects overlaps (including physical
patch aliases), and publishes final rights without a temporary writable mapping.
Abandonment releases reservations while preserving existing bytes, aliases,
mapping generation and reservation witnesses. Committed patches retain physical
identity and invalidate affected reservation witnesses. CPU execution/mutation
is rejected until RAM and consumer metadata publication finish and the transaction
is released. Transactions must remain on their creating thread. Backend
projections synchronize through the existing mapping-generation check before
execution; a projection error remains terminal and does not claim guest rollback.
One batch targets one address space, without a cross-space atomicity guarantee.

`refreshPublication(space, builder)` runs the adapter publication over existing
instances without requiring a new binding. A typed request for a declared,
unmapped dependency may expand the transaction through the same builder and
finalizer as materialization. Empty candidate batches skip finalization. Guest
patches and instance/adapter metadata commit together; a refresh without guest
patches uses the metadata lease. Failed source/link/patch preparation publishes
nothing. Refresh rejects a pending request; that request must use `supply`.

An optional `ModuleMappingFinalizer` links the complete stopped batch after its
byte layouts have been validated. It sees the previous instance table and
`ModuleMappingCandidate` references, and may change only draft bytes/exports.
Adapters can return immutable `ModuleMappingPreparation` input from a builder;
the finalizer must consume it on success. Those references never become live
instance state and must not escape the callback. A finalizer may also publish immutable
`ModuleRuntimeData` for adapter-owned facts tied to its final bytes. The common
instance table retains those facts but does not interpret them or grant runtime
capabilities from their presence.

`ModuleDependencyError` identifies a missing provider and, optionally, an already
declared instance. A known declaration in the same address space expands the
same atomic batch; the failed drafts are discarded before retry. Unknown inputs
remain explicit resource questions. Batch instance/mapped-byte limits and final
layout/export validation still apply. `ModuleDependencyMatcher` delegates an
environment's provider aliases; omission retains exact generic identities.

`ModuleExport::Forwarded` distinguishes an environment-resolved external target
from an ordinary direct export. It requires finalization and an appropriately
permitted planned/existing RAM target, while direct exports keep their original
image-bound rule. Metadata cannot grant guest permissions, model behavior or a
function ABI. The Windows adapter owns PE forwarding policy; the common layer
does not parse PE identities or search for files.

## Validated CPU resource stops

`BackendHooks::ResourcePause` is an optional, pure admission policy. It receives
a provisional candidate within the backend's supported restart shapes and must
not perform file IO, resolve modules, mutate/execute CPUs or dispatch models.
Only a subsequent typed `ExecutionExitKind::ResourcePause` publishes the final
backend validation. Existing callers that omit the policy retain their current
fault behavior. An untyped `CPU.run()` reports a resource stop as an error rather
than implying successful completion.

`ResourcePause` records the fault/access, actual `ResumePC`, originating observed
instruction, restart kind, whether committed memory effects may exist, and the
complete required permission mask. Checked execution uses the architecture's
existing fault boundary, including instruction-prefix stores and completed REP
iterations. It does not abandon those effects through an OS exception transfer.
Direct/native opaque execution does not enable this resource policy yet.

The software Unicorn x64 adapter admits instruction-fetch boundaries, scalar
integer MOV/MOVZX/MOVSX/MOVSXD memory loads, and exact `REP MOVSB` byte iterations.
After Unicorn returns, it verifies the expected status and actual PC before
publishing a resource exit. Other software operand shapes, protection failures,
MMIO/model failures and terminal faults retain their existing behavior. Software
ARM64 rejects resource hooks; checked ARM64 uses its architectural preflight.
The adapter never clears `FirstFault` to resume a terminal CPU.

While a resource pause is pending, CPU mutation and snapshots are rejected.
The owner repairs memory through the address-space/module transaction while
stopped. Module address requests can carry `RequiredPermissions` from the CPU
event; supply must satisfy every bit, including privilege or combined operands.
`ExecutionSession::acknowledgeResourcePause()` checks that the access is now
permitted and retains a mandatory continuation PC. The next run must use the
recorded `ResumePC`; acknowledgement alone does not allow a context replacement
or a different PC. This preserves an already pushed CALL return address and
completed REP progress. An existing watch step-over survives repair, while a
new target watch still stops before its instruction.

`ExecutionEnvironment::prepareExecution` runs while stopped after eager
preparation and before an ordinary CPU action. It can finish adapter metadata or
return a typed module dependency. The run retains the original action and uses
the same quota/request/supply path. Repaired CPU continuations skip this hook.
Time exhausted during preparation stays terminal, including when the hook
returns another error; it cannot create a later resource event or CPU/model
exit callback after the exhausted admission.

`SessionModelHooks` adds a separate OS admission boundary. `Stop` yields an
uncharged `model_boundary`; `Emulate` charges one attempt before yielding, with
no machine effects. `Defer` yields `deferred_instruction` for stopped validation
of an input-dependent model operation. The session retains that exact attempt
until `admitDeferredInstruction()` accepts it once, checking PC, shared credits
and the original run's local deadline. Pending run/context/watch operations are
rejected; quota failure retains the pending attempt. Session watches and call
returns have priority, and CPU faults retain their typed outcome. This is model
admission, without a missing-module request or permission to repair a terminal
fault. The existing Windows driver uses it for thunk/CFG/return boundaries and
modeled processor reads within its retained phase and invocation pump.

`ExecutionEnvironment::prepareCPUEntry` refreshes OS projections of committed
RAM immediately before every CPU entry, after eager mapping and ordinary
dependency preparation. Repaired CPU continuations also use this hook without
repeating `prepareExecution` or the retained scene action. It cannot execute the
CPU, change registers/context or acknowledge the retained stop. Projection
failure is terminal `environment_failure`; it cannot request another resource
or undo already committed modules. Shared time/instruction admission is checked
after success and error, and a cooperative pause arriving during projection is
observed before execution. Inspection and memory snapshots do not run the hook.

## Driver report views and primary import policy

The `windows-driver` environment accepts an optional `report` object with
`call_tail` and `write_tail`, each a positive count up to 1,000,000. Only the
serialized observation is bounded. Runtime records, guest effects and budgets
remain unchanged. Its `environment_report.trace_view` contains `total_calls`,
`first_call_index`, `total_writes` and `first_write_index`, identifying exactly
which suffix was returned. Omitting `report` keeps the complete legacy report;
omitting one tail field keeps that array complete. Use bounded views for large
workloads without treating unreturned records as absent execution.

`primary_import_policy` defaults to `modules`: primary imports without an
existing model binding use the shared module/dependency transaction. Explicit
`model-traps` retains the one-shot primary import contract, binding named
imports to existing models or lazy unsupported-API traps. An unmodeled routine
still has no callable ABI or invented return behavior. This compatibility
selection retains strict ordinal/primary-image admission and does not change
additional modules' shared linker. The report records the selected policy;
`capabilities.primary_module_imports` indicates whether the primary uses module
linking. Both choices use the same execution engine.

The [public SDK replay runner](../scripts/labs/README.md) consumes explicit
module/scenario files, pauses the original driver at `after-entry`, and performs
bounded buffer/capacity calls without source rewriting or a private executor.
It records actual outputs, API trace completeness, hashes and published owners.
See the [configuration examples](../resources/modules/examples/README.md).

## Time policy across pauses

`ExecutionBudget::create()` retains the original absolute-deadline policy.
`createWithTimePolicy(..., ExecutionTimePolicy::ActiveWork)` selects cumulative
active work. `enterWork()` returns an owned scope; nested scopes charge their
union once and stopped intervals between scopes do not consume active time.
Instruction/event credits and fractional clock time persist across resumptions.
Old clock samples cannot replenish an observed active interval. Scopes borrow
their budget, which must outlive them.

`ExecutionSession::run()` owns one work scope automatically. `ExecutionRun` wraps
initialization, its execution loop, module supply and resolver calls in outer
scopes, so resource/model work shares the cumulative budget. It checks time
after environment/resolver callbacks return, including callback errors. This
does not promise a hard wall-clock bound on arbitrary host callbacks.

## Persistent execution C API

[`NeverDCAPIExecution.h`](../include/neverd/sdk/NeverDCAPIExecution.h) provides
15 execution exports independently of analysis sessions. The exact-size
`neverd_execution_options_v1` contains `struct_size`, a valid `module_set` handle
and required `configuration_json`. An execution retains the same source state
after the caller destroys its public module-set handle. Execution handles are
typed, non-reused uint64 identities; passing a module handle as an execution
handle, or using a released handle, fails explicitly.

The configuration has an explicit `environment` object, optional `run` controls
and optional `bindings`. The `buffer` kind accepts explicitly supplied CPU
instruction/data bytes without an OS loader; `windows-driver` selects the
retained WDM driver adapter described below. Other kinds fail with an
unsupported-environment error; they never select a fallback. No Ring3 instance
is introduced by either factory.

```json
{
  "environment": {
    "kind": "buffer",
    "cpu": {"backend": "unicorn", "architecture": "x86_64"},
    "memory_limit": 1048576,
    "entry": "0x10000",
    "stop_pc": "0x10001",
    "stack_pointer": "0x80f00",
    "regions": [
      {"address": "0x10000", "size": 4096, "permissions": "rx", "bytes_hex": "9090"},
      {"address": "0x11000", "size": 4096, "permissions": "rx", "bytes_hex": "4889c84801d0c3"},
      {"address": "0x80000", "size": 4096, "permissions": "rw"},
      {"address": "0xa0000", "size": 4096, "permissions": "rw"}
    ],
    "invocation": {"stack_base": "0xa0000", "stack_size": 4096, "return_pc": "0x10001"}
  },
  "run": {"pause_stages": ["stopped"]}
}
```

Regions must be nonempty, page aligned, disjoint RAM ranges within the explicit
memory limit. Their `permissions` contain unique `r/w/x/u` letters; `bytes_hex`
initializes a bounded prefix and the rest is zero filled. `cpu` uses the existing
execution-configuration parser and capability checks. Creation maps the private
initial RAM but executes no instructions and prepares no module files. Reaching
the executable `stop_pc` yields the `stopped` stage if selected, then completion
after explicit resume. An unhandled fault/transport stop does not count as reaching
that stage. A finite-call plan is optional and has `stack_base`, `stack_size` and
`return_pc`; its stack must be a dedicated writable, nonexecutable region outside
the suspended caller. The existing invocation engine still performs frame checks.

| Operations | Contract |
|---|---|
| `create_v1`, `destroy` | Retain/release one workload; zero destroy is a no-op, stale or wrong-kind identities fail |
| `run_json` | Drive the same `ExecutionRun` to its next event; a failed guest is a valid report, API/precondition failure is NULL |
| `state_json`, `request_pause` | May be called concurrently with run; state reads the atomic lifecycle and pause is cooperative |
| `resume` | Paused to Ready, without execution or quota replenishment |
| `inspect_json`, `instances_json` | Stopped snapshots of cumulative state and actual instance publication |
| `declare_binding_json`, `supply_json` | Declare one instance or supply the retained token through the existing transaction path |
| `invoke_json` | Install a bounded explicit-ABI call; later run events carry its result |
| `read_memory`, `write_memory` | Caller-owned bounded buffers; reads copy on success only, terminal runs reject writes |
| `lookup_symbol_json` | Query a mapped instance and return its bound guest address; found=false is success |
| `command_json` | Validate one command envelope and dispatch through these same primitives and the retained source catalogue |

All operation names above have the `neverd_execution_` prefix. Inputs are
borrowed during a call. Errors and JSON results are owned UTF-8 strings released
with `neverd_free_string`; optional error outputs are cleared on entry. The same
input and report bounds described below apply. Other concurrent operations,
including destroy during a call, return busy. Execution operations coordinate
with the source catalogue guard: another run or source operation cannot race
source preparation/query caches. Stopped users may add a missing source through
the module API, then supply bindings and explicitly run again. Source registration
alone changes no guest instance and resumes no CPU. `state_json`/`request_pause`
do not acquire the busy guard.

The buffer mapper only accepts fixed link-address images of the selected ISA,
without imports, relocations or rebase requests. It maps original file bytes
through `ImageMappingPlan` and the common module transaction, preserving segment
permissions; a nonzero entry in nonexecutable RAM is rejected. Windows module
relocation/import/model policy belongs to the Windows driver adapter described
below.
Mapped symbols do not imply modeled API behavior. Python `HostAPI` has matching
ctypes handles, structure, owned-string and byte-buffer signatures.

The exports remain linkable with CPU emulation disabled and report that execution
is disabled. File-resource registration, preparation and symbol queries remain
available in that build. An isolated test target compiles the production
boundary/allocator without any emulation archive; the separate
`NeverDExecutionSDKDisabledTests` links the real feature-disabled SDK and
exercises every execution export, source lifetime and the public allocator.
The disabled CLI tests check buffer and driver startup. See the complete
[feature-disabled build instructions](testing.md#explicit-module-inputs).

## Persistent Windows driver environment

Select `windows-driver` through the same `neverd_execution_create_v1` call or
`neverd execution` CLI. C++ consumers include
[`DriverExecutionFactory.h`](../include/neverd/emulation/DriverExecutionFactory.h)
and call `createDriverExecution`. This factory uses the same retained environment
as the existing one-shot driver API, rather than running a second driver engine.

```json
{
  "environment": {
    "kind": "windows-driver",
    "image": "example.sys",
    "base_directory": "D:/analysis-inputs",
    "backend": "unicorn",
    "contract": "checked-x64-v1",
    "memory_limit": 67108864,
    "scenario": {"service_name": "Example"}
  },
  "run": {
    "pause_stages": ["after-entry"],
    "limits": {
      "instructions": 100000000,
      "events": 10000,
      "timeout_microseconds": 10000000
    }
  }
}
```

Specify exactly one of `image` or `image_module`. The path form prepares and pins an
explicit file through the common resource layer; `image_module` selects a
resource ID from the supplied `ModuleSet`. Both consume immutable bytes and the
same strict driver preflight.
`base_directory` is an explicit absolute origin for relative host files;
otherwise the retained module-set origin is used. Absolute files
need no origin. Neither cwd nor host-system directories are searched implicitly.
The same path admission resolves the optional scenario's `kernel_image`,
`kernel_pdb` and `service_images[].source`; guest image/registry/filesystem paths
remain guest identities. The scenario uses the existing strict driver parser.
Backend, kernel execution contract, memory capacity and common run controls are
selected outside it. `driver-strict` and `checked-x64-v1` are the supported driver
contracts; a user-mode contract does not select an application adapter.

For a driver already declared in the source catalogue, use the ID form. The
optional `kernel_module` selects the source observation used by the existing
bounded kernel-reference model. For example, a module configuration can declare:

```json
{
  "base_directory": "D:/analysis-inputs",
  "modules": [
    {"id": "driver-main", "image": "example.sys"},
    {"id": "kernel-reference", "image": "ntoskrnl.exe",
     "debug": {"kind": "pdb", "path": "ntkrnlmp.pdb"}}
  ]
}
```

Its execution configuration then selects those resources:

```json
{
  "environment": {
    "kind": "windows-driver",
    "image_module": "driver-main",
    "kernel_module": "kernel-reference",
    "backend": "unicorn",
    "contract": "checked-x64-v1",
    "scenario": {"windows_version": {"major": 10, "minor": 0, "build": 19041}}
  },
  "run": {"pause_stages": ["after-entry"]}
}
```

These are explicit bootstrap roles: constructing this environment is their
first use, so creation prepares those selected inputs, including a configured
PDB. It does not prepare unrelated catalogue entries. A source's eager/lazy
declaration remains recorded; selected bootstrap roles are prepared at creation,
while additional guest bindings follow the eager/first-use rules below. Supplying
both `image` and `image_module`, or combining
`kernel_module` with the scenario's `kernel_image`/`kernel_pdb`, fails explicitly.

The driver byte view and strict execution preflight share the same prepared
`BinaryImage`; no second read of the original driver path occurs for ID inputs.
Kernel reference facts use the same pinned PE/debug observation. The PDB provider
owns matching, while the existing kernel adapter continues to prove its bounded
layout and original-code shapes. Names-only RSDS identity does not grant general
function signatures or object extents; MAP debug names cannot supply these
kernel layout facts. Removing or replacing the original files after preparation
does not change the observation used by an execution. Later debug attachment
publishes a new source observation and does not rewrite an existing run's facts.

Creation publishes the already mapped primary driver as instance `$driver`, and
an optional reference as `$kernel-reference`. These instance IDs are reserved by
the adapter, count toward `instance_limit`, and cannot be redeclared or replaced.
Their reports set `environment_owned: true` and `role: "driver"` or
`"kernel-reference"`. Their `module_id` preserves the selected source ID; legacy
file inputs use local observation labels `@driver-image` and
`@kernel-reference`, without adding hidden entries to the caller's catalogue.
The `input_modules` environment report continues to list explicit source-ID
selections; all bootstrap instance reports include their pinned hashes.

Bootstrap adoption reuses the existing guest module-query row and main driver
loader record. It creates no duplicate row, link, allocation or initializer
execution. The reference retains its sparse mappings and guarded runtime data.
Its named export and ordinal metadata use the canonical registry's selected
addresses; explicitly absent names stay absent. PDB lookup through
`neverd_execution_lookup_symbol_json` with the instance ID projects into its
actual guest base. A source symbol is metadata and may still refer to opaque
storage. The reference's `entry` is zero because it has no initialization
request.

The persistent driver adapter links unbound external-provider imports, including
code, data and validated ordinal imports, before its first CPU action. Explicit
instance bindings provide the module identities; it does not infer a file from
the source ID or search the host filesystem. A required lazy provider's first use
is this load-time link, even with CPU resource pauses disabled. Missing providers
produce the common dependency request, with no main-driver instruction executed.
Callers can add/revise a source and supply its binding, then run the retained
action. Failed supply retains its token and publishes no partial IAT or module.
A provider's own dependencies may require supplying a complete batch.

`DriverImportPublication` stages primary IAT writes with guest loader records
and host instance state in the same RAM transaction. IAT permissions remain
unchanged. Provider initialization is still explicit and is never inferred from
successful linking. Environment snapshots report `primary_imports_linked`;
`capabilities.primary_module_imports` records the selected primary linking policy.

Preexisting registry bindings and explicit absence stay authoritative. Named
kernel APIs without a declared model retain their unsupported API traps. A
source file cannot replace a model's ABI or OS semantics. For a mapped provider's
named modeled exports, name and corresponding ordinal lookup share the same
registry target. Published exports and primary/extra import resolution use that
same rule, including forwarding. Source/PDB symbol lookup still projects source
addresses and grants no model or execution authority. The legacy one-shot driver
API retains its previous unknown-import/strict ordinal policy while using the
same persistent executor.

Creation maps/initializes this one driver but executes no main-driver instructions.
The selected `after-entry` stop occurs after a zero-status `DriverEntry` return
and `finishEntry`, before the first callback drain. Omission keeps the normal
entry/request/unload flow. A finite call requires this selected stage, an
executable target inside the current driver or a published guest module, explicit `win64`, and the
stopped foreground thread at PASSIVE_LEVEL without an active guest callback,
user attachment or power-managed callback. Unsupported targets/ABIs fail before
call effects.

Use `neverd_execution_lookup_symbol_json` with `$driver` to obtain the bound
address from its pinned matching PDB. Alternatively, use
`neverd_module_set_lookup_symbol_json` to obtain an original-image function
address, then translate explicitly as
`runtime_base + (symbol_address - preferred_base)`. The stopped environment
report supplies both bases. PDB symbols select addresses; they do not infer
function signatures, argument types or return meaning. An absolute runtime
address works without a PDB when the caller already knows it.

The invocation installs its ABI/payload once on a separate 1 MiB writable stack
with its own reserved return boundary. That storage is reserved when the
after-entry stage is selected. Calls use the existing API, callback, exception,
thread and scheduler owners. A validated return captures outputs, retires the
call stack's model lifetime, restores the caller's CPU/control and model selector,
and pauses at `after-entry` again. RAM, globals, OS side effects, queues, virtual
clock and cumulative budgets persist. The retired entry stack is not revived.
IRQL/ownership violations, an unbalanced stack, terminal memory faults or limits
terminate the workload. Another `invoke` repeats a call; `resume` continues the
original driver lifecycle.

The run's exact microsecond time limit remains authoritative. The legacy driver
configuration report expresses a rounded-up millisecond envelope; it neither
extends nor replaces the shared deadline. A call may consume its final
instruction and still reach an uncharged modeled API or reserved return point.
Attempting an additional machine instruction exhausts that local allowance.

Stopped `inspect_json` and CLI `inspect` append `environment_report` with actual
driver observations, API calls, entry status, addresses and capabilities.
Its `input_modules` lists selected source IDs, roles and the exact pinned
image/debug digests. These input observations are distinct from the generic
runtime instance table and do not imply publication of arbitrary kernel modules.
Inspection snapshots model output without draining callbacks or running a CPU.
Before lifecycle completion, `observation_only` is true and the legacy
`stop_reason`/`scenario_success` claims are omitted; the outer execution event
owns lifecycle and failure status. A failed entry status can be a completed
observation without successful driver initialization.

Additional guest modules use the same source catalogue and explicit bindings.
For example, declare a caller-provided Native x64 `helper.sys`, with its matching
PDB when available:

```json
{
  "base_directory": "D:/analysis-inputs",
  "modules": [
    {"id": "support", "image": "helper.sys", "load": "eager",
     "debug": {"kind": "pdb", "path": "helper.pdb"}}
  ]
}
```

Add this top-level `bindings` array to the execution configuration:

```json
{
  "bindings": [
    {"id": "helper-instance", "module_id": "support", "guest_name": "helper.sys",
     "scope": "kernel", "address_space": "kernel", "base": "0x1B0000000"}
  ]
}
```

An eager binding is prepared before the next CPU action, including an eager
binding explicitly declared at a later stopped boundary. Registering a source
alone does not map it. The mapper consumes pinned original bytes, validates the
Native x64 PE profile, applies supported relocations and links the complete batch
before publication. Imports use existing exact model identities or explicitly
bound guest providers, including named, ordinal and data imports. Export names and ordinals use the
canonical PE export reader. Guest aliases, overlapping image extents and reserved
model/call-stack ranges are rejected. A missing eager source can be corrected
through its retained declared-module request before any CPU action; failed supply
preserves that token for retry.

The same stopped transaction commits image pages, initialized bytes, generic
instances/exports and prepared OS metadata. The kernel model stages arena
allocations, read-only names and loader-list links without changing live RAM.
It validates the current ring, including service-owned records, before appending.
Failed preparation or physical conflicts abandon both RAM and metadata; guest
link corruption is reported rather than repaired. `ZwQuerySystemInformation(11)`
and guest loader-ring reads observe the same newly published records and each
record's own flags. A mapped instance pins its source/debug observation.
The stopped `lookup` command returns a runtime symbol address for `invoke`.

Publication installs the PE's initial file/zero-filled state. It does not execute
an additional module's entry point or manufacture boot-initialized globals.
Additional Native modules register immutable x64 unwind/C SEH facts with the
same mapping transaction. Appended planners preserve previous image indices,
metadata pointers and per-image cookie readers, including across later stopped
module publication. A suspended driver task reacquires the current planner.
The host only interprets supported loader-decoded contracts; unsupported language
personalities/unwind versions remain errors.

Finalized mapping facts retain witnesses for headers, directory/unwind bytes,
C scope and GS payloads, personality code regions and personality IAT bindings.
Dispatch and continuation validate those bytes against current guest backing;
changed/unmapped facts fail instead of using stale cached metadata. Writable
module globals and live cookie values are not frozen. Prepared facts and a new
planner publish only with successful RAM/instance/OS metadata commit. Physical
failure abandons the new planner and keeps the existing owner intact.

The default kernel profile declares `__C_specific_handler`, `__GSHandlerCheck`
and `__GSHandlerCheck_SEH` as structural personality identities. This grants no
callable DDI ABI: direct calls remain unsupported. Explicit absence and the
selected kernel-reference namespace retain authority. Imports used to establish a personality contract
must keep an existing kernel binding; a private same-named routine cannot inherit
that identity. Actual new module execution tests cover ordinary C SEH and
cross-image unwind; GS byte-range/cookie ownership uses the shared existing
planner, without a new standalone extra-module GS execution fixture.

With the explicit 10.0.19041 `page_tables` model enabled, eager modules and modules
supplied during a CPU resource stop have their current mappings projected into
the same guest PTE/PFN model before the next machine instruction. Executable and
writable data pages retain their own supervisor/NX/write permissions. The pager
publishes through the stopped shared address space; backend register/context
mutation guards remain in force across acknowledgement and original-PC retry.
The complete recursive PML4 window and table storage belong to the pager;
additional module extents cannot claim them, and missing recursive aliases do
not become missing-module requests. A projection capacity/unsupported-layout
failure stops execution; committed module publication remains intact. Stopped
raw PTE snapshots may still show the previous projection until CPU admission or
an explicit modeled paging query synchronizes it. These are read-only model
tables; guest PTE writes, CR3 switching and TLB invalidation remain unsupported.

Additional active CFG modules use the same exact-target check/dispatch policy
as the primary image. The loader validates GFIDS, suppression, read-only slots,
address-taken IAT metadata and complete supported relocations; Native no-entry
modules do not acquire an invented entry point. Final mapping patches their
guard slots to the existing helpers. A prepared policy and byte witnesses
publish with the RAM/import/unwind/loader transaction. The policy object keeps
its identity across suspended driver tasks; failed publication grants no new
targets. Guarded images grant only unsuppressed GFIDS entries. Unguarded images
grant exact direct function exports, excluding data, PDB labels and forwarded
aliases; exports cannot override a guarded image's suppression. Helpers validate
the retained metadata/slot witnesses before dispatch. An adjacent address is
not authorized by sharing an executable page. XFG/export suppression and other
unsupported active extensions retain the loader's explicit errors.

An unguarded caller can trigger ordinary lazy fetch/supply of a guarded module;
its CFG policy is installed before that original instruction resumes. A CFG
helper itself requires an already published valid target; it does not bypass
validation to speculate about an unmapped future target. Additional entry
initialization and arbitrary bitmap rounding remain outside this profile.

The adapter reports
`module_publication: true`, `module_dependencies: true` and
`module_x64_c_seh: true` and `module_cfg: true` for these additional
guest bindings, and
`module_entry_initialization: false`. `resource_pause` reports the selected CPU's
support; `resource_pause_enabled` records the run's explicit option. These facts
do not promise recovery for arbitrary instructions or model errors.

With `enable_resource_pauses: true`, the driver adapter admits backend-validated
unmapped CPU reads/fetches at supported restart boundaries. Existing image
extents, user pointers, model objects and fixed arena/stack/thunk/device ranges
remain owned. Bounds, lifetime, opaque-object and protection failures do not
become missing-module requests. Admission only probes model identity; it does
not record an attempted write or mutate RAM.

For automatic first-use loading, declare the source with `load: lazy` and its
runtime binding with both `base` and the exact PE `SizeOfImage` in `size`. A real
guest access within that declared extent prepares and publishes the source, then
continues the retained CPU action automatically. Source preparation alone still
does not map it. Finite-call target admission requires an already published
executable target; a call from that code can first-use a lazy dependency through
its actual guest address.

When `pause_unknown_addresses: true`, a supported access outside known owners
can instead yield `awaiting_resource`. Add an explicit source using `module_add`
or the module C API and supply its binding with the event's exact token. Failed
supply retains the request for correction. Successful supply maps the module
and loader metadata while stopped and acknowledges the existing continuation;
it executes no CPU instruction. A subsequent `run` resumes at the backend's
actual PC. Completed API calls, global writes, indirect-call return slots,
registers, phase, active invocation and cumulative credits remain retained.
Neither source registration nor inspection restarts the driver.

The linker matches the full import provider identity, preserving the established
kernel alias and Windows basename folding. Symbols remain case-sensitive. An
extensionless forwarder provider must identify one registered provider; an
ambiguous basename fails. No source directory or host-system search occurs.
Named/ordinal forwarders may resolve to guest functions/data or existing model
identities. Forwarder cycles, empty export targets and excessive depth/work fail
before RAM publication. Existing model bindings and explicit absence retain
their authority; the linker does not replace the live API registry.

A dependency on an explicitly declared lazy binding is its first use. The engine
expands the same batch, prepares that resource and revalidates the complete link
without publishing an intermediate module. Unrelated lazy inputs remain unread.
An undeclared dependency, or a missing file for a declared dependency, yields
`missing_dependency` before the deferred CPU action. Add/correct the source and
supply the requested provider binding. The deferred eager importers join that
same transaction automatically. A provided module lacking the requested export
is an explicit linking error; it is not treated as another missing file.

If a CPU resource stop already exists when the missing module's own dependency
is discovered, its original request/token/PC remain retained. Supply the original
requested instance together with the new provider bindings. Failed linking
abandons every draft and preserves the request; successful linking publishes
them together before acknowledgement. It does not replay the caller or the
indirect call that reached the missing target.

Stopped loader publication and environment snapshots use explicit RAM snapshot
reads. They can inspect owned backing while a resource stop is pending without
changing CPU permissions, acknowledging the stop, invoking MMIO or clearing a
fault. CPU operations keep their original continuation guards. Unsupported
software instruction restart shapes, model/API errors and additional-module
initialization remain explicit limitations. Setting `enable_resource_pauses`
to false keeps ordinary terminal CPU-fault behavior; the one-shot driver entry
retains that setting.

The existing explicit
`kernel_image`/`kernel_pdb` scenario path remains available for compatibility;
its reference loader now delegates file/debug preparation to `ModuleSet` too.
Ring3 remains a design reservation without new application runtime instances.

## Interactive execution CLI

Start one persistent workload with explicit configuration files:

```powershell
neverd execution E:\lab\execution.json --modules=E:\lab\modules.json
```

The CLI emits one `created` record containing its initial stopped snapshot, then
reads one JSON command per stdin line. `--modules` is optional; omission creates
an empty source catalogue without a relative-path origin. With a module file,
incremental source paths use that file's resolved origin, as in the module C API.
The execution configuration does not implicitly select a binary directory.

Commands require a nonempty, bounded UTF-8 string `id` and an `op`. Responses
carry that ID, `schema_version: 1`, `ok: true`, and a `result` object. IDs correlate
responses and do not replace the resource request's owner/version/sequence token.
An operation failure emits `ok: false` and an `error` string while retaining the
same workload for inspection or correction. A malformed transport may have a
null response ID. The CLI passes original command bytes to
`neverd_execution_command_json`; the SDK owns strict envelope admission, nested
input validation and dispatch. No frontend normalizes duplicate keys away or
chooses a resource recovery policy.

| `op` | Additional fields |
|---|---|
| `state`, `pause`, `resume`, `run`, `inspect`, `instances`, `modules`, `quit` | None |
| `declare` | `binding`: one module-instance object |
| `supply` | `supply`: the retained `token` and `bindings` batch |
| `invoke` | `invocation`: the finite-call object described below |
| `read` | `address`, `size` |
| `write` | `address`, `bytes_hex` |
| `lookup` | `instance_id`, `symbol` |
| `module_add`, `module_revise` | `module`: the same source input as the module C API |
| `module_debug` | `module_id`, `debug`: the explicit companion descriptor |

For example, send each dependent command after receiving the preceding response:

```json
{"id":"run-1","op":"run"}
{"id":"inspect-1","op":"inspect"}
{"id":"call-1","op":"invoke","invocation":{"entry":"0x11000","abi":"win64","arguments":[{"value":40},{"value":2}]}}
{"id":"call-run-1","op":"run"}
{"id":"quit-1","op":"quit"}
```

The configuration above provides a selected stage and a private call stack. Its
raw Win64 scalar function adds the two arguments and returns 42. At an
`awaiting_resource` event, add or correct the source, then send `supply` with the
event's exact token and explicit bindings, and finally send another `run`.
`module_add`/`module_revise` do no binary IO. An explicit `module_debug` may prepare
and validate its source/companion; already mapped instances retain their pinned
observation. These source operations do not resume a CPU or map an instance.

One `run` request executes on a CLI worker so stdin remains available for
`state` and `pause`. Responses may arrive out of order and must be matched by ID.
The CLI admits one pending run command; other concurrent operations follow the
SDK's busy rules. Wait for a run's event before sending dependent mutation,
resource-supply or resume commands. `quit` or EOF requests a cooperative pause of
an active run, joins the worker and releases both handles. No implicit resume or
retry occurs. A pipe that reaches EOF during a run therefore requests its pause.

Configuration and command lines use the public C input ceilings shared with
the engine in `ExecutionInputLimits.def`. Oversized/NUL-containing lines are
drained and rejected without dispatch; input/output records remain bounded.
Exit 0 denotes a successful transport session; determine the guest outcome from
`result.state`. A guest `failed` outcome remains a valid event. Startup/IO errors
exit 1; command errors remain recoverable in the session and produce exit 2 when
it ends. stdout contains JSON records and startup diagnostics go to stderr.

## Shared execution JSON protocol

The C++ [`ExecutionRunJSON`](../include/neverd/emulation/ExecutionRunJSON.h)
codec is implemented in `NeverDEmulationWorkload`. It is the common protocol
boundary used by the persistent C API and interactive CLI. Environment-specific
startup and binary/model policy remain in the selected factory.

`executionRunOptionsFromJSON` decodes control options. Bindings and environment
configuration are separate inputs; this decoder does not choose a loader,
address space or OS. Omitted controls use `ExecutionRunOptions` defaults.

```json
{
  "limits": {
    "instructions": "200000000",
    "events": "1000000",
    "timeout_microseconds": "10000000"
  },
  "time_policy": "active_work",
  "instruction_quantum": 100000,
  "pause_stages": ["after-entry"],
  "memory_transfer_limit": 1048576,
  "enable_resource_pauses": true,
  "pause_unknown_addresses": true
}
```

The decoder also accepts `actions_per_run` and `instance_limit`. Time policy
may be `active_work` or `absolute_deadline`; decoding it does not create or reset
a budget. Stage names must subsequently pass the selected environment's
inventory check. Binding arrays use `id`, `module_id`, `guest_name`, optional
`scope`/`address_space`, and optional `base`/`size`. The decoder returns owned
descriptions without touching a CPU, reading sources or creating address spaces.
`resolveExecutionBindings` uses an explicit stopped-environment lookup; an empty
space name asks that environment for its default. Unknown names and null lookup
results fail. The decoder never substitutes a host pointer or guesses a process.
`ModuleBindings` retains ownership of identity, extent, overlap and publication
admission.

`executionSupplyFromJSON` accepts exactly `token` and `bindings`. The token has
`owner`, `version`, and `sequence`; version zero is valid before the first
declaration, while owner/sequence must be nonzero. Decoding a token does not
validate its ownership or resume a CPU. The existing `ExecutionRun::supply`
path checks the live request and commits the resolved batch.

`guestInvocationFromJSON` requires an explicit entry and scalar ABI. Empty or
omitted `instance_id` selects an absolute guest address; a nonempty instance ID
makes `entry` an RVA. Scalar arguments use `value`; buffer pointers use `buffer`
and an optional `offset`. These representations cannot be mixed in one argument.

```json
{
  "instance_id": "helper-instance",
  "entry": "0x1234",
  "abi": "win64",
  "arguments": [{"buffer": 0}, {"value": 256}],
  "buffers": [{"size": 256, "input_hex": "0080ff01", "capture": true}],
  "limits": {"instructions": 1000000, "events": 10000, "timeout_microseconds": 1000000}
}
```

Supported explicit ABIs are `win64`, `sysv_amd64` and `aapcs64`; this codec adds
no OS adapter or application execution example. The engine still checks the
actual architecture, stage, private stack and environment capabilities before
installation. The codec bounds argument/buffer counts and the ABI-aligned
aggregate payload against the supplied transfer limit. It decodes only supplied
input bytes; zero fill and caller-state installation stay in `GuestInvocation`.

All inputs are at most 2 MiB, with bounded UTF-8 names and no NULs. Unknown or
duplicate object fields, unpaired Unicode surrogate escapes, fractional/exponent
numbers, negative values and uint64 overflow fail explicitly. Unsigned fields
accept JSON integers or decimal/`0x` strings. Strings are preferable for clients
whose JSON number representation loses integer precision. Binding extent and
runtime admission remain engine checks.

Event, snapshot and instance encoders return owned JSON with `schema_version: 1`.
Every uint64 value is a hexadecimal string, including token parts, counters and
addresses. Reports preserve state/reason/stage separately; CPU stop/deadline,
faults, service handoffs and validated resource restart facts stay distinct.
Absent optional facts are omitted. Address-space names require the environment's
explicit identifier callback. Invocation output is `bytes_hex`, with an empty
string for an uncaptured row; output does not invent a successful model or grant
execution permission. Transfer data is bounded at 64 MiB and encoded reports at
192 MiB. The shared ceilings in `ExecutionLimits.h` also govern the existing
engine admission checks, without changing their previous values.

## Persistent workload and environment

[`ExecutionRun`](../include/neverd/emulation/ExecutionRun.h) retains one CPU,
`ExecutionSession`, budget, module-instance table and
[`ExecutionEnvironment`](../include/neverd/emulation/ExecutionEnvironment.h).
The environment owns canonical OS objects, loader/scheduler continuations and
phase state. Its stopped `next` method returns a bounded action; the run alone
executes the CPU and then calls `afterCPUExit`. Service requests and modeled
guest exceptions remain the environment's responsibility. It must not replace
hooks, execute another CPU loop or acknowledge resource stops itself.

The run validates an environment's stage inventory and the caller's requested
`PauseStages` at creation. A `StageBoundary` pauses only when selected. The
inventory supports future application environments without requiring kernel
names, `DriverEntry`, Windows or a particular loader. Raw-buffer environments
exercise the generic layer. The private Windows driver adapter additionally
retains actual driver state and provides its after-entry stop through the same
public C/CLI creation boundary.

`ExecutionRun::create` optionally adopts a shared budget with matching limits and
time policy. Existing instruction/event usage and active/deadline time are not
reset. `ExecutionEnvironment::modelHooks` supplies policy closures once before
initialization; the session remains the hook and accounting owner. Failed scene
actions may carry a validated `FailureReason` for guest/model/limit outcomes;
pause, resource or completion reasons cannot be published as failures, and a
nonfailed action cannot select a failure reason.

For a scene with model admission, the session decides instruction charging even
when the cumulative instruction credit is zero. This permits an already reached
uncharged model/return boundary to finish without granting another instruction.
Plain CPU scenes and finite invocation admission retain their existing limits.

The synchronous lifecycle is:

- `create` initializes the same environment once and returns `Ready`.
- `run` requires `Ready` and returns a structured `Paused`, `AwaitingResource`,
  `Completed` or `Failed` event. Finite control quanta also yield `Paused`.
- `resume` changes `Paused` to `Ready`; a subsequent `run` continues execution.
- `supply(token, bindings)` or a successful `resolve` changes
  `AwaitingResource` to `Ready`. Neither operation executes guest instructions.
- `Completed` and `Failed` reject resumption, supply and guest-memory mutation.

Before an ordinary CPU action, eager declarations are prepared and mapped.
Failed file preparation preserves that action as well as the resource request,
so correcting the input and supplying it does not repeat the environment's
already completed work. Source caches can advance independently; guest mapping
and metadata publication use the common module transaction.

During a supported CPU resource stop, the hook only checks environment policy
and a cached range inventory. Once the CPU exits, the run creates the versioned
request. A declared lazy instance can be prepared/supplied automatically through
the same path. An unknown address remains a candidate for caller input, without
guessing a filename. A hole inside a published module is rejected; the
environment must also decline known OS-object bounds/state failures.

Successful repair acknowledges the CPU stop and retains its mandatory
`ResumePC`. The next CPU action uses it before calling `next` again. This keeps
the actual partial instruction progress and avoids restoring an older scheduler
snapshot. Input errors and invalid tokens retain the request for retry;
unrecoverable environment/CPU failures and exhausted budgets terminate the run.
Instruction/event credits are never replenished, and a final successful
instruction may still reach a stage/completion without needing another credit.

`inspect`, bounded `readMemory` and bounded `writeMemory` operate at stopped
boundaries. Memory transfers use existing RAM permissions and do not dispatch
MMIO. Inspection remains available after termination. `state` and
`requestPause` are thread-safe while the owner lives; all other calls require
caller serialization and reject same-run callback reentry. Resolver callbacks
run outside the CPU and may register/correct caller-provided sources; source
addition alone never maps or resumes a running workload.

## Finite guest calls at a selected stage

`ExecutionRun::invoke(GuestInvocationRequest)` starts a call while paused at a
selected stage, or after a completed call at that same stage. It returns `Ready`
without executing instructions. `run`, resource supply, user pause and `resume`
drive the call through the retained session and environment. A second external
invoke while one is active is rejected; the environment still owns nested guest
callbacks required by modeled APIs.

The request specifies an explicit `IntegerCallingConvention`, an absolute guest
`Entry` or an `InstanceID` plus RVA, scalar arguments, bounded buffers and finite
instruction/event/time limits. Instance entries use that instance's address
space/base/extent. A declared lazy target can be supplied on its initial fetch
if the environment/CPU resource policies admit it. No file address or ABI is
guessed. `IntegerABI` supplies the existing non-variadic 64-bit scalar conventions;
floating-point/aggregate arguments, implicit extension and variadic calls are
unsupported. The environment also checks which conventions/targets it admits.

Each `GuestInvocationBuffer` has `Size`, initial `Input` bytes and a `Capture`
flag. Buffers occupy bounded, aligned payload storage in the environment's
private call stack; bytes beyond `Input` start at zero. A scalar argument has
`Value` and no `Buffer`; a buffer pointer has the buffer index in `Buffer` and
its byte offset in `Value`. Offsets through one-past and zero-sized buffers are
explicitly permitted. At most 64 arguments and 64 buffers are accepted, with
aggregate aligned payload bounded by the run's memory-transfer limit. Captured
output is copied into the result; returned buffer addresses describe guest RAM.

The environment's stopped `planInvocation` performs effect-free stage, target,
thread/IRQL/loader admission and supplies a private, already mapped RAM stack and
an executable return boundary. The stack must remain alive across every yield
and must not alias live caller storage. Generic preflight checks architecture,
layout, permissions, sizes, pointer offsets and obvious caller-stack overlap
before installing a frame. `beginInvocation` saves canonical caller/model state
and enters its own nested frame. `next` and `afterCPUExit` continue to handle
actual APIs, waits, exception transfers and callbacks using the same session.

`SessionContext` saves CPU registers and execution/write watches, including the
caller's watch step-over state. Hooks, RAM and budgets stay shared. A reserved
return watch remains present when the environment changes its ordinary watches;
it yields `InvocationReturn` before instruction admission and cannot be stepped
over. Pending resource/service/exception outcomes and terminal sessions reject
context capture/restoration. Live observers also reject context/control changes
and recursive session execution. Resource repair resumes the actual call
continuation before another environment action, retaining completed instruction effects.

At the reserved return, the run checks stack balance and captures outputs.
`finishInvocation` validates/restores canonical thread/model ownership while
leaving CPU/session restoration to the run. The run restores the caller context
and original stage and returns `Paused`/`InvocationComplete`, with a
`GuestInvocationResult` containing the scalar return, buffers and accounting.
Guest RAM side effects remain. Calling `resume` then `run` continues the parent
phase; another `invoke` can call again from the same stage.

Invocation limits cap the remaining shared allowance. Start/completion each
consume one event; admitted attempts, resource events, model events and supply
also remain cumulative. A local time cap is passed to each session run, and
planning, preparation and environment/resolver work count toward call time.
Planning errors cannot conceal exhausted time. Active-work policy excludes
stopped user waiting. A failure after installation, unbalanced return, failed
model restoration, exhausted limit or terminal guest fault terminates the owning
workload rather than restoring an earlier CPU snapshot and implying success.
Private call-stack/return-boundary failures are not missing-binary candidates.

The call engine is available through C++ and the persistent C API. The public
driver factory adopts its prepared frame at after-entry and uses the existing
kernel pump for APIs, callbacks and scheduled execution. Additional supported
Native kernel modules use the transaction described above. The public replay
consumer uses this same call boundary. No Ring3 application adapter or runtime
example is introduced by these tests.

## Verification

Build and run `NeverDModuleTests`, `NeverDDebugInfoTests` and
`NeverDPDBIdentityTests`. The module suite covers pure registration, path origin,
policy inheritance, lazy preparation, batch failure, source lifetime, companion
failure, late attachment and names-only trust. Where Clang and `lld-link` are
available, CMake builds two small driver PE/PDB fixtures with different identities
for real matching/mismatching companion checks. They are file-analysis fixtures,
not new Ring3 execution examples. Missing toolchains are reported as skips.

`NeverDModuleBindingTests` covers declarations, independent instances,
eager/lazy selection, cross-page supply, request tokens, resolver reentrancy,
failed source/builder/publication/allocation steps, opaque exports and atomic
guest/host metadata. Its mapping tests check abandoned/committed patches,
alias identity, reservation witnesses and existing software/checked Unicorn
CPUs observing newly mapped or patched code. They use isolated CPU instruction
buffers, not a new Ring3 application adapter. Run affected memory lifecycle,
RAM transaction, projection, parallel-execution and execution-session suites
when changing the shared address-space transaction.

`NeverDResourcePauseTests` executes software/checked x64 reads, cross-page reads
and instruction fetches, CALL target fetches, forward/reverse REP source and
destination faults, ENTER's committed store prefix, watch interactions, scalar
load register aliasing and thread/vector state preservation. It also checks a
checked ARM64 load, rejected software ARM64 hooks, untyped-run failure reporting,
and terminal model/protection/policy failures. `NeverDExecutionBudgetTests`
checks both time policies with deterministic clock samples. These are low-level
CPU/session checks, not persistent driver or new Ring3 application validation.

`NeverDExecutionRunTests` checks persistent environment/CPU/memory state,
software/checked x64 supply, automatic lazy mapping, eager source correction,
metadata dependency requests, foreign/stale tokens, resolver errors/reentry,
cross-thread pause, stage/lifecycle bounds, terminal failures and cumulative
instruction/event/time limits. Module tests also cover unprepared declaration
correction and rejection of prepared-input replacement. These are generic CPU
scene tests; they do not establish a completed driver adapter or Ring3 runtime.
Its invocation cases cover Win64 registers/stack arguments, buffers, lazy target
fetch, resource supply, repeated calls, API/nested callback transfer, saved caller
state and terminal failures. Additional raw CPU cases exercise SysV AMD64 and
software/checked AAPCS64. Execution-session tests verify saved watch control,
foreign/pending context rejection, reserved returns and local time caps.

`NeverDModuleCAPITests` links the actual shared SDK, including a pure C
translation unit, and checks version prefixes, handle lifetime, borrowed inputs,
eager/lazy policy, failed batches, correction, immutable sources, explicit debug,
UTF-8/bounds and concurrent additions. Optional real PE/PDB fixtures verify the
same matching policy through the public boundary. `NeverDModuleCLITests` compares
CLI output/diagnostics with direct C calls and independent source hashes. Run
the existing session/driver public suites and the Python plugin SDK ABI audit
when changing these public surfaces. These are file-resource checks. Real driver
calls and the public driver factory are additionally covered by
`NeverDDriverPersistentTests`, `DriverExecutionCAPI.*` in the driver public target
and `NeverDDriverExecutionCLITests`.
