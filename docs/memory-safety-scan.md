# Native memory-safety scan

Run the static scan on a binary you own or are authorized to assess. By default it checks object lifetime, initialization, bounds and format strings.

```sh
neverd scan program -o scan.json
neverd scan program --analyzer=audit
neverd scan program --analyzer=hunt --max-paths=32
```

In the desktop workbench, open the binary and select **Options → Analysis → Memory Safety Scan**. Choose the analyzers and click **Scan**. The status line for each analyzer shows its coverage. Select a finding to inspect its scope, evidence and source. The worker uses the open project's image and symbol choices, with the default safety catalog and budgets. The CLI and workbench produce the same report with the same inputs and settings.

## Reading a report

The top-level `schema` is `neverd.memory-safety-scan` and `schema_version` is `1`. The report records the image format and architecture, an aggregate verdict, two analyzer summaries and a list of grouped findings. [JSON Schema](memory-safety-scan-v1.schema.json).

Each observation under `findings[].observations` names its `analyzer` and carries the complete `finding`, including symbol provenance, reachability and evidence. Groups identify a `property`, a `location` in the loaded image and the analyzed `scope`. The location contains the function and instruction VAs and exact lifted operation coordinates when available. The scope records the catalog or analyzer check and its applicable argument, path events, allocation and constraints. Reports of distinct properties or scopes at one instruction have separate groups. An ambiguous occurrence keeps its own observation separate.

Audit counts matched allocation sites, while Hunt counts sites it examined and the subset its prefilter resolved. `observations` counts records before grouping. `unknown` counts inconclusive observations. These are catalog and analysis counters for the admitted image.

| Analyzer state | Meaning |
| --- | --- |
| `completed` | The analyzer finished its inventory. Individual properties can still be `UNKNOWN`. |
| `disabled` | The caller selected the other analyzer. |
| `canceled` | A cancellation checkpoint stopped the analyzer. |
| `unsupported` | The input or the recovered lift could not support the requested analysis. The summary includes the reason. |
| `budget_exhausted` | A lifting or analysis limit cut coverage short. |
| `failed` | The request or analyzer failed. The summary carries a diagnostic. |

`coverage_complete` means all **selected** analyzers finished their inventories within budget. `ok` means at least one analyzer finished its pass (possibly with a budget limit) or produced observations before stopping. Consult the individual states for partial work. Any established `UNSAFE` finding yields exit code **2**, including when another analyzer stopped. With no such finding, an inconclusive observation or incomplete selected analyzer yields `UNKNOWN` and exit code **1**. Otherwise the verdict is `SAFE` within the selected static checks and the exit code is **0**. Argument, file loading and output errors also return **1**.

## MIPS, PowerPC and RISC-V ELF

The same `neverd scan` command reads the architecture, byte order and ABI from the ELF. The CLI, desktop worker and SDK share this selection.

| ELF target | Linked stack-read control |
| --- | --- |
| MIPS32r2 o32 and MIPS64r2 n64, big- and little-endian | Identifies the uninitialized read and its function as `UNSAFE`. |
| MIPS64r2 n32, big- and little-endian | Checks the 32-bit signed-word condition on the 64-bit stack register and corroborates the read as `UNSAFE`. |
| MIPS III o32, big- and little-endian | An access that needs runtime MMU translation yields `UNKNOWN` with `unresolved-memory-translation`. |
| PowerPC32 and PowerPC64 ELFv1/ELFv2, big- and little-endian | Identifies the uninitialized read and its function as `UNSAFE`. |
| RISC-V ILP32, LP64, ILP32D and LP64D, little-endian | Identifies the uninitialized read and its function as `UNSAFE`. The D-ABI controls load and store an eight-byte floating local. |

The paired control initializes a local of the same width before reading it. A second fixture calls a separately linked one-argument text function with an immutable literal. On PowerPC64 ELFv2 the scan resolves the local entry and proves the literal safe. On other admitted profiles the report can identify the call while leaving the string's origin inconclusive.

The RISC-V D-ABI control calls a function with two floating arguments, changes the sign and consumes the floating result. The lifter supports floating loads, stores, bit transfers and sign operations. Rounded floating arithmetic and floating control/status register accesses can leave a function unsupported. Audit checks ownership and initialization. Hunt reports `UNKNOWN` for copy and format sites that require its call analysis on these ABIs.

The regression runs against linked binaries on every test host. To rebuild them, run `python3 unittests/safety/fixtures/elf-profiles/build.py --clang clang --lld ld.lld --ppc64-ld powerpc64le-linux-gnu-ld` with Clang/LLD 22 and GNU PowerPC64 binutils 2.40. GNU ld supplies the ELFv1 function descriptors.

## Heap ownership and debug companions

A call to `malloc` can leave its pointer in the ABI return register even in a `void` function. A matching declaration lets the scan distinguish that leak from a function that transfers ownership to its caller. On x64 and ARM64 PE, the scan checks the PDB identity, the procedure's code contribution and the CodeView return type. For an external Mach-O dSYM, the image and companion must have matching architecture and build UUID. A map alone supplies names. With an unresolved return contract, the scan reports `UNKNOWN` for this pattern.

MIPS n32 carries a 32-bit allocator result in the 64-bit `v0` register. Audit uses that ABI rule when checking for a non-null allocation and follows the pointer into a matching release argument. A separate requirement applies when a function saves its return address on the stack. If the lift cannot establish that an external call preserves that saved link, the report marks coverage incomplete and includes the return-target diagnostic. This occurs in GNU n32 binaries that forward one external call's result to the next.

## Limits and evidence

One scan uses one loaded image, one native lift, one validated function inventory, and one set of interprocedural facts and resolved catalog sites. The lift stops once it has recovered the call ABI and prepared the verified machine IR that Audit and Hunt read. Each analyzer applies its usual path, step, loop, call-depth, fixed-point and solver budgets. The scan accepts symbol, catalog and budget options. Ctrl-C in the CLI or **Cancel scan** in the workbench requests a cooperative stop. Loading, the current lifting pass, shared preparation or a bounded analyzer operation can finish before the next checkpoint. Evidence from completed work stays in the report. The desktop worker reports an error if a reply exceeds its 8 MiB transport limit. Use `neverd scan program -o scan.json` to write a larger report.

The scan requires neither target execution nor a Lean installation. An independently checked proof in the Lean 4 M3 pilot applies to its exact property, location and assumptions. Version 1 reports static observations and does not issue proof certificates. Unresolved obligations keep their `UNKNOWN` verdict. [Shared evidence and the M3 pilot, #1675](https://github.com/NeverSight/NeverD/issues/1675).

## SDK

In C, call `neverd_session_scan_json(session, NULL)` for defaults. To select one track, zero-initialize `neverd_scan_options`, set `struct_size` and specify `NEVERD_SCAN_AUDIT` or `NEVERD_SCAN_HUNT` in `analyzers`. Supply a borrowed `neverd_safety_options` in `analysis` for catalog or budget overrides. The optional `cancel` callback and its `context` are borrowed until return. Free the JSON with `neverd_free_string`. In Python, `Session.scan(analyzer="both")` returns the same report as a mapping, including partial results. [Analysis limits and evidence fields](memory-safety.md).
