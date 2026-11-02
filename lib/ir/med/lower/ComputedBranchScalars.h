//===- ComputedBranchScalars.h - Numeric dispatch operands ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve the numeric coordinate of certified computed-code dispatches.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_COMPUTEDBRANCHSCALARS_H
#define NEVERD_LIB_IR_MED_LOWER_COMPUTEDBRANCHSCALARS_H

namespace neverd {
struct BinaryImage;
struct MedFunc;

/// A physical-target selector compares original machine address bits. Its
/// closed arithmetic producers cannot use relocated function/block identities.
/// Reject an escaping code value instead of changing an observable pointer.
bool preserveComputedBranchScalars(MedFunc &Function, const BinaryImage &Image);

} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_COMPUTEDBRANCHSCALARS_H
