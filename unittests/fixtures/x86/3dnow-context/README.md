# Owned 3DNow context guest

`entry.S` and `guest.c` execute the retained NeverD objects and original input
bytes in one QEMU x86-64 processor model. A private IDT records synchronous
faults and restores the harness stack; the faulting operation does not resume.
Comparisons include the eight 80-bit physical aliases, x87 control/status/tags,
instruction/data pointers and opcode, plus exception vector/error code.
Reserved FXSAVE bytes and unrelated vector state are outside this contract.

The bootstrap files `boot.S`, `boot.ld` and `payload.ld` are adapted from
[NeverC's 3DNow fixture](https://github.com/NeverSight/NeverC/tree/880d1e96ff48f41c5069c6d8f9839eaa8f37d64a/tests/fixtures/x86/3dnow).
Copyright (C) 2026 NeverC contributors. License: AGPL-3.0-only, reproduced in
[the repository license](../../../../../LICENSE). The 2026-10-27 changes move
the stack to 63 MiB and enable CR0.NE for native #MF delivery. This guest does
not include NeverC or LLVM compiler code.

The CPU model is `max`, with a second run disabling `3dnow` and `3dnowext`.
Successful QEMU comparisons are not evidence for physical processor numerical
accuracy, privileged task switching, arbitrary exception handlers or a complete
machine-state continuation ABI.
