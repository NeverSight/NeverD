# Generated x64 instructions over paged RAM

The V6 continuation export accepts `paged_memory: true`. Both HighC and
LLVM-to-C emit the actual selected instruction semantics with explicit guest
memory calls. `X64PagedExecution` binds those calls to `X86PagedMemory`, verifies
original instruction bytes through execute permissions, and commits scalar
instructions or delivers their synchronous faults. Guest addresses never become
host pointers.

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffff",
  "paging_address_bits": 48,
  "paged_memory": true,
  "instructions": [4096, 4099]
}
```

The exported entry is:

```c
uint64_t neverd_x64_run_paged_v6(neverd_x64_state_v6 *state,
                                uint8_t *context, uint64_t *budget);
```

`context` points to a live C++ `analysis::X64PagedExecution` constructed with
the embedding application's `translate::X86PagedMemory`. Link the
`NeverDX64PagedEventDelivery` component for the source-only C bridges declared
in `X64PagedExecution.h`. The generated bodies, validators and original bytes
must come from the same export. Their adapters preserve actual function types
across both C routes. This internal embedding contract is not a stable C SDK
runtime-construction interface.

## Instruction and fault ownership

The existing decoded LowIR owns arithmetic, flags, register slices, original
addresses and branch targets. The paged source adapter retains original stack
reference and read-modify-write identities before register-state rewriting.
Private state accesses stay separate from guest operands. Imported callback
types and SSA input lifetimes are explicit throughout both source pipelines.

The runtime validates the entry and IA-32e controls, then fetches the expected
instruction bytes. It stops at the first changed byte, before using later stale
bytes; the caller can relift at that PC. A complete instruction runs against a
staged state. Its final scalar store retains a prepared physical translation
plan, and publishes data only when all remaining checks succeed. Page-table
A/D effects survive failure. There are no external RAM writers during a step.

The current admission covers GPR/flags operations with zero or one scalar read
and zero or one final scalar write, at 1/2/4/8 bytes. It includes near CALL/RET,
indirect branches, stack operands, FS/GS bases, address-size overrides and
full-dividend DIV/IDIV. A read-modify-write operand checks write permissions even
before arithmetic. False CMOV conditions still perform their memory access.
CALL checks its stack write before a noncanonical target: a missing stack page
therefore produces #PF, while successful preflight followed by #GP publishes
neither the return address nor register changes.

#PF, #GP, #SS, #AC, #DE and canonical UD2's #UD retain the original restart PC.
The [shared paged event owner](x86-paged-memory.md) performs IDT entry, including
nested faults, #DF, CR2 updates, partial handler-frame writes and shutdown.
Execution stops at handler entry. Correcting a mapping and restoring the
interrupted state permits a retry without repeating a committed operand write.
RF clears only on successful instruction completion.

Existing status identities 0..16 remain unchanged. Internal status 17 is
consumed by the executor. Status 18 reports physical backing failure, 19 reports
shutdown, and 20 reports changed code requiring relift. A completed instruction
or an architectural fault attempt consumes one instruction-budget unit;
native/invalid/backing/changed-code stops do not. Full fault and backing details
are available through the context's `lastResult()` and `lastError()`.

## Verification and limits

Build `NeverDX64PagedExecutionTests` through `scripts/build.py`, together with
`NeverDX86PagingTests`, `NeverDX64ContinuationSourceTests` and
`NeverDX64ContinuationExportTests`. The generated execution fixture qualifies
Linux x64; other hosts explicitly skip it. Supply `NEVERD_TEST_NEVERC` to
compile each source route with NeverC as well as Clang, at O0/O2. Set
`NEVERD_ISA_REGRESSION_ARTIFACT_DIR` to preserve actual C and manifests.

An independent KVM CPU executes 38 original instruction forms across ten input
and memory configurations for every compiler profile. Checks compare all GPRs,
RFLAGS, PC, CR2, CS/SS and physical RAM outside paging-table pages, including
fault frames. Only architectural undefined arithmetic flags use witnesses from
that original CPU run. Fault-time A/D updates are not equated because their
publication can depend on the CPU. Additional checks cover self-modification,
backing failure, invalid binding, write-fault restart and refusal boundaries.

The [November 1 qualification](evidence/x86-paged-execution-2026-11-01.json)
records 534 passing tests across the native-intrinsic and pinned r6 SDKs, with
two explicit LA57 skips. Generated execution performs 6,080 original-CPU
comparisons, including 3,120 fault cases; the paging/event regression adds 1,800
comparisons. All 298 C/header/manifest artifacts match across SDKs, and the old
source/header outputs remain unchanged.

This is the scalar instruction commit domain. REP, gather/scatter, XSAVE and
other partial-completion instructions need their own progress models. LOCK,
native system providers, TF, IRQ/NMI scheduling, SMI/AEX, legacy i386 dispatch,
CET/FRED, MMIO, TLB and concurrent writers remain outside this binding.
Combining it with the old host-RAM event scheduler, direct dispatch or RTM
regions is explicitly rejected. The [older no-fault scheduler](x86-event-scheduling.md)
retains its separate contract. LLVM release/pin completion is not implied.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
volume 2 CALL/DIV/IDIV/CMOV and volume 3A paging and exception priority;
[KVM API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html).
