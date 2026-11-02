# x86 paged RAM and faultable IDT delivery

`X86Paging`, `X86PagedMemory` and `X64PagedEventDelivery` provide an explicit
owned-RAM execution path. They are internal C++ embedding interfaces, separate
from the existing generated-C continuation's successful-host-RAM contract.
Guest linear addresses are never cast to host pointers.

## Ownership

| Owner | Responsibility |
| --- | --- |
| `translate/X86/X86Paging` | CPU-bound paging controls, address translation, page permissions, A/D updates and architectural fault metadata. |
| `translate/GuestMemoryRuntime` | Owned physical bytes, backing permissions, extent validation and physical access failures. |
| `translate/X86/X86PagedMemory` | Whole-operand read/fetch publication and physical store preflight across discontiguous pages. |
| `analysis/arch/x86_64/X64ContinuationSegments`, `X64ContinuationEvents`, `X64ContinuationDelivery` | Shared descriptor validation, event admission, privilege changes and stack-frame semantics. |
| `analysis/arch/x86_64/X64PagedEventDelivery` | Concrete evaluation of those shared semantics, recursive delivery faults, #DF classification and shutdown reporting. |

The source writer retains its no-fault refusal behavior. The concrete event
writer uses the existing exact LowIR scalar evaluator and binds memory accesses
to paged RAM. It does not implement another descriptor/frame algorithm.
Descriptor accessed bits are published before later faultable accesses; the
no-fault source path retains its equivalent alias forwarding.
The faultable same-privilege path reads a new stack from the TSS only when a
privilege change or IST actually selects it. It never rereads the code
descriptor as an unused conditional operand, including when setting its A bit
changes an aliased page-table entry.

## Paging and operands

Supported walks include disabled paging, legacy two-level i386 paging, PAE,
four-level IA-32e paging and five-level IA-32e paging. Page sizes are 4 KiB,
2 MiB, 4 MiB and 1 GiB, subject to the supplied capabilities. PAE consumes
the cached PDPTE bank. Its separate reload validates all four entries before
replacing that bank; invalid present entries produce #GP, not #PF.

The checks cover canonical addresses, reserved physical-address bits, large-page
alignment, PSE-36, PAT availability, NX, U/S, R/W, CR0.WP, SMEP, SMAP, PKRU and
PKRS. Implicit system-table reads keep their supervisor access identity at CPL3.
Faults report the vector, error code and optional new CR2. Missing physical
backing is an embedding failure and never fabricated as a guest page fault.
Canonicality is checked for the whole operand before the first walk: a
noncanonical tail reports #GP/#SS even if its canonical first page is absent.

Loads and instruction fetches preserve the entire output on failure. Stores
translate and validate every physical fragment before publishing operand data.
A fault on a later page retains earlier page-table A/D updates. Physical RAM
regions use read/write backing permissions; guest execution permission is owned
by paging. Stores that alias their own paging structures use the translations
established before operand publication.

## Faults during event delivery

The paged V6 entry delivers ordinary IA-32e IDT events, including faults while
reading the IDT, GDT/LDT or TSS and while writing the handler frame. Gate,
selector and TSS errors retain their vector and selector/IDT error-code identity.
Software INT vectors remain benign first events regardless of their numeric
vector. Contributory/page-fault combinations select #DF according to the
architectural exception classes; another relevant fault during #DF reports
shutdown. No reset or instruction skipping is substituted for shutdown.

CR2 changes on every detected page fault, including one during #DF delivery.
Already completed descriptor, page-table and frame writes remain visible.
NMI blocking survives a fault during NMI delivery. The executor stops at handler
entry. A #DF frame chooses the original saved PC from undefined program state;
that choice does not make the interrupted program restartable.

The specifications are Intel SDM volume 3A, revision 093, chapters 5 and 7,
and the INT instruction's IA-32e entry algorithm in volume 2. See the
[official manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Qualification and remaining integration

Build `NeverDX86PagingTests`, `NeverDX64ContinuationSourceTests` and
`NeverDX64ContinuationExportTests` with `scripts/build.py`. The paging target
checks software contracts and executes independent original MOV/fetch
instructions in KVM. It compares operand results, complete affected data pages,
page-fault error codes, CR2, successful A/D updates and ordinary exception frames.
Nested cases exercise absent/invalid #PF gates, faults reading the #PF/#DF gates,
partially written frames, IST recovery and KVM shutdown. Undefined #DF saved
program state and implementation-dependent fault-time A updates are not equated.
Unavailable KVM or LA57 is an explicit skip.

The [November 1 evidence](evidence/x86-paged-memory-2026-11-01.json) records both
SDK builds, 820 independent CPU comparisons per SDK (including 240 event
comparisons within that total), and the existing 440 continuation regressions.
The only hardware-mode skip is LA57 on the local CPU. The same 282 generated
C/header/manifest artifacts are byte-identical across the SDKs.

This does **not** bind the existing generated HighC/LLVM-to-C instruction
dispatchers to paged RAM. That integration still needs an explicit memory
context, instruction commit boundaries and restart state. It must also preserve
partial completion for REP, gather/scatter, XSAVE and related instructions;
the failure-atomic operand primitive alone is not their instruction model.

Other remaining domains include legacy i386 segment/task-gate execution,
SMI entry, SGX AEX/enclave execution, APIC/device arrival, MMIO, EPT/NPT/HLAT,
LAM/LASS and CET/FRED entry. Some extreme descriptor-address overflow guards
still refuse their invocation. The CPU binding rejects active unsupported
controls rather than treating them as disabled. Physical RAM is confined to one
CPU with A20 enabled; there is no TLB or translated-code cache in this component.
LLVM release/pin completion remains separate from this qualification.
