//===- MedLLVMDeadSlotsTests.cpp - Private slot observability -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMDeadSlots.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <vector>

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

TEST_F(MedLLVMDeadSlots, DiscardsAnUnobservedEntryIdentityCopy) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Initial = neverd::llvm_source_unknown::createEntrySeed(
      Builder, Builder.getInt32Ty());
  llvm::WeakTrackingVH InitialHandle(Initial);
  Builder.CreateStore(Initial, Slot);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  Builder.CreateBr(Body);
  Builder.SetInsertPoint(Body);
  Builder.CreateStore(Builder.CreateLoad(Builder.getInt32Ty(), Slot), Slot);
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots({Slot});
  EXPECT_EQ(slots(), 0u);
  EXPECT_EQ(InitialHandle, nullptr);
  EXPECT_EQ(Body->size(), 1u);
}

TEST_F(MedLLVMDeadSlots, IdentityCopiesCannotHideObservedOrChangedState) {
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "observe", Module);
  std::vector<llvm::AllocaInst *> Slots;
  std::vector<llvm::WeakTrackingVH> Kept;
  // A live value, an intervening write, escape, ordered accesses and source
  // annotations each independently prevent the private identity proof.
  for (unsigned Case = 0; Case != 9; ++Case) {
    auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
    Slots.push_back(Slot);
    Builder.CreateStore(Builder.getInt32(41), Slot);
    auto *Read = Builder.CreateLoad(Builder.getInt32Ty(), Slot);
    if (Case == 1)
      Builder.CreateStore(Builder.getInt32(23), Slot);
    auto *Write = Builder.CreateStore(Read, Slot);
    Kept.emplace_back(Read);
    Kept.emplace_back(Write);
    auto *Annotation = llvm::MDNode::get(Context, {});
    switch (Case) {
    case 0: {
      auto *Use = Builder.CreateAlloca(Builder.getInt32Ty());
      Builder.CreateStore(Read, Use)->setVolatile(true);
      break;
    }
    case 2:
      Builder.CreateCall(Callee, {Slot});
      break;
    case 3:
      Read->setVolatile(true);
      break;
    case 4:
      Write->setVolatile(true);
      break;
    case 5:
      Read->setAtomic(llvm::AtomicOrdering::Monotonic);
      break;
    case 6:
      Write->setAtomic(llvm::AtomicOrdering::Monotonic);
      break;
    case 7:
      Read->setMetadata("source-operation", Annotation);
      break;
    case 8:
      Write->setMetadata("source-operation", Annotation);
      break;
    default:
      break;
    }
  }
  Builder.CreateRet(Builder.getInt32(19));
  neverd::discardDeadMedLLVMSlots(Slots);
  for (const auto &Handle : Kept)
    EXPECT_NE(Handle, nullptr);
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

TEST_F(MedLLVMDeadSlots, RemovesOnlyTheSeedOverwrittenBeforeEveryRead) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Initial = neverd::llvm_source_unknown::createEntrySeed(
      Builder, Builder.getInt32Ty());
  llvm::WeakTrackingVH InitialHandle(Initial);
  auto *Seed = Builder.CreateStore(Initial, Slot);
  llvm::WeakTrackingVH SeedHandle(Seed);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  Builder.CreateBr(Body);
  Builder.SetInsertPoint(Body);
  Builder.CreateStore(Builder.getInt32(41), Slot);
  Builder.CreateRet(Builder.CreateLoad(Builder.getInt32Ty(), Slot));

  neverd::discardOverwrittenMedLLVMSeeds({Seed});
  EXPECT_EQ(SeedHandle, nullptr);
  EXPECT_EQ(InitialHandle, nullptr);
  EXPECT_EQ(Slot->getNumUses(), 2u);
  llvm::DominatorTree Dominators(*Function);
  llvm::PromoteMemToReg({Slot}, Dominators);
  auto *Returned = llvm::cast<llvm::ReturnInst>(Body->getTerminator());
  EXPECT_EQ(Returned->getReturnValue(), Builder.getInt32(41));
}

TEST_F(MedLLVMDeadSlots, PreservesASeedObservedBeforeTheStoreInTheSameBlock) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Seed = Builder.CreateStore(Builder.getInt32(11), Slot);
  llvm::WeakTrackingVH SeedHandle(Seed);
  auto *Value = Builder.CreateLoad(Builder.getInt32Ty(), Slot);
  Builder.CreateStore(Builder.getInt32(41), Slot);
  Builder.CreateRet(Value);

  neverd::discardOverwrittenMedLLVMSeeds({Seed});
  EXPECT_NE(SeedHandle, nullptr);
  llvm::DominatorTree Dominators(*Function);
  llvm::PromoteMemToReg({Slot}, Dominators);
  auto *Returned =
      llvm::cast<llvm::ReturnInst>(Function->getEntryBlock().getTerminator());
  EXPECT_EQ(Returned->getReturnValue(), Builder.getInt32(11));
}

TEST_F(MedLLVMDeadSlots, AWriteBeforeTheSeedCannotReplaceIt) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  Builder.CreateStore(Builder.getInt32(41), Slot);
  auto *Seed = Builder.CreateStore(Builder.getInt32(11), Slot);
  llvm::WeakTrackingVH SeedHandle(Seed);
  Builder.CreateRet(Builder.CreateLoad(Builder.getInt32Ty(), Slot));

  neverd::discardOverwrittenMedLLVMSeeds({Seed});
  EXPECT_NE(SeedHandle, nullptr);
  llvm::DominatorTree Dominators(*Function);
  llvm::PromoteMemToReg({Slot}, Dominators);
  auto *Returned =
      llvm::cast<llvm::ReturnInst>(Function->getEntryBlock().getTerminator());
  EXPECT_EQ(Returned->getReturnValue(), Builder.getInt32(11));
}

TEST_F(MedLLVMDeadSlots, PreservesASeedOnTheFirstLoopIteration) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Initial = neverd::llvm_source_unknown::createEntrySeed(
      Builder, Builder.getInt32Ty());
  auto *Seed = Builder.CreateStore(Initial, Slot);
  llvm::WeakTrackingVH SeedHandle(Seed);
  auto *Header = llvm::BasicBlock::Create(Context, "header", Function);
  auto *Latch = llvm::BasicBlock::Create(Context, "latch", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  Builder.CreateBr(Header);
  Builder.SetInsertPoint(Header);
  auto *Value = Builder.CreateLoad(Builder.getInt32Ty(), Slot);
  Builder.CreateCondBr(Builder.CreateICmpEQ(Value, Builder.getInt32(41)), Exit,
                       Latch);
  Builder.SetInsertPoint(Latch);
  Builder.CreateStore(Builder.getInt32(41), Slot);
  Builder.CreateBr(Header);
  Builder.SetInsertPoint(Exit);
  Builder.CreateRet(Value);

  neverd::discardOverwrittenMedLLVMSeeds({Seed});
  EXPECT_NE(SeedHandle, nullptr);
  llvm::DominatorTree Dominators(*Function);
  llvm::PromoteMemToReg({Slot}, Dominators);
  auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Header->front());
  ASSERT_NE(Phi, nullptr);
  EXPECT_EQ(Phi->getIncomingValueForBlock(&Function->getEntryBlock()), Initial);
  EXPECT_EQ(Phi->getIncomingValueForBlock(Latch), Builder.getInt32(41));
}

TEST_F(MedLLVMDeadSlots, PreservesTheSeedOnAnEdgeThatBypassesItsDefinition) {
  auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Seed = Builder.CreateStore(Builder.getInt32(11), Slot);
  llvm::WeakTrackingVH SeedHandle(Seed);
  auto *Condition = neverd::llvm_source_unknown::createEntrySeed(
      Builder, Builder.getInt1Ty());
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  Builder.CreateCondBr(Condition, Body, Join);
  Builder.SetInsertPoint(Body);
  Builder.CreateStore(Builder.getInt32(41), Slot);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Join);
  Builder.CreateRet(Builder.CreateLoad(Builder.getInt32Ty(), Slot));

  neverd::discardOverwrittenMedLLVMSeeds({Seed});
  EXPECT_NE(SeedHandle, nullptr);
  llvm::DominatorTree Dominators(*Function);
  llvm::PromoteMemToReg({Slot}, Dominators);
  auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Join->front());
  ASSERT_NE(Phi, nullptr);
  EXPECT_EQ(Phi->getIncomingValueForBlock(&Function->getEntryBlock()),
            Builder.getInt32(11));
  EXPECT_EQ(Phi->getIncomingValueForBlock(Body), Builder.getInt32(41));
}

TEST_F(MedLLVMDeadSlots, DoesNotRemoveEscapingOrderedOrAnnotatedSeeds) {
  std::vector<llvm::StoreInst *> Seeds;
  std::vector<llvm::AllocaInst *> Slots;
  for (unsigned I = 0; I != 5; ++I) {
    auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
    Seeds.push_back(Builder.CreateStore(Builder.getInt32(11), Slot));
    Slots.push_back(Slot);
  }
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getVoidTy(), {Builder.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "observe", Module);
  Builder.CreateCall(Callee, {Slots[0]});
  Seeds[1]->setVolatile(true);
  Seeds[2]->setAtomic(llvm::AtomicOrdering::Monotonic);
  Seeds[3]->setMetadata("source-operation", llvm::MDNode::get(Context, {}));
  auto *EarlyRead = Builder.CreateLoad(Builder.getInt32Ty(), Slots[4]);
  EarlyRead->setVolatile(true);
  for (auto *Slot : Slots)
    Builder.CreateStore(Builder.getInt32(41), Slot);
  Builder.CreateRet(Builder.getInt32(0));
  std::vector<llvm::WeakTrackingVH> Handles(Seeds.begin(), Seeds.end());

  neverd::discardOverwrittenMedLLVMSeeds(Seeds);
  for (const auto &Handle : Handles)
    EXPECT_NE(Handle, nullptr);
}

TEST(MedLLVMEntrySeeds, CompleteDefinitionsAvoidRedundantPromotionFrontiers) {
  // Each temporary has one definition and a cross-block use. A redundant
  // entry store forces mem2reg to visit the whole dominator subtree per slot.
  // Inspect the raw emitter output, before any optimization can mask that cost.
  for (bool Source : {false, true}) {
    SCOPED_TRACE(Source);
    neverd::MedFunc Func;
    Func.Entry = 0x1000;
    Func.Name = "linear_definitions";
    Func.ReturnType = neverd::NdType::makeInt(4);
    Func.CC = neverd::CallingConv::Win64;
    constexpr unsigned Count = 128;
    Func.Blocks.resize(Count + 1);
    neverd::MedVar Value = neverd::MedVar::makeConst(19, 4);
    for (unsigned I = 0; I != Count; ++I) {
      auto &Block = Func.Blocks[I];
      Block.Id = I;
      Block.StartAddr = Func.Entry + I;
      Block.EndAddr = Block.StartAddr + 1;
      if (I != 0)
        Block.Preds.push_back(I - 1);
      Block.Succs.push_back(I + 1);
      neverd::MedOp Add;
      Add.Opcode = neverd::NdOp::INT_ADD;
      Add.Output.Kind = neverd::MedVar::Temp;
      Add.Output.Id = I + 1;
      Add.Output.SSAVer = 1;
      Add.Output.Size = 4;
      Add.addInput(Value);
      Add.addInput(neverd::MedVar::makeConst(1, 4));
      Value = Add.Output;
      Block.Ops.push_back(Add);
    }
    auto &Last = Func.Blocks.back();
    Last.Id = Count;
    Last.StartAddr = Func.Entry + Count;
    Last.EndAddr = Last.StartAddr + 1;
    Last.Preds.push_back(Count - 1);
    neverd::MedOp Return;
    Return.Opcode = neverd::NdOp::RETURN;
    Return.addInput(Value);
    Last.Ops.push_back(Return);

    llvm::LLVMContext Context;
    neverd::MedLLVMEmitter Emitter;
    Emitter.setPreserveUnknownSourceValues(Source);
    auto Module = Emitter.emit({Func}, Context, "linear", neverd::Arch::X64);
    ASSERT_NE(Module, nullptr);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto *Function = Module->getFunction(Func.Name);
    ASSERT_NE(Function, nullptr);
    std::vector<llvm::AllocaInst *> Slots;
    for (auto &Block : *Function) {
      for (auto &Instruction : Block) {
        EXPECT_FALSE(neverd::llvm_source_unknown::isValue(&Instruction));
        auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        if (!Slot)
          continue;
        unsigned Stores = 0;
        for (auto *User : Slot->users())
          Stores += llvm::isa<llvm::StoreInst>(User);
        EXPECT_EQ(Stores, 1u);
        Slots.push_back(Slot);
      }
    }
    EXPECT_EQ(Slots.size(), Count);
    llvm::DominatorTree Dominators(*Function);
    llvm::PromoteMemToReg(Slots, Dominators);
    // Fold the resulting constant additions independently of source printing.
    auto *Returned =
        llvm::cast<llvm::ReturnInst>(Function->back().getTerminator())
            ->getReturnValue();
    unsigned Sum = 0;
    while (auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(Returned)) {
      ASSERT_EQ(Add->getOpcode(), llvm::Instruction::Add);
      Sum += llvm::cast<llvm::ConstantInt>(Add->getOperand(1))->getZExtValue();
      Returned = Add->getOperand(0);
    }
    Sum += llvm::cast<llvm::ConstantInt>(Returned)->getZExtValue();
    EXPECT_EQ(Sum, 19 + Count);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  }
}
} // namespace
