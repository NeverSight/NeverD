//===- LLVMCVoidAnalysisTests.cpp - LLVM C return inference tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

namespace {

llvm::CallInst *emitReturnAddress(llvm::IRBuilder<> &Builder,
                                  bool IsSemanticProducer) {
  llvm::LLVMContext &Context = Builder.getContext();
  auto *PtrTy = llvm::PointerType::getUnqual(Context);
  llvm::CallInst *Call = Builder.CreateIntrinsic(
      llvm::Intrinsic::returnaddress, {PtrTy},
      {llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0)});
  if (IsSemanticProducer)
    neverd::llvm_value_provenance::markSemanticProducer(*Call);
  return Call;
}

llvm::Function *defineReturnedProducer(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "returned_producer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Address =
      Builder.CreatePtrToInt(emitReturnAddress(Builder, true), I32Ty);
  Builder.CreateRet(Address);
  return Function;
}

llvm::Function *defineDiscardedProducer(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "discarded_producer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  emitReturnAddress(Builder, true);
  Builder.CreateRet(llvm::ConstantInt::get(I32Ty, 0));
  return Function;
}

llvm::Function *defineUnmarkedCallResidual(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             "unmarked_call_residual", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto Callee = Module.getOrInsertFunction("ordinary_call", Type);
  Builder.CreateRet(Builder.CreateCall(Callee));
  return Function;
}

TEST(LLVMCVoidAnalysis, DistinguishesReturnedProducerFromDiscardedCall) {
  llvm::LLVMContext Context;
  llvm::Module Module("value-producer-void-analysis", Context);
  llvm::Function *Returned = defineReturnedProducer(Module);
  llvm::Function *Discarded = defineDiscardedProducer(Module);
  llvm::Function *Unmarked = defineUnmarkedCallResidual(Module);
  neverd::LLVMCAnalysisState State;

  EXPECT_FALSE(neverd::analyzeVoidReturn(State, *Returned));
  EXPECT_TRUE(neverd::analyzeVoidReturn(State, *Discarded));
  EXPECT_TRUE(neverd::analyzeVoidReturn(State, *Unmarked));
}

} // namespace
