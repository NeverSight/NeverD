//===- NeverDCAPIContinuation.h - Explicit x86 state C ------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Experimental state-source export, separate from ordinary function C.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_NEVERDCAPICONTINUATION_H
#define NEVERD_SDK_NEVERDCAPICONTINUATION_H

#include "neverd/sdk/NeverDCAPISession.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Export selected x64 instruction regions under an explicit kernel-events-v6
/// or intel-fast-system-v7 / intel-user-interrupt-v8 / intel-fred-return-v9
/// execution domain. RequestJSON is a size-delimited UTF-8 object; it selects
/// either instructions or entries, mxcsr_mask and paging_address_bits. Native
/// timestamp, prefetch, speculation MSR, descriptor and page-table bindings are
/// individually opt-in. UseLLVM is exactly 0 (HighC) or 1 (LLVM C).
/// V7 additionally requires sysenter_msr_address_bits and exports its separate
/// 2528-byte state. The V6 layout and driver statuses remain unchanged.
/// V8 also requires the boolean uiret_restores_uif feature binding and exports
/// its 2544-byte state. UIRET exits with status 11 before target execution.
/// V9 enables FRED with CET disabled and exports 2560-byte state. It only owns
/// canonical ERETS/ERETU to 64-bit targets and exits with status 12 before
/// target or pending-event execution. SYSENTER/UIRET bindings are not accepted.
/// amd-svm-v10 extends V4 to 2464 bytes with native SVM state and stops VMRUN
/// with status 13. It requires the documented native VMM domain and XCR0=7.
/// intel-mpx-v11 separately extends V4 to 2416 bytes with bounds, supervisor
/// MPX configuration and enable state. It requires host MPX disabled or
/// BNDPRESERVE set. Bounds faults stop with status 14 at the original PC before
/// exception delivery. V10/V11 require mxcsr_mask but no paging/Intel-return
/// bindings; neither inherits the V6/V7/V8/V9 state or return capabilities.
/// native-smm-v12 extends V4 to 2368 bytes and admits canonical RSM in active
/// native SMM, code64, XCR0=7. It requires mxcsr_mask and
/// segment_base_msr_address_bits. The SMM.XState image owns x87; explicit
/// vectors/MXCSR and CR2/CR8/KernelGSBase are transported before native RSM.
/// Successful transfer abandons the compiler frame and the bounded driver;
/// hardware consumes the ambient SMI save image. Invalid state returns 5.
/// This does not emulate SMI entry or supply SMRAM, other MSRs or devices.
/// native-sgx-v13 extends V4 to 2464 bytes and transports GPR/flags/x87/SIMD
/// state to canonical ENCLU leaves 2/3/4 at the original executable address.
/// It requires mxcsr_mask and native CPL3 code64 SGX, XCR0=7, IF=1, IOPL=0,
/// CET disabled and the documented original enclave/runtime/memory domain.
/// The terminal jump preserves EENTER's observable next PC; original native
/// continuation/AEP code owns subsequent execution. It supplies no software
/// EPC/SSA/AEX model or fault/async delivery during input transport.
///
/// V6 event_delivery exports neverd_x64_deliver_event_v6 for an explicit
/// selected fault/trap/IRQ/NMI, including compatibility entry and CPL0..3.
/// It returns 7 after handler setup, 15 while inhibited, or 5 for invalid
/// input/preconditions. It also routes canonical UD2 and register DIV/IDIV
/// at 8/16/32/64 bits to #UD/#DE. The producer owns priority, restart state and
/// shadows; delivery RAM must succeed. See docs/x86-event-delivery.md for the
/// contract.
///
/// The returned C includes its state layout and a bounded resume driver. It
/// does not use an ordinary function ABI or execute the loaded image. CALL,
/// RET, privilege return and exception entry stop at environment boundaries.
/// The optional boolean follow_internal_calls continues CALL/RET only when
/// their actual post-transfer PC is selected. It uses physical guest stack
/// bytes, including callback mutations; context transitions still stop.
/// An unimplemented native request leaves the instruction unexecuted.
/// The additional neverd_x64_run_vN driver accepts an explicit full-state
/// environment callback and an independent invocation budget. The provider
/// owns native/environment effects and may replace the physical stack or PC.
/// Invalid state and exhausted instruction budgets never invoke it; a null
/// provider retains the original stopping behavior. This supplies no missing
/// ordinary arguments or Windows service implementation.
/// Optional pause_call_sites requires bound function_calls.call_sites and
/// stops with status 21 before their original CALLs. The driver retains the
/// full pre-CALL state and spends no budget at that stop; zero instruction
/// budget still returns 9. The named bridge validates and performs the CALL.
/// Preserve caller and nested site contexts independently across pauses.
/// Environment providers do not handle status 21. This does not recover an
/// initial state from ordinary arguments or capture a live native process.
/// V6 function_context also supports paged_memory through
/// neverd_x64_run_paged_function_v6. Initialize its caller-owned activation at
/// an entry and retain the same paged execution context and guest virtual
/// return-cell lifetime. An attempted successful selected near RET through
/// that cell completes; fault, backing and changed-code stops remain active.
/// Paged function contexts do not accept function_calls, call-site pauses,
/// scheduled events or direct dispatch. They supply no native snapshot,
/// inferred mapping identity or ordinary scalar inputs.
///
/// On success, ManifestJSON receives an independently owned JSON manifest of
/// exact original bytes, input bindings, state offsets and the C source hash.
/// If non-NULL, it is cleared before any failure. Free both returned strings
/// with neverd_free_string(). NULL source indicates failure; inspect
/// neverd_last_error(). No implicit ordinary-decompile fallback is enabled.
NEVERD_API const char *
neverd_x64_continuation_source_v1(neverd_session_t Sess,
                                  const char *RequestJSON, size_t RequestSize,
                                  int UseLLVM, const char **ManifestJSON);

/// Export an i386 image's selected protected 16/32-bit original instructions.
/// RequestJSON declares profile protected-legacy-v1 and instructions containing
/// address (linear uint32), code_bits (16/32) and size (1..15). The executor's
/// grammar authenticates every complete extent; unsupported encodings fail.
/// The single runtime-backed C route calls X86LegacySourceExecution from the
/// matching source build. Its state, RAM, runtime profiles, devices and event
/// arbitration are supplied by the C++ embedding caller, not inferred here.
/// Generated code follows selected targets and stops for every event/backing
/// failure. Its opaque context must remain alive across pauses. This is not a
/// portable C state ABI, ordinary-function decompilation or native CPU capture.
/// ManifestJSON follows the ownership and failure rules documented above.
NEVERD_API const char *neverd_x86_legacy_source_v1(neverd_session_t Sess,
                                                   const char *RequestJSON,
                                                   size_t RequestSize,
                                                   const char **ManifestJSON);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_NEVERDCAPICONTINUATION_H
