//===- LLVMCIntegerCompare.h - Three-way integer comparison ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared scalar three-way comparison admission for C emission.

#ifndef NEVERD_BACKEND_C_LLVM_C_INTEGERCOMPARE_H
#define NEVERD_BACKEND_C_LLVM_C_INTEGERCOMPARE_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
#include <stdexcept>

namespace neverd {
inline const char *integerThreeWaySpelling(llvm::Intrinsic::ID Kind) {
  switch (Kind) {
  case llvm::Intrinsic::scmp:
    return "scmp";
  case llvm::Intrinsic::ucmp:
    return "ucmp";
  default:
    return nullptr;
  }
}
struct ScalarIntegerThreeWay {
  /// The width of the -1, 0 or 1 it gives.
  unsigned ResultBits;
  unsigned OperandBits;
  bool Signed;
};
/// One admission rule for helper declarations and assigned/inline calls.
inline std::optional<ScalarIntegerThreeWay>
scalarIntegerThreeWay(const llvm::CallBase &Call) {
  const auto *Callee = llvm::dyn_cast<llvm::Function>(
      Call.getCalledOperand()->stripPointerCasts());
  if (!Callee || !integerThreeWaySpelling(Callee->getIntrinsicID()))
    return std::nullopt;
  const auto *Result = llvm::dyn_cast<llvm::IntegerType>(Call.getType());
  const auto *Operand =
      Call.arg_size() == 2
          ? llvm::dyn_cast<llvm::IntegerType>(Call.getArgOperand(0)->getType())
          : nullptr;
  if (Call.getCalledFunction() != Callee || !llvm::isa<llvm::CallInst>(Call) ||
      !Result || !Operand || Result->getBitWidth() < 2 ||
      Result->getBitWidth() > 128 || Operand->getBitWidth() > 128 ||
      Call.getArgOperand(1)->getType() != Operand ||
      Call.getFunctionType() != Callee->getFunctionType() ||
      Call.getFunctionType()->isVarArg() || Call.hasOperandBundles())
    throw std::runtime_error("unsupported LLVM three-way comparison shape");
  return ScalarIntegerThreeWay{Result->getBitWidth(), Operand->getBitWidth(),
                               Callee->getIntrinsicID() ==
                                   llvm::Intrinsic::scmp};
}
} // namespace neverd
#endif
