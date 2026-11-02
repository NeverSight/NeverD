//===- MedProvenStackOffsets.h - Composing frame proofs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Extend current, independently proved definitions through stack arithmetic.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDPROVENSTACKOFFSETS_H
#define NEVERD_IR_MED_MEDPROVENSTACKOFFSETS_H

#include "neverd/ir/med/MedStackAlignment.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

struct ProvenStackOffset {
  const MedOp *Definition = nullptr;
  int64_t Offset = 0;
};

/// A query-local bound at one current operation or PHI definition. Exactly
/// one definition pointer is required; the caller proves every incoming
/// path, including exceptional entries, before supplying the range.
struct ProvenStackOffsetRange {
  const MedOp *Definition = nullptr;
  const PhiNode *Join = nullptr;
  EntryStackOffsetRange Range;
};

std::vector<std::optional<EntryStackOffsetRange>>
entryStackOffsetRangesFromProvenDefinitions(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
    llvm::ArrayRef<ProvenStackOffsetRange> Seeds, Arch Architecture,
    BinaryFormat Format, StackEntryKind Entry);

/// The caller owns the proof of each seed at this exact definition in Func.
/// Seeds are query-local: they neither annotate mutable IR nor remove memory
/// effects. The ordinary stack proof owns arithmetic, width and join rules.
std::vector<std::optional<int64_t>> entryStackOffsetsFromProvenDefinitions(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
    llvm::ArrayRef<ProvenStackOffset> Seeds, Arch Architecture,
    BinaryFormat Format, StackEntryKind Entry);

/// Offsets from one current definition, with no implicit entry-SP or runtime
/// frame roots. Mixing another origin into a join cannot establish a result.
std::vector<std::optional<int64_t>> stackOffsetsRelativeToDefinition(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values, const MedOp &Root,
    Arch Architecture, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDPROVENSTACKOFFSETS_H
