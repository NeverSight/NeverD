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

Set `"follow_internal_calls": true` to compose CALL and RET inside the selected
region. The default is `false`, which stops at every CALL and RET. The composed
driver follows the actual post-instruction PC, including indirect calls and
returns through callback-modified stack memory. It uses no host call stack and
assumes no relation between a return and the most recent call. Retained frames,
stack replacements and recursive transfers therefore keep their machine
semantics and share the same instruction budget.

An external CALL or RET still reports status 1 or 3, even when it consumes the
last budget unit. An internal transfer with no budget left reports status 9
before executing its target. Native requests, invalid state, exceptions and
all context transitions remain stopping boundaries even when their target is
selected. The manifest records `call_return_policy` as `stop` or
`follow-selected-targets`. Selecting this dispatch policy adds no instructions,
callee bodies, memory mappings or runtime bindings to the request.

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
| 10 | V7 fast-system-call context installed; handler has not executed |
| 11 | V8 user-interrupt return completed; target and pending delivery have not executed |
| 12 | V9 FRED return completed; target and pending delivery have not executed |

The invocation domain is CPL0, 64-bit entry with valid descriptor caches,
CET/FRED disabled, successful sequential ordinary RAM accesses and immutable
original instruction bytes. It excludes MMIO, concurrent writers, asynchronous
events and faulting execution. Canonical IRET (`CF`) and RETF (`CB`, `CA imm16`)
with no prefix, `66` or `48` select 32-, 16- or 64-bit frame fields and can
produce CPL0–3 long-mode or compatibility contexts;
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

### Intel fast system calls (V7)

Select `"profile": "intel-fast-system-v7"` and supply
`"sysenter_msr_address_bits": 48` or `57`, in addition to the MXCSR mask and
active paging width. This declares an Intel processor with SEP support. AMD
long-mode SYSENTER/SYSEXIT are outside this profile. The processor's MSR address
width is independent of active paging; if segment-base MSRs are also bound,
the two processor-width declarations must agree.

V7 exports `neverd_x64_state_v7` and `neverd_x64_continue_v7`. Its 2528-byte
layout contains the unchanged V6 prefix, followed by 64-bit `SysenterCS`,
`SysenterESP`, `SysenterEIP` and `ReservedV7` at offsets 2496, 2504, 2512 and
2520. V6 requests still export their original 2496-byte layout and symbols.
Initialize the three live MSR cells from the original machine. CS has 32
readable bits; EIP and ESP must be canonical at the declared processor width.
An invalid initial cell refuses the step without changing state or RAM.

`RDMSR`/`WRMSR` selectors `0x174`, `0x175` and `0x176` use these exact cells.
CS bits 16–31 are readable/writable even though only its low selector enters
the transfer; writes ignore bits 32–63 and reads return zero there. EIP/ESP
writes require processor-width canonical addresses. These are guest state
updates and never write the executing host's system-call MSRs. The rules
follow [Intel SDM Volume 4, Table 2-2](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf).

Canonical `SYSENTER` (`0F 34`) accepts a valid CPL0–3 64-bit or flat
compatibility source context, with either compatibility CS.D value.
Nonconforming CS requires DPL equal to CPL; conforming CS permits DPL below
or equal to CPL. CPL still comes from the live CS selector's RPL.
It installs the live entry PC and SP, CPL0 flat CS/SS caches, clears IF/RF, and
preserves the other admitted flags and registers. It returns status 10 before
the handler executes, including when the handler is selected in the manifest.
The target PC must be canonical for active paging. SYSENTER does not access
its new stack; its ESP cell retains the processor-width MSR requirement.

Canonical `SYSEXIT` (`0F 35`) and `SYSEXITQ` (`48 0F 35`) require CPL0 and
install CPL3 CS/SS caches from the live CS MSR. The former takes EIP/ESP from
EDX/ECX and supplies the same eight arbitrary legacy GPR high halves as SYSRET;
the latter takes the full RDX/RCX and checks both against active paging.
Both clear RF, preserve the other admitted flags and stop with status 6 before user code.
Unprefixed `SYSEXIT` also accepts flat compatibility entry at CPL0; its operand
semantics remain 32-bit with either CS.D value. `SYSEXITQ` requires 64-bit entry.
Compatibility entry requires CS.base=0 and the whole two-byte instruction
within CS.limit, so the exported address is also the architectural code offset.
Other steps retain their earlier mode guards. A driver can therefore return
through SYSEXIT to a selected SYSENTER stub and explicitly resume it; each
transition still exits before executing its destination.
Neither instruction reads descriptors or the old/new stack. Selector TI bits
and 16-bit wraparound remain significant; null entry selectors refuse before
any write. These transitions follow the SYSENTER/SYSEXIT instruction entries
in [Intel SDM Volume 2](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf).

Other instructions retain V6's CPL0 entry requirements. SYSENTER, SYSEXIT and
SYSRET accept RF, all IOPL values and VIF/VIP on entry. TF remains excluded
because debug-trap delivery is unowned. Other instruction domains are unchanged. Nonflat
compatibility code, legacy modes, faults, CET/FRED, asynchronous events and SMM
restoration are not added by V7. An opaque SYSENTER lift alone is not a proof of
an ordinary C ABI or a full native register continuation.

RETF reads IP/CS alone for a same-privilege return and retains SS. For a
return to an outer privilege level, its unsigned imm16 skips parameter bytes
before saved RSP/SS and also adjusts the restored RSP. The transition preserves
NMI blocking and flags except for ordinary RF clearing. RF, all IOPL values
and VIF/VIP are admitted on entry; TF remains excluded. It consumes one driver step and stops even
when the return target is selected; a returned CPL1–3 context cannot reenter
this supervisor profile. Frame fields use the operand width independently of
the target stack width. A narrow saved IP or SP is zero-extended before the
target's stack-size rules apply. Same-level RETF advances the original full
stack pointer by two operand-sized fields before target-width cleanup.

All three IRET operand sizes pop five fields when execution begins in 64-bit
code, including a same-CPL return. Word IRET replaces the low FLAGS bits and
retains AC/ID/VIF/VIP from the input and clears RF; dword/qword IRET restore
those bits from the frame. Both accept input RF, all IOPL values and VIF/VIP;
input TF remains excluded. RF clears at instruction entry, as described in
[Intel SDM Volume 3B, section 20.3.1.1](https://cdrdv2-public.intel.com/929360/253669-093-sdm-vol-3b.pdf).
The earlier statement that word IRET retained RF was overbroad: its qualified
entry domain required RF=0. The extended-entry regression covers RF=1 directly.
The [narrow-frame qualification](evidence/x86-narrow-frame-returns-2026-10-30.json)
compares both widths with original instructions and checks exact frame reads
against inaccessible guard pages.

IRET/RETF have no unconditional empty undefined-output certificate: a live
return descriptor can select compatibility mode, where the legacy GPR high
halves are arbitrary. V6 now owns either a long-mode or compatibility target.
Long-mode code requires CS.L=1/CS.D=0. Compatibility code permits either CS.D
value, returns the low 32 RIP bits as the target offset, and checks that offset
against the effective CS limit. This profile requires the raw frame RIP to be
canonical at the declared paging width before truncation; other raw high-bit
patterns remain outside its qualification. The environment interprets the
returned offset with the returned CS base and does not execute the target in
this step.

IRET and outer RETF require non-null SS for compatibility targets. A 32-bit
stack restores ESP. A 16-bit stack replaces SP and retains the original
ESP[31:16], including when frame pops cross a 64 KiB boundary. RETF performs
parameter cleanup at the target stack width without carrying into preserved
bits. Same-privilege RETF retains its original SS/cache and applies that cache's
stack width. IRET unblocks NMIs; RETF preserves their blocking state.

Successful compatibility returns consume all eight arbitrary-value slots for
the legacy GPR high halves, including RSP; all low halves and R8-R15 follow the
instruction's defined state. Long-mode returns and rejected invocations do not
apply these choices. Their manifest records the conditional profile transition
instead of inventing an original LowIR operation index. This supplies no
ordinary LowIR or native-preservation certificate. Earlier generic audits
incorrectly treated a checked profile condition as an instruction-wide fact.
The stack rules follow Intel SDM Volume 3A section 7.14.3 and the IRET/RET
entries; the retained middle ESP bits also match Linux's
[documented espfix requirement](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/espfix_64.c).
The [qualification record](evidence/x86-legacy-compatibility-returns-2026-10-30.json)
includes native and portable state/RAM matrices and the processor-dependent
raw-RIP experiment behind the canonical-input restriction.

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

### Intel user interrupts (V8)

Select `"profile": "intel-user-interrupt-v8"` with V7's required fields and
the boolean `"uiret_restores_uif"`. This binds the original CPU's
`CPUID.07H.01H:EDX.UIRET_UIF[17]`; omitting it is an error, including for the
older behavior (`false`). UINTR must be enabled, with execution outside an
enclave or RTM transaction. The other nonfaulting, no-asynchronous-delivery
requirements still apply.

The separate `neverd_x64_state_v8` is 2544 bytes. It contains the unchanged V7
prefix, followed by one-byte `UIF` and `MonitorActive` at 2528 and 2529, and
14 reserved bytes. Initialize both flags to 0 or 1. The environment retains
the monitor's range and pending user-interrupt state; an inactive monitor
cannot observe its old range. No host UIF, monitor or segment is changed.

Canonical UIRET (`F3 0F 01 EC`) reads exactly three 64-bit stack words: RIP,
RFLAGS and RSP. It checks RIP against active paging, restores the twelve
specified flag bits, updates UIF under the declared CPU rule and clears
monitor validity. With the enhancement, saved bit 1 selects UIF while actual
RFLAGS bit 1 remains set. Restored RSP need not be canonical until accessed.
Segments, NMI blocking, other GPRs and vector containers are preserved. Status
11 stops even when the target is listed, before executing it or delivering a
pending user interrupt.

The inherited IRET, SYSRET and SYSEXIT paths also clear `MonitorActive`, as
does RETF when it changes CPL. A same-CPL RETF and SYSENTER preserve it.
The return owners invoke one version-aware state writer, so earlier layouts
receive no writes beyond their state storage. Invalid entries and frames leave
the monitor unchanged. This follows the instruction interactions listed under
UMWAIT in the same SDM.

CLUI, STUI and TESTUI share the live UIF cell. TESTUI copies it to CF and
clears the other arithmetic flags. Their ordinary RF clearing is retained;
TF must be zero because debug-trap delivery is outside this profile. These
three steps and UIRET accept valid CPL0–3 64-bit code, including conforming
segments. Other instructions keep V7's entry domain. Faulting stack accesses,
shadow-stack checks, interrupt delivery and RTM aborts remain unsupported.
These rules follow [Intel SDM revision 090, UIRET and user interrupts section
9.7](https://cdrdv2-public.intel.com/874240/325462-090-sdm-vol-1-2abcd-3abcd-4.pdf).

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
confirmed valid hardware compatibility returns while V6 retained its then-current
state/RAM refusal. The matrix spans both compatibility code widths, all four
privilege levels, GDT/LDT, accessed-bit variants and the three qualified frame
return encodings. That historical record predates the compatibility-return
source implementation described above.

`X64ContinuationIntegerTest` compares complete state and memory with original
native instructions for audited legacy vector transfers, XOR and byte shifts,
64-bit products, scalar exchanges and locked memory updates. It exercises
arbitrary arithmetic flags, preserved upper vector lanes, negative bit-string
indices and unaligned RAM through both C routes at O0/O2 with ASan/UBSan.
LOCK BTS/BTR/BTC retain one sequentially consistent native transaction, including
the old bit returned in CF; an unlocked load/store pair is not equivalent.
The encoding audit rejects inconsistent decoder details and unaudited prefixes.
These tests do not extend the invocation domain to faulting or concurrent runs.

## Intel FRED return profile V9

`intel-fred-return-v9` enables the canonical ERETS and ERETU encodings at
CPL0 in 64-bit mode, with FRED enabled and CET/RTM disabled, outside VMX
non-root execution. It is a separate
execution domain: other instructions request native execution. It requires
`mxcsr_mask` and `paging_address_bits`; SYSENTER and UIRET feature bindings
are not accepted. It inherits the nonfaulting ordinary-RAM, immutable-code and
disjoint-state requirements. Descriptor caches must describe a valid entry
context, and no debug exception may be pending at entry.

The V9 state retains the complete V8 layout and appends `FredConfig` at offset
2544, `STIBlocked` at 2552, `PendingDebug` at 2553 and six reserved bytes.
Its total size is 2560 bytes. The low two FredConfig bits hold the current
stack level; other bits are preserved. Event bytes are booleans. A successful
step returns status **12 (FredReturn)** and always stops before the target or
pending-event delivery, consuming exactly one driver step.

Both instructions skip the error-code qword without reading it, then read
exactly five qwords: RIP, augmented CS, RFLAGS, RSP and augmented SS. Neither
the restored RSP nor the event-information area above the return words is
accessed. A 64-bit RIP must be canonical at the declared paging width. Invalid
frames reject before any state or descriptor write; this is invocation refusal,
not an emulated CPU exception.

ERETS checks that the selectors match the current CS/SS, validates the reserved
fields, restores the complete allowed flags and RSP, and lowers the stack level
to the minimum of its old and saved values. CS/SS caches and GS bases stay
unchanged. ERETU requires stack level zero and target CPL3. Its standard
64-bit or compatibility selector pair installs flat CS/SS caches without
consulting descriptor tables; other selector pairs use the shared GDT/LDT
descriptor owner, including ordered accessed-bit writes. It swaps GS.base with
the kernel GS-base MSR, invalidates the UMONITOR state, and preserves the DS/ES/FS/GS
selectors and other caches, even when their DPL is below 3.

On a compatibility return, ERETU clears the upper 32 bits of the restored RIP
and RSP, including for a 16-bit default code segment. The truncated RIP must
fit the target CS limit; a standard flat compatibility return always fits.
L=1/D=1 code descriptors and nonstandard null CS/SS selectors reject before
any write. The target mode and its segment caches are returned without running
that target or changing the host execution mode.
For compatibility code, `PC` is the returned IP offset; the environment uses
the returned CS base to interpret its linear code address.

The other seven legacy GPR high halves become explicit architectural choices
under SDM Volume 1 section 3.4.1.1. Slots 0,1,2,3,5,6,7 select RAX, RCX, RDX,
RBX, RBP, RSI and RDI high halves, respectively. The low 32 bits and R8-R15
remain intact. Slot 4 is unused: ERETU explicitly loads all 64 RSP bits after
clearing the upper half. These choices occur only on successful compatibility
returns; 64-bit returns and rejected frames neither apply them nor alter the
input slots. The manifest records `after_profile_transition` as
`successful_compatibility_return`, with no invented original `after_operation`
index. Ordinary lifting still has no complete arbitrary-output certificate
for these instructions.

Saved SS controls conditional NMI unblocking and single-step pendency. ERETS
may establish STI blocking when requested with restored IF set, provided it
was not already active at entry; prior STI blocking expires. ERETU does not
establish it. An entry TF or the saved pending-debug condition with restored TF
produces PendingDebug. No debug/NMI/interrupt handler runs in this step.

These transitions follow Intel SDM 325462-090, ERETS/ERETU and Volume 3A
sections 8.4.1-8.4.2. CET checks, faults, virtual-NMI
controls, physical FRED execution and subsequent event delivery remain
unqualified. Ordinary lifting refuses both forms without this state owner.
