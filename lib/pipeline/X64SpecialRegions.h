//===- X64SpecialRegions.h - Bounded physical-state regions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Select and connect special source regions without inferring a function ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_X64SPECIALREGIONS_H
#define NEVERD_PIPELINE_X64SPECIALREGIONS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <vector>

namespace llvm {
class raw_ostream;
}
namespace neverd {
struct BinaryImage;
class DebugContext;
namespace analysis {
struct X64ContinuationInstructionSource;
}

struct X64SpecialRegionSelection {
  std::vector<va_t> Addresses;
  llvm::json::Array Regions;
};

/// Recover each entry once, in breadth-first batches. Only original direct
/// CALL targets in mapped image code extend the next wave; imports, indirect
/// calls and the depth frontier stay environment boundaries. Instruction
/// budgets apply to the union, including overlaps and recursive call graphs.
/// No argument, pointer noncapture or ordinary-source fact follows from this.
llvm::Expected<X64SpecialRegionSelection>
selectX64SpecialRegions(const BinaryImage &Image, llvm::ArrayRef<va_t> Entries,
                        uint64_t MaxInstructions, unsigned CalleeDepth,
                        DebugContext *Debug);

/// Connect adjacent selected instructions after checking their actual next PC.
/// All other transfers use the common indexed dispatch. Keep instruction exits,
/// physical CALL/RET, resumption and exact budget precedence unchanged.
void emitX64SpecialRegionDriver(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls);

} // namespace neverd

#endif // NEVERD_PIPELINE_X64SPECIALREGIONS_H
