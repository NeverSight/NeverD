# Native SMM return

`native-smm-v12` admits canonical `RSM` (`0F AA`) while executing in active
native SMM in code64. It uses a terminal native LLVM intrinsic to restore the
software-owned state immediately before the processor consumes its SMI save
image. HighC and LLVM-to-C share the same state import and admission logic.

```json
{
  "profile": "native-smm-v12",
  "mxcsr_mask": "0xffbf",
  "segment_base_msr_address_bits": 48,
  "instructions": [4096]
}
```

The 2368-byte state extends V4. `SMM.XState` is a nonzero, 64-byte-aligned
linear address of an accessible 832-byte standard XSAVE image, disjoint from
the state, compiler frame and processor SMRAM save image. This profile requires
live XCR0=7 and unchanged XSAVE capabilities. XSTATE_BV may contain only bits
0–2, and XCOMP_BV and reserved header bytes must be zero. The image owns x87;
its x87 component must be valid for XRSTOR. Explicit `ZMM[0..15][0..31]` and
`MXCSR` own SSE/AVX and overwrite the corresponding image fields. Components
1 and 2 are marked present. Remaining vector storage stays unchanged.

`SMM.XStateMask`, `SMM.CR2`, `SMM.CR8` and `SMM.KernelGSBase` are output transport
slots populated from the profile and the core's authoritative cells. They are
not competing state inputs. KernelGSBase must be canonical at the declared
CPU MSR width; CR8 and MXCSR retain their existing validity checks. Invalid
metadata or state returns 5 before writing the state or image. This is an
invocation rejection, not an emulated CPU exception.

The final native sequence writes CR2, CR8 and IA32_KERNEL_GS_BASE, then executes
XRSTOR64 immediately followed by RSM. One terminal backend pseudo owns the
sequence until MC emission, after register allocation and scheduling. There is
no intervening compiler instruction, vector scratch use, stack access or
ordinary return epilogue. This also avoids reading context memory after a
restored PKRU in the more general compiler intrinsic.

The processor's actual SMI save image owns the restored GPRs, PC, RFLAGS,
segment caches, descriptor tables and RSM-managed system registers. The V4
core's pre-RSM GPRs and flags do not replace that image. Other live MSRs, debug
registers, device state and memory mappings remain owned by the native SMM
environment. The interface does not decode reserved SMRAM bytes or assume a
universal Intel/AMD save layout. Rules follow
[Intel SDM Volume 3C, section 34.4.1](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

A successful RSM leaves the generated function and its bounded driver entirely.
The native target executes in the context restored by hardware; there is no
success status or updated explicit post-RSM state for the old C caller. The
manifest counts the admitted instruction separately from unimplemented native
requests. Other instructions in this profile remain explicit native requests.
This profile does not inherit V6's descriptor/event or V10's SVM domain.

The compiler API is `void llvm.x86.rsm.context(ptr)` and NeverC's
`__builtin_ia32_rsm_context(const void *)`, with a typed wrapper in
`<smmcontextintrin.h>`. Its five 64-bit slots are XState, XStateMask, CR2, CR8 and
KernelGSBase. The compiler interface can restore any valid standard XRSTOR
image selected by live XCR0 and the mask; the NeverD V12 projection above
specifically owns XCR0=7. It requires x86-64 and the XSAVE target feature and
has convergent, noduplicate, noreturn, nounwind and conservative memory effects.

Verification uses KVM-mediated SMM, not a bare-metal firmware claim. An SMI
creates the save image independently; the harness establishes code64 inside
SMM without changing that image, executes the generated transfer and compares
it to original XRSTOR64/RSM bytes on another vCPU. Inputs include populated and
initial xstate components, nonzero register high halves, CR2/CR8/KernelGSBase,
multiple compiler frames, flags and destination stacks. Both memory spaces,
system/event state and XSAVE outputs are compared. Initial XSAVE components
are compared by their architectural values: Intel permits XINUSE to be set
even when a component holds its initial state. Populated payloads are still
compared in full. Both a replaced RSM opcode and an omitted XRSTOR must fail.
The source oracle keeps the independent input image before NeverD imports its
explicit SIMD cells, so an incorrect import cannot become its own reference.
Run the NeverC `verify_frame_returns.py --smm-context` verifier for this
compiler boundary, and `X64ContinuationExportTest.SmmReturn*` for source import
and rejection. Source artifact capture uses `NEVERD_ISA_REGRESSION_ARTIFACT_DIR`.

Run `scripts/isa/x86/verify_smm_return.py --sources CAPTURE --compiler NEVERC
--compiler-source NEVERC_SOURCE --llvm-bin LLVM_BIN --keep OUTPUT` to execute
both captured C routes through NeverC and the full LLVM legacy/new backends
at O0/O2. Each profile exercises four compiler frames, including live integer
pressure and a dynamically sized, aligned local array.

The [qualification record](evidence/x86-smm-return-2026-11-01.json) retains
both SDK source hashes, two complete 204-test continuation runs with no skips,
six compiler profiles (18,432 native state pairs), and twelve source-export
profiles (36,864 pairs). LLVM's compiler tests also cover x32 pointer widening,
ELF/COFF/Mach-O, both backend pass managers and rejected modes/features.

SMI selection/entry, CET, VMX/SVM interactions, enclave transitions, faults,
asynchronous interruption and bare-metal SMM remain outside this projection.
They must not be inferred from successful RSM transport or counted as qualified
by these tests. SGX and legacy i386 continuation work retain separate boundaries.
