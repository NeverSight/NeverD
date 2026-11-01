# Native SGX transfer state

The `native-sgx-v13` source profile connects canonical `0f 01 d7` ENCLU to
explicit GPR, flags and x87/SSE/AVX input state through HighC and LLVM-to-C.
It admits low EAX 2 (EENTER), 3 (ERESUME) and 4 (EEXIT). The transfer restores
the complete native input bank and jumps to the **original executable
instruction address**. EENTER's RCX next-PC output therefore retains the
original address plus three. A relocated ENCLU would change that output.

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
any state/image write. Invalid invocation returns status 5 unchanged. Other
instructions or prefixed ENCLU require native execution at status 4; they
are not silently replaced or advanced.

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
intrinsics, with no assembly fallback. The full fork also owns code32 and
x32 transport; V13 source itself remains x64.

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

The local host has no SGX capability or device. These tests stop before
ENCLU and execute zero enclave instructions. Successful EENTER/ERESUME/EEXIT,
SSA/AEX behavior and native enclave/runtime integration are therefore not
qualified here. A software enclave model, complete legacy i386 dispatcher and
general fault/asynchronous-event execution remain separate work; this profile
does not make them disappear.

Architectural ownership follows Intel SDM revision 090, ENCLU/EENTER/ERESUME/
EEXIT and enclave entry/exit chapters, available from the
[Intel SDM index](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
