//===- MedCallingConvDetail.h - Calling convention queries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
#define NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <tuple>

namespace neverd::med_calling_conv_detail {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
using ValueSet = llvm::DenseSet<ValueKey>;

ValueKey valueKey(const MedVar &V);
bool containsValue(const ValueSet &Values, const MedVar &V);

ValueSet computeForwardValueClosure(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Seeds,
    llvm::function_ref<bool(const MedOp &, unsigned)> ForwardsInput);

uint16_t findFirstUseSize(const MedFunc &Func, uint64_t ParamRegOff,
                          const TargetRegInfo &TRI);

} // namespace neverd::med_calling_conv_detail

#endif // NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
