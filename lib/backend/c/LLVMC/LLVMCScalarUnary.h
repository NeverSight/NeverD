//===- LLVMCScalarUnary.h - Scalar unary intrinsic shapes ---------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_SCALARUNARY_H
#define NEVERD_BACKEND_C_LLVMC_SCALARUNARY_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
#include <stdexcept>

namespace neverd {

inline const char *scalarUnarySpelling(llvm::Intrinsic::ID Kind) {
  switch (Kind) {
  case llvm::Intrinsic::bitreverse:
    return "bitreverse";
  case llvm::Intrinsic::fptosi_sat:
    return "fptosi_sat";
  case llvm::Intrinsic::fptoui_sat:
    return "fptoui_sat";
  default:
    return nullptr;
  }
}

struct ScalarUnary {
  llvm::Intrinsic::ID Kind;
  unsigned Bits;
  unsigned FloatBits;

  unsigned carrierBits() const {
    for (unsigned Width : {8u, 16u, 32u, 64u, 128u})
      if (Bits <= Width)
        return Width;
    llvm_unreachable("validated scalar width");
  }
};

/// Shared validation for helper declarations and assigned/inline calls.
/// x87 retains its separate target-specific conversion projection.
inline std::optional<ScalarUnary> scalarUnary(const llvm::CallBase &Call) {
  const auto *Callee = llvm::dyn_cast<llvm::Function>(
      Call.getCalledOperand()->stripPointerCasts());
  if (!Callee || !scalarUnarySpelling(Callee->getIntrinsicID()))
    return std::nullopt;
  const auto Kind = Callee->getIntrinsicID();
  const auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Call.getType());
  const unsigned Bits = Integer ? Integer->getBitWidth() : 0;
  if (Call.getCalledFunction() != Callee || !llvm::isa<llvm::CallInst>(Call) ||
      Call.arg_size() != 1 || Call.getFunctionType()->isVarArg() ||
      Call.getFunctionType() != Callee->getFunctionType())
    throw std::runtime_error("unsupported LLVM scalar unary intrinsic shape");
  const auto *Input = Call.getArgOperand(0)->getType();
  if (Kind == llvm::Intrinsic::fptosi_sat && Input->isX86_FP80Ty())
    return std::nullopt;
  if (Kind == llvm::Intrinsic::bitreverse) {
    if (Bits == 0 || Bits > 128 || Input != Integer)
      throw std::runtime_error("unsupported LLVM scalar bit reversal shape");
    return ScalarUnary{Kind, Bits, 0};
  }
  if ((Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64 &&
       Bits != 128) ||
      (!Input->isHalfTy() && !Input->isFloatTy() && !Input->isDoubleTy()))
    throw std::runtime_error("unsupported LLVM scalar saturating conversion");
  // A half converts to float exactly, so its helper takes a float.
  return ScalarUnary{Kind, Bits,
                     unsigned(Input->getPrimitiveSizeInBits().getFixedValue())};
}

} // namespace neverd

#endif
