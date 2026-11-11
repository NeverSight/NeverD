# Continuous protected, compatibility and 64-bit execution

`analysis::X86ModeExecution` retains one full-width `X86ModeState` while the
original instructions change CR0, CR3, CR4, EFER and CS. A program entry is
identified by its full linear address and execution mode. Protected and
compatibility code use the original-byte executor; code64 uses the authenticated
body and validator from the same paged HighC or LLVM-to-C export. Enable
`scheduled_events` for STI and MOV SS, whose source contract requires persistent
interruptibility state.
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
plus debug state and STI/SS/halt state, belong to `X86ModeState`. The non-64-bit
executor receives a temporary low-width view. Explicit write receipts publish
CR2 and CS/SS changes; unchanged low halves never prove that a full-width write
did not occur. Full GDTR/LDTR addresses remain available to compatibility far
transfers and full DR addresses remain available to instruction/event
comparators. Upper halves cannot alias a low-32-bit watchpoint address.

`planX86PagingControlWrite` owns CR0/CR3/CR4 transition checks. Its consumer
validates the destination domain and performs any legacy PAE PDPTE reload
before committing controls. Activation derives LMA from PG and LME; active
IA-32e paging does not load the new PML4 as four legacy PDPTEs. Direct far JMP
loads CS through the existing descriptor owner and can select code64. The
existing paged RETF/IRET owners can return to compatibility dispatch.

The explicitly bound EFER bank admits RDMSR/WRMSR of ECX=C0000080h. SCE is a
separate supplied capability; LME/NXE use the paging feature binding. LMA is
read-only. Reserved bits and changing LME while paging is active produce #GP;
LOCK and privilege checks follow authenticated fetch. Other MSR banks are
explicit refusals. These checks follow [Intel SDM Volume 3A, sections 2.2.1
and 5.1.1](https://cdrdv2.intel.com/v1/dl/getContent/671190).

## Events and retained progress

A non-64-bit instruction returns a typed event request before IDT access.
An illegal control write leaves the original controls and uses that mode's
event owner. Successful writes publish controls first: post-completion #DB
and the next fetch use the committed address space and IDT format. Delivery is
a terminal handoff; a low-width view cannot overwrite an installed code64
handler or a partially stored wide event frame.

Pending instruction and event work travels with the machine. Resume checks the
complete state, controls and features before effects and uses the accepted
instruction's frozen choices, bytes and source pointers. A new execution
context can repair physical backing without replaying completed descriptor or
frame accesses. A paid instruction resumes with zero instruction budget;
event work precedes the next mode lookup and fetch. Software INT completion is
reported once even if its subsequent #DB delivery repeatedly suspends. A
retired lower-owner error retires the outer token too.

Bind MXCSR validity, address width, debug policy and undefined values before
execution. In particular `Legacy.Segments.FullLimitChoice` is required up front,
so a partial stack operation cannot freeze a missing choice that it needs to
finish. Invalid CR8, NMI-blocking and reserved metadata fail before memory or
control effects. V1 `Undefined` slots supply non-64-bit GPR0..7 upper halves;
`X86ModeUndefinedState` supplies R8..15/XMM8..15 values when crossing LMA=0.
A transition solely between compatibility and code64 retains those extended
registers. Neither profile infers unspecified values from the host.

## Admitted scope and evidence

The non-64-bit boundary deliberately admits the existing ordinary scalar,
stack, direct far-JMP, control-register and EFER forms whose state ownership is
complete here. Segment reloads, debug-register instructions, port/string/MPX,
legacy returns, task/system selectors, descriptor queries, fast-system entry,
call gates and VM86 require their separate mode-aware integration. Real/unreal
mode, software SMM/SGX and unbound asynchronous producers remain open. A
missing instruction binding is an explicit result; unsupported semantics never
become a NOP.

The regression stream performs protected32 → compatibility32 → code64 →
compatibility32 → protected32 with original WRMSR/MOV-CR/far-JMP/RETFQ bytes.
The source matrix uses both emitters, both x64 compilers and O0/O2. Its native
counterpart writes SREGS once at initialization and runs uninterrupted to HLT;
no host stage changes select the intermediate modes. It compares architectural
state and the entire physical RAM image. Additional original streams select
legacy/IA-32e #GP, #UD, software INT and activation #DB frames. Separate tests
cover new-space fetch faults, complete CR2, high descriptor tables, accessed-bit
write suspension, zero-budget resume, full-bank changes, high debug addresses,
completion receipts and interrupt shadows.

The [dated evidence](evidence/x86-mode-execution-2026-11-11.json) records the
exact tested source, binaries, reports, generated sources and limitations.
This checkpoint does not declare global x86 completion or authorize the final
LLVM release. The production r6 pin and LLVM gitlink remain unchanged.
