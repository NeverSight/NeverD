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
