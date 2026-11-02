//===- MedAnalysisDispatch.h - Tentative physical dispatch -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission for displaying candidate targets without a selector proof.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDANALYSISDISPATCH_H
#define NEVERD_IR_MED_MEDANALYSISDISPATCH_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <cstdint>

namespace neverd {

inline bool hasAnalysisDispatchShape(bool AnalysisOnly, const MedOp &Branch,
                                     const JumpTable &Table) {
  return AnalysisOnly && Table.AnalysisOnly && !Table.MutatedUnsafe &&
         Table.InsnAddr == Branch.Addr && Branch.Opcode == NdOp::INDIR_BR &&
         Branch.NumInputs == 1 && Branch.Inputs[0].Size == sizeof(uint32_t) &&
         !Table.Targets.empty() &&
         Table.Targets.size() <= limits::ir::kAnalysisTableSlots &&
         std::all_of(Table.Targets.begin(), Table.Targets.end(),
                     [](va_t Address) { return Address <= UINT32_MAX; });
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDANALYSISDISPATCH_H
