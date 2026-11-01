# Explicit x64 Hyper-V hypercalls

`native_hyperv_hypercall_entry` binds canonical VMCALL/VMMCALL to a supplied
executable Hyper-V page. It accepts an integer or hexadecimal string and is
mutually exclusive with `native_kvm_hypercalls`. The ordinary lifter continues
to require an ABI binding. The request and native environment appear in the
continuation manifest.

Both HighC and LLVMC emit `__builtin_ia32_hyperv_call(entry, gprs, xmm, 64)`.
The new LLVM intrinsic `llvm.x86.hyperv.call` transports RCX/RDX/R8 and six
XMM inputs, then publishes RAX/RCX/RDX/R8 and six XMM outputs. Remaining GPRs,
vector upper lanes, flags and represented control state remain unchanged.
The compiler emits a native CALL to the registered page and owns its stack
frame; no handwritten assembly is required in generated C. The old SDK may
transport this IR to C but refuses native object generation until it knows
the intrinsic.

The invocation supplies compatible native privilege, features, mappings and
service behavior. Services may access guest RAM but must preserve the compiler
environment and must not access state, budget, generated code or compiler stack.
The original instruction and the page must denote the same service invocation.
The page owns transparent rep restarts. Explicit source publishes the completed
register bank and advances the original PC by three bytes. `SetVpRegisters`
(0x51) is rejected before effects because it can replace caller state.

Tests compare entire state objects and RAM with independent providers for
ordinary, fast, rep and fast-rep calls, both opcodes and both C routes. Native
compiler tests additionally check all three vector strides, unaligned banks,
SysV/Win64 callers and live stack data. These are transport tests; they do not
certify all services, asynchronous events or Microsoft Hyper-V hardware runs.

References: [TLFS interface](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface)
and [SetVpRegisters](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallsetvpregisters).

## Qualification on 2026-10-31

The [recorded matrix](evidence/x86-hyperv-hypercalls-2026-10-31.json) covers
22 native configurations, 8,448 original/candidate pairs and 16,896 actual
registered Hyper-V page calls. Of those, 4,608 pairs compile freshly exported
NeverD HighC/LLVMC through NeverC and both full LLVM pass managers at O0/O2.
KVM supplies the guest execution and page; an independent userspace service
supplies deterministic ordinary, fast and rep behavior. This qualifies the
register transport, not Microsoft service implementations.

The independent host page ABI test covers 9,216 bank comparisons and 3,072
live floating-point cases across SysV/Win64 callers, all three strides and
unaligned banks. Portable source checks compare 8,192 complete state/RAM cases.
Fresh VMRUN regressions add 600 contexts, 192 invalid-VMCB/CR2 pairs, 12 IDA
counterexamples and 12 addr32-RAX cases under QEMU TCG SVM. KVM hypercalls also
pass both optimization levels through all three compiler paths.

NeverD's shared-boundary matrix has 622 cases: r6 passes 600 with 22 skips;
the current LLVM development build passes 612 with 10 skips and no failures.
Nine skips are NeverC's intentionally unavailable i386 backend; the remaining
skip requires physical AMX-TILE hardware. The newly exported Hyper-V and SVM
C/manifests are byte-identical between the two LLVM builds. LLVM's focused lit
suite passes 13 checks, NeverC frontend tests pass seven and generated-table
tests pass 83. The evidence distinguishes implementation, built and published
commits. The development LLVM build is not a released SDK; the production
release pin and LLVM submodule remain unchanged pending the other instruction
families.
