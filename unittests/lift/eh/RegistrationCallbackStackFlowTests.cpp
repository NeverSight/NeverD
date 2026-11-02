//===- RegistrationCallbackStackFlowTests.cpp - PE32 stack SSA flow -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check private stack copies across branches, loops and runtime resumes.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86/X86RegistrationCatchStack.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

namespace {
using namespace neverd;
TEST(RegistrationCallbackStackFlow, RequiresAReachingDefinitionOnEveryPath) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("callback-stack-flow", Context);
    Module.setDataLayout("e-p:32:32-i64:64-n8:16:32");
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                {llvm::Type::getInt1Ty(Context)}, false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Main = llvm::BasicBlock::Create(Context, "parent.entry", Function);
    auto *Entry = llvm::BasicBlock::Create(Context, "catch.entry", Function);
    auto *Definition = llvm::BasicBlock::Create(Context, "define", Function);
    auto *Use = llvm::BasicBlock::Create(Context, "use", Function);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
    llvm::IRBuilder<> B(Main);
    auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp");
    auto *Copy = B.CreateAlloca(B.getInt32Ty(), nullptr, "copy");
    auto *Resumed = B.CreateAlloca(B.getInt32Ty(), nullptr, "resumed");
    for (auto *Slot : {ESP, Copy, Resumed})
      B.CreateStore(B.getInt32(0), Slot);
    B.CreateRetVoid();
    B.SetInsertPoint(Entry);
    auto *Seed = B.CreateStore(B.getInt32(0), ESP);
    auto *Address =
        B.CreateSub(B.CreateLoad(B.getInt32Ty(), ESP), B.getInt32(4));
    B.CreateStore(B.getInt32(17), B.CreateIntToPtr(Address, B.getPtrTy()));
    if (Mutation == 7)
      B.CreateStore(Address, Copy);
    std::vector<llvm::BasicBlock *> Body{Entry, Definition, Use, Exit};
    std::vector<X86RegistrationCatchStackResume> Resumes;
    llvm::BasicBlock *Suspended = nullptr;
    if (Mutation >= 6) {
      Suspended = llvm::BasicBlock::Create(Context, "suspended", Function);
      Body.push_back(Suspended);
      B.CreateCondBr(Function->getArg(0), Definition, Suspended);
    } else if (Mutation == 1)
      B.CreateCondBr(Function->getArg(0), Definition, Use);
    else
      B.CreateBr(Definition);
    B.SetInsertPoint(Definition);
    if (Mutation != 4 && Mutation != 7)
      B.CreateStore(Address, Copy);
    if (Mutation == 3)
      B.CreateStore(Address, Copy);
    if (Suspended) {
      B.CreateRetVoid();
      B.SetInsertPoint(Suspended);
      if (Mutation == 8)
        B.CreateStore(Address, Copy);
      B.CreateRetVoid();
    } else
      B.CreateBr(Use);
    B.SetInsertPoint(Use);
    if (Suspended) {
      X86RegistrationCatchStackResume Resume;
      Resume.Seed = B.CreateStore(B.getInt32(0), Resumed);
      Resume.Offset = -4;
      Resume.TargetVA = 0x1200;
      Resume.Sources = {Definition, Suspended};
      Resumes.push_back(Resume);
    }
    auto *Pointer = B.CreateLoad(B.getInt32Ty(), Copy);
    if (Mutation == 5)
      Pointer->setVolatile(true);
    B.CreateLoad(B.getInt32Ty(), B.CreateIntToPtr(Pointer, B.getPtrTy()));
    if (Mutation == 4)
      B.CreateStore(Address, Copy);
    if (Mutation == 2)
      B.CreateCondBr(Function->getArg(0), Use, Exit);
    else
      B.CreateBr(Exit);
    B.SetInsertPoint(Exit);
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Result =
        checkX86RegistrationCatchStack(*Entry, Body, *Seed, {}, Resumes);
    const bool Accepted = bool(Result);
    std::string Detail;
    if (!Result)
      Detail = llvm::toString(Result.takeError());
    EXPECT_EQ(Accepted,
              Mutation == 0 || Mutation == 2 || Mutation == 3 || Mutation == 7)
        << Detail;
    if (Accepted)
      EXPECT_EQ(*Result, 16u);
  }
}
} // namespace
