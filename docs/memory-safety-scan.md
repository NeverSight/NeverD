# Native memory-safety scan

Run the static scan on a binary you own or are authorized to assess. The default checks heap lifetime, initialization, bounds and format strings using the existing Audit and Hunt analyses.

```sh
neverd scan program -o scan.json
neverd scan program --analyzer=audit
neverd scan program --analyzer=hunt --max-paths=32
```

In the desktop workbench, open the binary and select **Options → Analysis → Memory Safety Scan**. Choose the analyzers and click **Scan**. The status line for each analyzer shows its coverage. Select a finding to inspect its scope, evidence and source. The worker uses the open project's image and symbol choices, with the default safety catalog and budgets. The CLI and workbench produce the same report with the same inputs and settings.

## Reading a report

The top-level `schema` is `neverd.memory-safety-scan` and `schema_version` is `1`. The report records the image format and architecture, an aggregate verdict, two analyzer summaries and a list of grouped findings. [JSON Schema](memory-safety-scan-v1.schema.json).

Each observation under `findings[].observations` names its `analyzer` and carries the complete legacy `finding`, including symbol provenance, reachability and evidence. Groups identify a `property`, a `location` in the loaded image and the analyzed `scope`. The location contains the function and instruction VAs and exact lifted operation coordinates when available. The scope records the catalog or analyzer check and its applicable argument, path events, allocation and constraints. Reports of distinct properties or scopes at one instruction have separate groups. An ambiguous occurrence keeps its own observation separate.

The per-analyzer `scanned` and `skipped` counters retain their legacy units. Audit counts matched allocation sites, while Hunt counts sites it examined and the subset its prefilter resolved. `observations` counts records before grouping. `unknown` counts inconclusive observations. These are catalog and analysis counters for the admitted image.

| Analyzer state | Meaning |
| --- | --- |
| `completed` | The analyzer finished its inventory. Individual properties can still be `UNKNOWN`. |
| `disabled` | The caller selected the other analyzer. |
| `canceled` | A cancellation checkpoint stopped the analyzer. |
| `unsupported` | The input or the recovered lift could not support the requested analysis. The summary includes the reason. |
| `budget_exhausted` | A lifting or analysis limit cut coverage short. |
| `failed` | The request or analyzer failed. The summary carries a diagnostic. |

`coverage_complete` means all **selected** analyzers finished their inventories within budget. `ok` means at least one analyzer finished its pass (possibly with a budget limit) or produced observations before stopping. Consult the individual states for partial work. Any established `UNSAFE` finding yields exit code **2**, including when another analyzer stopped. With no such finding, an inconclusive observation or incomplete selected analyzer yields `UNKNOWN` and exit code **1**. Otherwise the verdict is `SAFE` within the selected static checks and the exit code is **0**. Argument, file loading and output errors also return **1**.

## Limits and evidence

One scan uses one loaded image, one lift, one validated function inventory, and one set of interprocedural facts and resolved catalog sites. Each analyzer applies its usual path, step, loop, call-depth, fixed-point and solver budgets. The scan accepts the legacy symbol, catalog and budget options. Ctrl-C in the CLI or **Cancel scan** in the workbench requests a cooperative stop. Loading, the current lifting pass, shared preparation or a bounded analyzer operation can finish before the next checkpoint. Evidence from completed work stays in the report. The desktop worker reports an error if a reply exceeds its 8 MiB transport limit. Use `neverd scan program -o scan.json` to write a larger report.

The scan requires neither target execution nor a Lean installation. An independently checked proof in the Lean 4 M3 pilot applies to its exact property, location and assumptions. Version 1 reports static observations and does not issue proof certificates. Unresolved obligations keep their `UNKNOWN` verdict. [Shared evidence and the M3 pilot, #1675](https://github.com/NeverSight/NeverD/issues/1675).

## Migration and SDK

Existing consumers can continue using `neverd audit`, `neverd hunt`, their C functions and their version 1 JSON format. To migrate, use the scan's schema name to select the parser, read both analyzer states, then consume each group's observations. Single-analyzer selection preserves the legacy findings and coverage counters. The scan's aggregate also accounts for exhausted budgets when there are no findings.

In C, call `neverd_session_scan_json(session, NULL)` for defaults. To select one track, zero-initialize `neverd_scan_options`, set `struct_size` and specify `NEVERD_SCAN_AUDIT` or `NEVERD_SCAN_HUNT` in `analyzers`. Supply a borrowed `neverd_safety_options` in `analysis` for catalog or budget overrides. The optional `cancel` callback and its `context` are borrowed until return. Free the JSON with `neverd_free_string`. In Python, `Session.scan(analyzer="both")` returns the same report as a mapping, including partial results. [Legacy analysis limits and evidence fields](memory-safety.md).
