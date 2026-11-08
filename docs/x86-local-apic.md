# Owned local APIC fixed-interrupt banks

[`X86LocalApicFixed`](../include/neverd/translate/X86/X86LocalApic.h) owns a
copy of IRR, ISR, TMR, TPR and the EOI-broadcast suppression bit. It supplies
local fixed-message reception, priority and ordinary acknowledgment/EOI
transitions for an explicitly bound, enabled modern APIC. The profile uses
one coalescing IRR bit per vector; older P6 rejection behavior is outside it.
The profile retains TPR subpriority in PPR when the task and highest service
classes are equal, matching QEMU/KVM. Intel permits a model-specific zero
subpriority on that tie; the fixed profile does not infer which choice another
CPU implements. It has no dependency on a decoder, guest-memory provider, SDK
session or host APIC.

The producer must establish that this limited profile applies and serialize
access to the object. Arrival routing/timing, APIC configuration changes,
NMI/ExtINT arbitration, spurious/software interrupts, SynIC/automatic EOI,
posted interrupts, timer/device behavior and CPU wakeup are outside it.
The class does not construct a complete interrupt controller or certify a
saved machine as runnable.

## State transitions

`fromState` copies the initial banks and rejects fixed-vector bits 0–15 in
IRR, ISR or TMR. It does not clear invalid bits. The returned optional owns
its state; `state()` exposes a const view. No allocation or cache is used.

`receiveFixedInterrupt(vector, levelTriggered, asserted)` receives an already
routed fixed message. The result distinguishes four outcomes:

| Result | Bank effect |
| --- | --- |
| `RejectedVector` | Vectors 0–15 are unsupported; no effect or invented ESR/error interrupt. |
| `IgnoredDeassertion` | A level-triggered deassertion changes nothing, including existing IRR/TMR bits. |
| `Queued` | A previously clear IRR bit becomes set; TMR takes the message's trigger mode. |
| `Coalesced` | IRR was already set and stays set; TMR still takes the new message's trigger mode. |

Edge-triggered messages ignore the asserted bit. Reception leaves ISR, TPR
and EOI suppression unchanged and does not depend on CPU IF, instruction
shadows or current priority. Those gates affect later processor delivery,
not reception into IRR. A vector may have one request in service and another
pending; further arrivals collapse into that pending bit, without a hidden
counter or queue. TMR describes the latest admitted trigger mode even when
the vector is already in service. A level deassertion never withdraws a
pending request or clears its trigger bit.

Reception is not processor delivery, device-line sampling or destination
selection. The embedding supplies arrival timing and already matched fixed
messages, keeps access serialized and handles unsupported vectors explicitly.
It must not infer messages from a timer, stale injected-event field or saved
IRR bit: a captured IRR bit is already pending in the imported bank.

`processorPriority()` retains the full TPR when its priority class is at
least the highest ISR class, using the explicitly bound tie policy above.
Otherwise it returns that ISR class with zero low bits. `nextFixedInterrupt()`
returns the highest IRR vector only when its class is strictly higher than both. Subpriority cannot preempt another vector
in the same class. Reading a candidate changes no bank.

`acceptFixedInterrupt(vector)` checks the current candidate, clears precisely
its IRR bit and sets its ISR bit. A wrong, blocked or repeated acknowledgment
returns false without effects. TMR is retained. ISR survives handler execution
and IRET; neither operation implicitly supplies an EOI.

`endOfInterrupt()` clears the highest ISR bit and reports its vector,
trigger mode and whether an EOI broadcast is required. The producer must route
that external request and retain any unfinished routing obligation. The local
transition does not acknowledge an I/O APIC or deassert a device line. TMR and
IRR stay unchanged, including an IRR request for the same vector. An empty ISR
produces no EOI and no mutation.

## CR8 and TPR coherence

`fromState(bank, cpu.CR8)` additionally checks that the supplied CPU CR8 equals
the captured TPR's high nibble. Mismatched or reserved-bit CPU values reject;
neither observation overrides the other. Import retains the full TPR byte,
including its low nibble. The one-argument bank import remains available when
there is no separate CPU observation to check.

`readCr8()` returns the zero-extended task-priority class. It reads TPR, not
PPR: an in-service interrupt can raise processor priority without changing CR8.
A read changes no bank. `setTaskPriority(byte)` applies a direct TPR write,
retains all eight supplied bits and changes subsequent CR8 reads immediately.

`writeCr8(value)` applies the register effect of an admitted, ordered CPL0
MOV to CR8. Values above 15 return false without effects; the caller owns
privilege/mode admission and architectural fault delivery. A valid write
updates TPR's high nibble and clears its low nibble, even if the CR8 value is
unchanged. IRR, ISR, TMR and EOI suppression are retained. A later candidate
query sees the new priority, but the write neither accepts nor discards an IRQ.

The embedding mirrors successful writes into its explicit CPU bank. It must
serialize original TPR/CR8 accesses and preserve the selection/acknowledgment
boundary. A state import or a read is not a write-completion notification:
copying CR8 into TPR on every poll loses captured subpriority, while updating
only when CR8 changes misses same-value writes. These methods do not observe
instructions, establish ordering between guest MMIO and MOV CR8, or deliver
#GP themselves. Intel's [CR8/APIC interaction rules, section 11.8.6.1](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf)
define the read/write distinction and the need to order mixed access paths.

## Continuation producer boundary

The [continuation scheduler](x64-continuation-source.md) already accepts
`peek` and `accepted` producer callbacks. A fixed-source producer can use
`nextFixedInterrupt()` to fill `Vector` and `Kind=2`, and use
`acceptFixedInterrupt()` for acknowledgment. The embedding must compose that
source with other event sources and keep the bank/TPR unchanged between
selection and acknowledgment, including serializing arrival calls. A false
acknowledgment is a producer contract error; the scheduler's `accepted`
callback cannot fail after delivery effects.

The existing scheduler owns IF/STI/MOV-SS admission and acknowledgment timing.
For paged delivery, acceptance can precede a fault during IDT entry; the
producer still consumes the accepted IRQ exactly once. A deferred/unaccepted
event leaves these banks unchanged. This component does not add a callback,
choose a global event, execute a handler, wake a halted CPU or advance PC.
It is an explicit building block for a producer, not automatic scheduler wiring.
In particular, the current scheduler has no CR8-write completion notification;
its polling and acknowledgment callbacks alone cannot synchronize all writes.
The successful-RAM source profile excludes TPR aliases, and the paged source
currently leaves MOV CR8 at a native boundary. Neither contract is expanded by
these register methods; completing the instruction/device wiring remains work.

All queries visit at most the fixed eight-word banks they need. Acknowledgment
and EOI touch only the selected word after bounded priority checks. Reception
reads/writes only the selected IRR/TMR words, with no scan. CR8 read/write
uses only the TPR byte and a scalar reserved-bit check; no bank scan is needed.
Existing decompilation and generated instruction loops acquire no work unless an embedding explicitly
uses this component in its producer. No whole-request speedup is claimed.

## Capture and static qualification

The [offline QEMU adapter](qemu-save-snapshot.md#local-apic-fixed-bank-projection)
transports the banks through the same C++ type and validator. It does not
implement priority a second time in Python. Saved configuration and an
explicit delivery-profile binding are separate evidence; a bank initializer
does not discharge the producer obligations above.

Build the compile-only proof target:

```sh
python3 scripts/build.py build-release --target NeverDX86LocalApicProofs --parallel 2
```

The target contains `static_assert` checks over all 240 ordinary fixed vectors,
the complete task/service/pending priority-class grid, TPR low bits, nested
service, duplicate requests, repeat/wrong acknowledgments, all reserved bits,
edge/level EOI and broadcast suppression. It builds an object without a test
executable or guest execution. It is also a dependency of `NeverDUnitTests`.
These proofs establish the declared register model; they are not an original
hardware oracle or a full event-environment qualification.

The arrival proof source adds 7,680 independently tabulated message cases over
every fixed vector and 960 complete two-slot lifetimes. These retain unrelated
bank bits, distinguish trigger mode from message level, reject all reserved
vectors and check both directions of trigger-mode replacement through
coalescing and EOI. Each vector has a separate constant evaluation; no compiler
evaluation limit is raised. Neither these proofs nor a saved bank supplies an
observed future arrival stream.

The priority proof source covers every initial TPR byte and all 16 legal CR8
writes (4,096 combinations), including same-value writes and repeated writes.
It checks all 60 reserved source bits with empty/full legal low nibbles plus
all-ones input (30,976 refusals), coherent/mismatched paired imports, every
reserved bank bit, and 3,840 task/service/pending-class admission combinations
after a write. Reads and imports preserve subpriority; rejected operations
preserve all banks and the current candidate. These are static register proofs,
not execution or original-hardware qualification.

The independently authored rules follow Intel's
[local APIC priority and EOI specification](https://cdrdv2-public.intel.com/874240/325462-090-sdm-vol-1-2abcd-3abcd-4.pdf),
with the corresponding ordinary-bank transitions visible in
[Linux KVM LAPIC](https://github.com/torvalds/linux/blob/v6.18/arch/x86/kvm/lapic.c)
and [QEMU APIC](https://github.com/qemu/qemu/blob/v10.2.0/hw/intc/apic.c).
Their external virtualization/device routing is not incorporated into this model.
