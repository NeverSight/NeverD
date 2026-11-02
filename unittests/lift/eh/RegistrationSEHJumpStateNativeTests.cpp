//===- RegistrationSEHJumpStateNativeTests.cpp - State edits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Rejects edits to the logical-to-runtime state projection after capture.

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

void checkSEHJumpStateEdits(const llvm::Function &Parent,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::StoreInst *State = nullptr;
    llvm::CallBase *Capture = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = Call->getCalledFunction();
              Callee && Callee->getName() == "_setjmp3")
            Capture = Call;
        if (auto *MD = I.getMetadata(windows_eh_md::RegistrationJumpAttachment))
          if (const auto *Role = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                  MD->getOperand(2));
              Role && Role->getZExtValue() ==
                          unsigned(x86_registration::SEHJumpRole::StateRestore))
            State = llvm::dyn_cast<llvm::StoreInst>(&I);
      }
    auto *Select =
        State ? llvm::dyn_cast<llvm::SelectInst>(State->getValueOperand())
              : nullptr;
    llvm::Value *Projected = Select ? Select->getFalseValue() : nullptr;
    std::vector<llvm::SelectInst *> Mappings;
    while (auto *Mapping =
               llvm::dyn_cast_or_null<llvm::SelectInst>(Projected)) {
      Mappings.push_back(Mapping);
      Projected = Mapping->getFalseValue();
    }
    auto *Read = llvm::dyn_cast_or_null<llvm::LoadInst>(Projected);
    if (!Read || (Mutation >= 8 && Mappings.empty()))
      return;
    ASSERT_TRUE(Capture);
    llvm::IRBuilder<> B(Read);
    if (Mutation == 0)
      Read->setVolatile(false);
    if (Mutation == 1) {
      Read->setOrdering(llvm::AtomicOrdering::Monotonic);
      Read->setAlignment(llvm::Align(4));
    }
    if (Mutation == 2)
      Read->setOperand(0, B.CreateInBoundsGEP(B.getInt8Ty(),
                                              Read->getPointerOperand(),
                                              B.getInt32(4)));
    if (Mutation == 3) {
      Read->replaceAllUsesWith(B.getInt32(0));
      Read->eraseFromParent();
    }
    if (Mutation == 4)
      llvm::cast<llvm::ICmpInst>(Select->getCondition())
          ->setPredicate(llvm::CmpInst::ICMP_NE);
    if (Mutation == 5) {
      auto *Address = llvm::cast<llvm::Instruction>(Read->getPointerOperand());
      Address->moveBefore(Capture->getIterator());
      Read->moveBefore(Capture->getIterator());
    }
    if (Mutation == 6) {
      B.SetInsertPoint(Select);
      B.CreateFreeze(Read);
    }
    if (Mutation == 7)
      Read->setAlignment(llvm::Align(4));
    if (Mutation >= 8) {
      auto *Mapping = Mappings.front();
      auto *Both =
          llvm::dyn_cast<llvm::BinaryOperator>(Mapping->getCondition());
      auto *Test = llvm::cast<llvm::ICmpInst>(Both ? Both->getOperand(0)
                                                   : Mapping->getCondition());
      if (Mutation == 8)
        Test->setOperand(1, B.getInt32(42));
      if (Mutation == 9)
        Mapping->setOperand(1, B.getInt32(42));
      if (Mutation == 10)
        Test->setPredicate(llvm::CmpInst::ICMP_NE);
      if (Mutation == 11)
        Test->setOperand(0, Capture);
      if (Mutation == 12) {
        Mapping->replaceAllUsesWith(Mapping->getFalseValue());
        Mapping->eraseFromParent();
        if (Both) {
          auto *Bound = llvm::cast<llvm::ICmpInst>(Both->getOperand(1));
          Both->eraseFromParent();
          Bound->eraseFromParent();
        }
        Test->eraseFromParent();
      }
      if (Mutation == 13)
        Mapping->setOperand(2, B.getInt32(-1));
      if (Mutation == 14) {
        B.SetInsertPoint(Select);
        B.CreateFreeze(Mapping);
      }
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

} // namespace neverd::registration_test
