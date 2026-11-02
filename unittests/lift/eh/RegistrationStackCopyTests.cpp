//===- RegistrationStackCopyTests.cpp - Private catch stack PHI copies ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86/X86RegistrationCatchStack.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

namespace {
using namespace neverd;

TEST(RegistrationStackCopy, RequiresTheSameOffsetOnEveryReachingPath) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("stack-copies", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    llvm::IRBuilder<> B(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto Block = [&](const char *Name) {
      return llvm::BasicBlock::Create(Context, Name, Function);
    };
    auto *Parent = Block("parent");
    auto *Entry = Block("catch.entry");
    auto *Left = Block("left"), *Right = Block("right");
    auto *Join = Block("join");
    B.SetInsertPoint(Parent);
    auto *SeedSlot = B.CreateAlloca(B.getInt32Ty());
    auto *CopySlot = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(B.getInt32(Mutation == 5 ? 1 : 0), CopySlot);
    B.CreateRetVoid();
    B.SetInsertPoint(Entry);
    auto *Seed = B.CreateStore(B.getInt32(0), SeedSlot);
    auto *Stack = B.CreateLoad(B.getInt32Ty(), SeedSlot);
    llvm::BasicBlock *Independent = nullptr;
    if (Mutation == 10) {
      B.CreateStore(B.CreateSub(Stack, B.getInt32(4)), CopySlot);
      Independent = Block("independent.root");
    }
    if (Mutation == 6)
      B.CreateLoad(B.getInt32Ty(), CopySlot);
    B.CreateCondBr(B.getTrue(), Left, Right);
    B.SetInsertPoint(Left);
    auto *LeftValue = B.CreateSub(Stack, B.getInt32(4));
    auto *LeftStore = B.CreateStore(LeftValue, CopySlot);
    if (Mutation == 7)
      LeftStore->setVolatile(true);
    B.CreateBr(Join);
    B.SetInsertPoint(Right);
    llvm::Value *RightValue =
        B.CreateSub(Stack, B.getInt32(Mutation == 1 ? 8 : 4));
    if (Mutation == 2)
      RightValue = B.getInt32(7);
    if (Mutation == 3)
      RightValue = llvm::UndefValue::get(B.getInt32Ty());
    if (Mutation == 8)
      RightValue = B.CreateLoad(B.getInt32Ty(), CopySlot);
    if (Mutation != 4)
      B.CreateStore(RightValue, CopySlot);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    auto *Address =
        B.CreateIntToPtr(B.CreateLoad(B.getInt32Ty(), CopySlot), B.getPtrTy());
    B.CreateAlignedStore(B.getInt32(7), Address, llvm::Align(1));
    B.CreateRetVoid();
    if (Independent) {
      B.SetInsertPoint(Independent);
      B.CreateBr(Join);
    }
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    std::vector<llvm::BasicBlock *> Body{Entry, Left, Right, Join};
    if (Independent)
      Body.push_back(Independent);
    if (Mutation == 9)
      Body.erase(Body.begin() + 2);
    auto Bytes = checkX86RegistrationCatchStack(*Entry, Body, *Seed, {});
    if (Mutation == 0) {
      ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
      EXPECT_EQ(*Bytes, 16u);
    } else {
      EXPECT_FALSE(bool(Bytes));
      if (!Bytes)
        llvm::consumeError(Bytes.takeError());
    }
  }
}
} // namespace
