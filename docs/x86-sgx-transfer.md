# Native SGX transfer state

The `native-sgx-v13` source profile connects code64 `0f 01 d7` ENCLU to
explicit GPR, flags and x87/SSE/AVX input state through HighC and LLVM-to-C.
It admits low EAX 2 (EENTER), 3 (ERESUME) and 4 (EEXIT). The transfer restores
the complete native input bank and jumps to the **original executable
instruction address**. EENTER's RCX contains the address after the complete
instruction. A segment override, address-size override, or final REX contributes
to its length. ENCLU uses the full implicit register operands in code64, including
with `67`. The accepted grammar has at most one prefix per group.
[Intel SDM, ENCLU](https://cdrdv2-public.intel.com/868149/332831-089-sdm-vol-3d.pdf)

This is an explicit terminal native environment binding. The original
enclave, native continuation and AEP/runtime own execution after transfer.
The compiler frame and bounded NeverD driver are abandoned. It does not
declare an ordinary returning C enclave-call ABI or emulate EPC/SSA/AEX.

## Source contract

```json
{
  "profile": "native-sgx-v13",
  "mxcsr_mask": "0xffff",
  "instructions": [4096]
}
```

Use the actual processor's MXCSR mask. `entries` selection is also supported.
This terminal profile selects each entry instruction alone, without ordinary
function fallthrough discovery. Returning ENCLU leaves belong to a separate
contract; they cannot make V13 scan beyond its native transfer.
V13 extends the V4 storage prefix to 2464 bytes. The `SGX` field starts at
offset 2320 and contains sixteen u64 GPR slots, u64 RFLAGS and a u64 `XState`
address. Only `SGX.XState` is an input in that transport field; the explicit
core GPR/flags cells populate its other slots. GPR order is AX, CX, DX, BX,
SP, BP, SI, DI, R8 through R15, preserving every high half.

The aligned 832-byte standard XSAVE image owns x87. Explicit `ZMM[0..15]`
low 32-byte values and MXCSR override its SSE/AVX components; higher vector
containers are not live when XCR0=7. `X64ContinuationAvxImage.h` shares image
admission and SIMD import with SVM/SMM. The source validates the state PC,
flags, leaf, MXCSR, image alignment/range/disjointness and XSAVE header before
any state/image write. Invalid invocation returns status 5 unchanged.
Instructions outside the transfer profile require native execution at status 4.

The invocation supplies:

- Native CPL3 code64 SGX, live XCR0=7, IF=1, IOPL=0 and disabled CET. TF, RF,
  NT, VM, VIF and VIP are clear. Status flags, DF, AC and ID are explicit.
- Immutable executable original ENCLU bytes at the manifest PC, plus original
  native continuation and AEP code and mappings. The complete imported stack
  and other register values retain their original native meanings.
- Valid native EPC/TCS/SSA, FS/GS descriptor caches, debug and supervisor state
  and SGX runtime protocol. Untransported represented banks already match the
  ambient environment. Neither SGX leaf checks nor SGX success are invented.
- Accessible disjoint state/image/compiler storage, successful sequential
  ordinary memory accesses and no concurrent writes, faults or asynchronous
  event delivery during preparatory input transport.

After the original ENCLU is reached, the processor and original native
runtime own leaf faults, EEXIT, ERESUME and AEX. The profile does not intercept
them and publish a new NeverD software state. Faulting or asynchronous input
transport needs a separate restart/serialization contract.

## Native compiler interface

The LLVM fork owns
`void llvm.x86.enclu.transfer.at.context(ptr, i64 immarg)` and NeverC provides
`__builtin_ia32_enclu_transfer_at_context`. The backend restores xstate with
XRSTOR64, flags with POPFQ and all GPRs, installing SP and AX last. It then
jumps through a RIP-relative code-local address literal, consuming no restored
GPR and changing no flags. The last two context reads install SP and AX;
neither the AX read nor the jump uses the restored stack. No compiler-stack
access or epilogue follows. The original instruction retains its hardware-defined
next-PC and enclave semantics.

The target literal is naturally aligned. This is required after restoring
AC at CPL3 when CR0.AM enables alignment checking; a merely adjacent literal
can fault before reaching the original instruction.

The separate `llvm.x86.enclu.transfer.context(ptr)` interface emits ENCLU at
the generated site and deliberately traps at its generated continuation. Its
native-address protocol is useful independently, but NeverD's V13 original-PC
projection always uses the original-site interface. Both are native compiler
intrinsics. The LLVM backend also implements code32 and x32 transport. NeverC
and the V13 source export use x64 for this interface.

The source's shared intrinsic contract validates its two operands, complete
context pointer and constant u64 original PC. LLVM and C routes preserve
unknown memory effects, convergent/noduplicate/noreturn/nounwind attributes,
and SGX/XSAVE feature requirements. Old SDKs may carry this explicit IR and
export C, but native code generation refuses unless the SDK contains the
matching lowering.

## Verification boundary

Source regressions execute both exported C routes with a terminal test
provider and check all state/image outputs, exact original PC and unchanged
rejected inputs. `scripts/isa/x86/verify_sgx_transfer.py` compiles the actual
exports through NeverC and both full-LLVM pass managers, then reaches the
original ENCLU in a native KVM vCPU. It compares all GPRs/flags, segment/system
state, unchanged memory, x87/SSE/AVX and original instruction address. Its
independent XRSTOR oracle uses the image retained **before** the projection
imports explicit SIMD. Omitting XRSTOR or corrupting the target address must
fail. This verifies input transport and original-site preservation.
CR0.AM is enabled throughout the KVM matrix, including inputs with AC set.

The [recorded qualification](evidence/x86-sgx-transfer-2026-11-01.json)
contains 412 passing continuation tests across the r6 and native SDKs with no
skips, identical C/manifest artifacts, 331,776 compiler input-state observations
and 110,592 actual exported-C observations. The adjacent SMM regression adds
55,296 complete native state pairs. LLVM code64/x32 and the NeverC frontend
also retain an original site above 4 GiB without truncation.

The local host exposes no SGX device. The native compiler tests stop before
ENCLU and execute zero enclave instructions. Their results cover state
transport up to the transfer site. The software enclave tests below exercise
EENTER, ERESUME, EEXIT and AEX against imported EPC state.

## Actions runner discovery (2026-11-09)

The [discovery run in `gmh5225/test_mac_intel`](https://github.com/gmh5225/test_mac_intel/actions/runs/42757819869)
checked three hosted runner images. Ubuntu 24.04 exposed an AMD EPYC 9V45;
Intel macOS 15 and 26 exposed an Intel Core i7-8700B. All three reported no
SGX1, no EPC section and no accessible enclave device. The macOS guests did
expose CPUID's general SGX enumeration bit, but leaf 12 remained zero; that bit
alone does not make enclave execution available to these guests.

The [retained evidence](evidence/x86-sgx-runner-discovery-2026-11-09.json)
records exact image versions, source commit and raw CPUID/report hashes.
The workflow's successful status means capability discovery completed. Exactly
zero enclave instructions executed, so EENTER/ERESUME/EEXIT/AEX remain
unqualified. On November 20 the Linux runner also built the upstream v6.18
SGX harness and its enclave image. With SGX1, an EPC section and access to
`/dev/sgx_enclave`, the workflow executes `enclave.unclobbered_vdso` and
`enclave.pte_permissions`, retaining the results of entry/exit and fault/resume.
All three hosted runners reported zero EPC sections and the Linux runner
executed zero native test cases. [Run and artifact digests](evidence/x86-sgx-runner-discovery-2026-11-20.json)

The [November 23 repeat](https://github.com/gmh5225/test_mac_intel/actions/runs/45325488567)
tested NeverD `d7c5338cf5`: all 27 selected software mode/SMM/SGX/event cases
passed. Ubuntu 24.04 and both Intel macOS images again reported zero EPC,
zero native enclave instructions and no available native enclave selftest
cases. The Linux job used a freshly built x64 NeverC compiler whose source
revision and artifact hash are retained in the
[current qualification](evidence/x86-mode-smm-sgx-events-2026-11-21.json).

The [merged-source repeat](https://github.com/gmh5225/test_mac_intel/actions/runs/45334574616)
again found SGX1 false, zero EPC sections and zero native enclave instruction
executions on all three designated runners. The Linux native selftest recorded
zero available cases. Its freshly built x64 NeverC compiler passed the target
audit and all 27 selected software mode/SMM/SGX/event cases at NeverD
`0d5a4ee1df`; the compiler artifact hash matches the CI receipt. The later
`a02363b36b` source passed the same 27 software cases on both local SDKs.
The later `1bfbe32497` shared-analysis merge repeated those 27 cases on both
SDKs together with the complete 243-case mode execution owner.
At integrated `9a1252f88b`, both SDKs again passed the same 27 software
cases and 243 complete mode-execution cases. The designated runners still
provide no EPC for native enclave execution.
These are software and capability results, not native enclave execution passes.
The [qualification](evidence/x86-mode-smm-sgx-events-2026-11-21.json) retains
the runner, compiler and test-report digests separately.

Architectural ownership follows Intel SDM revision 090, ENCLU/EENTER/ERESUME/
EEXIT and enclave entry/exit chapters, available from the
[Intel SDM index](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Software SGX1 enclave execution

`X86SgxMemory` imports EPC pages, EPCM permissions and ownership, and the
logical processor's x87 state. Binding this bank to `X86PagedMemory` keeps
private bytes out of ordinary RAM. Normal paging checks precede EPCM checks.
A fetch crossing the enclave boundary raises #GP. An EPCM violation raises
#PF with the SGX error bit. For this processor profile, non-enclave access to
EPC reads all ones and discards writes.

The x64 software transfer profile consumes original EENTER, ERESUME and EEXIT
instructions against an initialized imported SECS and TCS. The processor
binds XCR0=3 or 7. The enclave selects x87/SSE or x87/SSE/AVX with XFRM
and optionally requests exception information with MISCSELECT.EXINFO. For a
user page fault the private SSA retains the exact faulting address and error
code. For a general-protection fault it retains the error code and a zero
address. The outside handler receives CR2 with its page offset cleared on
page faults. The XSAVE area is at the beginning of the SSA frame and the GPR
area is at its end.
Their EPC translations are cached independently when the frame spans multiple
pages. AEX writes the interrupted registers, flags and PC to that frame,
installs the outside register image, and dispatches the original event using
the AEP as the saved return address. An inhibited arrival leaves the enclave
running. IRETQ to the AEP and ERESUME restore the SSA contents. The outside interrupt frame clears RF.
The saved SSA uses the RF value belonging to the original event. With AVX
selected, AEX saves the upper YMM halves in the standard XSAVE layout and
clears them in the outside image. ERESUME loads or initializes each selected
vector component and loads MXCSR from the frame even when XSTATE_BV is zero.

A routed INIT performs AEX before the CPU enters its reset or wait-for-SIPI
state. Enclave pages, their ownership and the interrupted SSA survive INIT.
A machine-check exception performs AEX before the vector-18 frame is built.
It disables subsequent ENCLU instructions until a platform reset. This
processor profile also disables SGX after a guest clears enabled MCi_CTL bits.
[Intel SGX and MCA](https://cdrdv2-public.intel.com/843859/332831-sdm-vol-3d-dec-24.pdf)

A debug enclave can opt in through TCS.FLAGS.DBGOPTIN. Instruction and data
breakpoints then observe enclave code and data. The opt-out path suppresses
instruction matches inside the enclave and data matches against its address
range. Opt-in EENTER with TF set takes a single-step trap at the enclave
entry point. Opt-in INT1 and INT3 save the faulting instruction PC to the SSA.
A plain ERESUME preserves an outside TF on opt-in and resumes the interrupted
instruction. The saved SSA flags clear TF for both entry types.

The CPU binding enumerates AEX notification and EDECCSSA independently. A
thread that requests notification in the interrupted SSA byte at GPR offset
167 enters its notification handler through ERESUME. The CPU validates the
next SSA slot, keeps the interrupted slot and CSSA, and enters at TCS.OENTRY
with the outside register image. EDECCSSA validates the preceding slot before
committing its decrement and making that slot the destination for future AEX.
[Intel SGX enclave operation, EDECCSSA, ERESUME and debugging](https://cdrdv2-public.intel.com/843859/332831-sdm-vol-3d-dec-24.pdf)

The exception-handler regression executes the original `DIV ECX` through
both C emitters and x64 Clang/NeverC. A divide fault writes vector 0 and its
original PC, dividend and marker register to the first SSA slot. EENTER on
the same TCS reaches the nested handler at CSSA=1. It writes 7 to the saved
RCX, exits, and ERESUME retries the original instruction with 84/7 = 12.
Two successive faults exercise slot reuse. Intel's signed x64 simulation
enclave produces the same register, retry and quotient observations on the
designated runner. [Independent Intel SDK simulation](https://github.com/gmh5225/test_mac_intel/actions/runs/44873573672)

The mixed-mode executor authenticates ENCLU before delivering encoding faults.
LOCK, REP/REPNE and the operand-size prefix cause #UD before the task-switched
check. Outside an enclave, EREPORT and EGETKEY cause #GP(0) at the original RIP,
before any access to their operands. Inside the enclave, the 512-byte
TARGETINFO or KEYREQUEST starts at an aligned address in ELRANGE. EREPORT
checks reserved target fields, then the 128-byte-aligned REPORTDATA address
and the 512-byte-aligned report destination. EGETKEY checks its 16-byte-aligned
output before reporting reserved request fields. An EPCM read fault delivers
#PF(0x8005), and a write fault delivers #PF(0x8007) in this user-mode profile.
EXINFO retains the precise address across AEX and ERESUME, including its page
offset. [Intel SDM, EGETKEY and EREPORT](https://cdrdv2-public.intel.com/874252/332831-090-sdm-vol-3d.pdf)

Bind an `X86SgxKeyPlatform` to the EPC bank to execute successful cryptographic
requests. The platform supplies the CPU version relation, key derivation,
hidden initialized SECS fields, report key ID and local report MAC. The
executor owns operand access, version and attribute errors, the 384-byte report
body, and the prepared output store. EGETKEY writes 16 bytes on success. Its
errors set ZF and return 2 (attributes), 32 (CPU SVN), 64 (ISV/config SVN) or
256 (key name), preserving the destination. EREPORT commits 432 bytes and
preserves the input RAX and arithmetic flags. A missing platform yields
`ModeUnavailable` for an otherwise valid request.

Intel's signed SDK simulation independently returned 2, 2, 2, 256 and 256 for
five prohibited or unknown key names and preserved all five outputs. It also
verified a self-addressed report. A NeverD regression supplies its recorded MAC
only for the identical target, key ID and 384-byte body. It compares the full
432-byte output, including when REPORTDATA aliases the destination. Separate
tests cover a nonzero KSS identity, a refused MAC, and key version ordering.
[SDK simulation and public report](https://github.com/gmh5225/test_mac_intel/actions/runs/46139290670)

The paged enclave path authenticates the original prefix and opcode bytes
before delivering UD0 or an instruction-length fault. A ModR/M fetch that
crosses into a non-executable EPC page raises #PF. With SMEP clear, its
user error code is `0x8005` with NXE clear and `0x8015` with NXE set.
For a virtual-interrupt conflict at a private EEXIT, AEX saves the unexecuted
EEXIT in the SSA. Resolving the conflict outside and executing ERESUME
restores its PC and operands. The same EEXIT then reaches its original target.
[Mixed-mode execution evidence](evidence/x86-code64-prefix-faults-2026-11-28.json)

### SGX2 dynamic enclave pages

An imported EPC bank with `X86SgxFeatures::SGX2` admits the x64 EACCEPT,
EMODPE and EACCEPTCOPY user leaves. EACCEPT checks SECINFO before examining
RCX, matches the request against the page, and clears its pending,
modified and permission-restricted flags on success.
A fresh EAUG page can be accepted immediately. Type changes and permission
restrictions use the page's imported tracking result. An incomplete tracking
cycle returns 11. The platform can publish a completed shootdown with
`bindPageTracking` before the next EACCEPT. An attribute mismatch returns
19. A missing tracking observation stops the executor at the original instruction.
EMODPE retains existing access rights when it adds permissions. EACCEPTCOPY validates
the source and pending destination before publishing the 4 KiB copy and its
new access rights. The bank is serialized across these operations.
[Intel SDM volume 3D, SGX page management and instruction references](https://cdrdv2-public.intel.com/868149/332831-089-sdm-vol-3d.pdf) · [Linux SGX enclave page management](https://github.com/torvalds/linux/blob/v6.18/arch/x86/kernel/cpu/sgx/ioctl.c)

The x64 NeverC header exposes `_enclu_bounded_u32` for constant leaves 0, 1
and 5 through 11. At O0 and O2 each call produced `llvm.x86.enclu.bounded`
in IR. The NeverC backend and LLVM r12 and r17 all emitted the requested leaf
selector in EAX and `0F 01 D7` for ENCLU. The 54 disassembly checks cover
code generation. The software execution cases above cover the SGX2 page leaves.
[Per-function selectors and object hashes](evidence/x86-code64-prefix-faults-2026-11-28.json)

The same instruction admission applies to ordinary generated enclave bodies
and the mixed-mode executor. RDTSC and RDTSCP raise #UD on SGX1 and #GP(0)
at CPL3 on SGX2 with CR4.TSD set. When admitted they use the existing
code64 body and its time source. [Intel SDM, enclave instruction behavior](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)

### EGETKEY256 and EREPORT2

Bind SGX256 to execute ENCLU leaves 11 and 10. EGETKEY256 accepts a 512-byte
KEYREQUEST and a 32-byte-aligned destination. Successful requests use the
platform's 256-bit key derivation and write exactly 32 bytes. The report and
provisioning key names return 256 with ZF set. CPUID enumerates SHA-384
measurements independently of SGX256.

EREPORT2 reads 64 bytes of report data from RBX and checks the 1024-byte-aligned
destination at RCX. A nonzero subtype in the full RDX returns 33. Subtype zero
builds the 512-byte TEEINFO, hashes it with SHA-384 and submits the report
prefix to the bound processor for HMAC-SHA256. An overlapping input is read
before the output is written.

The ISE operation writes 768 bytes and authenticates 192. The structure table
describes the last 256 bytes as zero. The same publication says that another
enclave can verify the report, and the verifying instruction authenticates
224 bytes. `Report2Write` selects the observed store size. `Report2MACCoverage`
selects the 192-byte operation or the 224-byte interoperable form. A successful
store requires both choices. Test vectors constructed with a published HMAC
key exercise issuance and verification on the same software processor.
[Intel ISE 319433-063, sections 7.5.5 and 7.6](https://cdrdv2-public.intel.com/932753/319433-063-architecture-instruction-set-extensions-programming-reference.pdf)

### EVERIFYREPORT2

CPUID.(12H,0):EAX[7] enumerates leaf 8. It reads a 256-byte REPORTMACSTRUCT
from private EPC and authenticates the first 224 bytes with the processor's
report MAC key. A version mismatch returns 32 before any MAC check. A MAC
mismatch returns 28. The guest report remains read-only throughout.

Bind the verification profile for the processor being represented. SDM 093
specifies page alignment and an exact match with the current CPUSVN. The
earlier TDX CPU specification 343754-002 specifies 256-byte alignment, checks
the TDX report type and reserved fields, and asks whether the CPU supports the
reported SVN. Both documents describe status flags in the operation and list
the affected flags as “None”. The software model can bind either flag outcome.
When the two outcomes differ, an unknown binding stops at the original
instruction with the incoming RAX and flags intact.
[Intel SDM 332831-093, EVERIFYREPORT2](https://cdrdv2-public.intel.com/929362/332831-093-sdm-vol-3d.pdf) · [Intel TDX CPU 343754-002, section 3.1](https://cdrdv2-public.intel.com/733582/intel-tdx-cpu-architectural-specification.pdf)

The software profile starts with initialized EPC pages. Supervisor enclave
construction and paging, CET and XSTATE beyond AVX remain open. The designated
hosts reported zero usable EPC sections. Native enclave qualification and the
processor-specific report bindings remain open.
