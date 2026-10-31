# VMRUN: native context and IDA comparison

VMRUN transfers control into an SVM guest and resumes the host after VMEXIT.
AMD APM Volume 2, sections 15.5.1–15.5.2 distinguish hardware-restored host
RAX/RSP/RIP/RFLAGS from fourteen software-managed GPRs and the floating/vector
bank. A scalar helper taking only a VMCB address cannot express those outputs.

## Local IDA counterexample

The local IDA/Hex-Rays analysis of this original x64 function produced
`__svm_vmrun(a1); return 4660;`:

```asm
mov rax, rdi
mov ecx, 0x1234
vmrun
mov rax, rcx
ret
```

The independent guest writes RCX=0x5678 and exits using VMMCALL. Executing
these original host bytes in a disposable QEMU TCG SVM machine returns 0x5678,
not 0x1234. The analysis and byte fixtures are recorded in
[the evidence record](evidence/x86-vmrun-context-2026-10-30.json). This is one
concrete counterexample to the local scalar projection, not a claim about all
IDA versions, all inputs or overall decompiler quality.

## Implemented contract

The LLVM fork supplies `llvm.x86.vmrun.context(ptr, i32 immarg)` and NeverC
supplies `__builtin_ia32_vmrun_context(context, width)`. Its installed
`<svmcontextintrin.h>` defines the context and the constant-width wrappers
`__svm_vmrun_context64` and `__svm_vmrun_context32`.

The context is 144 bytes: four u64 fields (VMCB physical address, guest XSAVE
linear address, host XSAVE scratch linear address, XCR0 mask), followed by
RBX, RCX, RDX, RSI, RDI, RBP and R8..R15 as fourteen u64 input/output fields.
Guest RAX/RSP/RIP/RFLAGS and exit information stay in the VMCB. An invalid
VMCB exit must not be interpreted as successful execution of its guest state.

Both widths execute in 64-bit code. Addr32 truncates the VMCB physical address;
the context and XSAVE addresses remain full-width linear pointers. The native
backend isolates an RSP-based helper frame, transports every GPR regardless
of ordinary source liveness, saves guest xstate and restores host user xstate
and the compiler's GPR ABI. No inline assembly or unresolved runtime helper is
required in user code. Calls retain unknown memory effects, convergent and
noduplicate properties and a returning continuation.

The caller provides CPUID-sized, 64-byte-aligned standard XSAVE buffers,
initialized reserved header bytes and a mask covering all enabled XCR0 user
components. XCR0 stays unchanged. Context, buffers and compiler stack must
remain accessible and disjoint from guest-writable RAM, including after guest
xstate is loaded. They must also be disjoint from the hardware-written VMCB
and VM_HSAVE_PA pages. SVM setup, GIF, intercepts, supervisor state and any required
VMLOAD/VMSAVE remain VMM responsibilities. Guest system-state changes must
preserve the host's software ABI. Processor faults are not caught.

LLVMC recognizes this exact intrinsic, including modules read by the older
pinned SDK, and emits the NeverC builtin. It checks x64 mode, types, width,
returning/convergence effects and unknown memory effects. A readonly or
argument-memory-only declaration is rejected. A load of RCX at context offset
40 remains after the call; it cannot be folded into the pre-VMRUN value.

## Verification and remaining boundary

The NeverC runner `tests/neverc/X86/verify_vmrun_context.py` checks 48 contexts
per O0/O2 profile: eight seeds, two address widths, VMMCALL/HLT/invalid-VMCB
exits, all fourteen GPRs, x87, all sixteen XMM/YMM registers, host state and
dynamic caller frames. The original-byte IDA counterexample is independent
of the compiler wrapper and runs in every guest. `--neverd-source` compiles
the actual C exported by `LLVMCValues.VmrunContextKeepsGuestRegisterOutputs`;
the test exports that artifact when `NEVERD_ISA_REGRESSION_ARTIFACT_DIR` is set.
The evidence records the executed routes and binary hashes.
Each profile additionally executes the same RCX-writing guest through the
generated context, checking 0x5678 in the register bank and, for the NeverD
routes, the exported function's actual return. The final five-route matrix
therefore contains 490 generated-context round trips and ten independent
original-byte counterexamples.

This completes a native explicit GPR/user-xstate interface and its IR-to-C
projection. The original 2026-10-30 qualification did not yet connect raw
instruction selection to that interface. The following separate source profile
now provides that connection without inferring an ordinary function ABI.
Physical AMD execution, other XCR0 components, faults, nested SVM, SEV-ES and
arbitrary guest system/mode changes are unqualified. The SDK release, pin and
LLVM submodule update are not advanced on the strength of this bounded API.

## Original-byte source continuation

`amd-svm-v10` accepts canonical long-mode `0F 01 D8` and `67 0F 01 D8`
through the binary instruction reader. Both HighC and LLVM C consume one
checked state transition. The intrinsic has an explicit context pointer and
immediate address width, no scalar result, and unknown memory effects. Old
SDKs can transport this interface into LLVM C; native object/rewrite emission
refuses until the SDK actually supports it.

Pass this request to `--x64-continuation` (using the addresses of the selected
original instructions), or to the existing size-delimited C SDK:

```json
{
  "profile": "amd-svm-v10",
  "mxcsr_mask": "0xffff",
  "instructions": [4096, 4099, 4104, 4107, 4110]
}
```

The generated `neverd_x64_state_v10` is 2464 bytes with 16-byte alignment.
Its first 2320 bytes retain V4; it then appends the native `SVM` context. This
does not extend the Intel event/return profiles. Callers provide
`SVM.GuestXState` and `SVM.HostXState`: distinct, accessible, 64-byte-aligned
832-byte **standard** XSAVE images. The guest image owns x87; the explicit
state owns MXCSR and the low 32 bytes of ZMM0..15. Before entry the transition
imports these SIMD cells into the image and marks them present. After exit
it publishes all fourteen GPRs and SIMD/MXCSR outputs, including initial-state
components whose XSAVE bit is clear. Unrepresented upper-vector storage is
preserved. Other SVM context fields are exchange outputs/scratch, not another
set of instruction inputs.

Host RAX and RSP remain unchanged. Addr32 truncates only the VMCB physical
address, not restored host RAX. Host RFLAGS retain their values except RF;
guest RAX/RSP/RIP/RFLAGS and exit information remain in the VMCB. CR2 is read
from the actual CPU after VMEXIT because an invalid VMCB may exit before its
guest save area has been installed. Reading a VMCB field instead would invent
completion on that path.

Every VMEXIT, including VMEXIT_INVALID, returns status **13**, advances PC to
the original fallthrough, and consumes one instruction budget unit. Both
dispatcher forms stop even if the fallthrough is selected. The environment
can handle the exit and resume the following original instructions. Invalid
serialized controls return status 5 before any state or exchange-image write;
earlier profiles keep VMRUN as an unexecuted native boundary.

Function `entries` selection follows each canonical VMRUN's VMEXIT fallthrough
at the same call depth. The manifest records these `vmexit_successors`; the
ordinary strict lifter still refuses the instruction. Selection discovers the
resume code, while execution still stops at each VMEXIT so the VMM can handle
it before resuming. Direct CALL targets come from decoding the audited
instruction starts, including in a rejected containing body; they do not
depend on a surviving LowFunc or on propagated register values. Calls before
and after VMEXIT retain the requested `callee_depth` boundary.

The profile binds a CPL0 native SVM environment with unchanged XCR0=7. The
VMM owns VMCB/VM_HSAVE_PA, GIF, intercepts, debug/supervisor banks and mappings.
FS/GS/KernelGSBase and CR8 must agree with the explicit cells and remain
guest-preserved; other system changes must preserve the compiler's host ABI.
Generated code, state, buffers and compiler stack stay mapped and disjoint
from guest writes and hardware-written VMCB/VM_HSAVE_PA pages. Successful
ordinary RAM, immutable original code, no
concurrent writers, asynchronous events, faults, nested SVM, SEV-ES, SMM or
CET are part of the invocation domain. **Ordinary function-ABI lifting of
arbitrary VMRUN remains refused**: environmental facts are never inferred.

The original-byte qualification runs both actual exported C forms through
NeverC and through NeverC bitcode followed by the full LLVM fork, each at
O0/O2. Its eight profiles complete 400 generated-context calls, eight
independent executions of the original IDA counterexample and eight original
addr32/RAX checks. Each profile includes the complete selected original
function, including its VMEXIT stop and subsequent MOV/RET. See
[the source-continuation evidence](evidence/x86-vmrun-source-2026-10-31.json)
for commands, source/tool hashes and the bounded invocation domain.
