# Explicit x86 event delivery

`event_delivery: true` adds `neverd_x64_deliver_event_v6(state, event)` to
`kernel-events-v6` exports. HighC and LLVM-to-C consume the same LowIR delivery
function. This function installs an IA-32e interrupt/trap handler context from
an explicit architectural event. It accepts CPL0–3 and 64-bit or 16/32-bit
compatibility entry, including a non-flat compatibility CS.

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48,
  "event_delivery": true,
  "instructions": [4096]
}
```

The same option connects canonical `UD2` and the already admitted unsigned
64-bit `DIV` to delivery. `UD2` delivers #UD. A zero divisor or unrepresentable
quotient delivers #DE before quotient, remainder or arbitrary flags are
published. Their saved PC is the original faulting instruction. The driver
charges one attempted instruction and stops at handler entry. Successful DIV
retains its existing completion and arbitrary-flag contract. Other instruction
faults are not implicitly caught or inferred from a host signal.

## Event input and ownership

The generated `neverd_x64_event_v1` is a 32-byte immutable input, checked against
`X64ContinuationEventV1` in the analysis interface. Its fields are:

| Field | Meaning |
| --- | --- |
| `SavedPC` | Original architectural return offset relative to CS; faulting PC for faults, completed continuation for traps. |
| `ErrorCode` | Architectural error code, zero when the event does not push one. |
| `FaultAddress` | CR2 for #PF, otherwise zero. |
| `Vector` | IDT vector. |
| `Kind` | Fault (0), trap (1), external interrupt (2), NMI (3), software INT (4), instruction breakpoint (5), abort (6). |
| `STIBlocked`, `SSBlocked` | Current interrupt/debug shadows, each Boolean. |
| `NMIBlockedBySTI` | Explicit binding of the original processor's STI/NMI behavior. |
| `RepeatedString` | Nonfinal REP iteration interrupted by an interrupt, NMI or trap; saved RF is set. |
| `Reserved[2]` | Must be zero. |

The event producer owns priority, APIC/PIC acknowledgement, partial instruction
completion, shadow lifetime and cause-specific banks such as DR6 or machine-check
MSRs. It must supply an already selected event and the corresponding restart
state. `SavedPC` avoids confusing a non-flat compatibility offset with its
linear code address. Entry is outside RTM/HLE and enclave execution; any required transaction
abort or AEX must complete first. This interface neither polls the host interrupt controller
nor treats a software INT as a CPU-detected exception.

The continuation state retains its existing V6 layout. The explicit event call
does not consume an instruction or environment budget; a caller invoking it
inside an environment callback accounts for that callback through the existing
bounded driver. Successful delivery returns 7, an inhibited event returns 15,
and invalid metadata/state/delivery preconditions return 5. Status 15 and 5
preserve state and RAM. Status 7 stops before executing the handler.

## Delivery semantics

The shared owner validates IDT/GDT/LDT descriptors, gate privilege, handler
mode, TSS bounds and canonical stack/handler addresses. Conforming targets
retain CPL; nonconforming targets select their DPL. All seven IST selections
and RSP0–2 privilege stacks are supported. An inner-privilege transition
installs null SS with the new RPL; the same-privilege IST path retains SS.
A null segment's ignored cache bytes follow the existing V6 contract.

Frame elements are eight bytes, including on compatibility entry. The chosen
stack is aligned down to 16 bytes; SS, RSP, saved flags, CS and saved PC are
pushed in architectural order, followed by an error code when appropriate.
Descriptor accessed-bit writes precede TSS reads and frame stores; the existing
alias forwarding remains shared with INT/INT3.

Software INT and external interrupts never push CPU exception error codes,
even at vector 14. A true #PF pushes its error code and publishes CR2. Ordinary
faults save RF=1; instruction breakpoints preserve RF, software INT clears its
saved RF, and other traps/interrupts preserve it except for a nonfinal REP
iteration. Handler flags clear TF/NT/RF/VM; an interrupt gate also clears IF.
NMI delivery sets NMI blocking. Maskable interrupts check IF/STI/MOV-SS;
NMIs check NMI/MOV-SS and the explicit STI policy. Debug events check MOV-SS
inhibition before accessing descriptor or stack RAM.

The interface admits the documented fault, trap and abort vector classes in
`X64ContinuationDelivery.h`. Unknown kinds, incompatible vectors, invalid
Boolean inputs, nonzero reserved fields and mismatched error/CR2 metadata fail
before any effect. CET #CP and vendor-specific exception banks are not silently
accepted through a generic vector.

Rules follow the [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
revision 090: INT's inter/intra-privilege pseudocode, Volume 3A sections
7.12 and 7.14, and Volume 3B section 20.3.1.1 for saved RF. The native oracle
uses the [documented KVM API](https://docs.kernel.org/virt/kvm/api.html) only to
set initial CPU state and request IRQ/NMI; fault cases execute original
DIV, UD2, MOV CR0 and faulting loads.

## Verification and remaining boundary

`X64ContinuationExportTest.EventDelivery*` exercises both source routes at
O0/O2. The native matrix performs 26,880 independent original-CPU comparisons:
10 source/target CPL pairs, three source modes, conforming/nonconforming
handlers, eight IST choices, seven event causes and two gate/RF choices,
through four source/compiler configurations. It compares every represented
state byte and 4 KiB of descriptor/TSS/stack RAM. A separate functional matrix
covers every input event kind; blocked-event tests deliberately point guest
tables at inaccessible memory to prove that inhibition precedes those reads.
The existing all-256-software-vector and descriptor/frame alias regressions
exercise the same refactored delivery owner.

This adds event **delivery**, with direct instruction fault production for the
two stated cases. It does not implement page translation, arbitrary restartable
memory faults, recursive faults/#DF escalation, APIC scheduling, SMM entry/RSM,
SGX EENTER/ERESUME/EEXIT/AEX, CET/FRED event routing, or legacy non-IA-32e task
gates. Compatibility entry here is not a complete i386 instruction dispatcher.
Those boundaries must remain explicit in coverage claims and release decisions.

Capture native sources and manifests by setting
`NEVERD_ISA_REGRESSION_ARTIFACT_DIR` for the native event test. Then run
`scripts/isa/x86/verify_event_delivery.py --sources DIR --compiler NEVERC
--llvm-bin LLVM_BIN --keep OUTPUT`. NeverC and the full LLVM legacy/new
backends additionally pass 12 execution profiles and 80,640 original-CPU
comparisons. The verifier checks that the delivery function contains no inline
assembly; the independent harness alone uses its explicit KVM oracle request.
