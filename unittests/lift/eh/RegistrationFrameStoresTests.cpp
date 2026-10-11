//===- RegistrationFrameStoresTests.cpp - PE32 reaching spill proof ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove private spill addresses across every path, including partial writes
/// and loop suffixes, without repeatedly scanning unrelated LLVM instructions.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "gtest/gtest.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <vector>

namespace {
using namespace neverd::coff_registration;

struct FrameFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"frame-spills", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> Builder{
      llvm::BasicBlock::Create(Context, "entry", Function)};
  llvm::AllocaInst *Frame = Builder.CreateAlloca(Builder.getInt32Ty());
  llvm::Value *Address = Builder.CreatePtrToInt(Frame, Builder.getInt32Ty());

  FrameFixture() { Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32"); }

  void check(bool Valid) {
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Error = validateFramePrivacy(*Function, {}, Frame, {}, {});
    EXPECT_EQ(!bool(Error), Valid) << llvm::toString(std::move(Error));
    llvm::consumeError(std::move(Error));
  }
};

TEST(RegistrationFrameStores, ManyIndependentSpillsFitTheProofBudget) {
  FrameFixture F;
  auto &B = F.Builder;
  std::vector<llvm::AllocaInst *> Slots;
  for (unsigned I = 0; I != 1200; ++I) {
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(F.Address, Slot);
    Slots.push_back(Slot);
  }
  for (auto *Slot : Slots) {
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Slot);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Bits, B.getPtrTy()));
  }
  B.CreateRet(B.getInt32(0));
  F.check(true);
}

TEST(RegistrationFrameStores, RequiresTheSameFullDefinitionOnEveryPath) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FrameFixture F;
    auto &B = F.Builder;
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Function);
    auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Function);
    auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Function);
    B.CreateCondBr(F.Function->getArg(0), Left, Right);
    B.SetInsertPoint(Left);
    B.CreateStore(F.Address, Slot);
    B.CreateBr(Join);
    B.SetInsertPoint(Right);
    if (Mutation != 1) {
      auto *Value = Mutation == 2   ? B.getInt32(0)
                    : Mutation == 3 ? B.CreateTrunc(F.Address, B.getInt16Ty())
                                    : F.Address;
      auto *Store = B.CreateStore(Value, Slot);
      if (Mutation == 4)
        Store->setAtomic(llvm::AtomicOrdering::Release);
    }
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Slot);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Bits, B.getPtrTy()));
    B.CreateRet(B.getInt32(0));
    F.check(Mutation == 0);
  }
}

TEST(RegistrationFrameStores, ChecksLoopSuffix) {
  for (bool Overwrite : {false, true}) {
    SCOPED_TRACE(Overwrite);
    FrameFixture F;
    auto &B = F.Builder;
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(F.Address, Slot);
    auto *Loop = llvm::BasicBlock::Create(F.Context, "loop", F.Function);
    auto *Done = llvm::BasicBlock::Create(F.Context, "done", F.Function);
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Slot);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Bits, B.getPtrTy()));
    B.CreateStore(Overwrite ? B.getInt32(0) : F.Address, Slot);
    B.CreateCondBr(F.Function->getArg(0), Loop, Done);
    B.SetInsertPoint(Done);
    B.CreateRet(B.getInt32(0));
    F.check(!Overwrite);
  }
}

TEST(RegistrationFrameStores, ChecksAliasedPartialOverwrite) {
  for (bool Overwrite : {false, true}) {
    SCOPED_TRACE(Overwrite);
    FrameFixture F;
    auto &B = F.Builder;
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(F.Address, Slot);
    auto *Byte = B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(1));
    if (Overwrite)
      B.CreateStore(B.getInt8(0), Byte);
    else
      B.CreateLoad(B.getInt8Ty(), Byte);
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Slot);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Bits, B.getPtrTy()));
    B.CreateRet(B.getInt32(0));
    F.check(!Overwrite);
  }
}
} // namespace
