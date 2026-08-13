//===- ControlFlowRecoveryTests.cpp - Undoing dispatcher flattening ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the recovery against the flattening it undoes, and against the code
/// it must not touch.
///
/// The closed loop is the test worth having: NeverD's own flattening pass turns
/// a graph into a dispatcher loop, and the recovery has to give the graph back.
/// Running both means the input is a real flattening rather than a hand-written
/// impression of one, and the answer can be compared against the same function
/// that was never flattened at all -- which is the only comparison that says
/// the control flow came back rather than merely that something changed.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pass/ir/ControlFlowFlatteningPass.h"
#include "neverd/pass/ir/ControlFlowRecoveryPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {

std::string printFunction(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

template <typename InstT> unsigned countOf(const llvm::Function &F) {
  unsigned N = 0;
  for (const llvm::Instruction &I : llvm::instructions(F))
    if (llvm::isa<InstT>(&I))
      ++N;
  return N;
}

/// The operations \p F performs, block by block, without their names or their
/// flags.
///
/// Comparing the printed text instead would fail on two things neither pass is
/// answerable for: a value keeps the name the spill gave it, and the never
/// flattened form carries a `nuw` that the flattened one cannot. That flag is
/// real information the flattening destroyed -- the branch it came from implied
/// a range, and a select does not -- so demanding it back would be demanding
/// something no recovery can give.
std::string opcodeSketch(const llvm::Function &F) {
  std::string Sketch;
  for (const llvm::BasicBlock &BB : F) {
    if (!Sketch.empty())
      Sketch += " | ";
    for (const llvm::Instruction &I : BB) {
      Sketch += I.getOpcodeName();
      Sketch += ' ';
    }
  }
  return Sketch;
}

// `@f(i32 %x, i32 %y)` returning `x + y` or `x - y` depending on `x < y`: the
// smallest function with control flow worth hiding, and one whose recovered
// shape is recognisable at a glance.
llvm::Function *buildBranchingFunction(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "f", &M);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *Less = llvm::BasicBlock::Create(C, "less", F);
  auto *NotLess = llvm::BasicBlock::Create(C, "notless", F);
  auto *Join = llvm::BasicBlock::Create(C, "join", F);

  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);

  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpULT(X, Y), Less, NotLess);

  B.SetInsertPoint(Less);
  llvm::Value *Sum = B.CreateAdd(X, Y);
  B.CreateBr(Join);

  B.SetInsertPoint(NotLess);
  llvm::Value *Difference = B.CreateSub(X, Y);
  B.CreateBr(Join);

  B.SetInsertPoint(Join);
  auto *Phi = B.CreatePHI(I32, 2);
  Phi->addIncoming(Sum, Less);
  Phi->addIncoming(Difference, NotLess);
  B.CreateRet(Phi);
  return F;
}

TEST(ControlFlowRecovery, RecoversTheGraphItsOwnFlatteningHid) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildBranchingFunction(M);

  ASSERT_GT(ControlFlowFlatteningPass::inject(M), 0u);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  // Precondition: there really is a dispatcher to dismantle, and the branch the
  // function was written with really is gone.
  ASSERT_EQ(countOf<llvm::SwitchInst>(*F), 1u) << printFunction(*F);
  ASSERT_EQ(countOf<llvm::CondBrInst>(*F), 0u) << printFunction(*F);

  EXPECT_GT(ControlFlowRecoveryPass::recover(*F), 0u);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  // Every jump into the dispatcher now goes where the dispatcher would have
  // sent it, so nothing reaches the switch any more.
  for (llvm::Instruction &I : llvm::instructions(*F))
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&I))
      EXPECT_TRUE(llvm::pred_empty(Switch->getParent()))
          << printFunction(*F);
  EXPECT_GT(countOf<llvm::CondBrInst>(*F), 0u) << printFunction(*F);
}

TEST(ControlFlowRecovery, FlattenAndRecoverReachesWhatWasNeverFlattened) {
  // The comparison that matters.  One module is flattened and put back, the
  // other is left alone; both then go through the real optimizer. If recovery
  // works, the pipeline cannot tell them apart at the end -- the dispatcher,
  // the state slot and the spill slots the flattening needed are all gone, and
  // what is left is the branch the function was written with.
  llvm::LLVMContext C;
  llvm::Module Flattened("flattened", C);
  llvm::Module Untouched("untouched", C);
  llvm::Function *WasFlattened = buildBranchingFunction(Flattened);
  llvm::Function *Never = buildBranchingFunction(Untouched);

  ASSERT_GT(ControlFlowFlatteningPass::inject(Flattened), 0u);

  Pipeline::optimizeModule(Flattened);
  Pipeline::optimizeModule(Untouched);
  ASSERT_FALSE(llvm::verifyModule(Flattened, &llvm::errs()));

  EXPECT_EQ(countOf<llvm::SwitchInst>(*WasFlattened), 0u)
      << printFunction(*WasFlattened);
  EXPECT_EQ(countOf<llvm::AllocaInst>(*WasFlattened), 0u)
      << printFunction(*WasFlattened);
  EXPECT_EQ(WasFlattened->size(), Never->size())
      << printFunction(*WasFlattened) << "\nvs\n" << printFunction(*Never);
  EXPECT_EQ(opcodeSketch(*WasFlattened), opcodeSketch(*Never))
      << printFunction(*WasFlattened) << "\nvs\n" << printFunction(*Never);
}

// `@g(i32 %x)` switching on a value the caller supplies rather than on one its
// predecessor wrote: an ordinary switch statement, which has to survive.
TEST(ControlFlowRecovery, LeavesASwitchNoPredecessorDecides) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "g", &M);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *One = llvm::BasicBlock::Create(C, "one", F);
  auto *Other = llvm::BasicBlock::Create(C, "other", F);

  llvm::IRBuilder<> B(Entry);
  auto *Slot = B.CreateAlloca(I32, nullptr, "slot");
  // Stored from the argument, so which case runs is not known here.
  B.CreateStore(F->getArg(0), Slot);
  auto *Value = B.CreateLoad(I32, Slot);
  auto *Switch = B.CreateSwitch(Value, Other, 1);
  Switch->addCase(llvm::ConstantInt::get(I32, 1), One);

  B.SetInsertPoint(One);
  B.CreateRet(llvm::ConstantInt::get(I32, 11));
  B.SetInsertPoint(Other);
  B.CreateRet(llvm::ConstantInt::get(I32, 22));

  EXPECT_EQ(ControlFlowRecoveryPass::recover(*F), 0u) << printFunction(*F);
  EXPECT_EQ(countOf<llvm::SwitchInst>(*F), 1u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A slot whose address reaches a call could be written by that call, so the
// value a predecessor stored is not the value the switch will read.
TEST(ControlFlowRecovery, LeavesASlotWhoseAddressEscapes) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(C), {Ptr}, false),
      llvm::Function::ExternalLinkage, "meddle", &M);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {}, false), llvm::Function::ExternalLinkage,
      "h", &M);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *Dispatch = llvm::BasicBlock::Create(C, "dispatch", F);
  auto *One = llvm::BasicBlock::Create(C, "one", F);
  auto *Other = llvm::BasicBlock::Create(C, "other", F);

  llvm::IRBuilder<> B(Entry);
  auto *Slot = B.CreateAlloca(I32, nullptr, "slot");
  B.CreateStore(llvm::ConstantInt::get(I32, 1), Slot);
  B.CreateCall(Callee, {Slot});
  B.CreateBr(Dispatch);

  B.SetInsertPoint(Dispatch);
  auto *Value = B.CreateLoad(I32, Slot);
  auto *Switch = B.CreateSwitch(Value, Other, 1);
  Switch->addCase(llvm::ConstantInt::get(I32, 1), One);

  B.SetInsertPoint(One);
  B.CreateRet(llvm::ConstantInt::get(I32, 11));
  B.SetInsertPoint(Other);
  B.CreateRet(llvm::ConstantInt::get(I32, 22));

  EXPECT_EQ(ControlFlowRecoveryPass::recover(*F), 0u) << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

} // namespace
