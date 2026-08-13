//===- LLVMCHiLoCollapse.cpp - Hi/Lo pattern collapse -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pattern matching to collapse hi/lo 32-bit halves back into a single
/// 64-bit value in the LLVM-route C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"

namespace neverd {

const llvm::Value *tryCollapseHiLo(const LLVMCAnalysisState &State,
                                   const llvm::Value *RV) {
  auto *OrOp = llvm::dyn_cast<llvm::BinaryOperator>(RV);
  if (!OrOp || OrOp->getOpcode() != llvm::Instruction::Or)
    return nullptr;

  auto Resolve = [&](const llvm::Value *V) -> const llvm::Value * {
    auto Fwd = State.ForwardedLoads.find(V);
    return (Fwd != State.ForwardedLoads.end()) ? Fwd->second : V;
  };
  auto Unwrap = [&](const llvm::Value *V) -> const llvm::Value * {
    V = Resolve(V);
    if (auto *ZE = llvm::dyn_cast<llvm::ZExtInst>(V))
      V = Resolve(ZE->getOperand(0));
    if (auto *SE = llvm::dyn_cast<llvm::SExtInst>(V))
      V = Resolve(SE->getOperand(0));
    if (auto *TR = llvm::dyn_cast<llvm::TruncInst>(V))
      V = TR->getOperand(0);
    return V;
  };

  for (int Swap = 0; Swap < 2; ++Swap) {
    auto *HiSide = Swap ? OrOp->getOperand(1) : OrOp->getOperand(0);
    auto *LoSide = Swap ? OrOp->getOperand(0) : OrOp->getOperand(1);

    auto *Shl = llvm::dyn_cast<llvm::BinaryOperator>(HiSide);
    if (!Shl || Shl->getOpcode() != llvm::Instruction::Shl)
      continue;
    auto *ShlAmt = llvm::dyn_cast<llvm::ConstantInt>(Shl->getOperand(1));
    if (!ShlAmt || ShlAmt->getZExtValue() != 32)
      continue;

    auto *HiInner = Unwrap(Shl->getOperand(0));
    auto *LShr = llvm::dyn_cast<llvm::BinaryOperator>(HiInner);
    if (!LShr || LShr->getOpcode() != llvm::Instruction::LShr)
      continue;
    auto *LShrAmt = llvm::dyn_cast<llvm::ConstantInt>(LShr->getOperand(1));
    if (!LShrAmt || LShrAmt->getZExtValue() != 32)
      continue;

    auto *SourceHi = LShr->getOperand(0);
    auto *SourceLo = Unwrap(LoSide);
    if (SourceHi == SourceLo)
      return SourceHi;
  }
  return nullptr;
}

} // namespace neverd
