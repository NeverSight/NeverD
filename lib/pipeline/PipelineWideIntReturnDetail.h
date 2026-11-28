//===- PipelineWideIntReturnDetail.h - Register-pair evidence ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the high-word carrier at every return before widening a function.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINEWIDEINTRETURNDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINEWIDEINTRETURNDETAIL_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/bit.h"

namespace neverd {
struct LowFunc;
struct MedFunc;
struct MedOp;
struct TargetRegInfo;

/// Every return must carry a computed high word, or forward a tail call whose
/// complete return contract the caller authenticates. Independent entries and
/// unmodelled calls cannot supply a missing carrier.
bool returnsRegisterPairI64(
    const MedFunc &Func, const TargetRegInfo &TRI,
    llvm::function_ref<bool(const MedOp &)> TailReturnsPair,
    bool ShiftReturnsInputOnZero = false);

bool returnsRegisterPairI64(const MedFunc &Func, const TargetRegInfo &TRI);

/// A leaf adds two pairs of incoming integer argument words and delivers the
/// complete sum, including the carry from its low word, in the ABI result pair.
bool returnsDoubleWordSum(const MedFunc &Func, const TargetRegInfo &TRI);

/// Both ABI words of a leaf compute one double-word shift on every path.
bool returnsDoubleWordShift(const LowFunc &Low, const MedFunc &Med,
                            const TargetRegInfo &TRI, llvm::endianness Order);

} // namespace neverd

#endif
