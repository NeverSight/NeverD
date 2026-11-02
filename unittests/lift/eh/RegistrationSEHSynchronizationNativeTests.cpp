//===- RegistrationSEHSynchronizationNativeTests.cpp - Atomic call edits --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck actual original storage, source words, runtime identity and effects.
//===----------------------------------------------------------------------===//

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace neverd::registration_test {
void checkSEHSynchronizationEdits(const llvm::Function &Parent,
                                  const ExceptionFunction &Source,
                                  const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (bool Atomic : {false, true})
    for (unsigned Mutation = 0; Mutation != (Atomic ? 21u : 15u); ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Atomic << ' ' << Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      llvm::CallBase *Runtime = nullptr;
      for (auto &Block : *Function)
        for (auto &I : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
            if (const auto *Callee = Call->getCalledFunction();
                !Runtime && Callee &&
                Callee->getName() ==
                    (Atomic ? "InterlockedCompareExchange" : "Sleep"))
              Runtime = Call;
      ASSERT_TRUE(Runtime);
      ASSERT_EQ(Runtime->arg_size(), Atomic ? 3u : 1u);
      auto *Callee = Runtime->getCalledFunction();
      auto *Scalar = llvm::dyn_cast<llvm::LoadInst>(
          Runtime->getArgOperand(Atomic ? 1 : 0));
      ASSERT_TRUE(Scalar);
      llvm::IRBuilder<> B(Runtime);
      auto ChangedImage = Image;
      uint8_t *ChangedByte = nullptr;
      uint8_t OriginalByte = 0;
      auto Restore = llvm::scope_exit([&] {
        if (ChangedByte)
          *ChangedByte = OriginalByte;
      });
      if (Mutation == 0)
        Runtime->setCallingConv(llvm::CallingConv::C);
      if (Mutation == 1)
        Callee->setCallingConv(llvm::CallingConv::C);
      if (Mutation == 2)
        Callee->setName("unproved_synchronization");
      if (Mutation == 3)
        rewrite_source::setOriginalVA(*Callee, Source.CodeRange.Begin);
      if (Mutation == 4)
        Scalar->setVolatile(false);
      if (Mutation == 5) {
        B.SetInsertPoint(Scalar);
        Scalar->setOperand(0, B.CreateInBoundsGEP(B.getInt8Ty(),
                                                  Scalar->getPointerOperand(),
                                                  B.getInt32(4)));
      }
      if (Mutation == 6) {
        B.SetInsertPoint(Scalar->getNextNode());
        B.CreateStore(B.getInt32(0), Scalar->getPointerOperand())
            ->setVolatile(true);
      }
      if (Mutation == 7)
        Runtime->addFnAttr(llvm::Attribute::NoUnwind);
      if (Mutation == 8)
        Runtime->setMemoryEffects(llvm::MemoryEffects::readOnly());
      if (Mutation == 9)
        Runtime->addFnAttr(llvm::Attribute::NoCallback);
      if (Mutation == 10)
        Runtime->addFnAttr(llvm::Attribute::WillReturn);
      if (Mutation == 11)
        Runtime->setArgOperand(Atomic ? 1 : 0, B.getInt32(1000));
      if (Mutation == 12)
        Callee->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
      if (Mutation == 13) {
        auto Identity = rewrite_source::getOriginalVA(*Callee);
        ASSERT_TRUE(bool(Identity)) << llvm::toString(Identity.takeError());
        ASSERT_TRUE(*Identity);
        bool Changed = false;
        for (auto &Import : ChangedImage.Imports)
          if (Import.IATAddr == **Identity) {
            Import.Module = "unproved-runtime.dll";
            Changed = true;
          }
        ASSERT_TRUE(Changed);
      }
      if (Mutation == 14 || Mutation == 20) {
        const auto *Marker = Runtime->getMetadata(
            windows_eh_md::RegistrationOperationAttachment);
        ASSERT_TRUE(Marker);
        const auto *Address = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
            Marker->getOperand(1));
        ASSERT_TRUE(Address);
        const auto VA = Address->getZExtValue();
        const auto *Byte =
            ChangedImage.readVA(VA - (Mutation == 20 ? 4 : 0), 1);
        ASSERT_TRUE(Byte);
        if (Mutation == 20) {
          const auto *Push = ChangedImage.readVA(VA - 5, 1);
          ASSERT_TRUE(Push);
          ASSERT_EQ(*Push, 0x68);
        }
        ChangedByte = const_cast<uint8_t *>(Byte);
        OriginalByte = *ChangedByte;
        *ChangedByte = Mutation == 20 ? uint8_t(*ChangedByte | 1) : 0x90;
      }
      if (Mutation == 15) {
        size_t Work = 0;
        const auto Address =
            registration_frame::originalImagePointerArgumentAddress(*Runtime, 0,
                                                                    Work);
        ASSERT_TRUE(Address);
        Runtime->setArgOperand(
            0, B.CreateIntToPtr(B.getInt32(*Address), B.getPtrTy()));
      }
      if (Mutation == 16 || Mutation == 17)
        Runtime->setArgOperand(
            0, B.CreateInBoundsGEP(B.getInt8Ty(), Runtime->getArgOperand(0),
                                   B.getInt32(Mutation == 16 ? 4 : 1)));
      if (Mutation == 18) {
        auto *Storage = B.CreateAlloca(B.getPtrTy());
        B.CreateStore(Runtime->getArgOperand(0), Storage);
        auto *Value = B.CreateLoad(B.getPtrTy(), Storage);
        Value->setVolatile(true);
        Runtime->setArgOperand(0, Value);
      }
      if (Mutation == 19)
        Runtime->setArgOperand(0, llvm::PoisonValue::get(B.getPtrTy()));
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationIR(*Function, Source, ChangedImage);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
#endif
}
} // namespace neverd::registration_test
