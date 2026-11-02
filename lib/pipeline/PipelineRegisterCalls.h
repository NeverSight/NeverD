//===- PipelineRegisterCalls.h - Source register call expansion -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Expand authenticated scalar query leaves at source call boundaries.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINEREGISTERCALLS_H
#define NEVERD_LIB_PIPELINE_PIPELINEREGISTERCALLS_H

#include "neverd/ir/low/LowIR.h"

#include <map>
#include <memory>
#include <vector>

namespace neverd {
struct BinaryImage;
struct MedFunc;

/// Call only after prepareMedStackAccesses has checked the original physical
/// calls. The original LowIR and native calling convention remain unchanged.
void expandSourceRegisterCalls(
    MedFunc &Function, const BinaryImage &Image,
    const std::map<va_t, std::shared_ptr<const std::vector<LowOp>>> &Leaves);
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINEREGISTERCALLS_H
