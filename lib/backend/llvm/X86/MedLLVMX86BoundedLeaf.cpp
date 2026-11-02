//===- MedLLVMX86BoundedLeaf.cpp - Native SGX/SMX leaf completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prefer the fork's native intrinsic; retain a checked old-SDK fallback.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86BoundedLeaf.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#include <vector>

namespace neverd {
llvm::Value *MedLLVMEmitter::emitX86BoundedLeaf(const MedOp &Op, Intrinsic Id,
                                                llvm::IRBuilder<> &B) {
  if (!x86BoundedLeafShapeIsValid(Id, x86BoundedLeafMedShape(Op, TargetArch)) ||
      isX86BoundedLeafPending(Id))
    llvm::report_fatal_error(X86BoundedLeafUnproven);
  auto *Word = sizeToType(Op.Output.Size);
  std::vector<llvm::Type *> Types = {Word, Word, Word, Word,
                                     sizeToType(X86ArithmeticFlagsBytes)};
  std::vector<llvm::Value *> Arguments;
  for (unsigned I = 1; I < Op.NumInputs; ++I) {
    auto *V = getVar(Op.Inputs[I], B);
    Arguments.push_back(V->getType()->isPointerTy()
                            ? B.CreatePtrToInt(V, Types[I - 1])
                            : B.CreateZExtOrTrunc(V, Types[I - 1]));
  }
  auto *Function = B.GetInsertBlock()->getParent();
  auto Features =
      Function->getFnAttribute("target-features").getValueAsString().str();
  if (TargetArch == Arch::X64)
    Features += ",+sahf";
  if (isX86EncluLeaf(Id))
    Features += ",+sgx";
  if (!Features.empty() && Features.front() == ',')
    Features.erase(Features.begin());
  if (!Features.empty())
    Function->addFnAttr("target-features", Features);
  llvm::CallInst *Call;
  const auto Native = llvm::Intrinsic::lookupIntrinsicID(
      x86BoundedLeafNativeName(Id, TargetArch));
  if (Native)
    Call = B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Function->getParent(), Native),
        Arguments);
  else {
    auto *Signature = llvm::FunctionType::get(
        llvm::StructType::get(*Ctx, Types), Types, false);
    auto *Asm =
        llvm::InlineAsm::get(Signature, x86BoundedLeafAsm(Id, TargetArch),
                             X86BoundedLeafAsmConstraints, true);
    Call = B.CreateCall(Asm, Arguments);
  }
  PendingIntrinsicCount = X86BoundedLeafAuxiliaryOutputs;
  for (unsigned I = 0; I < PendingIntrinsicCount; ++I)
    PendingIntrinsicOutputs[I] = B.CreateExtractValue(Call, I + 1);
  return B.CreateExtractValue(Call, 0);
}
} // namespace neverd
