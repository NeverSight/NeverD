# Native memory-safety scan

Run the static scan on a binary you own or are authorized to assess. By default it checks object lifetime, initialization, bounds and format strings.

```sh
neverd scan program -o scan.json
neverd scan program --analyzer=audit
neverd scan program --analyzer=hunt --max-paths=32
```

In the desktop workbench, open the binary and select **Options → Analysis → Memory Safety Scan**. Choose the analyzers and click **Scan**. The status line for each analyzer shows its coverage. Select a finding to inspect its scope, evidence and source. The worker uses the open project's image and symbol choices, with the default safety catalog and budgets. The CLI and workbench produce the same report with the same inputs and settings.

## Data pointers

`neverd scan binary --data-ptr` writes a `neverd.data-ptr-scan` v1 JSON
report to stdout or `-o file`. The native loader selects the ISA, ABI,
pointer width and byte order. PE/COFF and ELF use `.data`. Mach-O uses
`__data`.

* `code_references` counts distinct instructions with a recovered read, write
  or address reference into `.data`. An instruction that both reads and
  writes the same object counts once. The native reference decoder visits the
  session's functions after native function discovery. Symbols and recoverable
  addresses affect coverage.
  `incomplete_functions` counts visited functions with an incomplete decode or
  an unlifted instruction. Their recovered references still contribute.
* `data_pointers` counts full-width, file-backed slots in `.data` whose
  relocation resolves inside a readable, non-executable data section.
  Separate slots can point to the same object. Stripping relocations can
  reduce this count even when the stored bytes stay identical.

The regression inspects Windows x64 and ARM64 PE, Linux x64, ARM64,
MIPS, RISC-V and PowerPC ELF, Android and FreeBSD ARM64 ELF, macOS x64
and ARM64 Mach-O, and iOS ARM64 Mach-O. Linked ELF controls cover MIPS
o32, n32 and n64 in both byte orders, RV32 and RV64 with soft- and
double-float ABIs, and big- and little-endian PowerPC32 and PowerPC64
ELFv1/ELFv2. The ELFv1 control uses GNU PowerPC64 ld.

Code references require a recoverable address in a discovered function.
Loads through mutable pointer tables can leave the final address unknown,
including GOT-based accesses in MIPS and PowerPC PIC code. A stripped image
can also lose function boundaries. Consult `functions_scanned` alongside
the counts. For MIPS, RISC-V and PowerPC, scan linked ELF images.

Success returns 0. A missing data section, a canceled scan or an incompatible
memory-safety option returns 1. SDK callers can use
`neverd_session_data_ptr_scan_json` or Python's `Session.scan_data_pointers()`.

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

When Audit reaches the memory pass, `analyzers.audit.direct_read_initialization` counts the direct MedIR loads and read-modify-write operations it visited. Each operation contributes to one row.

| Counter | Evidence for the read |
| --- | --- |
| `preceding_constant_store` | The immediately preceding ordinary store in the same block wrote constant bytes covering the entire ordinary load. The two operations use the same recovered address or a provably contained stack range. |
| `bounded_index_store` | A preceding ordinary constant store covers the entire range of a runtime-indexed read. This counter is present when nonzero. |
| `local_stack` | The entire read lies below the function-entry stack pointer. The local stack analysis checks reaching writes. Consult `uninitialized_read` findings for failures or uncertainty. |
| `outside_or_unresolved` | The audit has no whole-read proof from the preceding store and cannot assess the entire read as a local stack slot. This includes caller storage, unresolved addresses and unsupported address widths or spaces. |

The counters describe lifted operations visited, including a partial inventory if a scan stops after starting Audit. A missing object means the pass did not run. Reads performed by callees, including cataloged copy functions, have separate checks. A nonzero `outside_or_unresolved` can accompany `SAFE` and `coverage_complete: true`. In a routine that fills a caller-provided buffer, such a result leaves initialization of those bytes open. Proving that a source is within the buffer also leaves its write history open.

### Runtime-indexed reads and function outputs

`analyzers.audit.indexed_read_output` reports two separate questions for an ordinary load with a recovered runtime address calculation. The `written_before_read` and `reaches_output` objects count reads by answer. Each histogram sums to `reads`.

| Question | Evidence |
| --- | --- |
| `written_before_read.YES` | A completed constant store in the same block covers every possible byte of the load. Audit checks the entire access width. An exact shared address or a bounded runtime displacement can establish coverage. |
| `reaches_output.YES` | All the loaded bytes survive in the explicit scalar machine result (`returned_value`) or in the final store at the address returned by that function (`returned_storage`). |
| `reaches_output.NO` | A constant return has no later observable write from the read, or the function completely replaces its only such write at a returned address whose value is independent of the read. |
| `UNKNOWN` | Evidence for the question is incomplete. An unresolved source leaves both questions open. A proved source can still have an unresolved output. |

A bounded backwards lookup can use a variable distance into a previously written region. The proof follows same-width SSA address arithmetic, an AND bound or a signed OR bound, and byte-preserving register views. Indexing visible through a truncated or extended address stays in the inventory and has an unresolved address identity. A call, atomic operation, unmodeled effect or control transfer can end the proof. `unmatched_native` counts recognized instruction-local indexed loads missing from machine MedIR. These contribute one `UNKNOWN` to each question.

With any unresolved indexed read, Audit and the overall scan return `UNKNOWN` (exit 1) unless another check establishes `UNSAFE`. `coverage_complete` still refers to completion of the selected inventories. With zero observed indexed reads the property itself is `UNKNOWN` and other checks determine the overall verdict. A proven result describes the modeled function return. Cross-iteration write history, arbitrary transforms, callers and eventual process or device output require their own evidence.

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

MIPS n32 carries a 32-bit allocator result in the 64-bit `v0` register. Audit uses that ABI rule when checking for a non-null allocation and follows the pointer into a matching release argument. For a GNU n32 caller that saves its return address on the stack, Audit can prove that the result of a versioned glibc `malloc` is disjoint from that private frame and is passed to the matching `free`, including in a call delay slot. If the delay slot instead passes a full-register scalar zero to `free`, the original allocation remains live. Audit reports a `heap_leak` at `malloc` when that allocation leaves the function without escaping. The proof checks the primary GOT entries, their required provider and version, the uses of the saved link, and writes and calls in the scanned image. The modeled bindings are `malloc@GLIBC_2.0` and `free@GLIBC_2.0` from `libc.so.6`. An unresolved binding, possible external write or unproven return address leaves coverage incomplete. The ownership fixtures can be rebuilt with `unittests/safety/fixtures/elf-profiles/build_ownership.py` and Debian's GNU n32 cross tools.

## Limits and evidence

One scan uses one loaded image, one native lift, one validated function inventory, and one set of interprocedural facts and resolved catalog sites. The lift stops once it has recovered the call ABI and prepared the verified machine IR that Audit and Hunt read. Each analyzer applies its usual path, step, loop, call-depth, fixed-point and solver budgets. The scan accepts symbol, catalog and budget options. Ctrl-C in the CLI or **Cancel scan** in the workbench requests a cooperative stop. Loading, the current lifting pass, shared preparation or a bounded analyzer operation can finish before the next checkpoint. Evidence from completed work stays in the report. The desktop worker reports an error if a reply exceeds its 8 MiB transport limit. Use `neverd scan program -o scan.json` to write a larger report.

The scan requires neither target execution nor a Lean installation. An independently checked proof in the Lean 4 M3 pilot applies to its exact property, location and assumptions. Version 1 reports static observations and does not issue proof certificates. Unresolved obligations keep their `UNKNOWN` verdict. [Shared evidence and the M3 pilot, #1675](https://github.com/NeverSight/NeverD/issues/1675).

## SDK

In C, call `neverd_session_scan_json(session, NULL)` for defaults. To select one track, zero-initialize `neverd_scan_options`, set `struct_size` and specify `NEVERD_SCAN_AUDIT` or `NEVERD_SCAN_HUNT` in `analyzers`. Supply a borrowed `neverd_safety_options` in `analysis` for catalog or budget overrides. The optional `cancel` callback and its `context` are borrowed until return. Free the JSON with `neverd_free_string`. In Python, `Session.scan(analyzer="both")` returns the same report as a mapping, including partial results. [Analysis limits and evidence fields](memory-safety.md).
