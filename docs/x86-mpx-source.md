# Explicit MPX source continuation

`intel-mpx-v11` exports original long-mode BNDMK, BNDMOV, BNDCL, BNDCU,
BNDCN, BNDLDX and BNDSTX through HighC and LLVM-to-C. It owns the four bounds
registers, enable state, table translation, BNDSTATUS and original branch
initialization. Generated code executes these explicit values; it does not
require host MPX instructions. The separate native LLVM/NeverC MPX intrinsics
operate on the actual processor bank. The C execution environment keeps host
MPX disabled or BNDPRESERVE set, so compiler-generated branches cannot
initialize a host bank that the caller needs to preserve.

```json
{
  "profile": "intel-mpx-v11",
  "mxcsr_mask": "0xffff",
  "entries": [4096],
  "follow_internal_calls": true
}
```

Use the same SDK or CLI entry point as the other
[x64 continuations](x64-continuation-source.md). The manifest binds exact
original bytes and state offsets. Entry selection follows the fallthrough
after qualified MPX instructions even when ordinary lifting refuses the
containing function; `mpx_successors` records this extension. An `instructions`
array selects exact starts directly. Both switch and direct dispatch retain
instruction boundaries and budgets.

## State and execution domain

V11 extends V4, independently of V5–V10. Its 2416-byte, 16-byte-aligned layout
adds `Bounds[4][2]`, `BNDCFGS`, `BNDSTATUS`, `XCR0`, `MPXSupported`, `OSXSAVE`,
`InTransaction` and five reserved bytes. Bounds contain complete 64-bit lower
and stored upper fields; the upper field is normally the one's complement of
the upper limit. Existing cells retain V4 meanings; reserved bytes are preserved.

The profile requires CPL0 long mode, no RTM/HLE, successful sequential ordinary
RAM accesses and no asynchronous events. State/budget storage is disjoint from
guest memory. Capability Booleans are zero or one, `InTransaction` is zero,
and BNDCFGS bits 11:2 are zero. Invalid inputs return status 5 without state,
memory or budget changes. The environment owns architectural validity of the
supervisor configuration and mappings.

MPX is enabled only when CPUID MPX and CR4.OSXSAVE are true, both XCR0 bits 3/4
are set, and BNDCFGS.ENABLE is set. Disabled instructions preserve bounds and
BNDSTATUS without accessing memory. This is an explicit architectural result,
not an assumption about the build host.

BNDMK and checks use unsegmented effective values. BNDMOV uses the original
FS/GS base when present; other bases are zero in long mode. The MPX decoder
owns ignored `67H`: it does not truncate addresses or replace RIP with EIP.
BNDMOV loads publish both fields after successful reads. LOCK stores are
admitted under the sequential, nonfaulting RAM domain; no contention or
partial-write guarantee follows from that projection.

Table instructions use segmented base plus displacement as the pointer address.
MIB scale is ignored; an absent base discards displacement, and an absent index
supplies pointer value zero. The pointer address is not itself dereferenced or
checked for canonicality. Only derived BDE/BTE accesses need valid memory.
CPL0 uses supervisor directory indexing, without user MAWA. A load reads all
three table fields before publishing bounds; pointer mismatch initializes its
destination to zero. A store updates three fields and leaves the fourth intact.

## Bounds faults and branches

Status **14 (BoundsFault)** publishes BNDSTATUS, preserves PC on the original
faulting instruction and stops before exception delivery. A failed check sets
status to 1. An invalid BDE sets status to its address OR 2, preserving the
bank/table. The bounded driver counts this attempted step once. The environment
can handle pending `#BR`, repair bounds or the table, and resume at that PC.
No handler frame is fabricated.

Original CALL, near RET, E9/FF near JMP and short/near Jcc initialize all bounds
when enabled, BNDPRESERVE is clear and BND/F2 is absent. The effect follows
the transfer's stack/target work and survives source CFG optimization.
BND/F2 branches, EB JMP, LOOP and J*CXZ preserve the bank. Compiler-created
branches never become original BNDINIT events. The shared physical-stack reader
accepts BND-prefixed near RET; other prefixes still require their own admission.

These rules follow [Intel SDM Volume 1, Appendix E](https://cdrdv2-public.intel.com/671436/253665-sdm-vol-1.pdf)
and the [BND instruction descriptions](https://www.intel.com/content/www/us/en/content-details/671110/intel-64-and-ia-32-architectures-software-developer-s-manual-combined-volumes-2a-2b-2c-and-2d-instruction-set-reference-a-z.html).
Concrete evaluation and source share comparison and directory/table arithmetic
in `X86MpxSemantics.h`; operands retain the `X86MpxEncoding` authority.

Ordinary function-ABI lifting, i386/compatibility source entry, exception delivery,
memory faults, transactional rollback and physical MPX remain outside V11.
The concrete memory provider separately records partial/fault outcomes. Successful
V11 tests do not qualify those domains.

## Qualification

The [qualification record](evidence/x86-mpx-source-2026-10-31.json) binds the
integrated sources, generated C/manifests, build inputs and compiler hashes.
All 156 selected encodings passed 479,232 complete state/RAM comparisons through
both C routes, both dispatchers and O0/O2 under ASan/UBSan. Original branch
initialization passed another 1,152 state/stack comparisons. Invalid inputs and
entry-selection repair/resume have separate regressions. All 269 related tests
passed without skips after integration.

NeverC and both full LLVM code-generation pass managers additionally passed
16,128 original-byte/generated-source comparisons in QEMU TCG across 12 profiles.
Each profile executes 56 encodings and explicitly excludes 100: 96 affected by
QEMU's register-check, LOCK BNDMOV and addr32-memory defects, plus four absent-base
table forms outside that native fixture. Those exclusions are covered by the
portable matrix and are not counted as native passes. The native fixture keeps
host BNDPRESERVE set and checks actual pending #BR and BNDSTATUS publication.
