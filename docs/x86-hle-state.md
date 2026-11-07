# Explicit native HLE completion

HLE prefixes belong to their memory operation. The explicit x64 continuation
binding `"native_hle_steps": true` admits the 19 operation families listed by
Intel's XACQUIRE/XRELEASE entry: ADD, ADC, AND, BTC, BTR, BTS, CMPXCHG,
CMPXCHG8B, DEC, INC, NEG, NOT, OR, SBB, SUB, XOR, XADD, XCHG and release-only
MOV. Ordinary strict and permissive lifting retain their refusal to turn an
unowned transactional instruction into an ordinary memory operation.

The binding requires an existing x64 control-state profile, entry outside
external HLE/RTM, long-mode LAHF, and nonfaulting original RAM accesses. Original
memory must exclude state, budget, code and the compiler frame. This is a native
step with a publication boundary: PAUSE executes after the prefixed operation
and before publishing any register or flag output. Intel SDM Volume 1,
§17.3.9.1 specifies that PAUSE aborts an active transaction. HLE abort restores
the acquire checkpoint and retries that acquire without elision. On a CPU
without HLE, the legal hints are ignored by the processor. No success status,
transaction checkpoint or ordinary atomic substitute is synthesized.

The raw reader retains operand width, register/high-byte selection, immediate
sign extension, addr32/addr64, RIP-relative and SIB addressing, and original
FS/GS bases. Register bit indexes are signed at the operand width; their word
adjustment wraps in the original address size before segment-base addition.
Immediate bit indexes select a bit within the addressed word. The last F2/F3
selects the HLE hint, including when both are present. Other duplicate prefix
groups, nonfinal REX, register destinations, LOCK MOV, XACQUIRE MOV and
CMPXCHG16B are refused. CMPXCHG16B is not in the SDM HLE entry's admitted list.
CMPXCHG8B's redundant operand-size override is outside this raw grammar.

`ir/intrinsics/X86/X86Hle.h` owns the state shape, native control ABI and output
mapping; `lift/X86/X86HleEncoding.h` owns byte admission. The original bank is
never used as temporary RCX storage. Both emitters use a private four-register
bank, then copy only the original architectural destinations. Byte/word XADD
and XCHG retain the other destination bits; dword writes clear the upper half.
CMPXCHG and CMPXCHG8B keep their actual conditional accumulator completion.

The native compiler interface is:

```llvm
declare void @llvm.x86.hle.context(ptr memory, ptr gprs, ptr flags,
                                 i32 immarg control, i32 immarg bytes)
```

The GPR object is four unaligned u64 slots in RAX/RCX/RDX/RBX order. RCX holds
the explicit operand or bit index; CMPXCHG8B consumes all four slots. Flags use
a separate unaligned u64 object. All objects and the compiler frame must be
disjoint from the original memory operation. Native control selects operation,
acquire/release, and optional abort-before-publication. Calls retain unknown
memory effects, convergence, no-duplication and their exact constant controls.
Source output rejects narrowed types, erased effects or invalid controls.
Older SDKs may transport the named call to LLVM C; they refuse object generation
until the native backend recognizes it.

NeverC's `<hlecontextintrin.h>` supplies `struct __hle_context`, `__hle_native`
and `__hle_step`. The native form leaves ownership of the entire critical
region to the C caller; the step form requests the abort boundary. These are
different contracts. The NeverD continuation always uses the step boundary.

Qualification compares native instructions with independent raw assembly,
including defined flags, partial and zero-extending outputs, unaligned context
objects, memory canaries, signed bit indexes, live integer/FP values and faults
before publication. Source qualification additionally checks every represented
state byte and budget behavior in HighC/LLVM C with switch/direct dispatch.
Controlled provider tests check adaptation separately from native instruction
semantics. The local CPU does not advertise HLE, so successful physical
transaction commits and rollback remain unqualified. Full RTM checkpoint and
fault/exception continuation are outside this binding.

Primary sources are Intel SDM 090, Volume 1 §§17.2.1 and 17.3.9.1, and the
XACQUIRE/XRELEASE entry in Volume 2D. The instruction entry is also available
in Intel's [Volume 2 instruction reference](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf).

## Qualification, November 1, 2026

The [qualification record](evidence/x86-hle-completion-2026-11-01.json) binds
the compiler revisions, source/manifests and executable hashes. All 34 O0/O2
profiles passed: 348,160 direct native/LLVM pairs and 835,584 pairs compiled
from actual HighC/LLVM C exports, with 9,248 fault-before-publication checks.
The source matrix covers both switch and direct dispatch through NeverC and
both LLVM pass managers. Ten additional provider cases isolate original high
byte/extended registers, immediate sign extension, bit-index masking and
address wrapping from the native instruction semantics.

The decoder regression found that F2/F3 plus 66 rejected 16-bit BTC and
Group 8 immediate forms. The published Capstone fix preserves operand size
without changing adjacent mandatory-prefix instructions. Its 1,344 prefix
combinations pass, together with 320 exchange-effect combinations that retain
XADD's flags, CMPXCHG destination read/write access and CMPXCHG8B/16B's full
implicit register banks. The latter includes CMPXCHG16B as an ordinary atomic
instruction; it does not expand HLE admission.
Default 16-bit decoding uses the same prefix resolver, so the reported memory
width remains consistent with the original register width.

The host does not advertise HLE. These results qualify native fallback and
state transport, and do not establish physical transactional commit/rollback,
full RTM checkpoints or fault continuation. The SDK release and LLVM gitlink
remain unchanged while the wider instruction work is unfinished.
