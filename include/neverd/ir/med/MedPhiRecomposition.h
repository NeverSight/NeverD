//===- MedPhiRecomposition.h - Correlated word slices -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recompose complete word slices merged by the same ordinary CFG edges.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDPHIRECOMPOSITION_H
#define NEVERD_IR_MED_MEDPHIRECOMPOSITION_H

namespace neverd {
struct MedFunc;

/// Replace a concatenation of two PHIs by one whole-value PHI when each
/// incoming edge supplies the exact complementary slices of one SSA value.
/// Keep partial values used elsewhere, calls, memory effects and CFG edges.
/// Returns whether any composition was replaced.
bool recomposeSplitPhis(MedFunc &Func);
} // namespace neverd

#endif // NEVERD_IR_MED_MEDPHIRECOMPOSITION_H
