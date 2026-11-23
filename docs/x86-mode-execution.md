# Continuous x86 machine execution

The mixed-mode CPU also executes real-address firmware and virtual-8086
threads. In real-address mode CS.D selects the instruction width. Enabling PE
retains CPL0 and the old code cache until the first protected CS load, including
when the visible selector has a nonzero RPL. Protected task gates and nested
IRET publish TR, LDTR, the incoming register image and each loaded segment.
During a backing stop the continuation identifies the original instruction or
accepted event even when the incoming task has already replaced EIP. Once
the incoming task is installed, its continuation owns the remaining descriptor
loads and completes the original instruction once.

Compatibility CALL and JMP can load a 16-byte IA-32e call gate. A privilege
change reads the incoming full-width stack pointer from the TSS and pushes the
old SS, ESP, CS and return address in eight-byte slots. The stack and branch
target can reside in the upper canonical half. A direct compatibility CALL to
long code uses the flat ESP address and pushes CS and the return offset in
slots of the instruction's operand width. Word and dword IRET and far RET
read the caller's compatibility frame and select the destination code width
from its descriptor.

An optional per-CPU SMM state and separate system RAM bind the KVM save map
selected by the processor’s long-mode capability. A software SGX bank binds imported SECS, TCS and EPCM pages for x64
enclave threads. Machine checks, SMI, INIT, SIPI, NMI and fixed APIC arrivals
are polled at instruction boundaries, including HLT and a zero instruction
budget. See the
[SMM execution profile](x86-smm-return.md), [SGX execution profile](x86-sgx-transfer.md)
and [local APIC delivery](x86-local-apic.md).

`analysis::X86ModeExecution` retains one full-width `X86ModeState` while the
original instructions change CR0, CR3, CR4, EFER and CS. A program entry is
identified by its full linear address and execution mode. Protected and
compatibility code use the original-byte executor; code64 uses the authenticated
body and validator from the same paged HighC or LLVM-to-C export. Enable
`scheduled_events` for STI and MOV SS, whose source contract requires persistent
interruptibility state. Select `notify_cr8_writes` when the exported
program uses MOV CR8. The raw paged step reports the completed write to the
mixed-mode CPU, which updates the bound APIC before polling the next event.
The exported scheduled driver uses the producer callback. [CR8 write completion](x64-continuation-source.md#cr8-write-completion)

Bind exact-type wrappers around the export's paged adapters; keep generated
libraries and borrowed callback contexts alive through resume. Generated
source compiles for the x64 host with Clang or NeverC. Guest code16/code32 does
not add a NeverC 32-bit source target.

This is an internal C++ embedding API in `NeverDX64PagedEventDelivery`, not a
new stable SDK state layout or a general mixed-mode source-export request.
The existing V6/V7 layouts and legacy source/scheduled x64 entry points retain
their contracts. Construct the machine, memory, instruction table and bindings
explicitly; no host CPU state is inferred.

## State and transition ownership

The complete V7 register, segment, descriptor-table, CR2 and SYSENTER banks,
plus debug state, STI/SS interrupt shadows, halt and shutdown, belong to
`X86ModeState`. The non-64-bit executor receives a temporary low-width view.
It reports writes to CR2, DR0..3, CS/SS, data segments and task state. The
mixed-mode owner uses those receipts to publish full-width values even when
the low bits compare equal. Compatibility transfers use full GDTR, LDTR and
TSS addresses. Instruction and event comparators receive full DR addresses.

`planX86PagingControlWrite` owns CR0/CR3/CR4 transition checks. Its consumer
validates the destination domain and performs any legacy PAE PDPTE reload
before committing controls. Activation derives LMA from PG and LME; active
IA-32e paging does not load the new PML4 as four legacy PDPTEs. Direct far JMP
loads CS through the existing descriptor owner and can select code64. The
existing paged RETF/IRET owners can return to compatibility dispatch.

The explicitly bound EFER bank admits RDMSR/WRMSR of ECX=C0000080h. SCE is a
separate supplied capability; LME/NXE use the paging feature binding. LMA is
read-only. Reserved bits and changing LME while paging is active produce #GP;
LOCK and privilege checks follow authenticated fetch. Conventional MCA
registers belong to the optional machine-check bank described below. These checks follow [Intel SDM Volume 3A, sections 2.2.1
and 5.1.1](https://cdrdv2.intel.com/v1/dl/getContent/671190).

## Intel fast system calls

Binding `Legacy.FastSystem` to `IntelSEP` enables original SYSENTER and
SYSEXIT instructions and guest reads and writes of the three SYSENTER MSRs.
SYSENTER from compatibility code installs the full MSR instruction and stack
pointers and selects CPL0 code64. Protected and virtual-8086 callers enter
CPL0 protected32. Real-address SYSENTER raises #GP. SYSEXIT selects CPL3 and
uses EDX and ECX for the compatibility or protected destination. An effective
REX.W in code64 selects the complete RDX and RCX and a code64 destination.

These instructions install fixed segment caches without reading the GDT or
the destination stack. A single-step trap is delivered after the new RIP, RSP,
CS, SS and flags commit. When EIP or ESP is written by WRMSR, the modeled
Intel CPU checks its maximum linear-address width, including when its current
page tables use four levels. The protected guest and compatibility guest use
x64 NeverC or Clang for compiled host bodies. [Intel SYSENTER and SYSEXIT](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf)

## Machine status and paid operand progress

SMSW, LMSW and CLTS reuse the shared scalar owner. LMSW reads a word and
updates CR0.MP/EM/TS. It can set PE in real mode and preserves PE once set.
CLTS clears TS. Full CR0 publishes before post-instruction
#DB, while CR3, CR4, EFER/LMA and active or inactive PDPTE banks stay unchanged.
These operations do not perform a MOV-CR reload. Dword SMSW requires the
explicit `Legacy.MachineStatus = FullCR0` choice for its undefined high word.
Authenticated fetch still precedes CPL/UMIP checks and operand access.

A memory SMSW/LMSW stopped on physical backing retains the accepted instruction
and its paid attempt in a typed machine-status continuation. Resume can use
zero budget; it neither fetches changed opcode bytes nor charges another
attempt. The complete machine bank remains authenticated and accepted options
stay frozen across context reconstruction. Completion, a guest operand fault
or owner refusal retires the instruction marker before event delivery. A later
#DB or fault-entry backing stop has its own event continuation and cannot
replay the completed instruction. This retains one incomplete logical operand
access; it does not preserve partially read physical fragments.

The [machine-status evidence](evidence/x86-mode-machine-status-2026-11-16.json) records 1398 passes and
2 LA57 skips per SDK at `d314e78acf`. Each SDK checks 768 raw original
execution cases and 768 complete-owner comparisons, including repeated CLTS
encodings across parameter combinations. Another 96 original/generated cases
observe committed controls after real far jumps to code64, through both C
emitters, x64 Clang/NeverC and O0/O2. Twenty-four original TF/SS cases and
16 CPL/UMIP/fetch cases compare represented banks, frames and complete RAM.
Native tests use pinned Intel KVM with pre-faulted backing. LMSW cases changing
EM remain KVM comparisons; they do not qualify physical data-watchpoint traps.

Sixteen software operand-repair cases and sixteen software #DB-entry recovery
cases check repeated stops, full-bank rejection, changed opcode memory, frozen
bindings and single attempt/completion accounting. Standalone legacy negatives
also cover retry into guest #PF followed by suspended fault entry. Full paging
regression includes the other consumers of the shared transfer journal.

A test-only follow-up at `c86cda5c49` additionally passes four compiled-source
tests per SDK, counted separately. Its 32 software cases per SDK cover
legacy/PAE paging, guest16/32 and both memory forms through x64 Clang/NeverC at
O0/O2. Changed bindings reject without losing the paid marker; repeated stops
and reconstruction of both RAM and context finish at zero budget. The complete
control/RAM image matches the uninterrupted compiled baseline, including PTE
A/D bits and the explicitly changed opcode byte. Production code is unchanged.

The [post-merge evidence](evidence/x86-mode-machine-status-integration-2026-11-16.json)
records 553 passing x86/source/SDK checks per SDK at `3dbd8d8470`, after merging
`dev`'s SEH changes. A test-only follow-up at `6ce665a8e4` repairs an inherited
SEH assertion for the current unknown-argument trap; all 99 SEH tests and the
real x64 Windows LLVM-to-C corpus case then pass per SDK. The product code is
unchanged between these two sources. These 1306 passing checks are separate
from the preceding full paging qualification and its hardware limitations.

Each SDK's 3324 paged-source and 1940 legacy-source exported files have identical
names and SHA-256 values to the preceding qualification plus its compiled-resume
follow-up. The SEH repair requires the actual first argument and a trap at the
unknown second argument, preserving rejection rather than inventing a value.
NeverC stays host/source x64; other machine owners and final LLVM qualification
remain separate work.

## Selector loads and full caches

MOV in both directions, selector PUSH/POP and all five LxS forms use the existing
`X86LegacySegmentExecution` owner in protected16/32 and compatibility16/32.
Operand offsets keep their declared 16/32-bit rules. Descriptor addresses use
complete GDTR/LDTR bases while LMA is set; legacy tables retain their 32-bit
wrapping domain. A nonnull load replaces the complete cache and zero-extends
its descriptor base even when the low half equals the old base. A non-CS
cache's L/D metadata never changes execution mode.

Bind `Legacy.Segments.NullBasePolicy` before a mixed-mode FS/GS load. A null
load updates selector/unusable and the declared Clear/Preserve base action;
Preserve keeps the complete old base for later code64 observation. Other null
cache fields stay opaque, and KernelGSBase is unaffected. The shared enum is
`X86NullSegmentBasePolicy`; the existing x64 spelling remains an alias.

Compatibility loads also require `Legacy.Segments.DescriptorCanonicality`
before authenticated fetch. Both profiles check the complete eight-byte
extent before any page walk. `SelectorForStart` reports #GP(selector) for a
noncanonical starting address and #GP(0) for only a noncanonical tail;
`SelectorForRange` uses #GP(selector) in either case. Original local P/E-core
streams distinguish these cases, including a missing first-page translation;
runtime code does not infer the profile from its host. This compatibility
measurement alone does not select a code64 policy. Code64 MOV/POP/LFS/LGS/LSS
now have an [independent descriptor qualification](x86-paged-execution.md#code64-descriptor-boundary-profiles);
returns have a [separate code64 qualification](x86-paged-execution.md#code64-return-descriptor-profiles),
and events have an [independent code64 qualification](x86-paged-execution.md#code64-event-descriptor-profiles).

The existing transfer journal retains completed pointer fields, selector
reads and descriptor accessed writes, including aliases with paging A bits
or original instruction bytes. Empty journals retain a paid attempt.
Watchpoint causes and MOV/POP SS inhibition survive backing repair. LSS does
not acquire MOV-SS inhibition. GPR/cache publication waits for the complete
load, and a guest fault retires that journal before retaining event entry.
`SegmentWritten` accompanies the completing step even when immediate #DB
entry suspends; event-only resumes do not repeat it or the completion count.

## Debug-register transfers and complete addresses

MOV-DR in protected16/32 and compatibility16/32 uses the existing shared debug
access owner. Bind `Legacy.Debug` to `DebugRegisters`, supply the complete debug
bank and choose `GeneralDetectFrame` explicitly. The low executor requires each
DR0..3 low32 value to agree with its supplied full-width address before fetch.
DR4/5 aliasing, CR4.DE, CPL, LOCK and GD priority retain the common rules.

Non64 transfers always use dword operands; ignored operand/address prefixes do
not change the width. Reads expose low32 without changing the full address.
Every completed DR0..3 write clears that register's high32, even when its low32
value is unchanged, and leaves all other address banks intact. The explicit
`DebugAddressWritten` receipt identifies that write. The same instruction's
post-completion debug comparator uses the committed full-width bank, including
a DR7 enable/disable change; a high address cannot alias the next low32 PC.

The mode owner publishes the completed write before entering #DB. Its event
continuation authenticates the complete bank and freezes the accepted policy.
IDT/frame backing repair resumes without another instruction attempt, address
write or completion receipt. A GD fault does not complete the MOV or produce
a DR address write receipt. Missing required bindings are explicit refusals;
contradictory low/full banks fail before fetch or state effects.

The [November 16 evidence](evidence/x86-mode-debug-registers-2026-11-16.json) records 569 passing tests and
1 LA57 skip per SDK at `f95dc84266`. Each SDK checks 256 raw original
transfers, 256 owner comparisons, 128 original/generated observations after
real far jumps to code64, 32 immediate-TF cases, 20 GD/encoding cases and
12 CPL3 priority cases. Both C emitters and x64 Clang/NeverC run at O0/O2.
The original streams compare all 16 represented GPRs, debug banks, relevant
segment/control state, frames and every RAM byte. They do not qualify
unrepresented machine banks or complete XSAVE state.

Twenty-four software recovery cases cover completed TF and uncompleted GD at
IDT, first-frame and second-frame backing. Twenty admission cases check the
full/low bank consistency contract. These compare software behavior separately
from uninterrupted original hardware execution. Native fixtures use fresh
pinned Intel VMs, one initial SREGS write and KVM pre-faulted backing. GD/TF/SS
setup uses real guest instructions after releasing lazy KVM MOV-DR interception.

A test-only follow-up at `4712c53af6` adds explicit Intel and KVM pre-fault
admission; unavailable native capability skips at the caller before observation.
All eight new groups pass again per SDK, counted separately from the full
qualification. The software recovery/admission groups remain independent of
those native checks. No production code changes between these two commits.
At that checkpoint the production LLVM pin was r6. The software SMM and SGX
profiles at the beginning of this document describe the current implementation.

## Events and retained progress

A non-64-bit instruction returns a typed event request before IDT access.
An illegal control write leaves the original controls and uses that mode's
event owner. Successful writes publish controls first: post-completion #DB
and the next fetch use the committed address space and IDT format. Delivery is
a terminal handoff; a low-width view cannot overwrite an installed code64
handler or a partially stored wide event frame.

Pending instruction and event work travels with the machine. Resume checks the
complete state, controls and features before halt/shutdown/budget exits or
effects and uses the accepted
instruction's frozen choices, bytes and source pointers. A new execution
context can repair physical backing without replaying completed descriptor or
frame accesses. A paid instruction resumes with zero instruction budget;
event work precedes the next mode lookup and fetch. The `InstructionCompleted`
receipt and completion count report software INT once even if its subsequent
#DB delivery repeatedly suspends. A
retired lower-owner error retires the outer token too.

Bind MXCSR validity, address width, debug policy and undefined values before
execution. In particular `Legacy.Segments.FullLimitChoice` is required up front,
so a partial stack operation cannot freeze a missing choice that it needs to
finish. Invalid CR8, NMI-blocking and reserved metadata fail before memory or
control effects. V1 `Undefined` slots supply non-64-bit GPR0..7 upper halves;
`X86ModeUndefinedState` supplies R8..15/XMM8..15 values when crossing LMA=0.
A transition solely between compatibility and code64 retains those extended
registers. Neither profile infers unspecified values from the host.

A fault or single-step trap from a compatibility-mode system instruction uses
those same GPR0..7 witnesses before the 64-bit IDT owner builds its frame.
For a reserved-bit WRMSR to IA32_MC1_CTL2 this includes the pre-fault stack
pointer, the original instruction address and the flags with RF set. A
retired RSM uses the complete CPU image restored from SMRAM for its trap.

## Selected external IRQ/NMI

`interrupt(State, Kind, Vector)` accepts a producer-selected maskable IRQ or
vector-2 NMI at an instruction boundary. The machine supplies the saved PC and
STI/SS/NMI inhibition; the caller cannot replace them with event inputs. The
committed LMA selects the legacy or IA-32e IDT/frame owner. The old next PC may
already exceed its segment limit or be noncanonical: an accepted event precedes
that fetch. IRQ vectors 2 and 14 retain IRQ classification rather than acquiring
NMI blocking or a page-fault error code/CR2 write.

A deferred or refused input preserves the complete CPU, control cache and RAM.
Compatibility entry applies the bound GPR0..7 high-half witnesses to a trial
bank, including the saved old RSP and same-level stack address, then publishes
it only after acceptance. R8..15 and XMM8..15 remain intact across LMA=1. An
accepted event clears halt and interrupt shadows before accessing its frame;
NMI blocking survives backing pauses. An explicit NMI-under-STI choice is
required when STI is the deciding blocker. A halted CPU must be CPL0 with no
STI/SS shadow.

`EventAccepted` reports consumption of the original input exactly once across
its entire continuation, including secondary exceptions and owner refusals.
Resume it with `run` and zero instruction budget after repairing physical
backing. Pending paid or unpaid instructions and event work reject a new IRQ
or NMI before effects. The continuation authenticates the complete bank and
uses frozen options. A CPU in shutdown accepts INIT when startup state is
bound. The embedding can also supply a reset machine. A protected-mode IDT task gate
uses the bound task-switch policy and preserves the incoming task bank across
a backing stop.

Callers supply arrival timing. `run` with an `X86ModeInterrupts` bank arbitrates
latched machine checks, SMI, INIT, SIPI, NMI and fixed local APIC vectors.
`interrupt` accepts an already selected IRQ or NMI. The embedding supplies
error records, controller arrivals and startup signals.

### Conventional machine checks

Bind `X86ModeState::MCA` to the virtual CPU's banks and optional global control
register. At an instruction boundary `X86ModeInterrupts::MachineCheck` carries
one bank number, MCi_STATUS, MCi_ADDR, MCi_MISC and MCG_STATUS. A signaled
uncorrected error enters vector 18 ahead of SMI, NMI and fixed interrupts,
including with IF clear or the CPU halted. A second signaled error while MCIP
is set, or a signaled error with CR4.MCE clear, enters shutdown. Corrected
records and nonsignaling uncorrected records update the bound banks. An
existing uncorrected record has priority over a corrected record. Disabled
reporting consumes the input once. Malformed reports remain available for
correction.

Original RDMSR and WRMSR instructions read and acknowledge these banks across
code widths. This model binds a single Intel KVM CPU with conventional MCi
registers. With `MCA.CMCI` enabled, MCG_CAP advertises the per-bank
IA32_MCi_CTL2 registers. Each bank binds its writable threshold width. A bank
with a zero width reads back zero. An injected uncorrectable-no-action record
can signal CMCI regardless of CR4.MCE. An already formed corrected KVM record
updates the bank without a threshold notification. A device submitting one
corrected occurrence sets `CountCorrectedError`. The CPU then increments
the architectural count, retains its sticky overflow bit and signals CMCI
when the programmed threshold equals that count.

`X86ModeInterrupts::LocalVectorCMCI` supplies the local APIC vector, delivery
mode and mask from its CMCI LVT register. Fixed delivery enters the existing
IRR, priority and EOI path. NMI and SMI delivery use the CPU's corresponding
latches. Masking the LVT still allows the bank record to commit. An unavailable
or invalid route retains the report and the previous bank for retry. INIT
masks the bound LVT. Cross-CPU bank sharing and the external devices that
generate errors require separate platform state.
[Linux KVM machine-check injection](https://github.com/torvalds/linux/blob/v6.18/arch/x86/kvm/x86.c)
[Intel SDM Volume 3B, machine-check architecture](https://cdrdv2-public.intel.com/671427/253669-sdm-vol-3b.pdf)

### Processor initialization and startup

Bind `X86ModeState::Startup` with CPUID.01H:EAX and the logical processor's
bootstrap role. A routed INIT clears the register and local interrupt state,
preserves the CR0 cache-disable bits, and installs real-address CS with base
0xffff0000 and IP 0xfff0. DR0–DR7 return to their initial values. The x87
and vector banks, SYSENTER MSRs, STAR, KernelGSBase and machine-check records
survive INIT. An application processor waits with its instruction budget
intact until a SIPI selects CS.base = vector × 4096 and IP = 0. A running
processor discards a SIPI. SMM latches INIT until RSM. When INIT interrupts
an enclave, the CPU first saves the interrupted thread to its SSA. Startup
keeps that enclave's EPC pages and ownership available for a later ERESUME.
This profile accepts one logical processor outside VMX operation.
[Linux KVM INIT and SIPI](https://github.com/torvalds/linux/blob/v6.18/arch/x86/kvm/lapic.c)
[Intel SGX section 40.9](https://cdrdv2-public.intel.com/843859/332831-sdm-vol-3d-dec-24.pdf)

## Qualification at `6f81b8559b`

| Build | Machine execution | Paging | C source regression | Python SDK integration |
| --- | ---: | ---: | ---: | ---: |
| Published r7 SDK | 243/243 | 853/855 | 253/253 | 4/4 |
| Locally built LLVM SDK | 243/243 | 853/855 | 253/253 | 4/4 |

Entries count passed and registered cases. The JSON records the LA57 skips,
both hybrid-core replays, binary and XML hashes, and KVM save images and fault
frames. [Qualification record](evidence/x86-mode-smm-sgx-events-2026-11-21.json)

After merging 77 subsequent `dev` commits at `bf4da7c3f9`, both SDKs repeated
the same complete matrix: 243/243 machine-execution cases, 853/855 paging cases
with two LA57 capability skips, 253/253 generated-C source cases and 4/4
Python SDK integrations each. The opposite-core selections also passed. The
HighC and LLVM-to-C VMRUN exports kept their exact source hashes.

After the later `dev` integrations at `d45fe8b1fb` and `dbed3c1ba2`, both
published r7 and local LLVM SDKs repeated the complete matrix at each source.
At `dbed3c1ba2` each SDK passed 243/243 execution, 853/855 paging, 48/48
opposite-core execution, 4/4 opposite-core paging, 253/253 generated-C and 4/4
Python integration cases. The only skips require LA57, which the bound CPU
does not expose. The portable x64 NeverC compiler emitted native
`vmrun.context`, `vmread`, `vmwrite`, `rsm.context` and
`enclu.transfer.context` calls; the installed r7 backend produced the native
VMRUN, VMREAD, VMWRITE, ENCLU, XRSTOR64 and RSM opcodes. It completed 200
returning VMRUN contexts in QEMU TCG across HighC/LLVM-to-C and O0/O2. Its
i386, i686 and x32 target requests failed explicitly.

The Linux Actions job built the pinned x64 NeverC revision and passed 27
required software cases. The signed Intel simulation enclave completed three
calls, including two nested exception repairs. Both Intel macOS runners and
the Linux runner enumerated zero EPC sections.
[Hosted software and hardware probes](https://github.com/gmh5225/test_mac_intel/actions/runs/44886176997) ·
[Intel SDK simulation](https://github.com/gmh5225/test_mac_intel/actions/runs/44873573672)

## Earlier qualification records

The November 11 regression stream performs protected32 → compatibility32 → code64 →
compatibility32 → protected32 with original WRMSR/MOV-CR/far-JMP/RETFQ bytes.
The source matrix uses both emitters, both x64 compilers and O0/O2. Its native
counterpart writes SREGS once at initialization and runs uninterrupted to HLT;
no host stage changes select the intermediate modes. It compares architectural
state and the entire physical RAM image. Additional original streams select
legacy/IA-32e #GP, #UD, software INT and activation #DB frames. Separate tests
cover new-space fetch faults, complete CR2, high descriptor tables, accessed-bit
write suspension, zero-budget resume, full-bank changes, high debug addresses,
completion receipts and interrupt shadows. External input tests cover all five
modes, masked/refused inputs without effects, compatibility stack witnesses,
IDT/frame suspension, secondary faults, shutdown and task-owner refusal. Native
STI/HLT wake and repeated NMI/IRETQ streams retain a single initialization,
checking event frames, CS/SS caches, controls, inhibition and ordinary RAM.

The [continuous-mode evidence](evidence/x86-mode-execution-2026-11-11.json)
and [external-input evidence](evidence/x86-mode-interrupts-2026-11-11.json)
record the exact tested source, binaries, reports, generated sources and
limitations. The external-input qualification passes 476 primary cases per
SDK, with 29 alternate-core replays and five upstream integration checks per
SDK reported separately; none fails or skips.
This checkpoint does not declare global x86 completion or authorize the final
LLVM release. The production r6 pin and LLVM gitlink remain unchanged.

The [selector-transfer evidence](evidence/x86-mode-segments-2026-11-11.json)
records the full lower paging, paged source, legacy source, instruction boundary
and SDK qualification. Each SDK passes 1,342 primary cases, with
2 LA57 hardware skips; 48 mode and 13 control passes on the other core are
counted separately (1 alternate LA57 skip). Original comparisons cover 144 MOV/LxS
metadata cases, 32 null-base observations after real far jumps into generated
code64, and 16 descriptor faults per primary core. Every independent stream
starts with a fresh VM/vCPU and one initial SREGS write. The complete CPU/RAM
comparison remains strict; this avoids stale TLBs between reset RAM images.


### Integration with current dev (2026-11-16)

The [integration evidence](evidence/x86-mode-segment-integration-2026-11-16.json) records a fresh rebuild at
`de0eabb090` after merging upstream `eb173c9ec7`. Each SDK passes
1,058 cases across the full HighIR control-flow, paged-source, legacy-source,
instruction-boundary and SDK suites, with no skips. These counts are separate
from the November 11 full paging qualification above.

The replay exposed a shared HighIR address-index regression: two reserved hash
keys collided with valid synthetic continuation return labels. Restoring the
original complete address domain fixes the generated validator crash; explicit
label-retention and branch-result tests cover those addresses. The expanded
host-C suite also needed its POSIX FMA fixture to link the math library.

Upstream independently advanced the LLVM source gitlink to `09ff60dd1ddb`; this
merge preserves that change. The development SDK remains the existing intrinsic
snapshot and the production SDK remains r6. This integration creates no LLVM
release and does not close the outstanding x86 qualification boundaries.


### Retained code64 segment instructions (2026-11-16)

The long-mode branch now retains MOV/PUSH/POP segment and LFS/LGS/LSS across
physical backing stops, including an unpaid stop before the first completed
access. Full-mode reconstruction keeps the original instruction and excludes
new event selection until it completes or faults. Resume preserves aliased
operands, original bytes and paging effects, and charges the attempt once.
The [runtime contract](x86-paged-execution.md#resuming-a-suspended-segment-transfer)
and [evidence](evidence/x86-code64-segment-continuation-2026-11-16.json) record 524 passing cases and 1 LA57
skip per SDK. This closes code64 segment progress; remaining mode-aware
owners, event descriptor-tail qualification, real/unreal/VM86 composition, software
SMM/SGX and unbound asynchronous producers remain separate work. NeverC remains
host x64 and the production LLVM release pin remains r6.


### Code64 descriptor fault profiles (2026-11-16)

The long-mode branch accepts the explicit `TailGPZero` variants for selector
loads, using the same shared descriptor owner as the plain paged context.
Pending full-mode work retains its original profile even when a new context
supplies the opposite choice. The [boundary qualification](evidence/x86-code64-descriptor-boundaries-2026-11-16.json)
records 529 passing tests and 1 LA57 skip per SDK, including sixteen
full-mode profile comparisons. This does not expand NeverC source targets,
qualify return/event descriptor-tail choices, or close the remaining mode and
machine-owner work. The production LLVM release pin remains r6.

### Code64 return descriptor profiles (2026-11-16)

IRET and RETF now expose the independently measured `TailGPZero` variants.
Lower return continuations retain that choice, and full-mode pending work keeps
its accepted options across backing repair. New caller options apply after the
accepted work. The [return descriptor evidence](evidence/x86-code64-return-descriptors-2026-11-16.json) records
535 passing tests and 1 LA57 skip per SDK, including 64 full-mode profile
comparisons. Guest operand widths do not add NeverC source targets. Event
descriptor-tail choices and the remaining full-machine work are still open;
production LLVM remains r6.

### Code64 event descriptor profiles (2026-11-16)

`Long.EventDescriptorCanonicality` selects the event descriptor's tail error
code and retains EXT. Full-mode accepted work preserves it through IDT backing
and event-budget stops even when a reconstructed caller supplies the opposite
choice. The [event descriptor evidence](evidence/x86-code64-event-descriptors-2026-11-16.json) records 543
passing tests and 1 LA57 skip per SDK, including 96 full-mode comparisons.
At that checkpoint, event accessed-update CR2/locked transaction and split-lock
qualification remained open, alongside the remaining mode and machine-state owners. NeverC stays
host x64; no final LLVM release or production pin change is included.

### Code64 event accessed updates (2026-11-16)

The [accessed-update qualification](evidence/x86-code64-event-accessed-2026-11-16.json)
includes 96 full-mode
recoveries per SDK. Accepted entry keeps the fresh descriptor cache image even
when a completed frame store overwrites the descriptor before a later backing
stop. Pending software entry publishes its one completion after successful
resume, with no new instruction attempt; external events retain their original
acceptance. Eight additional debug-receipt cases preserve the accessed-write
match through repeated stops. The complete affected matrix passes 551 tests
with 1 LA57 skip per SDK. Cross-line updates, remaining machine owners and
physical SGX still gate the final LLVM release; NeverC remains host/source x64.
