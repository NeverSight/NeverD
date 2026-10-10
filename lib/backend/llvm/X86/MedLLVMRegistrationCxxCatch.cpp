//===- MedLLVMRegistrationCxxCatch.cpp - Ordered PE32 catch lowering
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxCatch.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/ExceptionInfo.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif

namespace neverd {
llvm::BasicBlock *emitRegistrationCxxCatches(
    const ExceptionFunction &EH,
    llvm::MutableArrayRef<RegistrationCxxCatchPlan> Plans,
    llvm::AllocaInst &Frame, llvm::Function &Parent) {
#ifndef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  llvm_unreachable("native C++ catches require compiler subfield support");
#else
  auto &Context = Parent.getContext();
  auto &Module = *Parent.getParent();
  auto *Dispatch =
      llvm::BasicBlock::Create(Context, "registration.cxx.dispatch", &Parent);
  llvm::IRBuilder<> D(Dispatch);
  auto *Switch = D.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context),
                                     nullptr, Plans.size());
  for (uint32_t Index = 0; Index < Plans.size(); ++Index) {
    const auto &Catch = EH.Cxx->TryBlocks[0].Handlers[Index];
    auto &Plan = Plans[Index];
    Switch->addHandler(Plan.Handler);
    llvm::IRBuilder<> H(&*Plan.Handler->getFirstInsertionPt());
    auto *Null = llvm::ConstantPointerNull::get(H.getPtrTy());
    llvm::Constant *Type = Null;
    if (Catch.TypeDescriptorVA) {
      Type = Module.getNamedGlobal(makeNdDataSymbol(Catch.TypeDescriptorVA));
      if (!Type)
        Type = new llvm::GlobalVariable(
            Module, H.getInt8Ty(), false, llvm::GlobalValue::ExternalLinkage,
            nullptr, makeNdDataSymbol(Catch.TypeDescriptorVA));
    }
    auto *Pad = H.CreateCatchPad(
        Switch, {Type, H.getInt32(Catch.Adjectives),
                 Plan.Object.Home ? static_cast<llvm::Value *>(&Frame) : Null});
    Plan.Pad = Pad;
    if (const auto &Home = Plan.Object.Home)
      Pad->setMetadata(
          llvm::RewriteWinX86CxxCatchObjectAttachment,
          llvm::MDNode::get(
              Context, {med_llvm_eh::mdUInt(Context, 1, 32),
                        med_llvm_eh::mdUInt(Context, Home->Offset, 32),
                        med_llvm_eh::mdUInt(Context, Home->slotSize(), 32)}));
    const auto Token =
        windows_eh_semantics::getCxxCatchSemanticToken(EH, Arch::X86, 0, Index);
    if (!Token || !med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, *Token))
      llvm_unreachable("prevalidated C++ catch semantic token rejected");
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        H, windows_eh_md::NativeProvenanceModel::X86RegistrationCxx,
        windows_eh_md::NativeProvenanceRole::RegionDispatch, EH.CodeRange.Begin,
        Catch.HandlerVA, 0, Index, Pad, Catch.TypeDescriptorVA,
        Catch.Adjectives);
    if (Plan.Stack)
      emitRegistrationCxxStack(*Plan.Stack, EH.CodeRange.Begin,
                               Catch.HandlerVA);
  }
  return Dispatch;
#endif
}
} // namespace neverd
