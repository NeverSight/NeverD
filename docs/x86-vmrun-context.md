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
xstate is loaded. SVM setup, GIF, intercepts, supervisor state and any required
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
projection. **Ordinary strict lifting of raw VMRUN remains refused.** It still
needs a pipeline-owned complete machine-state continuation, including the
software-managed supervisor state and the guest/host memory environment.
The successful native test cannot supply that missing ownership by inference.
Physical AMD execution, other XCR0 components, faults, nested SVM, SEV-ES and
arbitrary guest system/mode changes are unqualified. The SDK release, pin and
LLVM submodule update are not advanced on the strength of this bounded API.
