//===- X64RtmRegions.h - Native RTM region ownership -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a complete selected transaction inside one native checkpoint.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64RTMREGIONS_H
#define NEVERD_LIB_PIPELINE_X86_X64RTMREGIONS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>

namespace llvm {
class raw_ostream;
}
namespace neverd {
namespace analysis {
struct X64ContinuationInstructionSource;
}

/// Canonical rel16/rel32 XBEGIN with a canonical original fallback. The
/// architecture decoder remains the owner of signed displacement arithmetic.
std::optional<va_t> x64RtmFallback(llvm::ArrayRef<uint8_t> Bytes, va_t Address,
                                   unsigned AddressBits);

/// Emit the V6 callback and its admission receipt. The caller supplies an
/// instruction budget for each attempt; the callback returns only after
/// commit, original abort, or complete rollback on an unsupported path.
llvm::Expected<llvm::json::Object> emitX64RtmEnvironment(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits, bool FollowInternalCalls);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64RTMREGIONS_H
