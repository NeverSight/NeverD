//===- MedLLVMRegistrationSEHDynamicScope.cpp - Dynamic SEH intervals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHDynamicScope.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/X86/X86RegistrationSEHScope.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/MC/BinaryRewrite.h"

namespace neverd::x86_registration {

bool canEmitDynamicSEHScope(const llvm::BasicBlock &Body) {
  for (const auto &I : Body) {
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!Call || llvm::isa<llvm::IntrinsicInst>(Call))
      continue;
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
    return false;
#else
    const auto *CI = llvm::dyn_cast<llvm::CallInst>(Call);
    if (!CI || CI->isMustTailCall() || CI->isInlineAsm() ||
        CI->getNumOperandBundles() ||
        CI->getAttributes().hasFnAttr(llvm::Attribute::AlwaysInline))
      return false;
#endif
  }
  return true;
}

unsigned
emitDynamicSEHScope(llvm::BasicBlock &Body, llvm::AllocaInst &Frame,
                    uint64_t EntrySP, va_t Owner, const MedBlock &Source,
                    llvm::ArrayRef<int32_t> Levels,
                    const std::map<int32_t, llvm::BasicBlock *> &Unwinds,
                    llvm::Function &TryBegin, llvm::Function &TryEnd) {
  using Role = windows_eh_md::NativeProvenanceRole;
  constexpr auto Model =
      windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
  auto &Context = Body.getContext();
  auto *Parent = Body.getParent();
  auto *After =
      Body.splitBasicBlock(Body.getTerminator(), "registration.dynamic.exit");
  auto *Entry = llvm::BasicBlock::Create(Context, "registration.dynamic.enter",
                                         Parent, &Body);
  Body.replaceAllUsesWith(Entry);
  // PHIs still select the original source predecessor. Move them before the
  // scope selection instead of duplicating their incoming values per scope.
  Entry->splice(Entry->end(), &Body, Body.begin(), Body.getFirstNonPHIIt());
  unsigned ProtectedCalls = 0;
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
  for (auto &I : Body)
    if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
        Call && !llvm::isa<llvm::IntrinsicInst>(Call)) {
      Call->addFnAttr(llvm::Attribute::get(
          Context, llvm::mc_rewrite::RewriteWinX86SEHInheritStateAttribute));
      Call->addFnAttr(llvm::Attribute::NoInline);
      Call->setTailCallKind(llvm::CallInst::TCK_NoTail);
      const auto *MD =
          Call->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      assert(MD && MD->getNumOperands() == 5 && "missing source call identity");
      const auto Address =
          llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1))
              ->getZExtValue();
      const auto Seq =
          llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(2))
              ->getZExtValue();
      llvm::IRBuilder<> Receipt(Call);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          Receipt, Model, Role::RegistrationDynamicCall, Owner, Address,
          Source.Id, Seq);
      ++ProtectedCalls;
    }
#endif
  llvm::IRBuilder<> B(Entry);
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      B, Model, Role::RangeEnterTarget, Owner, Source.StartAddr, Source.Id, 0,
      nullptr, Source.EndAddr, uint32_t(DynamicScopeLevel));
  B.CreateCall(&TryEnd);
  auto *State =
      B.CreateInBoundsGEP(B.getInt8Ty(), &Frame, B.getInt32(EntrySP - 8));
  auto *Level = B.CreateLoad(B.getInt32Ty(), State, "registration.scope");
  Level->setVolatile(true);
  Level->setAlignment(llvm::Align(1));
  const bool HasInactive =
      llvm::any_of(Levels, [](int32_t L) { return L < 0; });
  llvm::BasicBlock *Invalid = nullptr;
  if (!HasInactive) {
    Invalid =
        llvm::BasicBlock::Create(Context, "registration.invalid.scope", Parent);
    llvm::IRBuilder<>(Invalid).CreateUnreachable();
  }
  auto *SelectEntry = B.CreateSwitch(Level, HasInactive ? &Body : Invalid);
  Body.getTerminator()->eraseFromParent();
  llvm::IRBuilder<> Exit(&Body);
  auto *SelectExit = Exit.CreateSwitch(Level, HasInactive ? After : Invalid);
  for (int32_t Scope : Levels) {
    if (Scope < 0)
      continue;
    auto *Begin = llvm::BasicBlock::Create(Context, "registration.scope.enter",
                                           Parent, &Body);
    llvm::IRBuilder<> Before(Begin);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(Before, Model, Role::RangeEnter,
                                               Owner, Source.StartAddr, Scope,
                                               Source.Id);
    Before.CreateInvoke(&TryBegin, &Body, Unwinds.at(Scope));
    SelectEntry->addCase(B.getInt32(Scope), Begin);
    auto *End = llvm::BasicBlock::Create(Context, "registration.scope.exit",
                                         Parent, After);
    llvm::IRBuilder<> EndBuilder(End);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        EndBuilder, Model, Role::RangeExit, Owner, Source.EndAddr, Scope,
        Source.Id);
    EndBuilder.CreateInvoke(&TryEnd, After, Unwinds.at(Scope));
    SelectExit->addCase(B.getInt32(Scope), End);
  }
  llvm::IRBuilder<> AB(After->getTerminator());
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      AB, Model, Role::RangeExitTarget, Owner, Source.EndAddr, Source.Id, 0,
      nullptr, Source.StartAddr, uint32_t(DynamicScopeLevel));
  return ProtectedCalls;
}

} // namespace neverd::x86_registration
