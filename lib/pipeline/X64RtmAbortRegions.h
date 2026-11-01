//===- X64RtmAbortRegions.h - Bound immediate RTM aborts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate and execute an indivisible XBEGIN / immediate XABORT region.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X64RTMABORTREGIONS_H
#define NEVERD_LIB_PIPELINE_X64RTMABORTREGIONS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <cstdint>
#include <optional>

namespace llvm {
class raw_ostream;
}
namespace neverd {
struct BinaryImage;
namespace analysis {
struct X64ContinuationInstructionSource;
}

struct X64RtmAbortRegion {
  va_t Address;
  va_t Fallback;
  std::array<uint8_t, 9> Bytes;
};

/// Canonical x64 rel32 XBEGIN immediately followed by canonical XABORT imm8.
/// The existing transaction decoder owns signed fallback arithmetic. No other
/// transactional body or nested transaction receives this native binding.
std::optional<X64RtmAbortRegion> readX64RtmAbortRegion(const BinaryImage &Image,
                                                       va_t Address,
                                                       unsigned AddressBits);

/// Emit an explicit V6 environment callback and receipts for its accepted
/// pairs. One environment invocation owns the complete pair. Instruction steps
/// retain their native boundary and never expose an active transaction.
llvm::json::Array emitX64RtmAbortEnvironment(
    llvm::raw_ostream &OS, const BinaryImage &Image,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X64RTMABORTREGIONS_H
