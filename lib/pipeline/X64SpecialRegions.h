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
struct X64ContinuationSourceOptions;
} // namespace analysis

struct X64SpecialRegionSelection {
  std::vector<va_t> Addresses;
  llvm::json::Array Regions;
};

/// Recover each entry once, in breadth-first batches. Only original direct
/// CALL targets in mapped image code extend the next call depth; imports,
/// indirect calls and the depth frontier stay environment boundaries.
/// Decode those targets at the audit's instruction starts even if ordinary
/// lifting rejects the containing body. No LowFunc is required for selection.
/// FollowDirectJumps selects original direct JMP targets outside each audited
/// body at the same depth. Their displacement and PC arithmetic come from the
/// architecture lifter, never an ordinary tail-call or propagated target.
/// Instruction budgets apply to the union, including overlaps and recursive
/// call graphs. No argument, pointer noncapture or ordinary-source fact follows
/// from this. The SVM profile additionally selects canonical VMRUN's original
/// VMEXIT fallthrough at the same call depth. Execution still stops at that
/// boundary.
llvm::Expected<X64SpecialRegionSelection> selectX64SpecialRegions(
    const BinaryImage &Image, llvm::ArrayRef<va_t> Entries,
    const analysis::X64ContinuationSourceOptions &SourceOptions,
    unsigned CalleeDepth, bool FollowDirectJumps, DebugContext *Debug);

/// Connect adjacent selected instructions after checking their actual next PC.
/// All other transfers use the common indexed dispatch. Keep instruction exits,
/// physical CALL/RET, resumption and exact budget precedence unchanged.
void emitX64SpecialRegionDriver(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls);

} // namespace neverd

#endif // NEVERD_PIPELINE_X64SPECIALREGIONS_H
