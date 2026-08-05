//===- MedCallingConvDetail.h - Calling convention queries -----*- C++ -*-===//

#ifndef NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
#define NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd::med_calling_conv_detail {

uint16_t findFirstUseSize(const MedFunc &Func, uint64_t ParamRegOff,
                          const TargetRegInfo &TRI);

} // namespace neverd::med_calling_conv_detail

#endif // NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
