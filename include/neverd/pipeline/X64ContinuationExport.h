//===- X64ContinuationExport.h - Explicit x64 state source -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded source and manifest export for an explicitly declared x64 domain.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_X64CONTINUATIONEXPORT_H
#define NEVERD_PIPELINE_X64CONTINUATIONEXPORT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <string>

namespace neverd {
struct BinaryImage;
class DebugContext;

namespace limits::pipeline {
inline constexpr size_t kX64ContinuationRequestBytes = 4u << 20;
inline constexpr size_t kX64ContinuationSourceBytes = 128u << 20;
/// Limit dependent CFG-discovery waves; instruction/operation budgets still
/// bound their union. Explicit roots can select deeper regions independently.
inline constexpr unsigned kX64ContinuationCalleeDepth = 8;
/// Bound each linked driver's optimization graph. A single 9,900-step function
/// exceeded the five-minute qualification compile bound at O2.
inline constexpr size_t kX64ContinuationLinkedSteps = 64;
} // namespace limits::pipeline

struct X64ContinuationExport {
  std::string Source;
  std::string Manifest;
};

/// The request explicitly selects kernel-events-v6 or intel-fast-system-v7, an
/// instruction-address list or selected CFG entries, MXCSR mask and active
/// paging width. Native bindings are individually opt-in. Neither selection
/// claims complete kernel coverage nor authorizes an ordinary C ABI, source
/// patch or native execution. Both backends consume the same checked
/// instruction-state LowIR. Calls and returns stop by default; the explicit
/// follow_internal_calls option continues them only at selected actual targets.
/// Context transitions always stop, including selected targets.
/// An optional callee_depth selects bounded original direct CALL descendants
/// of entries, once per body. It requires follow_internal_calls. The optional
/// follow_direct_jumps discovers original direct JMP targets outside each
/// entry region at the same call depth, retaining physical transfer semantics.
/// It requires entries and defaults to false. dispatch may
/// select direct PC-checked links instead of the default per-step switch;
/// neither option changes the explicit-state interface or native boundaries.
/// Each export also defines neverd_x64_run_vN with an explicit full-state
/// environment callback and a separate provider-invocation budget. The
/// original continue driver remains available without environment dispatch.
/// Providers own their effects; no ordinary ABI or native service is inferred.
/// V6 function_context adds begin/continue/run_function entry points for a
/// producer-declared entry return cell. Successful selected near RET through
/// that cell completes the activation even at a selected destination. It
/// requires entries and follow_internal_calls. With paged_memory, the entry is
/// neverd_x64_run_paged_function_v6: retain the same paged execution context
/// and guest virtual return-cell lifetime across pauses. Only attempted
/// successful near RET completes; faults, backing stops and changed code remain
/// active. Scheduled contexts retain their pending-event and inhibition state
/// across pauses; native RTM regions remain a separate contract.
/// Ordinary drivers retain their original behavior. This supplies no dynamic
/// frame, address-space ownership or source-ABI proof. Optional function_calls
/// declare checked Win64 scalar entry views alongside plain or paged
/// function_context. Arguments must match the original state; no register or
/// RAM input is initialized. Separate resume wrappers retain the activation and
/// cache its explicitly completed result. Declarations do not infer a native
/// signature or remove the original environment obligations. A null argument
/// width keeps that physical slot only in the native context, without a scalar
/// formal or entry read. Later arguments retain their slots. Optional
/// call_sites on a function_calls declaration bind selected original direct
/// CALLs to that entry. Pre-CALL checks use the shared caller layout; the
/// original step creates the return cell before function-context entry.
/// Separate site contexts retain pending and active calls across budget pauses.
/// This does not capture full native state from an ordinary C caller.
/// pause_call_sites optionally stops both drivers with status 21 before each
/// bound CALL, preserving state and RAM without spending an instruction or
/// environment unit. Zero instruction budget still returns 9. Keep the caller
/// activation, invoke the site's bridge with this pre-CALL state, then resume
/// the caller after that bridge completes. Nested stops need separate site
/// contexts. The original step still owns state validation and CALL effects.
/// V7 requires sysenter_msr_address_bits and returns a distinct V7
/// state/driver. The intel-user-interrupt-v8 profile additionally requires
/// uiret_restores_uif, owns UIF/monitor validity and stops UIRET at its
/// completed return context. intel-fred-return-v9 owns canonical ERETS/ERETU to
/// 64-bit targets with explicit FRED configuration and pending-event outputs.
/// Its 2560-byte state exits with status 12; it does not inherit the
/// FRED-disabled V6/V7/V8 domains. Unsupported native requests and invalid
/// state leave the step unexecuted.
/// amd-svm-v10 instead extends V4 with the explicit native SVM context. It
/// requires MXCSR mask, unchanged XCR0=7 and the documented native VMM domain;
/// it does not accept paging/Intel-return bindings. Completed VMRUN stops at
/// status 13 with all represented guest outputs and the original fallthrough.
/// intel-mpx-v11 separately extends V4 with explicit bounds, supervisor MPX
/// configuration and enable state. Host MPX is disabled or BNDPRESERVE is set.
/// It requires MXCSR mask and no paging/Intel-return bindings. A bounds fault
/// stops with status 14 at its original PC before exception delivery; original
/// near transfers own conditional bounds initialization.
/// V6 paged_memory selects the owned-RAM scalar instruction executor and
/// neverd_x64_run_paged_v6. Its explicit context is X64PagedExecution; guest
/// memory and synchronous faults use that C++ component's bridge. It cannot
/// be combined with host-RAM native providers, RTM regions or direct dispatch.
/// Its scheduled entry separately owns pending events and inhibition shadows.
/// paged_debug adds a nontransactional instruction-debug bank; runtime profiles
/// additionally bind execution comparators and MOV-DR/GD. Function contexts
/// currently require their separate debug completion ABI.
/// Fetch verification reports changed code as status 20;
/// backing failure and shutdown are statuses 18 and 19. Partial-completion
/// instructions retain a native boundary. This bridge is not a stable C SDK
/// runtime interface.
llvm::Expected<X64ContinuationExport>
exportX64Continuation(const BinaryImage &Image, llvm::StringRef RequestJSON,
                      bool UseLLVM, DebugContext *Debug = nullptr);

} // namespace neverd

#endif // NEVERD_PIPELINE_X64CONTINUATIONEXPORT_H
