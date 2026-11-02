//===- MedLLVMX64CallRCX.cpp - Occurrence-bound native RCX results --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Capture the extra result in the same native call that produces RAX.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X64CallRCXNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/X64CallRCXProfile.h"

#include "llvm/IR/InlineAsm.h"

#include <vector>

namespace neverd {
llvm::Value *MedLLVMEmitter::emitX64CallRCX(const MedOp &Op,
                                            llvm::IRBuilder<> &B) {
  if (TargetFormat != BinaryFormat::COFF ||
      !x64CallRCXShapeIsValid(x64CallRCXMedShape(Op, TargetArch)))
    llvm::report_fatal_error(X64CallRCXDiagnostic);
  auto *Function = B.GetInsertBlock()->getParent();
  if (!Function->hasFnAttribute(X64CallRCXProfileAttribute)) {
    if (!CurMedFunc || !x64CallRCXCallerProfile(*CurMedFunc))
      llvm::report_fatal_error(X64CallRCXDiagnostic);
    Function->addFnAttr(X64CallRCXProfileAttribute);
    Function->addFnAttr("target-cpu", X64CallRCXTargetCPU);
    Function->addFnAttr("target-features", X64CallRCXTargetFeatures);
    Function->setUWTableKind(llvm::UWTableKind::Async);
  }
  auto *Word = B.getInt64Ty();
  std::vector<llvm::Type *> Types(X64CallRCXNativeResults, Word);
  auto *Signature =
      llvm::FunctionType::get(llvm::StructType::get(*Ctx, Types), Types, false);
  std::vector<llvm::Value *> Arguments;
  for (unsigned I = 1; I < Op.NumInputs; ++I) {
    auto *Value = getVar(Op.Inputs[I], B);
    if (!Value->getType()->isPointerTy() && !Value->getType()->isIntegerTy())
      llvm::report_fatal_error(X64CallRCXDiagnostic);
    Arguments.push_back(Value->getType()->isPointerTy()
                            ? B.CreatePtrToInt(Value, Word)
                            : B.CreateZExtOrTrunc(Value, Word));
  }
  Function->removeFnAttr(llvm::Attribute::NoUnwind);
  Function->addFnAttr(llvm::Attribute::NoRedZone);
  Function->addFnAttr("frame-pointer", "all");
  auto *Asm =
      llvm::InlineAsm::get(Signature, X64CallRCXAsm, X64CallRCXConstraints,
                           /*hasSideEffects=*/true, /*isAlignStack=*/false,
                           llvm::InlineAsm::AD_ATT, /*canThrow=*/true);
  auto *Call = B.CreateCall(Asm, Arguments);
  PendingIntrinsicCount = 1;
  PendingIntrinsicOutputs[0] = B.CreateExtractValue(Call, 1);
  return B.CreateExtractValue(Call, 0);
}
} // namespace neverd
