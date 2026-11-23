//===- X64RtmAbortRegions.h - Bound immediate RTM aborts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate and execute an indivisible XBEGIN / immediate XABORT region.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64RTMABORTREGIONS_H
#define NEVERD_LIB_PIPELINE_X86_X64RTMABORTREGIONS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>
#include <string>

namespace llvm {
class raw_ostream;
}
namespace neverd {
struct BinaryImage;
namespace analysis {
struct X64ContinuationInstructionSource;
}

/// Evidence belongs to the original execution CPU. An unadvertised feature
/// alone cannot distinguish an unsupported instruction from TSX CPUID masking.
struct X64RtmAbortSelection {
  enum Mode { Off, Supported, Probe, Unadvertised } Kind = Off;
  std::optional<uint32_t> MaxBasic;
  std::optional<uint32_t> Leaf7EBX;
  static llvm::Expected<X64RtmAbortSelection> read(const llvm::json::Value &V);
  llvm::json::Object report(llvm::json::Array Regions) const;
};

struct X64RtmAbortRegion {
  va_t Address;
  va_t Fallback;
  llvm::SmallVector<uint8_t, 9> Bytes;
};

/// x64 rel16/rel32 XBEGIN immediately followed by XABORT imm8, including
/// the ignored prefixes authenticated by the transaction byte reader.
/// The existing transaction decoder owns signed fallback arithmetic. No other
/// transactional body or nested transaction receives this native binding.
std::optional<X64RtmAbortRegion> readX64RtmAbortRegion(const BinaryImage &Image,
                                                       va_t Address,
                                                       unsigned AddressBits);

/// The public instruction entry calls its original V6 guard, then executes
/// the authenticated pair under the invocation's declared RTM support.
struct X64RtmAbortStep {
  std::string Name;
  X64RtmAbortRegion Region;
};

/// A successful live query is local to a driver activation (one bounded
/// chunk in direct dispatch). A resumed or independent invocation probes anew.
class X64RtmAbortDispatch {
  llvm::DenseSet<va_t> Sites;

public:
  X64RtmAbortDispatch(X64RtmAbortSelection::Mode Mode,
                      llvm::ArrayRef<X64RtmAbortStep> Steps);
  void declare(
      llvm::raw_ostream &OS,
      llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units) const;
  bool contains(va_t Site) const {
    return !Sites.empty() && Sites.contains(Site);
  }
  void emit(llvm::raw_ostream &OS, llvm::StringRef Name) const;
};

llvm::json::Array emitX64RtmAbortSteps(llvm::raw_ostream &OS,
                                       llvm::ArrayRef<X64RtmAbortStep> Steps,
                                       X64RtmAbortSelection::Mode Mode);

/// Emit an explicit V6 environment callback and receipts for its accepted
/// pairs. One environment invocation owns the complete pair. Instruction steps
/// retain their native boundary and never expose an active transaction.
llvm::json::Array emitX64RtmAbortEnvironment(
    llvm::raw_ostream &OS, const BinaryImage &Image,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64RTMABORTREGIONS_H
