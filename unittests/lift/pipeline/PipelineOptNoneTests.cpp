//===- PipelineOptNoneTests.cpp - Preserve native runtime contracts
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsSEHEmulatedFrame.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace neverd;

namespace {

llvm::Function *buildRoot(llvm::Module &M, llvm::StringRef Name, bool OptNone) {
  auto &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, M);
  if (OptNone) {
    F->addFnAttr(llvm::Attribute::NoInline);
    F->addFnAttr(llvm::Attribute::OptimizeNone);
  }
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  auto *Slot = B.CreateAlloca(I32);
  auto *Store = B.CreateStore(F->getArg(0), Slot);
  Store->setMetadata(windows_eh_md::RegistrationRootAttachment,
                     llvm::MDNode::get(C, llvm::MDString::get(C, "root")));
  B.CreateRet(B.CreateLoad(I32, Slot));
  return F;
}

std::string body(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  for (const llvm::BasicBlock &BB : F)
    BB.print(OS);
  return Text;
}

bool hasAlloca(const llvm::Function &F) {
  for (const llvm::Instruction &I : llvm::instructions(F))
    if (llvm::isa<llvm::AllocaInst>(I))
      return true;
  return false;
}

TEST(PipelineOptNone, PromotionPreservesRuntimeRootStores) {
  llvm::LLVMContext C;
  llvm::Module M("roots", C);
  const std::string Before = body(*buildRoot(M, "callback", true));
  buildRoot(M, "ordinary", false);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  Pipeline::promoteScaffoldingAllocas(M);

  EXPECT_EQ(body(*M.getFunction("callback")), Before);
  EXPECT_FALSE(hasAlloca(*M.getFunction("ordinary")));
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(PipelineOptNone, OptimizationPreservesRuntimeRootStores) {
  for (auto Strength :
       {Pipeline::OptStrength::Thin, Pipeline::OptStrength::Deep,
        Pipeline::OptStrength::Source}) {
    for (bool Conservative : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Strength));
      SCOPED_TRACE(Conservative);
      llvm::LLVMContext C;
      llvm::Module M("roots", C);
      const std::string Before = body(*buildRoot(M, "callback", true));
      buildRoot(M, "ordinary", false);
      Pipeline::OptimizationOptions Options;
      Options.Strength = Strength;
      Options.Conservative = Conservative;

      const auto Result = Pipeline::optimizeModule(M, Options);

      EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
      EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
      EXPECT_EQ(body(*M.getFunction("callback")), Before);
      EXPECT_FALSE(hasAlloca(*M.getFunction("ordinary")));
      EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    }
  }
}

TEST(PipelineOptNone, ManualFrameCanonicalizationAlsoPreservesOptNone) {
  llvm::LLVMContext C;
  llvm::Module M("frame", C);
  M.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  M.setDataLayout("e-p:32:32");
  auto *F = buildRoot(M, "callback", true);
  auto &Entry = F->getEntryBlock();
  Entry.setName(kFrameSetupBlock);
  llvm::IRBuilder<> B(Entry.getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  auto *End = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(64), "frame_end");
  auto *SP = B.CreatePtrToInt(End, B.getInt32Ty(), kRspInitValue);
  auto *Pointer =
      B.CreateIntToPtr(B.CreateSub(SP, B.getInt32(8)), B.getPtrTy());
  B.CreateStore(F->getArg(0), Pointer);
  // The stack-pointer repair follows constant inttoptr accesses separately.
  auto *Negative =
      llvm::ConstantExpr::getIntToPtr(B.getInt32(-4), B.getPtrTy());
  B.CreateStore(B.getInt32(5), Negative);
  const std::string Before = body(*F);

  const auto Result =
      Pipeline::optimizeModule(M, Pipeline::OptimizationOptions{});

  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_EQ(body(*M.getFunction("callback")), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(PipelineOptNone,
     InterproceduralPassesRetainCallbackArgumentsAndSelectors) {
  llvm::LLVMContext C;
  llvm::Module M("callback-abi", C);
  auto *Callback = buildRoot(M, "callback", true);
  Callback->setLinkage(llvm::GlobalValue::InternalLinkage);
  auto &Entry = Callback->getEntryBlock();
  auto *Save = llvm::cast<llvm::StoreInst>(&*std::next(Entry.begin()));
  Save->setOperand(0, llvm::ConstantInt::get(llvm::Type::getInt32Ty(C), 7));
  ASSERT_TRUE(Callback->getArg(0)->use_empty());
  llvm::appendToCompilerUsed(M, {Callback});
  auto *Ordinary = buildRoot(M, "ordinary", false);
  llvm::IRBuilder<> B(Ordinary->getEntryBlock().getTerminator());
  auto *Call = B.CreateCall(Callback, {B.getInt32(1)});
  Ordinary->getEntryBlock().getTerminator()->setOperand(0, Call);
  auto *Choice = llvm::BasicBlock::Create(C, "choice", Callback);
  auto *Reject = llvm::BasicBlock::Create(C, "reject", Callback);
  B.SetInsertPoint(Entry.getTerminator());
  auto *Read = B.CreateLoad(B.getInt32Ty(), &Entry.front());
  Read->setVolatile(true);
  auto *Switch = B.CreateSwitch(Read, Reject, 1);
  Switch->addCase(B.getInt32(7), Choice);
  Entry.getTerminator()->eraseFromParent();
  B.SetInsertPoint(Choice);
  B.CreateRet(B.getInt32(7));
  B.SetInsertPoint(Reject);
  B.CreateRet(B.getInt32(0));
  const auto Before = body(*Callback);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  const auto Result = Pipeline::optimizeModule(M, Options);

  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  auto *After = M.getFunction("callback");
  ASSERT_TRUE(After);
  EXPECT_EQ(body(*After), Before);
  unsigned Calls = 0;
  for (const auto &I : llvm::instructions(*M.getFunction("ordinary")))
    if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
        Call && Call->getCalledFunction() == After) {
      ++Calls;
      ASSERT_EQ(Call->arg_size(), 1u);
      const auto *Argument =
          llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(0));
      ASSERT_TRUE(Argument);
      EXPECT_TRUE(Argument->isOne());
    }
  EXPECT_EQ(Calls, 1u);
  EXPECT_FALSE(hasAlloca(*M.getFunction("ordinary")));
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(PipelineOptNone, OrdinaryModulesKeepInterproceduralOptimization) {
  llvm::LLVMContext C;
  llvm::Module M("ordinary-abi", C);
  auto *Callee = buildRoot(M, "callee", false);
  Callee->setLinkage(llvm::GlobalValue::InternalLinkage);
  auto *Caller = buildRoot(M, "caller", false);
  llvm::IRBuilder<> B(Caller->getEntryBlock().getTerminator());
  Caller->getEntryBlock().getTerminator()->setOperand(
      0, B.CreateCall(Callee, {B.getInt32(1)}));
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;

  const auto Result = Pipeline::optimizeModule(M, Options);

  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  Caller = M.getFunction("caller");
  ASSERT_TRUE(Caller);
  ASSERT_EQ(Caller->size(), 1u);
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(Caller->getEntryBlock().getTerminator());
  ASSERT_TRUE(Return);
  const auto *Value =
      llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
  ASSERT_TRUE(Value);
  EXPECT_TRUE(Value->isOne());
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(PipelineOptNone, AFilterAdapterKeepsInterproceduralOptimization) {
  // An SEH filter adapter is optnone so that output finds its exact shape,
  // but only the runtime takes its address: it must not turn off the
  // interprocedural passes for the rest of its module.  Any other optnone
  // definition still does.
  for (const bool Adapter : {true, false}) {
    SCOPED_TRACE(Adapter);
    llvm::LLVMContext C;
    llvm::Module M("adapter-ipo", C);
    auto *Callee = buildRoot(M, "callee", false);
    Callee->setLinkage(llvm::GlobalValue::InternalLinkage);
    auto *Caller = buildRoot(M, "caller", false);
    llvm::IRBuilder<> B(Caller->getEntryBlock().getTerminator());
    Caller->getEntryBlock().getTerminator()->setOperand(
        0, B.CreateCall(Callee, {B.getInt32(1)}));
    auto *Protected = buildRoot(
        M, Adapter ? sehFilterAdapterName(0x140001000, 0) : "protected", true);
    Protected->setLinkage(llvm::GlobalValue::InternalLinkage);
    llvm::appendToCompilerUsed(M, {Protected});
    const std::string Name = Protected->getName().str();
    const auto Before = body(*Protected);
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    Pipeline::OptimizationOptions Options;
    Options.Strength = Pipeline::OptStrength::Deep;

    const auto Result = Pipeline::optimizeModule(M, Options);

    EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
    Protected = M.getFunction(Name);
    ASSERT_TRUE(Protected);
    EXPECT_EQ(body(*Protected), Before);
    Caller = M.getFunction("caller");
    ASSERT_TRUE(Caller);
    const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
        Caller->getEntryBlock().getTerminator());
    ASSERT_TRUE(Return);
    const auto *Value =
        llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
    EXPECT_EQ(Value && Value->isOne(), Adapter);
    EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  }
}

} // namespace
