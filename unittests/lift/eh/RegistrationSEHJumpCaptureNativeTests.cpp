//===- RegistrationSEHJumpCaptureNativeTests.cpp - Capture state edits
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rejects verifier-clean edits to the source capture's saved logical bound.

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHJumpCaptureStateEdits(const llvm::Function &Parent,
                                   const ExceptionFunction &Source,
                                   const BinaryImage &Image) {
  using x86_registration::SEHJumpRole;
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::AllocaInst *Slot = nullptr;
    llvm::StoreInst *State = nullptr;
    llvm::CallBase *Call = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *C = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = C->getCalledFunction();
              Callee && Callee->getName() == "_setjmp3")
            Call = C;
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationJumpAttachment);
        if (!MD)
          continue;
        const auto *Role =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
        if (Role &&
            Role->getZExtValue() == unsigned(SEHJumpRole::CapturedState))
          Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (Role && Role->getZExtValue() == unsigned(SEHJumpRole::StateRestore))
          State = llvm::dyn_cast<llvm::StoreInst>(&I);
      }
    if (!Slot)
      return;
    ASSERT_TRUE(Call && State);
    llvm::StoreInst *Save = nullptr;
    llvm::LoadInst *Reload = nullptr;
    for (auto *User : Slot->users()) {
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(User))
        Save = S;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(User))
        Reload = L;
    }
    ASSERT_TRUE(Save && Reload);
    auto *Read = llvm::cast<llvm::LoadInst>(Save->getValueOperand());
    auto *Select = llvm::cast<llvm::SelectInst>(State->getValueOperand());
    auto *Mapping = llvm::dyn_cast<llvm::SelectInst>(Select->getFalseValue());
    llvm::IRBuilder<> B(Save);
    if (Mutation == 0)
      Slot->setMetadata(windows_eh_md::RegistrationJumpAttachment, nullptr);
    if (Mutation == 1) {
      B.SetInsertPoint(Read);
      Read->setOperand(0, B.CreateGEP(B.getInt8Ty(), Read->getPointerOperand(),
                                      B.getInt32(4)));
    }
    if (Mutation == 2)
      Read->setVolatile(false);
    if (Mutation == 3)
      Save->setOperand(0, B.getInt32(0));
    if (Mutation == 4)
      Reload->setVolatile(false);
    if (Mutation == 5)
      Save->moveBefore(Reload->getIterator());
    if (Mutation == 6)
      Reload->moveBefore(Call->getIterator());
    if (Mutation == 7) {
      B.SetInsertPoint(State);
      B.CreateFreeze(Reload);
    }
    if (Mutation == 8 || Mutation == 9) {
      if (!Mapping)
        continue;
      auto *Both = llvm::cast<llvm::BinaryOperator>(Mapping->getCondition());
      auto *Test = llvm::cast<llvm::ICmpInst>(Both->getOperand(1));
      if (Mutation == 8)
        Test->setOperand(1, B.getInt32(42));
      else
        Test->setOperand(0, Call);
    }
    if (Mutation == 10)
      Select->setOperand(1, B.getInt32(0));
    if (Mutation == 11)
      B.CreateStore(B.getInt32(0), Slot);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

} // namespace neverd::registration_test
