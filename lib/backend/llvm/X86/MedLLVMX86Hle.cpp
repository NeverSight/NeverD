//===- MedLLVMX86Hle.cpp - Native HLE register publication ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the original GPR destination to the native four-register bank.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ModRef.h"

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86Hle(const MedOp &Op, llvm::IRBuilder<> &B) {
  if (!x86HleShapeIsValid(x86HleMedShape(Op, TargetArch)))
    llvm::report_fatal_error("invalid x86 HLE state contract");
  const auto P = *x86HlePlan(Op.Inputs[5].ConstVal);
  auto Pointer = [&](unsigned I) {
    auto *V = getVar(Op.Inputs[I], B);
    return V->getType()->isPointerTy() ? V : B.CreateIntToPtr(V, B.getPtrTy());
  };
  auto *Memory = Pointer(1), *Gprs = Pointer(2), *Flags = Pointer(3);
  auto &EntryBlock = B.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> Entry(&EntryBlock, EntryBlock.begin());
  auto *Bank = Entry.CreateAlloca(
      llvm::ArrayType::get(B.getInt8Ty(), X86HleNativeGprBytes), nullptr,
      "hle_gprs");
  B.CreateMemCpy(Bank, llvm::Align(1), Gprs, llvm::Align(1),
                 X86HleNativeGprBytes);
  if (P.ExplicitSource) {
    auto *Value = getVar(Op.Inputs[4], B);
    if (Value->getType()->isPointerTy())
      Value = B.CreatePtrToInt(Value, B.getInt64Ty());
    B.CreateAlignedStore(Value, B.CreateConstGEP1_64(B.getInt8Ty(), Bank, 8),
                         llvm::Align(1));
  }
  auto Callee = Mod->getOrInsertFunction(
      "llvm.x86.hle.context",
      llvm::FunctionType::get(B.getVoidTy(),
                              {B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                               B.getInt32Ty(), B.getInt32Ty()},
                              false));
  auto *F = llvm::cast<llvm::Function>(Callee.getCallee());
  F->setMemoryEffects(llvm::MemoryEffects::unknown());
  for (auto A : {llvm::Attribute::Convergent, llvm::Attribute::NoDuplicate,
                 llvm::Attribute::NoUnwind})
    F->addFnAttr(A);
  if (F->getIntrinsicID()) {
    F->addParamAttr(3, llvm::Attribute::ImmArg);
    F->addParamAttr(4, llvm::Attribute::ImmArg);
  }
  B.CreateCall(Callee, {Memory, Bank, Flags, B.getInt32(P.NativeControl),
                        B.getInt32(P.Bytes)});
  for (const auto &Copy : P.Outputs)
    if (Copy.Bytes)
      B.CreateMemCpy(
          B.CreateConstGEP1_64(B.getInt8Ty(), Gprs, Copy.OriginalOffset),
          llvm::Align(1),
          B.CreateConstGEP1_64(B.getInt8Ty(), Bank, Copy.NativeOffset),
          llvm::Align(1), Copy.Bytes);
  return nullptr;
}

} // namespace neverd
