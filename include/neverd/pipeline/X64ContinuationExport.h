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
/// Context transitions always stop, including selected targets. V7 requires
/// sysenter_msr_address_bits and returns a distinct V7 state/driver. The
/// intel-user-interrupt-v8 profile additionally requires uiret_restores_uif,
/// owns UIF/monitor validity and stops UIRET at its completed return context.
/// intel-fred-return-v9 owns canonical ERETS/ERETU to 64-bit targets with
/// explicit FRED configuration and pending-event outputs. Its 2560-byte state
/// exits with status 12; it does not inherit the FRED-disabled V6/V7/V8
/// domains. Unsupported native requests and invalid state leave the step
/// unexecuted.
llvm::Expected<X64ContinuationExport>
exportX64Continuation(const BinaryImage &Image, llvm::StringRef RequestJSON,
                      bool UseLLVM, DebugContext *Debug = nullptr);

} // namespace neverd

#endif // NEVERD_PIPELINE_X64CONTINUATIONEXPORT_H
