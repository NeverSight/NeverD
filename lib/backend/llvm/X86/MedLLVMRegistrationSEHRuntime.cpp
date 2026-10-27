//===- MedLLVMRegistrationSEHRuntime.cpp - Authenticated import calls ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Supply actual native arguments for checked PE32 runtime import calls.

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Module.h"

#include <map>

namespace neverd {

llvm::Function *
MedLLVMEmitter::getX86RegistrationSEHRuntimeTarget(const MedOp &Op) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !CurMedFunc || !CurMedFunc->RegistrationStates || !FrameAlloca ||
      (Op.Opcode != NdOp::INDIR_CALL && Op.Opcode != NdOp::CALL))
    return nullptr;
  const auto Calls = getX86RegistrationSEHBoundCalls(*CurMedFunc);
  if (!Calls)
    return nullptr;
  const auto Found = Calls->find(Op.Addr);
  if (Found == Calls->end() || Found->second.Operation != &Op)
    return nullptr;
  const auto &Source = *Found->second.Source;
  const auto Name = getX86RegistrationSEHBoundImport(Source);
  auto *Callee = Name.empty() ? nullptr : Mod->getFunction(Name);
  if (!Callee || !Callee->isDeclaration() ||
      Callee->getFunctionType() !=
          getX86RegistrationSEHBoundType(Source, *Ctx) ||
      Callee->hasDLLImportStorageClass() !=
          usesX86RegistrationSEHImportSlot(Found->second) ||
      Callee->getCallingConv() !=
          getX86RegistrationSEHBoundConvention(Source) ||
      !x86_registration::hasSEHBoundFrameArguments(Source, *FrameAlloca,
                                                   FrameEntrySPOffset))
    return nullptr;
  return Callee;
}

llvm::Function *MedLLVMEmitter::prepareX86RegistrationSEHRuntimeCall(
    const MedOp &Op, llvm::IRBuilder<> &Builder,
    std::vector<llvm::Value *> &Arguments) {
  auto *Callee = getX86RegistrationSEHRuntimeTarget(Op);
  if (!Callee)
    return nullptr;
  const auto Calls = getX86RegistrationSEHBoundCalls(*CurMedFunc);
  const auto &Bound = Calls->at(Op.Addr);
  const auto &Source = *Bound.Source;
  if (Bound.StackRoot) {
    auto *SP = getVar(Bound.StackRoot->Output, Builder);
    assert(SP->getType()->isIntegerTy(32) && "checked callback ESP is PE32");
    Arguments.clear();
    for (const auto &Arg : Source.Arguments) {
      if (Arg.ArgumentKind == RegistrationSEHBoundArgument::Kind::NullPointer) {
        Arguments.push_back(llvm::ConstantPointerNull::get(Builder.getPtrTy()));
        continue;
      }
      auto *Address = Builder.CreateAdd(SP, Builder.getInt32(Arg.Offset));
      auto *Pointer = Builder.CreateIntToPtr(Address, Builder.getPtrTy());
      auto *Value = Builder.CreateLoad(Builder.getInt32Ty(), Pointer);
      Value->setAlignment(llvm::Align(1));
      Value->setVolatile(true);
      Arguments.push_back(Value);
    }
    return Callee;
  }
  std::map<va_t, llvm::Value *> ImagePointers;
  for (const auto &Arg : Source.Arguments)
    if (Arg.ArgumentKind == RegistrationSEHBoundArgument::Kind::ImagePointer) {
      auto *Pointer = tryResolveGlobalData(Arg.ImageVA, 4);
      if (!Pointer)
        return nullptr;
      ImagePointers.emplace(Arg.ImageVA, Pointer);
    }
  const auto Projected = x86_registration::projectSEHBoundArguments(
      Source, Builder, *FrameAlloca, FrameEntrySPOffset, {}, ImagePointers);
  Arguments.assign(Projected.begin(), Projected.end());
  return Callee;
}

} // namespace neverd
