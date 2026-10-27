//===- RegistrationSEHThreadNativeTests.cpp - Thread read mutations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Require the real current-thread pointer and exact original field reads.
//===----------------------------------------------------------------------===//

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdint>

namespace neverd::registration_test {
void checkSEHThreadEdits(const llvm::Function &Parent,
                         const ExceptionFunction &Source,
                         const BinaryImage &Image,
                         const RegistrationStateAnalysis &States) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  using Thread = RegistrationSEHThreadAccess;
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::LoadInst *Self = nullptr, *Field = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto *VA =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
        const auto *Seq =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
        ASSERT_TRUE(VA && Seq);
        for (const auto &Access : States.SEHThreadAccesses)
          if (VA->getZExtValue() == Access.Address &&
              Seq->getZExtValue() == unsigned(Access.OpSeq)) {
            auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
            ASSERT_TRUE(Load);
            (Access.Field == Thread::Kind::Self ? Self : Field) = Load;
          }
      }
    ASSERT_TRUE(Self && Field);
    llvm::IRBuilder<> B(Field);
    uint8_t *ChangedByte = nullptr;
    uint8_t OriginalByte = 0;
    auto Restore = llvm::scope_exit([&] {
      if (ChangedByte)
        *ChangedByte = OriginalByte;
    });
    if (Mutation == 0)
      Self->setOperand(
          0, llvm::ConstantExpr::getIntToPtr(
                 B.getInt32(0x1c), Self->getPointerOperand()->getType()));
    if (Mutation == 1)
      Self->setOperand(
          0, llvm::ConstantExpr::getIntToPtr(B.getInt32(0x18), B.getPtrTy()));
    if (Mutation == 2)
      Self->setAtomic(llvm::AtomicOrdering::Monotonic);
    if (Mutation == 3)
      Self->setVolatile(false);
    if (Mutation == 4)
      Self->setAlignment(llvm::Align(4));
    if (Mutation == 5)
      Field->setOperand(
          0, llvm::ConstantExpr::getIntToPtr(B.getInt32(4), B.getPtrTy()));
    if (Mutation == 6)
      Field->setOperand(0,
                        B.CreateGEP(B.getInt8Ty(), Field->getPointerOperand(),
                                    B.getInt32(4)));
    if (Mutation == 7)
      Field->setAtomic(llvm::AtomicOrdering::Monotonic);
    if (Mutation == 8)
      Field->setVolatile(false);
    if (Mutation == 9) {
      auto *SelfMD =
          Self->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      Self->setMetadata(
          windows_eh_md::RegistrationOperationAttachment,
          Field->getMetadata(windows_eh_md::RegistrationOperationAttachment));
      Field->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                         SelfMD);
    }
    if (Mutation == 10)
      Self->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                        nullptr);
    if (Mutation == 11 || Mutation == 12) {
      auto *Load = Mutation == 11 ? Self : Field;
      const auto *MD =
          Load->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      const auto *VA =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
      ASSERT_TRUE(VA);
      // Authored source uses 64 A1 18 00 00 00, then 8B 40 04.
      const auto *Byte = Image.readVA(VA->getZExtValue() + 2, 1);
      ASSERT_TRUE(Byte);
      ASSERT_EQ(*Byte, Mutation == 11 ? 0x18 : 4);
      ChangedByte = const_cast<uint8_t *>(Byte);
      OriginalByte = *Byte;
      *ChangedByte += 4;
    }
    if (Mutation == 13) {
      auto *Extra = B.CreateLoad(B.getInt32Ty(), Self->getPointerOperand());
      Extra->setVolatile(true);
      Extra->setAlignment(llvm::Align(1));
    }
    if (Mutation == 14) {
      auto *Other = B.CreateLoad(B.getInt32Ty(), Self->getPointerOperand());
      Other->setVolatile(true);
      Other->setAlignment(llvm::Align(1));
      Field->setOperand(
          0, B.CreateIntToPtr(B.CreateAdd(Other, B.getInt32(4)), B.getPtrTy()));
    }
    if (Mutation == 15)
      B.CreateStore(B.getInt32(0), Self->getPointerOperand())
          ->setVolatile(true);
    if (Mutation == 16) {
      auto *Narrow = B.CreateLoad(B.getInt16Ty(), Field->getPointerOperand());
      Narrow->setVolatile(true);
      Narrow->setAlignment(llvm::Align(1));
      Narrow->setMetadata(
          windows_eh_md::RegistrationOperationAttachment,
          Field->getMetadata(windows_eh_md::RegistrationOperationAttachment));
      Field->replaceAllUsesWith(B.CreateZExt(Narrow, B.getInt32Ty()));
      Field->eraseFromParent();
    }
    if (Mutation == 17) {
      B.SetInsertPoint(Self);
      auto *Duplicate = B.CreateLoad(B.getInt32Ty(), Self->getPointerOperand());
      Duplicate->setVolatile(true);
      Duplicate->setAlignment(llvm::Align(1));
      Duplicate->setMetadata(
          windows_eh_md::RegistrationOperationAttachment,
          Self->getMetadata(windows_eh_md::RegistrationOperationAttachment));
    }
    if (Mutation == 18 || Mutation == 19) {
      const unsigned Bits = Mutation == 18 ? 33u
                                           : (unsigned(UINT16_MAX) + 1) * 8 +
                                                 Thread::WordBytes * 8;
      Self->setOperand(0, llvm::ConstantExpr::getIntToPtr(
                              llvm::ConstantInt::get(B.getIntNTy(Bits), 0x18),
                              Self->getPointerOperand()->getType()));
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}
} // namespace neverd::registration_test
