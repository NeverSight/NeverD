//===- SourceCallObservationsTests.cpp - Source body bounds ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dead machine scaffolding must not hide real input and memory observers.
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineSourceCallObservations.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/pipeline/Limits.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace {
using namespace neverd;

struct Fixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"observations", Context};
  llvm::IRBuilder<> Builder{Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt64Ty(),
                              {Builder.getPtrTy(), Builder.getInt64Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "body", Module);

  Fixture() {
    Builder.SetInsertPoint(
        llvm::BasicBlock::Create(Context, "entry", Function));
  }

  llvm::Value *unknown() {
    return llvm_source_unknown::create(Builder, Builder.getInt64Ty());
  }

  std::string text() {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Module.print(OS, nullptr);
    return Text;
  }
};

TEST(SourceCallObservations, DropsDeadCalculationsButKeepsEveryMemoryEffect) {
  Fixture F;
  auto *Unknown = F.unknown();
  LLVMSourceHandle UnknownHandle(Unknown);
  auto *Dead = F.Function->getArg(1);
  llvm::Value *Chain = Dead;
  for (size_t I = 0; I <= limits::pipeline::kSourceInlineBodyOperations; ++I)
    Chain = F.Builder.CreateAdd(Chain, Unknown);
  auto *Load =
      F.Builder.CreateLoad(F.Builder.getInt64Ty(), F.Function->getArg(0));
  F.Builder.CreateStore(F.Builder.getInt64(7), F.Function->getArg(0));
  auto *Ordinary = llvm::Function::Create(
      llvm::FunctionType::get(F.Builder.getInt64Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "ordinary", F.Module);
  Ordinary->setMemoryEffects(llvm::MemoryEffects::none());
  Ordinary->addFnAttr(llvm::Attribute::NoUnwind);
  Ordinary->addFnAttr(llvm::Attribute::WillReturn);
  auto *Call = F.Builder.CreateCall(Ordinary);
  LLVMSourceHandle LoadHandle(Load), CallHandle(Call);
  F.Builder.CreateRet(F.Builder.getInt64(0));
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  EXPECT_FALSE(source_inline::hasUnavailableObservations(*F.Function));
  const auto Result = source_inline::compactSourceBody(*F.Function);
  ASSERT_TRUE(Result);
  EXPECT_FALSE(Result->Unavailable);
  EXPECT_TRUE(F.Function->getArg(1)->use_empty());
  EXPECT_EQ(static_cast<llvm::Value *>(UnknownHandle), nullptr);
  EXPECT_EQ(static_cast<llvm::Value *>(LoadHandle), Load);
  EXPECT_EQ(static_cast<llvm::Value *>(CallHandle), Call);
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}

TEST(SourceCallObservations, RemovesUnusedPhiCyclesWithoutChangingEdges) {
  Fixture F;
  auto *Unknown = F.unknown();
  auto *Entry = F.Builder.GetInsertBlock();
  auto *Loop = llvm::BasicBlock::Create(F.Context, "loop", F.Function);
  auto *Exit = llvm::BasicBlock::Create(F.Context, "exit", F.Function);
  F.Builder.CreateBr(Loop);
  F.Builder.SetInsertPoint(Loop);
  auto *Phi = F.Builder.CreatePHI(F.Builder.getInt64Ty(), 2);
  auto *Next = F.Builder.CreateAdd(Phi, F.Builder.getInt64(1));
  Phi->addIncoming(Unknown, Entry);
  Phi->addIncoming(Next, Loop);
  auto *Branch = F.Builder.CreateCondBr(
      F.Builder.CreateICmpEQ(F.Function->getArg(1), F.Builder.getInt64(0)),
      Loop, Exit);
  F.Builder.SetInsertPoint(Exit);
  F.Builder.CreateRet(F.Builder.getInt64(3));
  LLVMSourceHandle PhiHandle(Phi);
  const auto Result = source_inline::compactSourceBody(*F.Function);
  ASSERT_TRUE(Result);
  EXPECT_FALSE(Result->Unavailable);
  EXPECT_EQ(static_cast<llvm::Value *>(PhiHandle), nullptr);
  EXPECT_EQ(Branch->getSuccessor(0), Loop);
  EXPECT_EQ(Branch->getSuccessor(1), Exit);
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}

TEST(SourceCallObservations, RetainsUnknownMemoryControlReturnAndCallInputs) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    Fixture F;
    auto *Unknown = F.unknown();
    LLVMSourceHandle Kept(Unknown);
    if (Mode == 0)
      F.Builder.CreateStore(Unknown, F.Function->getArg(0));
    else if (Mode == 1)
      F.Builder.CreateLoad(
          F.Builder.getInt8Ty(),
          F.Builder.CreateIntToPtr(Unknown, F.Builder.getPtrTy()));
    else if (Mode == 2) {
      auto *End = llvm::BasicBlock::Create(F.Context, "end", F.Function);
      F.Builder.CreateCondBr(
          F.Builder.CreateICmpNE(Unknown, F.Builder.getInt64(0)), End, End);
      F.Builder.SetInsertPoint(End);
    } else if (Mode == 3) {
      auto *Call = llvm::Function::Create(
          llvm::FunctionType::get(F.Builder.getVoidTy(),
                                  {F.Builder.getInt64Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, "observer", F.Module);
      F.Builder.CreateCall(Call, Unknown);
    }
    F.Builder.CreateRet(Mode == 4 ? Unknown : F.Builder.getInt64(0));
    EXPECT_TRUE(source_inline::hasUnavailableObservations(*F.Function));
    const auto Result = source_inline::compactSourceBody(*F.Function);
    ASSERT_TRUE(Result);
    EXPECT_TRUE(Result->Unavailable);
    EXPECT_EQ(static_cast<llvm::Value *>(Kept), Unknown);
    EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  }
}

TEST(SourceCallObservations, ExhaustedBoundsLeaveTheWholeBodyUntouched) {
  for (bool Live : {false, true}) {
    SCOPED_TRACE(Live);
    Fixture F;
    const size_t Count =
        Live ? limits::pipeline::kSourceInlineBodyOperations
             : limits::pipeline::kSourceInlineExpansionOperations;
    llvm::Value *Chain = F.Function->getArg(1);
    for (size_t I = 0; I != Count; ++I)
      if (Live)
        F.Builder.CreateLoad(F.Builder.getInt8Ty(), F.Function->getArg(0));
      else
        Chain = F.Builder.CreateAdd(Chain, F.Builder.getInt64(1));
    F.Builder.CreateRet(F.Builder.getInt64(0));
    const auto Before = F.text();
    EXPECT_FALSE(source_inline::compactSourceBody(*F.Function));
    EXPECT_TRUE(source_inline::hasUnavailableObservations(*F.Function));
    EXPECT_EQ(F.text(), Before);
  }
}

TEST(SourceCallObservations, ARealFunctionWithTheMarkerIsStillAnObserver) {
  Fixture F;
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(F.Builder.getInt64Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "real_body", F.Module);
  Callee->addFnAttr(llvm_source_unknown::Attribute);
  auto *Body = llvm::BasicBlock::Create(F.Context, "entry", Callee);
  llvm::IRBuilder<> B(Body);
  B.CreateRet(B.getInt64(9));
  auto *Call = F.Builder.CreateCall(Callee);
  LLVMSourceHandle Kept(Call);
  F.Builder.CreateRet(F.Builder.getInt64(0));
  const auto Result = source_inline::compactSourceBody(*F.Function);
  ASSERT_TRUE(Result);
  EXPECT_EQ(static_cast<llvm::Value *>(Kept), Call);
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}
} // namespace
