//===- MedLLVMRegistrationLocalUnwindRuntime.cpp - CRT arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Give checked source local unwinds their exact stack argument convention.

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86RegistrationLocalUnwind.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"

#include "llvm/IR/Module.h"

namespace neverd {
namespace {

llvm::StringRef runtimeName(RegistrationLocalUnwindContract::Kind Protocol) {
  return Protocol == RegistrationLocalUnwindContract::Kind::EH4
             ? "_local_unwind4"
             : "_local_unwind2";
}

} // namespace

void MedLLVMEmitter::declareX86RegistrationLocalUnwinds(const MedFunc &Func) {
  const auto Calls = getX86RegistrationSEHLocalUnwinds(Func);
  if (!Calls)
    return;
  for (const auto &[Address, Call] : *Calls) {
    const auto &Source = Call.Source->Runtime;
    const auto Name = runtimeName(Source.Protocol);
    auto *Type =
        x86_registration::localUnwindRuntimeType(*Ctx, Source.Protocol);
    auto *Existing = Mod->getFunction(Name);
    if (Mod->getNamedValue(Name) &&
        (!Existing || !Existing->isDeclaration() ||
         Existing->getFunctionType() != Type ||
         !Existing->hasExternalLinkage() ||
         Existing->getCallingConv() != llvm::CallingConv::C ||
         !Existing->getAttributes().isEmpty()))
      continue;
    if (Existing) {
      auto Identity = rewrite_source::getOriginalVA(*Existing);
      if (!Identity) {
        llvm::consumeError(Identity.takeError());
        continue;
      }
      if (*Identity && **Identity != Source.Target)
        continue;
    }
    auto *Runtime = llvm::cast<llvm::Function>(
        Mod->getOrInsertFunction(Name, Type).getCallee());
    rewrite_source::setOriginalVA(*Runtime, Source.Target);
    Runtime->setDLLStorageClass(Source.Indirect
                                    ? llvm::GlobalValue::DLLImportStorageClass
                                    : llvm::GlobalValue::DefaultStorageClass);
    FuncNames[Source.Target] = Name.str();
    EmittedFuncNames[Source.Target] = Name.str();
  }
}

llvm::Function *MedLLVMEmitter::prepareX86RegistrationLocalUnwindCall(
    const MedOp &Op, llvm::IRBuilder<> &Builder,
    std::vector<llvm::Value *> &Arguments) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !CurMedFunc || !FrameAlloca || FrameEntrySPOffset < 20)
    return nullptr;
  const auto Calls = getX86RegistrationSEHLocalUnwinds(*CurMedFunc);
  if (!Calls)
    return nullptr;
  const auto Found = Calls->find(Op.Addr);
  if (Found == Calls->end() || Found->second.Operation != &Op)
    return nullptr;
  const auto &Source = *Found->second.Source;
  auto *Runtime = Mod->getFunction(runtimeName(Source.Runtime.Protocol));
  if (!Runtime || !Runtime->isDeclaration() ||
      Runtime->getFunctionType() != x86_registration::localUnwindRuntimeType(
                                        *Ctx, Source.Runtime.Protocol))
    return nullptr;
  auto Identity = rewrite_source::getOriginalVA(*Runtime);
  if (!Identity) {
    llvm::consumeError(Identity.takeError());
    return nullptr;
  }
  if (!*Identity || **Identity != Source.Runtime.Target)
    return nullptr;
  const auto Size =
      Mod->getDataLayout().getTypeAllocSize(FrameAlloca->getAllocatedType());
  if (Size.isScalable() || FrameEntrySPOffset > Size.getFixedValue())
    return nullptr;
  const bool EH4 =
      Source.Runtime.Protocol == RegistrationLocalUnwindContract::Kind::EH4;
  llvm::GlobalVariable *Cookie = nullptr;
  if (EH4) {
    Cookie = Mod->getNamedGlobal("__security_cookie");
    if (Mod->getNamedValue("__security_cookie") &&
        (!Cookie || !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
         !Cookie->getValueType()->isIntegerTy(32) || Cookie->isConstant() ||
         Cookie->isThreadLocal() || Cookie->getAddressSpace() ||
         Cookie->hasDLLImportStorageClass()))
      return nullptr;
    if (!Cookie)
      Cookie = new llvm::GlobalVariable(*Mod, Builder.getInt32Ty(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "__security_cookie");
  }
  Arguments.clear();
  if (Cookie)
    Arguments.push_back(Cookie);
  Arguments.push_back(
      Builder.CreateInBoundsGEP(Builder.getInt8Ty(), FrameAlloca,
                                Builder.getInt32(FrameEntrySPOffset - 20)));
  Arguments.push_back(Builder.getInt32(Source.Bound));
  return Runtime;
}

} // namespace neverd
