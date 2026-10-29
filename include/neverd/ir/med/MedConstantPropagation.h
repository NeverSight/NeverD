//===- MedConstantPropagation.h - Stable MedIR constants --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared SSA identity rules and bounded invariant-value propagation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H
#define NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H

#include "neverd/ir/NdOps.h"

#include <optional>
#include <set>

namespace neverd {
struct MedFunc;
struct MedBlock;
struct PhiNode;
struct BinaryImage;
struct MedVar;

/// A boolean fixed by comparing the same, equally sized SSA observation.
/// Unspecified values and distinct occurrences remain input-dependent.
std::optional<bool> selfComparisonConstant(NdOp Opcode, const MedVar &A,
                                           const MedVar &B);

/// A PHI supplies one same-width value for every actual ordinary incoming
/// edge, with no extra, duplicate, or implicit exceptional entry value.
bool hasCompleteOrdinaryPhiInputs(const MedBlock &Block, const PhiNode &Phi,
                                  const std::set<int> &Incoming);

/// A literal INDIR_CALL target denotes implicit pointer storage, whereas its
/// register/temp operand is already a callable value. Keep that value's SSA
/// occurrence even when its producer becomes constant; substituting the bits
/// would introduce a memory read. Ordinary arguments retain constant folding.
constexpr bool canPropagateConstantToOperand(NdOp Opcode, unsigned Index) {
  return Opcode != NdOp::INDIR_CALL || Index != 0;
}

/// Propagate equal, fully seeded constants through pure same-width SSA copies
/// and complete PHIs. Preserve occurrence-sensitive address provenance and
/// reject uninitialized cycles. Returns whether any operand changed; exhausted
/// analysis budgets leave the function unchanged.
bool propagateInvariantConstants(MedFunc &Func);

/// Evaluate bounded, side-effect-free scans of immutable linked-image tables.
/// A scan is replaced only after every iteration and its exit values have
/// been proved. Mutable/volatile memory, unknown control, and exhausted bounds
/// leave that loop intact.
bool foldImmutableTableScans(MedFunc &Func, const BinaryImage &Image);
} // namespace neverd

#endif // NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H
