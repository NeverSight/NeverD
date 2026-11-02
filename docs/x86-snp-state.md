# Explicit SNP and SEV-ES completion

The decoder now distinguishes PSMASH, PVALIDATE, RMPADJUST, RMPUPDATE,
RMPQUERY, RMPREAD, RMPOPT, RMPCHKD and VMGEXIT. In particular, the mandatory
F2/F3 prefixes on `0F 01 FC` and `0F 01 D9` no longer disappear into CLZERO
or VMMCALL. IDs are appended, and both VMGEXIT encodings are recognized.
PVALIDATE and VMGEXIT decode in 16/32/64-bit modes; the other seven require
64-bit mode. This does not add a NeverD real-mode target.

For original x64 code, the continuation exporter accepts two independent
bindings in an existing control-state profile:

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48,
  "entries": [4096],
  "follow_internal_calls": true,
  "native_snp_instructions": true,
  "native_sev_guest_exit": true
}
```

`native_snp_instructions` binds the original supervisor RMP, paging and SNP
environment. The eight instructions retain their native memory operations,
normal-completion register outputs and arithmetic flags. `RMPQUERY` returns
RAX/RCX/RDX, `RMPCHKD` returns RAX/RCX, and `RMPOPT` changes only CF. Status
writes to EAX retain their zero extension. The four undefined RMPCHKD flags
come from the actual CPU; unrelated flags and registers retain their supplied
values. PVALIDATE uses ECX/EDX and optionally a wrapped EAX address. No RMP
access is replaced by an ordinary C load/store or a synthetic success value.

`native_sev_guest_exit` binds a returning SEV-ES GHCB/MSR service with an
optional RAX VMGEXITParameter. Private CPU state and the compiler environment
must survive the service. Non-SEV-ES VMMCALL fallback and caller-state-replacing
services are outside this binding. It does not infer a hypervisor ABI from the
opcode. The two bindings are independently validated and recorded in the
manifest, and discovery follows their normal-completion successors only when
the corresponding binding is supplied.

Both bindings retain the existing control profile's entry constraints. They
require nonfaulting normal completion. State, budget, code and compiler frame
must remain accessible and disjoint from instruction/service memory, RMP pages
and mapping changes. Native faults, interrupted RMPCHKD progress and SEV event
delivery are not serialized as a new exception-state object.

`X86Snp.def` owns the inventory and `X86SnpEncoding.h` owns source admission:
canonical four-byte instructions, either F2/F3 VMGEXIT, and a leading `67` on
PVALIDATE. Other prefix forms fail source export explicitly. In particular,
a source FS/GS offset is not silently rebound to the compiler's segment base.
The decoder recognizes a larger encoding domain than this source contract.
Ordinary ABI lifting and unbound execution remain refused.

HighC and LLVM C emit native NeverC builtins. Eight LLVM interfaces have the
shape `llvm.x86.<instruction>.context(ptr gprs, ptr flags, i32 immarg control)`;
`llvm.x86.vmgexit.sev(i64 parameter)` owns the returning SEV-ES exit. The GPR
object is three unaligned u64 slots in RAX/RCX/RDX order; the flags object is a
separate unaligned u64. The native control allows PVALIDATE address32 and an
optional native segment-prefix byte. The objects must be disjoint from each
other and instruction-owned memory. Only normal completion publishes outputs.
NeverC installs `<snpcontextintrin.h>` with a 32-byte `struct __snp_context`
and `__snp_*`/`__sev_vmgexit` wrappers. Caller C needs no inline assembly.

Native calls keep unknown memory effects, convergence, no-duplication and
returning contracts. Both source routes reject malformed types, controls or
erased effects. An older LLVM SDK can transport the exact named interface to
LLVM C, but native object generation refuses until the backend recognizes it.

The compiler tests compare exact encodings, modes, all output banks, flags,
unaligned buffers and live integer/FP values. Original-byte references use a
separate PUSHFQ transport. Controlled signal services qualify transport and
fault-before-publication; they do not certify AMD's RMP algorithms or real
SEV-ES services. The source tests compare complete state and memory through
both C routes and dispatch forms, including budget exhaustion and missing
bindings. Physical SNP/SEV-ES execution remains unqualified.

The [completion record](evidence/x86-snp-completion-2026-10-31.json) records
98,304 original/candidate pairs across 34 compiler profiles, including 67,584
pairs from actual exported C, 196,608 controlled instruction faults and 768
fault-before-publication checks. The two SDKs produce identical source and
manifest files in both routes and dispatch forms. Each SDK ran the same 642
regressions without failure: the native SDK passed 632 with ten skips, and r6
passed 620 with 22 skips. Skips cover unsupported i386 NeverC compilation,
unavailable native backend interfaces in r6, and physical AMX execution.

The prior [decoder census](evidence/x86-snp-census-2026-10-31.json) is historical
preimplementation evidence. Architecture sources are AMD64 APM Volume 3,
24594 revision 3.39 (October 2026), the instruction entries for these nine
families; Volume 2, 24593, sections 15.35.6–15.35.8; and the
[GHCB specification](https://www.amd.com/content/dam/amd/en/documents/epyc-technical-docs/specifications/56421.pdf).
