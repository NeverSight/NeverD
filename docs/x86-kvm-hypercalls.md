# Explicit KVM hypercalls

`native_kvm_hypercalls: true` binds canonical original `0F 01 C1` (VMCALL)
and `0F 01 D9` (VMMCALL) to the returning KVM register ABI in the explicit
[x64 continuation](x64-continuation-source.md). RAX supplies the call number;
RBX, RCX, RDX and RSI supply four arguments. The returned value replaces RAX.
Both HighC and LLVM C preserve the other represented state and advance PC by
three bytes after completion. A completed call consumes one instruction from
the driver budget. Its unknown memory effects remain ordered with guest RAM.

```json
{
  "profile": "kernel-events-v6",
  "entries": ["0x140001000"],
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48,
  "native_kvm_hypercalls": true
}
```

The option declares the executing environment. It does not detect KVM from
the opcode, CPUID or input file. Without it, both instructions stop with
`NativeInstruction` and preserve state and budget. Ordinary strict and
permissive lifting also refuse both raw instructions: the earlier implicit
Hyper-V RCX/RDX/R8 interpretation was unsound. Existing manually constructed
legacy intrinsic spellings do not establish a generic hypervisor ABI.

The byte reader owns the canonical encoding check. Selection follows bound
hypercalls to their fallthrough at the same call depth, even when ordinary
lifting refuses the containing function; the manifest records these edges
as `hypercall_successors`. Prefix variants are not admitted by this binding.
The exact request, original bytes and source hash remain in the manifest.

## Execution domain

The [KVM ABI](https://docs.kernel.org/virt/kvm/x86/hypercalls.html) preserves
registers other than rAX unless a particular service states otherwise. This
binding additionally requires the selected service to return normally,
preserve all other represented state and the compiler execution environment,
and operate on the same mapped guest RAM. Code, compiler stack, continuation
state and budget stay accessible and disjoint from service writes. Services
that depend on caller state beyond those five inputs and guest RAM, change registers or
system state, remap required storage, or transfer execution need a different
contract. All entry-state guards and fault/event restrictions of the selected
continuation profile still apply; no new TF/RF or fault-delivery domain is
introduced here.

Hyper-V has a separate [hypercall contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface),
including repeated calls and fast calls with additional register outputs.
This KVM binding does not implement it or arbitrary full-machine hypercalls.

## Native compiler interfaces

NeverSight LLVM provides `llvm.x86.vmcall.kvm` and `llvm.x86.vmmcall.kvm`,
each returning i64 from five i64 inputs. Their `kvm32` variants use six i32
values and require 32-bit code. Mismatched modes and 16-bit code fail clearly.
The intrinsics are convergent, nonduplicable and may read/write arbitrary
memory, including when the result is unused. Dedicated machine pseudos retain
the five physical inputs and result through register allocation; object
emission uses the original instruction bytes with no inline assembly helper.

NeverC's x64 frontend exposes `__builtin_ia32_vmcall_kvm` and
`__builtin_ia32_vmmcall_kvm`, plus `__kvm_vmcall` and `__kvm_vmmcall` from
`<kvmhypercallintrin.h>`. Its pruned backend remains x64-only; the full LLVM
fork supplies code32. NeverD's older pinned SDK can carry the conservative
named declarations for LLVM C output. Native object emission refuses them
until the SDK recognizes the intrinsic, instead of emitting an unresolved
`llvm.*` call. The final SDK release and pin remain part of the wider x86 work.

## Reproducing qualification

`X64ContinuationExportTest.KvmHypercallsTransportAllFiveInputs` checks both
C routes and dispatch styles against a five-input provider at O0/O2, including
all represented state, return values, memory effects, fallthrough selection
and multi-instruction resume. The companion unbound test verifies the explicit
boundary without executing a hypercall.

NeverC's `tests/neverc/X86/verify_kvm_hypercalls.py` runs independent original
instruction bytes and compiler-generated calls in an isolated KVM guest.
`KVM_CAP_EXIT_HYPERCALL` routes call 12 to a test service that observes all five
physical inputs, returns an independently computed value and writes shared
RAM. This qualifies ABI transport, not the production semantics of that call.
Each O0/O2 profile compares 256 original/candidate pairs and executes 512 native
hypercalls. Unavailable KVM is an explicit failure, never a passing skip.

Use `--llvm-only --backend-llc /path/to/llc --bits 32` or `--bits 64` to test
the full fork, and add `--new-pm` for its other codegen pipeline. The default
route uses NeverC directly; `--backend-llc` uses NeverC IR with the full fork.
`--source kvm-high.c --manifest kvm-high.json` (or `kvm-llvm`) consumes actual
NeverD output and verifies its source hash before compiling. Every invocation
requires a separate `--keep DIRECTORY` and retains tool/file hashes, commands
and runtime logs.
