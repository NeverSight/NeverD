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

/// The request explicitly selects kernel-events-v6, an instruction-address
/// list or selected CFG entries, MXCSR mask and active paging width. Native
/// bindings are individually opt-in. Neither selection claims complete kernel
/// coverage nor authorizes an ordinary C ABI, source patch or native execution.
/// Both backends consume the same checked instruction-state LowIR. The emitted
/// driver stops at calls, returns, privilege transitions and exception entry.
/// Unsupported native requests and invalid state leave the step unexecuted.
llvm::Expected<X64ContinuationExport>
exportX64Continuation(const BinaryImage &Image, llvm::StringRef RequestJSON,
                      bool UseLLVM, DebugContext *Debug = nullptr);

} // namespace neverd

#endif // NEVERD_PIPELINE_X64CONTINUATIONEXPORT_H
