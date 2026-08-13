//===- SymSimplifyGuardTests.cpp - Obfuscation guard for SymSimplify -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins the contract SymSimplifyPass owes the rest of the optimizer.
///
/// Three things are held here.  The one-way barrier that keeps an
/// obfuscate-then-patch run from undoing itself: the pass measures mixed
/// boolean-arithmetic back to its shortest form -- exactly what the MBA
/// obfuscator injects -- so a stamped function must be left untouched while an
/// unstamped one is still simplified.  The promise that a rewrite never costs
/// the reader more than it saves, on inputs where the shortest measured form is
/// not the shortest instruction sequence.  And the joint fixed point with
/// InstCombine, checked through the real default pipeline rather than through a
/// pass list assembled for the test.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pass/ir/SymSimplifyPass.h"
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

unsigned instructionCount(const llvm::Function &F) {
  unsigned N = 0;
  for (const llvm::BasicBlock &BB : F)
    N += static_cast<unsigned>(BB.size());
  return N;
}

std::string printFunction(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

// `@f(i32 %x, i32 %y)` whose body is the carry-save spelling of `x + y`,
//   (x ^ y) + 2*(x & y),
// the canonical MBA rewriting of an addition.  It is at once what the simplifier
// is meant to recover and what the obfuscator emits, so it doubles as the thing
// that has to survive untouched once the function is stamped.
llvm::Function *buildCarrySaveAdd(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "f", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Xor = B.CreateXor(X, Y);
  llvm::Value *And = B.CreateAnd(X, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  return F;
}

// Without the stamp the pass sees straight through the carry-save form; pinning
// this on its own is what makes the skip test below meaningful rather than
// vacuous (a pass that never fires would also "leave a stamped function alone").
TEST(SymSimplifyGuard, RewritesMixedBooleanArithmeticWhenUnstamped) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A rewrite must not leave the expression it replaced behind.  The pipeline
// runs InstCombine after this pass and would sweep it, but the C ABI, the CLI
// and the plugins call `simplify` on its own, so the entry point has to return
// clean IR by itself -- the carry-save spelling's `xor` and `and` are gone once
// the addition they encoded has been recovered.
TEST(SymSimplifyGuard, SweepsTheExpressionItRewroteAway) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);

  ASSERT_GT(SymSimplifyPass::simplify(*F), 0u);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  unsigned Leftover = 0;
  for (llvm::Instruction &I : llvm::instructions(*F))
    if (I.getOpcode() == llvm::Instruction::Xor ||
        I.getOpcode() == llvm::Instruction::And)
      ++Leftover;
  EXPECT_EQ(Leftover, 0u);
}

// `@g(i32 %x, i32 %y, i32 %z)` spelling
//   (x | y | z) - (x & y & z) - ((x ^ y) & (y ^ z)),
// a three-input linear MBA whose value is `x ^ z`.  The point of it is that the
// answer does not mention `y` at all: a measurement discovers the result cannot
// depend on it, whereas nothing InstCombine can state ever drops an input that
// appears on both sides of a subtraction.  So a run that leaves `%y` unused is
// evidence the measurement fired, not the peephole simplifier.
llvm::Function *buildVanishingVariable(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT =
      llvm::FunctionType::get(I32, {I32, I32, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "g", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Z = F->getArg(2);
  llvm::Value *Or = B.CreateOr(B.CreateOr(X, Y), Z);
  llvm::Value *And = B.CreateAnd(B.CreateAnd(X, Y), Z);
  llvm::Value *Cross = B.CreateAnd(B.CreateXor(X, Y), B.CreateXor(Y, Z));
  B.CreateRet(B.CreateSub(B.CreateSub(Or, And), Cross));
  return F;
}

// The rewrite is judged as instructions, not as the measured tree, so it can
// never hand a reader more than it was given.  Pinning it on the carry-save add
// keeps the guarantee observable: the recovered `add` is strictly fewer
// instructions than the xor/and/mul/add it replaced, and a plain arithmetic
// function -- nothing to measure -- is returned untouched rather than churned.
TEST(SymSimplifyGuard, NeverGrowsTheInstructionCount) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);

  llvm::Function *Mba = buildCarrySaveAdd(M);
  unsigned Before = instructionCount(*Mba);
  ASSERT_GT(SymSimplifyPass::simplify(*Mba), 0u);
  EXPECT_LT(instructionCount(*Mba), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *Plain =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "h", &M);
  auto *BB = llvm::BasicBlock::Create(C, "entry", Plain);
  llvm::IRBuilder<> B(BB);
  llvm::Value *Prod = B.CreateMul(Plain->getArg(0), Plain->getArg(1));
  B.CreateRet(B.CreateAdd(Prod, llvm::ConstantInt::get(I32, 3)));
  unsigned PlainBefore = instructionCount(*Plain);
  EXPECT_EQ(SymSimplifyPass::simplify(*Plain), 0u);
  EXPECT_EQ(instructionCount(*Plain), PlainBefore);
}

// The joint fixed point, checked through the real default pipeline rather than a
// pass list assembled for the test.  optimizeModule wires SemanticFixedPointPass
// exactly as production decompilation does; after it, the three-input MBA is
// `x ^ z` and `%y` -- the input the value never truly depended on -- has no uses
// left.  A single canonicalize/measure pairing would leave measurable residue;
// reaching this state is what the alternation is for.
TEST(SymSimplifyGuard, ReachesTheJointFixedPointThroughTheRealPipeline) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildVanishingVariable(M);

  Pipeline::optimizeModule(M, /*Conservative=*/false);

  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  EXPECT_TRUE(F->getArg(1)->use_empty())
      << "the vanishing input is still read:\n"
      << printFunction(*F);
}

// The conservative path is the patch pipeline's, and it must not measure MBA
// away: an obfuscate-then-patch run compiles through it, and simplifying there
// would strip the payload the patch just placed.  The carry-save spelling comes
// through with its xor and and intact.
TEST(SymSimplifyGuard, ConservativePipelineLeavesMixedBooleanArithmetic) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);

  Pipeline::optimizeModule(M, /*Conservative=*/true);

  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  unsigned Bitwise = 0;
  for (llvm::Instruction &I : llvm::instructions(*F))
    if (I.getOpcode() == llvm::Instruction::Xor ||
        I.getOpcode() == llvm::Instruction::And)
      ++Bitwise;
  EXPECT_GT(Bitwise, 0u) << printFunction(*F);
}

// The identical expression, once stamped, is left exactly as written.
TEST(SymSimplifyGuard, LeavesAStampedFunctionAlone) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);
  F->addFnAttr(kObfuscatedFnAttr);

  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u);
}

// End to end: the obfuscator stamps what it rewrites, so a later simplify over
// that module is a no-op -- the MBA the patch pipeline just injected is not
// measured back off.
TEST(SymSimplifyGuard, ObfuscationStampsSoSimplifyLeavesItsPayload) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);

  Pipeline::ObfuscationConfig Cfg;
  Cfg.MBA = true;
  Pipeline::ObfuscationCounts Counts = Pipeline::runObfuscationPasses(M, Cfg);

  // Precondition: the obfuscator actually injected something worth protecting.
  ASSERT_GT(Counts.total(), 0u);
  EXPECT_TRUE(F->hasFnAttribute(kObfuscatedFnAttr));
  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u);
}

//===----------------------------------------------------------------------===//
// Branches that were never a choice
//===----------------------------------------------------------------------===//

// `@name(i32 %x, i32 %y)` branching on a caller-supplied predicate.  The side
// not taken computes something recognisable, so a test can say whether the
// block survived rather than only how many blocks are left.
llvm::Function *buildGuardedFunction(
    llvm::Module &M, llvm::StringRef Name,
    llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *,
                                     llvm::Value *)>
        Condition) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *Taken = llvm::BasicBlock::Create(C, "taken", F);
  auto *Guarded = llvm::BasicBlock::Create(C, "guarded", F);

  llvm::IRBuilder<> B(Entry);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  B.CreateCondBr(Condition(B, X, Y), Taken, Guarded);

  B.SetInsertPoint(Taken);
  B.CreateRet(B.CreateAdd(X, Y));

  B.SetInsertPoint(Guarded);
  B.CreateRet(B.CreateMul(X, llvm::ConstantInt::get(I32, 0x1234)));
  return F;
}

// The condition of the one conditional branch, or null once there is none.
const llvm::Value *branchCondition(const llvm::Function &F) {
  for (const llvm::BasicBlock &BB : F)
    if (const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator()))
      return Br->getCondition();
  return nullptr;
}

bool mentionsGuardedConstant(const llvm::Function &F) {
  for (const llvm::Instruction &I : llvm::instructions(F))
    for (const llvm::Value *Op : I.operand_values())
      if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Op))
        if (CI->equalsInt(0x1234))
          return true;
  return false;
}

// `(x | ~x) != 0` holds for every x.  That is what an opaque predicate is: a
// branch with one reachable side, written so nothing reading the shape of the
// condition can tell.
TEST(SymSimplifyGuard, FoldsAPredicateThatCannotVary) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildGuardedFunction(
      M, "opaque", [](llvm::IRBuilder<> &B, llvm::Value *X, llvm::Value *) {
        llvm::Value *Or = B.CreateOr(X, B.CreateNot(X));
        return B.CreateICmpNE(Or, llvm::ConstantInt::get(X->getType(), 0));
      });

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u);
  EXPECT_EQ(branchCondition(*F), llvm::ConstantInt::getTrue(C))
      << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// The harder shape, and the one a rule set cannot reach: both sides compute the
// same value, one of them spelled as mixed boolean-arithmetic.  Measuring the
// operands makes them the same expression, and a comparison of an expression
// with itself folds by construction rather than by a rule about comparisons.
TEST(SymSimplifyGuard, FoldsAPredicateComparingTwoSpellingsOfOneValue) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildGuardedFunction(
      M, "opaque_mba",
      [](llvm::IRBuilder<> &B, llvm::Value *X, llvm::Value *Y) {
        llvm::Value *Carry =
            B.CreateMul(B.CreateAnd(X, Y),
                        llvm::ConstantInt::get(X->getType(), 2));
        llvm::Value *Mba = B.CreateAdd(B.CreateXor(X, Y), Carry);
        return B.CreateICmpEQ(Mba, B.CreateAdd(X, Y));
      });

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u);
  EXPECT_EQ(branchCondition(*F), llvm::ConstantInt::getTrue(C))
      << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A branch that is a real choice has to stay one.  Folding this would not be an
// optimisation, it would be wrong.
TEST(SymSimplifyGuard, LeavesABranchThatIsAGenuineChoice) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildGuardedFunction(
      M, "genuine",
      [](llvm::IRBuilder<> &B, llvm::Value *X, llvm::Value *Y) {
        return B.CreateICmpULT(X, Y);
      });

  SymSimplifyPass::simplify(*F);
  const llvm::Value *Cond = branchCondition(*F);
  ASSERT_NE(Cond, nullptr) << printFunction(*F);
  EXPECT_FALSE(llvm::isa<llvm::Constant>(Cond)) << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// End to end through the real pipeline: this pass makes the condition a
// constant and stops there, and the SimplifyCFG already in that pipeline is
// what removes the side the constant made unreachable.  Deleting blocks is not
// this pass's job, so the two halves are only worth checking together.
TEST(SymSimplifyGuard, TheRealPipelineDropsWhatAnOpaquePredicateGuarded) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildGuardedFunction(
      M, "opaque", [](llvm::IRBuilder<> &B, llvm::Value *X, llvm::Value *) {
        llvm::Value *Or = B.CreateOr(X, B.CreateNot(X));
        return B.CreateICmpNE(Or, llvm::ConstantInt::get(X->getType(), 0));
      });
  ASSERT_TRUE(mentionsGuardedConstant(*F));

  Pipeline::optimizeModule(M);

  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  EXPECT_EQ(branchCondition(*F), nullptr) << printFunction(*F);
  EXPECT_FALSE(mentionsGuardedConstant(*F)) << printFunction(*F);
}

} // namespace
