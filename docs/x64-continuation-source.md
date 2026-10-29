# Explicit x64 continuation C

`neverd decompile --x64-continuation=request.json` exports selected machine
instructions from linked x64 ELF or PE images as C with an explicit architectural
state. Raw images and relocatable objects retain their existing native-provider
refusal. Use it for interrupt
entries, stack pivots and physical dispatch frames that have no proven ordinary
C calling convention. Both HighC and `--llvm` consume the same checked LowIR.

```json
{
  "profile": "kernel-events-v6",
  "entries": ["0x140001000"],
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48
}
```

```sh
neverd decompile input.exe --x64-continuation=request.json \
  --continuation-report=state.manifest.json -o state.c
neverd decompile input.exe --x64-continuation=request.json --llvm \
  --continuation-report=state-llvm.manifest.json -o state-llvm.c
```

The profile, effective MXCSR mask and active paging width are declarations
about the execution environment. Set them from that environment; the example
values are not inferred from a PE file. `paging_address_bits` accepts 48 or 57.
Supply exactly one nonempty array: `entries` selects recovered CFG regions;
`instructions` selects exact instruction addresses. Decimal integers and hex
strings are accepted. An ordinary source rejection does not erase its native
instruction selection: every selected instruction is relifted independently.
Manifest schema 2 records each region's `lowir_disposition`,
`lowir_unproved_returns` and `lowir_unsupported_instructions`. These describe
the LowIR selection stage only; full ordinary ABI and call/return validation
has not run (`ordinary_source_validation: "not-run"`). Original bytes and
overlapping instruction starts remain in the manifest. Selection does not
prove whole-function or callee coverage.

The request can lower `max_instructions` and `max_operations` from the shared
continuation limits. Unknown options, duplicate addresses, missing bytes,
malformed instructions and exhausted generation budgets fail the export.
Ordinary `--func`, frame edits, devirtualization and source optimization options
cannot be combined with this mode. The ordinary decompiler never silently
changes its interface to state C.

## Invocation

The generated translation unit defines `neverd_x64_state_v6`, its checked field
layout, instruction steps, and:

```c
uint64_t neverd_x64_continue_v6(neverd_x64_state_v6 *state, uint64_t *budget);
```

Initialize all architectural inputs from the intended machine context, including
GPRs in x86 encoding order, RFLAGS, PC, vector containers, FS/GS bases, controls,
segment caches and descriptor tables. Zero-initialization alone is not a valid
kernel entry context. The flattened C layout is checked against the semantic
owner's versioned state, and the manifest publishes every field offset.

Guest pointers name actual accessible memory. State and budget storage must be
disjoint from each other and every guest access. CALL writes its original next
PC to the physical guest stack. RET reads that stack, including changes made by
an intervening callback. The environment can change every state field and guest
byte before resuming. No source parameter count or callee preservation is
assumed. Arithmetic arbitrary-value slots represent caller-selected choices
for each dynamic instruction; reusing them selects a subset of allowed runs.

The driver continues ordinary instructions and jumps to listed addresses. Each
successful instruction consumes one budget unit, including a stopping transfer.
A native request or invalid-state refusal executes nothing and consumes none.

| Status | Meaning |
| --- | --- |
| 1 | CALL completed; environment owns the target |
| 2 | Jump completed to an address outside the selection |
| 3 | RET completed from the physical stack |
| 4 | Native execution required; original instruction remains unexecuted |
| 5 | Invocation is outside the declared state/precondition domain; unchanged state |
| 6 | Privilege-return context installed; target has not executed |
| 7 | Exception-handler context installed; handler has not executed |
| 8 | Current PC is outside the selection |
| 9 | Instruction budget exhausted |

The invocation domain is CPL0, 64-bit entry with valid descriptor caches,
CET/FRED disabled, successful sequential ordinary RAM accesses and immutable
original instruction bytes. It excludes MMIO, concurrent writers, asynchronous
events and faulting execution. IRETQ and canonical RETFQ (`48 CB`,
`48 CA imm16`) can produce CPL0–3 long-mode contexts;
INT3 and canonical `INT imm8` (`CD 00` through `CD FF`) install a CPL0
interrupt/trap-gate context with optional IST. Each vector reads its own live
IDT entry, including vectors ordinarily used for OS services. Software INT
does not push a hardware exception's error code or acquire NMI blocking for
vector 2. These transfers stop before the target executes. Status 5 is not
emulated fault delivery. Other prefixed encodings remain native requests.

RETFQ reads RIP/CS alone for a same-privilege return and retains SS. For a
return to an outer privilege level, its unsigned imm16 skips parameter bytes
before saved RSP/SS and also adjusts the restored RSP. The transition preserves
NMI blocking and flags within the existing non-interrupt entry domain (TF/RF,
IOPL and VIF/VIP remain excluded). It consumes one driver step and stops even
when the return target is selected; a returned CPL1–3 context cannot reenter
this supervisor profile. Compatibility-mode and 16-/32-bit far returns still
require native execution.

## Native environment bindings

All bindings below are disabled by default. They must match the executing
machine environment; enabling them is not an emulation implementation.

| Request field | Effect |
| --- | --- |
| `native_timestamp_reads: true` | Executing host's TSC and TSC_AUX |
| `native_write_prefetch: true` | Host PREFETCHW and its actual memory environment |
| `segment_base_msr_address_bits: 48` or `57` | FS_BASE, GS_BASE and KERNEL_GS_BASE use the existing state cells and declared MSR canonical width |
| `native_speculation_control_msrs: true` | Real IA32_SPEC_CTRL and IA32_PRED_CMD writes; feature support and valid values are required |
| `native_descriptor_queries: true` | Real SGDT, memory VERW and register LSL in the bound descriptor environment |
| `native_page_table_access: true` | Real CR3 accesses, mapping changes and TLB invalidation |

Native descriptor and paging bindings must agree with the declared architectural
context. Both address spaces must retain accessible generated code, host stack
and state storage across native CR3 writes. Other MSRs and unsupported system
effects retain status 4. A zero native-request count describes the selected
instructions under these declarations; it is not evidence that a complete OS
has run.

## SDK and verification

`neverd_x64_continuation_source_v1` takes a size-delimited request, route 0 or 1,
and returns owned source plus an independently owned manifest. Release both
with `neverd_free_string`. Failure returns NULL, clears the manifest output and
sets `neverd_last_error`. The manifest contains exact original instruction bytes,
state layout, arbitrary-effect ordering, environment declarations and a SHA-256
of the complete emitted C.

Python plugins can use the same interface through `session.raw`. The owned-string
helper releases the source; the caller must separately release the manifest:

```python
import ctypes
import json

request = json.dumps({
    "profile": "kernel-events-v6",
    "instructions": [0x140001000],
    "mxcsr_mask": "0xffbf",
    "paging_address_bits": 48,
}).encode("utf-8")  # Use the execution environment's mask and paging width.
manifest_pointer = ctypes.c_void_p()
try:
    source = session.raw.session_owned_string(
        "neverd_x64_continuation_source_v1", request, len(request), 0,
        ctypes.byref(manifest_pointer))
    if source is None:
        raise RuntimeError(session.raw.session_owned_string("neverd_last_error"))
    manifest = json.loads(ctypes.string_at(manifest_pointer).decode("utf-8"))
finally:
    if manifest_pointer.value:
        session.raw.call("neverd_free_string",
                         ctypes.cast(manifest_pointer, ctypes.c_char_p))
```

Pass `None` as the last argument if no manifest is needed. Never use `c_char_p`
as its output cell: implicit string conversion would lose the owned address.

Build `NeverDX64ContinuationExportTests` and `NeverDX64ContinuationSourceTests`
with `scripts/build.py`. The public exporter suite checks SDK/CLI parity,
strict request failures, physical CALL/RET after a stack pivot, bounded loops,
unchanged native/invalid boundaries, and successful privilege-exit accounting.
It compiles and executes both C routes at O0/O2 with ASan and UBSan. The semantic
suite separately compares supported privileged transitions with native KVM
fixtures; missing KVM coverage is reported as skipped.

The [RETFQ qualification record](evidence/x86-retfq-continuation-2026-10-29.json)
records 6,628 complete state/RAM comparisons, all 65,536 imm16 encoding audits,
guard-page and read-only descriptor checks, and 378 passing continuation,
export and proof regressions on the recorded source and toolchain. It qualifies
the V6 transition domain above, not an ordinary C return ABI.

`X64ContinuationIntegerTest` compares complete state and memory with original
native instructions for audited legacy vector transfers, XOR and byte shifts,
64-bit products, scalar exchanges and locked memory updates. It exercises
arbitrary arithmetic flags, preserved upper vector lanes, negative bit-string
indices and unaligned RAM through both C routes at O0/O2 with ASan/UBSan.
LOCK BTS/BTR/BTC retain one sequentially consistent native transaction, including
the old bit returned in CF; an unlocked load/store pair is not equivalent.
The encoding audit rejects inconsistent decoder details and unaudited prefixes.
These tests do not extend the invocation domain to faulting or concurrent runs.
