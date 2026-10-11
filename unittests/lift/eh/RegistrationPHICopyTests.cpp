//===- RegistrationPHICopyTests.cpp - PE32 callback copy edges ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationCxxIRProof.h"
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationCxxBlocks.h"
#include "gtest/gtest.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

namespace {
using namespace neverd;

TEST(RegistrationPHICopy, RequiresTheExactSameInvocationEdge) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("callback-copy", Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *From = llvm::BasicBlock::Create(Context, "from", Function);
    auto *Other = llvm::BasicBlock::Create(Context, "other", Function);
    auto *Copy = llvm::BasicBlock::Create(Context, "copy", Function);
    auto *To = llvm::BasicBlock::Create(Context, "to", Function);
    llvm::IRBuilder<> Builder(From);
    Builder.CreateCondBr(Builder.getTrue(), Copy, Other);
    Builder.SetInsertPoint(Other);
    Builder.CreateBr(To);
    Builder.SetInsertPoint(Copy);
    Builder.CreateBr(To);
    Builder.SetInsertPoint(To);
    Builder.CreateRetVoid();
    std::map<int, llvm::BasicBlock *> Blocks{{0, From}, {1, Other}, {2, To}};
    std::map<int, X86RegistrationCatchIdentity> Owners{
        {0, {1, 0}}, {1, {1, 0}}, {2, {1, 0}}};
    std::map<llvm::BasicBlock *, std::pair<int, int>> Edges{{Copy, {0, 2}}};
    if (Mutation == 1)
      Owners.clear();
    if (Mutation == 2)
      Owners[2] = {1, 1};
    if (Mutation == 3)
      Owners.erase(0);
    if (Mutation == 4)
      Owners.erase(2);
    if (Mutation == 5)
      Edges[Copy].first = 1;
    if (Mutation == 6)
      Edges[Copy].second = 1;
    if (Mutation == 7)
      Blocks.erase(2);
    if (Mutation == 8)
      Blocks[3] = Copy;
    if (Mutation == 9)
      Blocks[3] = From;
    if (Mutation == 10)
      Other->getTerminator()->setSuccessor(0, Copy);
    if (Mutation == 11) {
      Copy->getTerminator()->eraseFromParent();
      Builder.SetInsertPoint(Copy);
      Builder.CreateCondBr(Builder.getTrue(), To, Other);
    }
    if (Mutation == 12)
      Edges.emplace(nullptr, std::pair{0, 2});
    const auto Result = registrationCxxPHICopyOwners(Owners, Blocks, Edges);
    ASSERT_EQ(bool(Result), Mutation <= 1);
    if (Mutation == 0) {
      ASSERT_EQ(Result->size(), 1u);
      EXPECT_EQ(Result->at(Copy), (X86RegistrationCatchIdentity{1, 0}));
    }
    if (Mutation == 1)
      EXPECT_TRUE(Result->empty());
  }
}
TEST(RegistrationPHICopy, InstallationRequiresPrivateScalarCopies) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("copy-proof", Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *From = llvm::BasicBlock::Create(Context, "from", Function);
    auto *Copy = llvm::BasicBlock::Create(Context, "unnamed-edge", Function);
    auto *To = llvm::BasicBlock::Create(Context, "to", Function);
    llvm::IRBuilder<> Builder(From);
    auto *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
    Builder.CreateStore(Builder.getInt32(7), Slot);
    auto *Exit = Builder.CreateBr(Copy);
    Builder.SetInsertPoint(Copy);
    auto *Load = Builder.CreateLoad(Builder.getInt32Ty(), Slot);
    auto *Store = Builder.CreateStore(Load, Slot);
    auto *Branch = Builder.CreateBr(To);
    Builder.SetInsertPoint(To);
    auto *Return = Builder.CreateRetVoid();
    coff_registration::CxxIRControlProof Proof;
    Proof.Source.Blocks.emplace_back();
    Proof.Source.Blocks.back().Id = 0;
    Proof.Source.Blocks.back().Succs = {1};
    Proof.Segments[0].Exit = Exit;
    Proof.Segments[1].Enter = Return;
    if (Mutation == 1)
      Load->setVolatile(true);
    if (Mutation == 2)
      Store->setVolatile(true);
    if (Mutation == 3) {
      Builder.SetInsertPoint(Branch);
      Store->setOperand(
          1, Builder.CreateGEP(Builder.getInt8Ty(), Slot, Builder.getInt32(1)));
    }
    if (Mutation == 4) {
      auto *Global = new llvm::GlobalVariable(
          Module, Builder.getInt32Ty(), false,
          llvm::GlobalValue::ExternalLinkage, nullptr, "external");
      Store->setOperand(1, Global);
    }
    if (Mutation == 5) {
      Builder.SetInsertPoint(Exit);
      Builder.CreatePtrToInt(Slot, Builder.getInt32Ty());
    }
    if (Mutation == 6) {
      Builder.SetInsertPoint(Branch);
      Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::sideeffect));
    }
    if (Mutation == 7)
      Branch->setSuccessor(0, From);
    if (Mutation == 8) {
      Branch->eraseFromParent();
      Builder.SetInsertPoint(Copy);
      Builder.CreateCondBr(Builder.getTrue(), To, To);
    }
    if (Mutation == 9)
      llvm::BlockAddress::get(Copy);
    if (Mutation == 10) {
      auto *Other = llvm::BasicBlock::Create(Context, "other", Function);
      Builder.SetInsertPoint(Other);
      Builder.CreateBr(Copy);
    }
    if (Mutation == 11) {
      Builder.SetInsertPoint(Branch);
      Builder.CreateStore(Builder.getInt8(1), Slot);
    }
    if (Mutation == 12) {
      Load->setAtomic(llvm::AtomicOrdering::Monotonic);
    }
    auto Error = coff_registration::bindCxxCopyEdges(Proof);
    EXPECT_EQ(bool(Error), Mutation != 0);
    if (Error)
      llvm::consumeError(std::move(Error));
    else {
      ASSERT_EQ(Proof.CopyEdges.size(), 1u);
      EXPECT_EQ(Proof.CopyEdges.at(Copy), To);
    }
  }
}
} // namespace
