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
assumed. Arbitrary-value slots represent caller-selected choices
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

Canonical SYSRET (`0F 07`) and SYSRETQ (`48 0F 07`) install user CS/SS caches
from the live STAR field without reading descriptors. SYSRET selects a 32-bit
compatibility context and takes EIP from ECX; SYSRETQ selects a 64-bit context
and checks the full RCX against the declared canonical width. Both derive flags
from R11 and stop with status 6 before user code executes, even if its address
is selected. A returned context cannot reenter this CPL0 64-bit profile.

SYSRET preserves ESP and the low halves of the other seven legacy GPRs. Their
high halves are architecturally undefined on this mode switch; `Undefined[0..7]`
supplies their new 32-bit values in RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI order.
Only the low 32 bits of each slot are used. R8–R15 remain fully preserved.
SYSRETQ preserves all GPRs. The manifest records all eight SYSRET arbitrary
effects and their exact register ranges. This follows
[Intel SDM Volume 1, section 3.4.1.1](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf);
a particular CPU's high-half results are witnesses, not a preservation guarantee.
Successful execution requires SYSCALL/SYSRET enabled and the profile's existing
CET/FRED-disabled environment. Ordinary source and proof admission stay closed.

V5/V6 also bind `RDMSR` and `WRMSR` selector `0xc0000081` to that same live
`STAR` field. A write followed by a read or SYSRET observes the new selectors;
the executing host's STAR is unchanged. The low 32 bits of RCX select the MSR,
and only EDX:EAX supply its write value. Reads zero-extend EAX and EDX.
Writes require zero in STAR's low 32-bit reserved field. Nonzero values return
status 5 with unchanged state and budget; this restriction does not assert
that every processor faults on them. The upper selector fields are not
canonical addresses. These fields follow
[Intel SDM Volume 3, Figure 6-14](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf).

This STAR binding needs no additional request option because V5/V6 already
declare it as live return state. Earlier profiles retain their previous MSR
boundaries. Segment-base and native speculation MSRs still require their own
options below; an unbound runtime selector returns status 5 before effects.

RETFQ reads RIP/CS alone for a same-privilege return and retains SS. For a
return to an outer privilege level, its unsigned imm16 skips parameter bytes
before saved RSP/SS and also adjusts the restored RSP. The transition preserves
NMI blocking and flags within the existing non-interrupt entry domain (TF/RF,
IOPL and VIF/VIP remain excluded). It consumes one driver step and stops even
when the return target is selected; a returned CPL1–3 context cannot reenter
this supervisor profile. Compatibility-mode and 16-/32-bit far returns still
require native execution.

IRETQ/RETFQ have no unconditional empty undefined-output certificate: a live
return descriptor can select compatibility mode, where the legacy GPR high
halves are arbitrary. V6's shared descriptor check requires CS.L=1 and CS.D=0
before publishing state or descriptor writes. That checked long-mode domain
owns the empty arbitrary-effect list for these continuations. It does not
grant an ordinary LowIR or native-preservation certificate. Earlier generic
audits incorrectly treated this profile condition as an instruction-wide fact.

LSS/LFS/LGS also continue through V6. The shared descriptor owner loads the
16/32/64-bit offset, validates the GDT or LDT descriptor, updates its accessed
bit and installs the selected cache and base together with the destination
GPR. LSS requires writable data with RPL=DPL=CPL=0; LFS/LGS accept the
corresponding data/readable-code privilege rules. A 16-bit destination retains its upper
bits; a 32-bit destination zero-extends. Both pointer fields are read before
the descriptor write, even when they alias it. The next PC is the original
fallthrough, and flags, NMI blocking and other state remain unchanged.

These forms use Intel operand-size rules. The byte grammar admits single
operand/address-size and FS/GS prefixes in any order, an optional final REX,
and complete ModRM/SIB/displacement addressing. Each field's address wraps
independently at the address size before adding the original FS/GS base. Thus
an addr32 selector may be 4 GiB away from the offset's end in linear memory.
Null SS selector 0 is supported at CPL0; selectors 1..3 reject. LSS does not
establish MOV SS / POP SS interrupt/debug inhibition, as specified in
[Intel SDM Volume 3A, section 7.8.3](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).
The previous documentation incorrectly attributed that inhibition to LSS.
Null LFS/LGS selectors 0..3 require `"null_segment_base_policy": "clear"` or
`"preserve"` in the request (`NullSegmentBasePolicy` in C++). Bind the actual
original processor's behavior; vendor identity alone is insufficient. Both
policies install the selector and mark it unusable, retain ignored cache bytes,
and read no descriptor. The live FS/GS base follows the selected policy. When
omitted, null LFS/LGS still reject with status 5 before descriptor access or
any write. LSS does not need this option. The manifest retains the declaration.
LDS/LES and compatibility-mode loads remain outside this long-mode profile.
Ordinary strict/permissive lifting continues to refuse these far loads. The
continuation receipt retains exact bytes with an incomplete ordinary sidecar;
it supplies no NOP approximation or ordinary source/native proof.

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

The [capability ledger](capabilities.json) records this interface as experimental
under `analysis.x64-continuation-source`, with its C entry point, both CLI options
and the public exporter regressions. SDK request/CLI parity checks cover the
portable interface; the sanitizer-backed generated-C execution entries are
scoped to Linux. Native Windows/macOS generated execution remains separate
qualification. The ledger keeps ordinary source ABI recovery outside this claim.

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

The [LFS/LGS qualification record](evidence/x86-lfs-lgs-continuation-2026-10-29.json)
records 132,544 native state/RAM comparisons across HighC/LLVMC at O0/O2,
including independent addr32 field wrapping, old segment bases, every GPR,
descriptor-bit variants and accessed-bit aliasing into either pointer field.
It also records 408 unchanged-state/RAM refusals, 24 exact-width/read-only
page checks and 387 passing continuation, export and proof regressions.
The native CPU was Intel; null-selector policy and LSS were not qualified by
those earlier counts. The subsequent
[far-load completion record](evidence/x86-far-load-continuation-2026-10-29.json)
covers LSS, null loads and the public policy binding: 175,040 native state/RAM
comparisons, immediate fallthrough breakpoints and zero CPU interrupt shadow.
Guard pages check that null loads never read descriptors and retain exact
pointer widths. Both base policies and the original-base read/new-base write
sequence execute under ASan/UBSan; hardware qualifies the clearing policy on
the measured Intel CPU. A public driver test switches SS/RSP, resumes a stack
load and returns using the new physical stack. Fault delivery remains outside
these successful-completion contracts.

The [SYSRET32 qualification record](evidence/x86-sysret32-continuation-2026-10-29.json)
records 14,040 complete native state comparisons and 468,000 portable state
comparisons with independent arbitrary-value choices. It checks selector wrap,
every R11 bit, noncanonical RCX high halves, both paging widths and V5/V6.
The native breakpoint stops before compatibility code; the SDK manifest and
driver retain all eight high-half choices and the privilege-return boundary.
All 1,009 continuation, export, audit and proof regressions passed without skips.

The [return-mode audit correction](evidence/x86-return-mode-audit-2026-10-29.json)
records the two failing pre-fix audit tests and 1,011 passing regressions after
the fix and integration with `dev`. Another 384 paired native/source cases
confirm valid hardware compatibility returns while V6 retains its unchanged
state/RAM refusal. The matrix spans both compatibility code widths, all four
privilege levels, GDT/LDT, accessed-bit variants and the three qualified frame
return encodings. It does not claim compatibility-return source support.

`X64ContinuationIntegerTest` compares complete state and memory with original
native instructions for audited legacy vector transfers, XOR and byte shifts,
64-bit products, scalar exchanges and locked memory updates. It exercises
arbitrary arithmetic flags, preserved upper vector lanes, negative bit-string
indices and unaligned RAM through both C routes at O0/O2 with ASan/UBSan.
LOCK BTS/BTR/BTC retain one sequentially consistent native transaction, including
the old bit returned in CF; an unlocked load/store pair is not equivalent.
The encoding audit rejects inconsistent decoder details and unaudited prefixes.
These tests do not extend the invocation domain to faulting or concurrent runs.
