//===- MedLLVMDeadSlotsTests.cpp - Private slot observability -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMDeadSlots.h"
#include "gtest/gtest.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"

namespace {
class MedLLVMDeadSlots : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"private-slots", Context};
  llvm::IRBuilder<> Builder{Context};
  llvm::Function *Function = nullptr;

  void SetUp() override {
    Function = llvm::Function::Create(
        llvm::FunctionType::get(Builder.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "probe", Module);
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
  }

  size_t slots() const {
    size_t Count = 0;
    for (const auto &Block : *Function)
      for (const auto &Instruction : Block)
        Count += llvm::isa<llvm::AllocaInst>(Instruction);
    return Count;
  }

  void TearDown() override {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  }
};

TEST_F(MedLLVMDeadSlots, DiscardsUnobservedIncomingValueTransitively) {
  auto *Incoming = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Phi = Builder.CreateAlloca(Builder.getInt32Ty());
  // An incoming register is unavailable on this path, but its PHI has no use.
  auto *Value = Builder.CreateLoad(Builder.getInt32Ty(), Incoming);
  Builder.CreateStore(Builder.CreateAdd(Value, Builder.getInt32(7)), Phi);
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots({Incoming, Phi});
  EXPECT_EQ(slots(), 0u);
  EXPECT_EQ(Function->getEntryBlock().size(), 1u);
}

TEST_F(MedLLVMDeadSlots, PreservesLiveReturnValue) {
  auto *Incoming = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Phi = Builder.CreateAlloca(Builder.getInt32Ty());
  Builder.CreateStore(Builder.getInt32(41), Incoming);
  auto *Value = Builder.CreateLoad(Builder.getInt32Ty(), Incoming);
  Builder.CreateStore(Value, Phi);
  Builder.CreateRet(Builder.CreateLoad(Builder.getInt32Ty(), Phi));
  neverd::discardDeadMedLLVMSlots({Incoming, Phi});
  EXPECT_EQ(slots(), 2u);
}

TEST_F(MedLLVMDeadSlots, PreservesVolatileReadEvenWhenItsValueIsDead) {
  auto *Source = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Phi = Builder.CreateAlloca(Builder.getInt32Ty());
  Builder.CreateStore(Builder.getInt32(41), Source);
  auto *Read = Builder.CreateLoad(Builder.getInt32Ty(), Source);
  Read->setVolatile(true);
  llvm::WeakTrackingVH Kept(Read);
  Builder.CreateStore(Read, Phi);
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots({Source, Phi});
  EXPECT_EQ(slots(), 1u);
  EXPECT_NE(Kept, nullptr);
}

TEST_F(MedLLVMDeadSlots, PreservesEscapingAndObservableStores) {
  auto *Escaped = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Volatile = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Atomic = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Source = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "observe", Module);
  Builder.CreateCall(Callee, {Escaped});
  Builder.CreateStore(Builder.getInt32(1), Volatile)->setVolatile(true);
  Builder.CreateStore(Builder.getInt32(2), Atomic)
      ->setAtomic(llvm::AtomicOrdering::Monotonic);
  Builder.CreateStore(Builder.getInt32(3), Source)
      ->setMetadata("source-operation", llvm::MDNode::get(Context, {}));
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots({Escaped, Volatile, Atomic, Source});
  EXPECT_EQ(slots(), 4u);
}

TEST_F(MedLLVMDeadSlots, DoesNotReclassifySourceStorageAsPrivate) {
  auto *Source = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Phi = Builder.CreateAlloca(Builder.getInt32Ty());
  Builder.CreateStore(Builder.getInt32(41), Source);
  Builder.CreateStore(Builder.CreateLoad(Builder.getInt32Ty(), Source), Phi);
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots({Phi});
  EXPECT_EQ(slots(), 1u);
  EXPECT_EQ(Source->getNumUses(), 2u);
}
} // namespace
