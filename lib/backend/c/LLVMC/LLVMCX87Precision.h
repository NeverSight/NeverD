//===- LLVMCX87Precision.h - x87 precision conversion C -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind constrained LLVM x87 conversions to the shared native C helpers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_LLVMCX87PRECISION_H
#define NEVERD_BACKEND_C_LLVMC_LLVMCX87PRECISION_H

#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include "llvm/IR/IntrinsicInst.h"

#include <optional>
#include <stdexcept>

namespace neverd {

inline std::optional<X87CHelper>
llvmX87PrecisionConversion(const llvm::CallBase &Call, Arch Target) {
  const auto ID = Call.getIntrinsicID();
  const bool Narrow = ID == llvm::Intrinsic::experimental_constrained_fptrunc;
  if (!Narrow && ID != llvm::Intrinsic::experimental_constrained_fpext)
    return std::nullopt;
  const auto *Conversion = llvm::dyn_cast<llvm::ConstrainedFPIntrinsic>(&Call);
  if (!Conversion || Call.arg_size() != (Narrow ? 3u : 2u) ||
      !Call.getType()->isFloatingPointTy() ||
      !Call.getArgOperand(0)->getType()->isFloatingPointTy() ||
      (Narrow &&
       Conversion->getRoundingMode() != llvm::RoundingMode::Dynamic) ||
      (Conversion->getExceptionBehavior() !=
           llvm::fp::ExceptionBehavior::ebStrict &&
       Conversion->getExceptionBehavior() !=
           llvm::fp::ExceptionBehavior::ebIgnore))
    throw std::runtime_error(
        "unsupported constrained x87 precision conversion");
  auto Helper = x87PrecisionConversionHelper(
      Target, Call.getArgOperand(0)->getType()->getPrimitiveSizeInBits() / 8,
      Call.getType()->getPrimitiveSizeInBits() / 8);
  if (!Helper)
    throw std::runtime_error("unsupported constrained x87 precision widths");
  return Helper;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_LLVMC_LLVMCX87PRECISION_H
