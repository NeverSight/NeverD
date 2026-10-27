//===- RegistrationSEHRuntimeNativeTests.cpp - Imported SEH call edits ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject changed runtime identities, ABI properties and source arguments.

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdint>
#include <utility>

namespace neverd::registration_test {

void checkSEHHeapPolicyEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Runtime = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = Call->getCalledFunction();
              Callee && Callee->getName() == "HeapSetInformation") {
            ASSERT_FALSE(Runtime);
            Runtime = Call;
          }
    ASSERT_TRUE(Runtime);
    ASSERT_EQ(Runtime->arg_size(), 4u);
    auto *Callee = Runtime->getCalledFunction();
    auto ChangedImage = Image;
    uint8_t *ChangedByte = nullptr;
    uint8_t OriginalByte = 0;
    auto Restore = llvm::scope_exit([&] {
      if (ChangedByte)
        *ChangedByte = OriginalByte;
    });
    llvm::IRBuilder<> B(Runtime);
    if (Mutation == 0)
      Runtime->setCallingConv(llvm::CallingConv::C);
    if (Mutation == 1)
      Callee->setCallingConv(llvm::CallingConv::C);
    if (Mutation == 2)
      Callee->setName("unproved_heap_policy");
    if (Mutation == 3)
      rewrite_source::setOriginalVA(*Callee, Source.CodeRange.Begin);
    if (Mutation == 4 || Mutation == 5)
      Runtime->setArgOperand(Mutation == 4 ? 0 : 2,
                             B.CreateIntToPtr(B.getInt32(1), B.getPtrTy()));
    if (Mutation == 6 || Mutation == 7)
      Runtime->setArgOperand(Mutation == 6 ? 1 : 3, B.getInt32(2));
    if (Mutation == 8) {
      auto *Storage = B.CreateAlloca(B.getInt32Ty());
      B.CreateStore(B.getInt32(1), Storage);
      auto *Value = B.CreateLoad(B.getInt32Ty(), Storage);
      Value->setVolatile(true);
      Runtime->setArgOperand(1, Value);
    }
    if (Mutation == 9)
      Runtime->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 10)
      Runtime->setMemoryEffects(llvm::MemoryEffects::readOnly());
    if (Mutation == 11)
      Runtime->addFnAttr(llvm::Attribute::NoCallback);
    if (Mutation == 12) {
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
    if (Mutation == 13) {
      const auto *Marker =
          Runtime->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      ASSERT_TRUE(Marker);
      const auto *Address =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(Marker->getOperand(1));
      ASSERT_TRUE(Address);
      const auto *Argument =
          ChangedImage.readVA(Address->getZExtValue() - 4, 2);
      ASSERT_TRUE(Argument);
      ASSERT_EQ(Argument[0], 0x6a);
      ASSERT_EQ(Argument[1], 1);
      ChangedByte = const_cast<uint8_t *>(Argument + 1);
      OriginalByte = *ChangedByte;
      *ChangedByte = 2;
    }
    if (Mutation == 14)
      Runtime->addParamAttr(0, llvm::Attribute::NoAlias);
    if (Mutation == 15)
      Callee->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    if (Mutation == 16)
      Runtime->addFnAttr(llvm::Attribute::NoReturn);
    if (Mutation == 17)
      Runtime->setArgOperand(1, llvm::UndefValue::get(B.getInt32Ty()));
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, ChangedImage);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

void checkSEHRuntimeEdits(const llvm::Function &Parent,
                          const ExceptionFunction &Source,
                          const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Runtime = nullptr;
    for (auto &Owner : *Module)
      for (auto &Block : Owner)
        for (auto &I : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
            if (const auto *Callee = Call->getCalledFunction();
                Callee && (Callee->getName() == "RaiseException" ||
                           Callee->getName() == "_controlfp_s")) {
              ASSERT_FALSE(Runtime);
              Runtime = Call;
            }
    ASSERT_TRUE(Runtime);
    auto *Callee = Runtime->getCalledFunction();
    const bool ControlFP = Callee->getName() == "_controlfp_s";
    auto *Scalar = llvm::dyn_cast<llvm::LoadInst>(
        Runtime->getArgOperand(ControlFP ? 1 : 0));
    const auto *Precision =
        ControlFP ? llvm::dyn_cast<llvm::ConstantInt>(Runtime->getArgOperand(1))
                  : nullptr;
    const bool NullControlFP =
        ControlFP &&
        llvm::isa<llvm::ConstantPointerNull>(Runtime->getArgOperand(0));
    ASSERT_TRUE(Scalar || (Precision && NullControlFP));
    llvm::IRBuilder<> B(Runtime);
    auto ChangedImage = Image;
    uint8_t *ChangedByte = nullptr;
    uint8_t OriginalByte = 0;
    auto Restore = llvm::scope_exit([&] {
      if (ChangedByte)
        *ChangedByte = OriginalByte;
    });
    if (Mutation == 0)
      Callee->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    if (Mutation == 1)
      Runtime->setCallingConv(ControlFP ? llvm::CallingConv::X86_StdCall
                                        : llvm::CallingConv::C);
    if (Mutation == 2)
      Callee->setCallingConv(ControlFP ? llvm::CallingConv::X86_StdCall
                                       : llvm::CallingConv::C);
    if (Mutation == 3)
      rewrite_source::setOriginalVA(*Callee, Source.CodeRange.Begin);
    if (Mutation == 4)
      Runtime->setArgOperand(
          0, ControlFP
                 ? B.CreateInBoundsGEP(B.getInt8Ty(), Runtime->getArgOperand(0),
                                       B.getInt32(4))
                 : static_cast<llvm::Value *>(B.getInt32(42)));
    if (Mutation == 5) {
      if (Precision)
        Runtime->setArgOperand(1, llvm::UndefValue::get(B.getInt32Ty()));
      else
        Scalar->setVolatile(false);
    }
    if (Mutation == 6) {
      if (Precision)
        Runtime->setArgOperand(2, B.getInt32(0x20000));
      else {
        B.SetInsertPoint(Scalar);
        Scalar->setOperand(0, B.CreateInBoundsGEP(B.getInt8Ty(),
                                                  Scalar->getPointerOperand(),
                                                  B.getInt32(4)));
      }
    }
    if (Mutation == 7) {
      if (Precision)
        Runtime->setArgOperand(
            1, B.getInt32(Precision->getZExtValue() == 0x10000 ? 0x20000
                                                               : 0x10000));
      else {
        B.SetInsertPoint(Scalar->getNextNode());
        B.CreateStore(B.getInt32(0), Scalar->getPointerOperand())
            ->setVolatile(true);
      }
    }
    if (Mutation == 8)
      Runtime->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 9)
      Runtime->addFnAttr(llvm::Attribute::NoCallback);
    if (Mutation == 10)
      Runtime->addFnAttr(llvm::Attribute::NoReturn);
    if (Mutation == 11)
      Runtime->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                           nullptr);
    if (Mutation == 12)
      Runtime->setArgOperand(ControlFP ? 2 : 3,
                             ControlFP
                                 ? static_cast<llvm::Value *>(B.getInt32(0))
                                 : B.CreateAlloca(B.getInt32Ty()));
    if (Mutation == 13)
      Callee->setName("unproved_runtime");
    if (Mutation == 14) {
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
    if (Mutation == 15) {
      const auto *Marker =
          Runtime->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      ASSERT_TRUE(Marker);
      const auto *Address =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(Marker->getOperand(1));
      ASSERT_TRUE(Address);
      const unsigned Distance = Precision
                                    ? 5 + (Precision->isZero() ? 2 : 5) + 2
                                : ControlFP ? (NullControlFP ? 6 : 8)
                                            : 7;
      const auto *Push = ChangedImage.readVA(Address->getZExtValue() - Distance,
                                             Precision ? 5 : 2);
      ASSERT_TRUE(Push);
      ASSERT_EQ(Push[0], Precision ? 0x68 : 0x6a);
      if (Precision)
        ASSERT_EQ(readLE<uint32_t>(Push + 1), 0x30000u);
      else
        ASSERT_LE(Push[1], ControlFP ? 0 : 1);
      ChangedByte = const_cast<uint8_t *>(Push + (Precision ? 3 : 1));
      OriginalByte = *ChangedByte;
      // Invalid exception flags or a mask admitting parameter-error hooks
      // must invalidate the source runtime contract, not inherit the old IR.
      *ChangedByte = Precision ? 7 : ControlFP ? 0xff : 2;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, ChangedImage);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

void checkSEHStartupInfoEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Runtime = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Callee = Call->getCalledFunction();
              Callee && Callee->getName() == "GetStartupInfoW") {
            ASSERT_FALSE(Runtime);
            Runtime = Call;
          }
    ASSERT_TRUE(Runtime);
    ASSERT_EQ(Runtime->arg_size(), 1u);
    auto *Callee = Runtime->getCalledFunction();
    auto ChangedImage = Image;
    uint8_t *ChangedByte = nullptr;
    uint8_t OriginalByte = 0;
    auto Restore = llvm::scope_exit([&] {
      if (ChangedByte)
        *ChangedByte = OriginalByte;
    });
    llvm::IRBuilder<> B(Runtime);
    if (Mutation == 0)
      Callee->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    if (Mutation == 1)
      Runtime->setCallingConv(llvm::CallingConv::C);
    if (Mutation == 2)
      Callee->setCallingConv(llvm::CallingConv::C);
    if (Mutation == 3)
      rewrite_source::setOriginalVA(*Callee, Source.CodeRange.Begin);
    if (Mutation == 4 || Mutation == 5)
      Runtime->setArgOperand(
          0, B.CreateInBoundsGEP(B.getInt8Ty(), Runtime->getArgOperand(0),
                                 B.getInt32(Mutation == 4 ? 4 : -4)));
    if (Mutation == 6)
      Callee->setName("unproved_startup_info");
    if (Mutation == 7)
      Runtime->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                           nullptr);
    if (Mutation == 8)
      Runtime->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 9)
      Runtime->addFnAttr(llvm::Attribute::NoReturn);
    if (Mutation == 10)
      Runtime->setMemoryEffects(llvm::MemoryEffects::readOnly());
    if (Mutation == 11)
      Runtime->addParamAttr(0, llvm::Attribute::NoAlias);
    if (Mutation == 12) {
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
    if (Mutation == 13) {
      const auto *Marker =
          Runtime->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      ASSERT_TRUE(Marker);
      const auto *Address =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(Marker->getOperand(1));
      ASSERT_TRUE(Address);
      const auto *Argument =
          ChangedImage.readVA(Address->getZExtValue() - 4, 4);
      ASSERT_TRUE(Argument);
      ASSERT_EQ(Argument[0], 0x8d);
      ASSERT_EQ(Argument[1], 0x45);
      ASSERT_EQ(Argument[3], 0x50);
      ChangedByte = const_cast<uint8_t *>(Argument + 2);
      OriginalByte = *ChangedByte;
      // A single byte at the end now overlaps SavedESP administration.
      *ChangedByte = uint8_t(-91);
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, ChangedImage);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

void checkSEHComputedStateEdits(const llvm::Function &Parent,
                                const ExceptionFunction &Source,
                                const BinaryImage &Image,
                                const RegistrationStateAnalysis &States) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  const auto Candidate =
      llvm::find_if(States.SEHStateStores, [&](const auto &S) {
        return llvm::none_of(
            Source.Registration->TryLevelStores,
            [&](const auto &O) { return O.StoreVA == S.Address; });
      });
  if (Candidate == States.SEHStateStores.end())
    return;
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::StoreInst *Store = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto *Address =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
        const auto *Seq =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
        if (Address && Seq && Address->getZExtValue() == Candidate->Address &&
            Seq->getZExtValue() == uint32_t(Candidate->OpSeq)) {
          ASSERT_FALSE(Store);
          Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        }
      }
    ASSERT_TRUE(Store);
    auto ChangedImage = Image;
    uint8_t *ChangedByte = nullptr;
    uint8_t OriginalByte = 0;
    auto Restore = llvm::scope_exit([&] {
      if (ChangedByte)
        *ChangedByte = OriginalByte;
    });
    llvm::IRBuilder<> B(Store);
    if (Mutation == 0)
      Store->setOperand(0, B.getInt32(Candidate->Value ^ 1));
    if (Mutation == 1)
      Store->setOperand(1,
                        B.CreateGEP(B.getInt8Ty(), Store->getPointerOperand(),
                                    B.getInt32(4)));
    if (Mutation == 2)
      Store->setOperand(0, B.getInt16(uint16_t(Candidate->Value)));
    if (Mutation == 3)
      Store->setAlignment(llvm::Align(4));
    if (Mutation == 4)
      Store->setVolatile(false);
    if (Mutation == 5)
      Store->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                         nullptr);
    if (Mutation == 6 || Mutation == 7) {
      const auto *Bytes = ChangedImage.readVA(Candidate->Address, 4);
      ASSERT_TRUE(Bytes);
      ASSERT_TRUE(Bytes[0] == 0x89 || Bytes[0] == 0x83);
      ASSERT_EQ(Bytes[2], 0xfc);
      const va_t Changed = Mutation == 6      ? Candidate->Address + 2
                           : Bytes[0] == 0x83 ? Candidate->Address + 3
                                              : Candidate->Address - 5;
      ChangedByte = const_cast<uint8_t *>(ChangedImage.readVA(Changed, 1));
      ASSERT_TRUE(ChangedByte);
      OriginalByte = *ChangedByte;
      *ChangedByte = Mutation == 6 ? 0xf8 : 7;
    }
    if (Mutation == 8 || Mutation == 9) {
      llvm::Value *Poison = llvm::PoisonValue::get(B.getInt32Ty());
      if (Mutation == 9) {
        llvm::IRBuilder<> Entry(
            &*Function->getEntryBlock().getFirstInsertionPt());
        auto *Slot = Entry.CreateAlloca(B.getInt32Ty());
        B.CreateStore(Poison, Slot);
        Poison = B.CreateLoad(B.getInt32Ty(), Slot);
      }
      Store->setOperand(
          0, B.Insert(llvm::BinaryOperator::CreateAnd(Poison, B.getInt32(0))));
    }
    if (Mutation == 10) {
      llvm::LoadInst *Read = nullptr;
      for (auto &Block : *Function)
        for (auto &I : Block)
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            if (const auto *MD = I.getMetadata(
                    windows_eh_md::RegistrationOperationAttachment)) {
              const auto *Address =
                  llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                      MD->getOperand(1));
              if (Address && Address->getZExtValue() == Candidate->Address) {
                ASSERT_FALSE(Read);
                Read = Load;
              }
            }
      if (!Read)
        continue;
      Read->replaceAllUsesWith(llvm::UndefValue::get(Read->getType()));
      Read->eraseFromParent();
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, ChangedImage);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
