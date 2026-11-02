//===- AnalysisDispatch.h - Tentative physical branch targets --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Display bounded target candidates without asserting a selector domain.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_LOWER_ANALYSISDISPATCH_H
#define NEVERD_LIB_IR_HIGH_LOWER_ANALYSISDISPATCH_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd::high_detail {

/// Compare the original computed target with candidate addresses. Snapshot
/// its value once and retain an explicit unknown-target arm.
bool appendAnalysisDispatch(HighFunc &Function, const MedOp &Branch,
                            const JumpTable &Table, ExprPtr Target,
                            const MedVar &Snapshot);

} // namespace neverd::high_detail

#endif // NEVERD_LIB_IR_HIGH_LOWER_ANALYSISDISPATCH_H
