//===- RegistrationSEH4NativeTestUtils.h -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check that current shared-frame callbacks cannot clobber saved source ESP.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4NATIVETESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4NATIVETESTUTILS_H

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include <utility>
#include <vector>

namespace neverd::registration_test {

inline void checkSharedSEH4SavedStackEdits(llvm::Function &Parent,
                                           const ExceptionFunction &Source,
                                           const BinaryImage &Image) {
  const llvm::AllocaInst *Frame = nullptr;
  const llvm::IntrinsicInst *Escape = nullptr;
  for (const auto &Instruction : Parent.getEntryBlock()) {
    if (Instruction.getMetadata(windows_eh_md::RegistrationFrameAttachment))
      Frame = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
    if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
        Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
      Escape = Call;
  }
  ASSERT_TRUE(Frame);
  ASSERT_TRUE(Escape);
  const auto *EntrySP = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
      Frame->getMetadata(windows_eh_md::RegistrationFrameAttachment)
          ->getOperand(1));
  ASSERT_TRUE(EntrySP);
  std::vector<llvm::IntrinsicInst *> Recovered;
  for (auto &Callback : *Parent.getParent())
    for (auto &Block : Callback)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
            Call && Call->getIntrinsicID() == llvm::Intrinsic::localrecover &&
            Call->getArgOperand(0) == &Parent) {
          const auto *Index =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(2));
          if (Index && Index->getZExtValue() < Escape->arg_size() &&
              Escape->getArgOperand(Index->getZExtValue()) == Frame)
            Recovered.push_back(Call);
        }
  ASSERT_FALSE(Recovered.empty());
  // Check filters and normal/abnormal finally bodies independently. Partial
  // writes must invalidate the same reaching definition as full-word writes.
  for (auto *Root : Recovered)
    for (unsigned Width : {1u, 4u}) {
      SCOPED_TRACE(Root->getFunction()->getName().str());
      SCOPED_TRACE(Width);
      llvm::IRBuilder<> Builder(
          Root->getFunction()->getEntryBlock().getTerminator());
      auto *Address = llvm::cast<llvm::Instruction>(
          Builder.CreateGEP(Builder.getInt8Ty(), Root,
                            Builder.getInt32(EntrySP->getZExtValue() - 28)));
      auto *Write = Builder.CreateStore(
          llvm::ConstantInt::get(Builder.getIntNTy(8 * Width), 0), Address,
          true);
      Write->setAlignment(llvm::Align(1));
      auto Changed = validateCOFFRegistrationIR(Parent, Source, Image);
      EXPECT_TRUE(bool(Changed));
      llvm::consumeError(std::move(Changed));
      Write->eraseFromParent();
      Address->eraseFromParent();
    }
}

} // namespace neverd::registration_test

#endif // NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4NATIVETESTUTILS_H
