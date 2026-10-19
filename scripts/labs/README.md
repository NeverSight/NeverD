# Public SDK driver replay

`run_module_driver_replay.py` is a consumer of the public execution C API and
the existing Python ABI declarations. It loads an original driver, pauses at
`after-entry`, and makes finite Win64 calls in the same retained environment.
Guest memory effects and cumulative budgets survive between calls. It does
not compile or rewrite an executor or driver source.

Requires Python 3.10+ and a built NeverD shared library. All files are supplied
explicitly. Register kernel/reference files in a modules configuration, select
the reference with `--kernel-module`, and supply the workload's own scenario.
Relative scenario files resolve against the scenario directory; module paths
resolve against the modules configuration directory through the public API.
No host-system search or download is performed.

```powershell
python scripts/labs/run_module_driver_replay.py `
  --library build-release/bin/neverd.dll `
  --image C:/my-input/driver.sys `
  --modules C:/my-input/modules.json --kernel-module kernel `
  --scenario C:/my-input/driver.scenario.json `
  --entry 0x140001000 --capacity 0 --capacity 260 `
  --output C:/my-results/replay.json
```

Replace the entry with a verified address in the loaded image. This runner's
call shape is an output-buffer pointer followed by a caller-supplied capacity;
it is a hypothesis supplied by the caller, not a prototype inferred from PDB.
Use `--buffer-bytes` for the backing/capture extent and `--fill` for its initial
byte. Capacity and buffer length are separate. Calls use the public SDK's
private invocation stack. The default capacities are 0/2/18/260/1024; the default
capture is 4096 bytes filled with `0xa5`.

The default `--primary-import-policy modules` resolves primary imports through
the shared module/dependency linker. An explicit `model-traps` selection retains
the existing one-shot primary import contract: named imports bind existing
models or unsupported-API traps, without inferred ABI or guessed behavior.
Additional native modules continue to use the shared linker. This option does
not add native initialization or models for undocumented routines. It is useful
when migrating an existing one-shot scenario with unused unmodeled imports.

The runner requests a bounded read-only trace view (`--report-tail`, default64).
Reports include full retained call/write counts and the first serialized index;
omission from a view does not remove runtime events or replenish budgets. Each
invocation records whether all of its API records fit the view. Output includes
source/library hashes, configuration, actual entry and invocation results,
captured buffers, published instances and failure/resource details. A pending
resource or failed call ends the batch visibly; use the interactive `execution`
CLI or the public SDK to perform manual supply and continuation.

Examples without binary payloads are in
[`resources/modules/examples`](../../resources/modules/examples/README.md).
Ring3 remains a design reservation; this runner exercises the driver adapter.
