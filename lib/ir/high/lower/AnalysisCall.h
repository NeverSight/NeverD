//===- AnalysisCall.h - Explicit analysis call expressions -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Display machine calling conventions without claiming an executable C ABI.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_LOWER_ANALYSISCALL_H
#define NEVERD_LIB_IR_HIGH_LOWER_ANALYSISCALL_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::high_detail {

bool hasAnalysisX87CallShape(const MedOp &Operation);

/// Append a visibly analysis-only call, preserving the target/slot and every
/// raw binary80 argument. No adapter implementation or ordinary ABI is added.
bool appendAnalysisX87Call(
    HighFunc &Function, const MedOp &Operation,
    llvm::function_ref<ExprPtr(const MedVar &)> Expression);

} // namespace neverd::high_detail

#endif // NEVERD_LIB_IR_HIGH_LOWER_ANALYSISCALL_H
