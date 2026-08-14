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

#include "SemanticConvergence.h"
#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>
#include <string>
#include <vector>

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

std::string printModule(const llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  return Text;
}

// `@f(i32 %x, i32 %y)` whose body is the carry-save spelling of `x + y`,
//   (x ^ y) + 2*(x & y),
// the canonical MBA rewriting of an addition.  It is at once what the
// simplifier is meant to recover and what the obfuscator emits, so it doubles
// as the thing that has to survive untouched once the function is stamped.
llvm::Function *buildCarrySaveAdd(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "f", &M);

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

/// The smallest expression for which semantic measurement can improve on the
/// canonical builders: `~x + 1 == -x`.
llvm::Function *buildComplementPlusOne(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "complement_plus_one", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  B.CreateRet(B.CreateAdd(B.CreateNot(X), llvm::ConstantInt::get(I32, 1)));
  return F;
}

/// A carry-save spelling whose left input is supplied by \p MakeLeft.  It gives
/// the poison-domain tests below a shape the solver would otherwise shorten
/// from `(left ^ y) + 2*(left & y)` to `left + y`.
llvm::Function *buildCarrySaveWithLeft(
    llvm::Module &M, llvm::StringRef Name,
    llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *)>
        MakeLeft) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *Left = MakeLeft(B, F->getArg(0));
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Xor = B.CreateXor(Left, Y);
  llvm::Value *And = B.CreateAnd(Left, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  return F;
}

/// The repeated counterpart to buildCarrySaveWithLeft.  Each equivalent left
/// instruction has one use, so the eligibility matrix exercises translation
/// of that instruction instead of the opaque-leaf path.
llvm::Function *buildCarrySaveWithRepeatedLeft(
    llvm::Module &M, llvm::StringRef Name,
    llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *)>
        MakeLeft) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  llvm::Value *LeftForXor = MakeLeft(B, X);
  llvm::Value *LeftForAnd = MakeLeft(B, X);
  llvm::Value *Xor = B.CreateXor(LeftForXor, Y);
  llvm::Value *And = B.CreateAnd(LeftForAnd, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  return F;
}

/// Two structurally equal, separately built values whose xor is zero.  This is
/// the direct eligibility probe for safe operators: translating the two
/// one-use definitions makes them the same symbolic node, while leaving either
/// definition opaque gives two unrelated inputs and cannot prove the result.
llvm::Function *buildCancellationWithRepeatedLeft(
    llvm::Module &M, llvm::StringRef Name,
    llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *)>
        MakeLeft) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  B.CreateRet(B.CreateXor(MakeLeft(B, X), MakeLeft(B, X)));
  return F;
}

/// A carry-save spelling whose two left inputs are separate instances of the
/// same binary operation.  Giving each instance one use makes the translator
/// inspect the operation itself instead of treating a shared instruction as an
/// opaque leaf.
llvm::Function *
buildCarrySaveWithRepeatedBinaryLeft(llvm::Module &M, llvm::StringRef Name,
                                     llvm::Instruction::BinaryOps Opcode) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Divisor = F->getArg(1);
  llvm::Value *Y = F->getArg(2);
  llvm::Value *LeftForXor = B.CreateBinOp(Opcode, X, Divisor);
  llvm::Value *LeftForAnd = B.CreateBinOp(Opcode, X, Divisor);
  llvm::Value *Xor = B.CreateXor(LeftForXor, Y);
  llvm::Value *And = B.CreateAnd(LeftForAnd, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  return F;
}

/// Put one unsafe value behind a shared PHI.  The PHI itself remains opaque to
/// the symbolic engine, so rejecting this root demonstrates that the
/// definedness walk crosses control-flow leaves without trying to translate
/// them.
llvm::Function *buildCarrySaveWithPhiLeft(
    llvm::Module &M, llvm::StringRef Name,
    llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *)>
        MakeUnsafe) {
  llvm::LLVMContext &C = M.getContext();
  auto *I1 = llvm::Type::getInt1Ty(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I1, I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *UnsafeBB = llvm::BasicBlock::Create(C, "unsafe", F);
  auto *SafeBB = llvm::BasicBlock::Create(C, "safe", F);
  auto *MergeBB = llvm::BasicBlock::Create(C, "merge", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(F->getArg(0), UnsafeBB, SafeBB);

  B.SetInsertPoint(UnsafeBB);
  llvm::Value *Unsafe = MakeUnsafe(B, F->getArg(1));
  B.CreateBr(MergeBB);
  B.SetInsertPoint(SafeBB);
  B.CreateBr(MergeBB);

  B.SetInsertPoint(MergeBB);
  auto *Left = B.CreatePHI(I32, 2);
  Left->addIncoming(Unsafe, UnsafeBB);
  Left->addIncoming(F->getArg(1), SafeBB);
  llvm::Value *Y = F->getArg(2);
  llvm::Value *Xor = B.CreateXor(Left, Y);
  llvm::Value *And = B.CreateAnd(Left, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  return F;
}

// Without the stamp the pass sees straight through the carry-save form; pinning
// this on its own is what makes the skip test below meaningful rather than
// vacuous (a pass that never fires would also "leave a stamped function
// alone").
TEST(SymSimplifyGuard, RewritesMixedBooleanArithmeticWhenUnstamped) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveAdd(M);

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, RewritesTheSmallestNontrivialIdentity) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildComplementPlusOne(M);
  unsigned Before = instructionCount(*F);

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u);
  EXPECT_LT(instructionCount(*F), Before);
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
  auto *FT = llvm::FunctionType::get(I32, {I32, I32, I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "g", &M);

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

// Undef and poison are not ordinary symbolic inputs.  Undef may choose a
// different value at each use, while poison lies outside the engine's total
// bitvector domain.  Turning either into one stable placeholder would make the
// carry-save identity look applicable when LLVM does not grant that premise.
TEST(SymSimplifyGuard, LeavesExplicitUndefAndPoisonOpaque) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);

  llvm::Function *WithUndef = buildCarrySaveWithLeft(
      M, "with_undef", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateXor(X, llvm::UndefValue::get(X->getType()));
      });
  llvm::Function *WithPoison = buildCarrySaveWithLeft(
      M, "with_poison", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateXor(X, llvm::PoisonValue::get(X->getType()));
      });
  llvm::Function *WithUndefSelect = buildCarrySaveWithLeft(
      M, "with_undef_select", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        llvm::Value *Cond =
            B.CreateICmpEQ(X, llvm::ConstantInt::get(X->getType(), 0));
        return B.CreateSelect(Cond, llvm::UndefValue::get(X->getType()), X);
      });

  EXPECT_EQ(SymSimplifyPass::simplify(*WithUndef), 0u);
  EXPECT_EQ(SymSimplifyPass::simplify(*WithPoison), 0u);
  EXPECT_EQ(SymSimplifyPass::simplify(*WithUndefSelect), 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A total bitvector shift and an LLVM shift have different domains: the latter
// produces poison when the amount is out of range.  Poison-generating flags
// similarly narrow the domain of otherwise wrapping arithmetic.  Until those
// preconditions are represented explicitly, the pass must not reason through
// either operation or cancel it as an opaque placeholder.
TEST(SymSimplifyGuard, LeavesPoisonGeneratingOperationsOpaque) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);

  llvm::Function *WithOvershift = buildCarrySaveWithLeft(
      M, "with_overshift", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(
            X, llvm::ConstantInt::get(X->getType(),
                                      X->getType()->getIntegerBitWidth()));
      });
  llvm::Function *WithNSW = buildCarrySaveWithLeft(
      M, "with_nsw", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNSWAdd(X, llvm::ConstantInt::get(X->getType(), 1));
      });

  EXPECT_EQ(SymSimplifyPass::simplify(*WithOvershift), 0u);
  EXPECT_EQ(SymSimplifyPass::simplify(*WithNSW), 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A same-sign comparison is poison when its operands do not have the same
// sign.  Comparisons are opaque to the ordinary expression rewrite, so this
// also pins the hidden-leaf path: standing one symbolic input in front of the
// select must not authorize reasoning across a boundary whose value is outside
// the engine's total algebra on some inputs.
TEST(SymSimplifyGuard, HiddenSameSignComparisonRejectsTheCandidate) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildCarrySaveWithLeft(
      M, "hidden_samesign", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *Cmp = llvm::cast<llvm::ICmpInst>(
            B.CreateICmpULT(X, llvm::ConstantInt::get(X->getType(), 0)));
        Cmp->setSameSign();
        llvm::Value *Wide = B.CreateZExt(Cmp, X->getType());
        llvm::Value *ChooseWide =
            B.CreateICmpSLT(X, llvm::ConstantInt::get(X->getType(), 0));
        return B.CreateSelect(ChooseWide, Wide, X);
      });

  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// The symbolic algebra totalises division and remainder, while LLVM gives all
// four instructions a smaller domain.  Until that precondition is represented
// in the proof, none may authorize a production rewrite -- even when two
// identical operations make the shorter total-algebra form obvious.
TEST(SymSimplifyGuard,
     EligibilityRejectsDivisionAndRemainderUntilDefinednessIsModeled) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  struct {
    llvm::Instruction::BinaryOps Opcode;
    const char *Name;
  } Cases[] = {
      {llvm::Instruction::UDiv, "udiv_domain"},
      {llvm::Instruction::SDiv, "sdiv_domain"},
      {llvm::Instruction::URem, "urem_domain"},
      {llvm::Instruction::SRem, "srem_domain"},
  };

  for (const auto &Case : Cases) {
    llvm::Function *F =
        buildCarrySaveWithRepeatedBinaryLeft(M, Case.Name, Case.Opcode);
    const std::string Before = printFunction(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
    EXPECT_EQ(printFunction(*F), Before);
  }
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// This is the production value-and-definedness table, expressed through the
// public pass rather than a private classifier.  Accepted rows either shorten
// or make a repeated safe value provably constant.  Rejected rows are compared
// byte for byte so standing an unsafe instruction in as an opaque value cannot
// accidentally count as success.
TEST(SymSimplifyGuard, EligibilityMatchesTheValueAndDefinednessMatrix) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto C32 = [](llvm::Value *X, uint64_t V) {
    return llvm::ConstantInt::get(X->getType(), V);
  };
  auto ExpectRewritten = [&](llvm::Function *F) {
    const std::string Before = printFunction(*F);
    EXPECT_GT(SymSimplifyPass::simplify(*F), 0u) << F->getName().str() << ":\n"
                                                 << printFunction(*F);
    EXPECT_NE(printFunction(*F), Before) << F->getName().str();
  };
  auto ExpectUnchanged = [&](llvm::Function *F) {
    const std::string Before = printFunction(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << F->getName().str() << ":\n"
                                                 << printFunction(*F);
    EXPECT_EQ(printFunction(*F), Before) << F->getName().str();
  };
  auto ExpectConstantZero = [&](llvm::Function *F) {
    auto *Ret =
        llvm::cast<llvm::ReturnInst>(F->getEntryBlock().getTerminator());
    auto *Root = llvm::cast<llvm::Instruction>(Ret->getReturnValue());
    std::optional<llvm::APInt> Value = SymSimplifyPass::constantValueOf(Root);
    ASSERT_TRUE(Value.has_value()) << F->getName().str();
    EXPECT_TRUE(Value->isZero()) << F->getName().str();
  };

  ExpectRewritten(buildCarrySaveAdd(M));
  ExpectConstantZero(buildCancellationWithRepeatedLeft(
      M, "plain_sub", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateSub(X, C32(X, 3));
      }));
  ExpectConstantZero(buildCancellationWithRepeatedLeft(
      M, "plain_or", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateOr(X, C32(X, 3));
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_shl", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(X, C32(X, 3));
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_lshr", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateLShr(X, C32(X, 3));
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_ashr", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateAShr(X, C32(X, 3));
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_trunc", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I64 = llvm::Type::getInt64Ty(B.getContext());
        return B.CreateTrunc(B.CreateZExt(X, I64), X->getType());
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_zext", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I16 = llvm::Type::getInt16Ty(B.getContext());
        return B.CreateZExt(B.CreateTrunc(X, I16), X->getType());
      }));
  ExpectRewritten(buildCarrySaveWithRepeatedLeft(
      M, "plain_sext", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I16 = llvm::Type::getInt16Ty(B.getContext());
        return B.CreateSExt(B.CreateTrunc(X, I16), X->getType());
      }));

  auto *I1 = llvm::Type::getInt1Ty(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *CmpTy = llvm::FunctionType::get(I1, {I32}, /*isVarArg=*/false);
  auto *CmpF = llvm::Function::Create(CmpTy, llvm::Function::ExternalLinkage,
                                      "plain_icmp", &M);
  auto *CmpBB = llvm::BasicBlock::Create(C, "entry", CmpF);
  llvm::IRBuilder<> CmpBuilder(CmpBB);
  auto *PlainCmp = llvm::cast<llvm::ICmpInst>(
      CmpBuilder.CreateICmpEQ(CmpF->getArg(0), CmpF->getArg(0)));
  CmpBuilder.CreateRet(PlainCmp);
  std::optional<llvm::APInt> CmpValue =
      SymSimplifyPass::constantValueOf(PlainCmp);
  ASSERT_TRUE(CmpValue.has_value());
  EXPECT_TRUE(CmpValue->getBoolValue());

  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "add_nsw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNSWAdd(X, C32(X, 1));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "add_nuw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNUWAdd(X, C32(X, 1));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "sub_nsw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNSWSub(X, C32(X, 1));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "sub_nuw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNUWSub(X, C32(X, 1));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "mul_nsw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNSWMul(X, C32(X, 3));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "mul_nuw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateNUWMul(X, C32(X, 3));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "shl_nsw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(X, C32(X, 3), "", /*HasNUW=*/false,
                           /*HasNSW=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "shl_nuw", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(X, C32(X, 3), "", /*HasNUW=*/true,
                           /*HasNSW=*/false);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "lshr_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateLShr(X, C32(X, 3), "", /*isExact=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "ashr_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateAShr(X, C32(X, 3), "", /*isExact=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "or_disjoint", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateDisjointOr(X, C32(X, 1));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "trunc_nsw", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I64 = llvm::Type::getInt64Ty(B.getContext());
        llvm::Value *Wide = B.CreateZExt(X, I64);
        return B.CreateTrunc(Wide, X->getType(), "", /*IsNUW=*/false,
                             /*IsNSW=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "trunc_nuw", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I64 = llvm::Type::getInt64Ty(B.getContext());
        llvm::Value *Wide = B.CreateSExt(X, I64);
        return B.CreateTrunc(Wide, X->getType(), "", /*IsNUW=*/true,
                             /*IsNSW=*/false);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "zext_nneg", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I16 = llvm::Type::getInt16Ty(B.getContext());
        llvm::Value *Narrow = B.CreateTrunc(X, I16);
        return B.CreateZExt(Narrow, X->getType(), "", /*IsNonNeg=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "icmp_samesign", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *Cmp = llvm::cast<llvm::ICmpInst>(B.CreateICmpSLT(X, C32(X, 0)));
        Cmp->setSameSign();
        return B.CreateZExt(Cmp, X->getType());
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "udiv_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateUDiv(X, C32(X, 3), "", /*isExact=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "sdiv_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateSDiv(X, C32(X, 3), "", /*isExact=*/true);
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedBinaryLeft(M, "shl_variable",
                                                       llvm::Instruction::Shl));
  ExpectUnchanged(buildCarrySaveWithRepeatedBinaryLeft(
      M, "lshr_variable", llvm::Instruction::LShr));
  ExpectUnchanged(buildCarrySaveWithRepeatedBinaryLeft(
      M, "ashr_variable", llvm::Instruction::AShr));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "shl_overshift", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(X, C32(X, 32));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "lshr_overshift", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateLShr(X, C32(X, 32));
      }));
  ExpectUnchanged(buildCarrySaveWithRepeatedLeft(
      M, "ashr_overshift", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateAShr(X, C32(X, 32));
      }));
  ExpectUnchanged(buildCarrySaveWithLeft(
      M, "explicit_undef", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateXor(X, llvm::UndefValue::get(X->getType()));
      }));
  ExpectUnchanged(buildCarrySaveWithLeft(
      M, "explicit_poison", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateXor(X, llvm::PoisonValue::get(X->getType()));
      }));
  ExpectUnchanged(buildCarrySaveWithLeft(
      M, "explicit_freeze",
      [](llvm::IRBuilder<> &B, llvm::Value *X) { return B.CreateFreeze(X); }));

  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// Opaque boundaries do not end the definedness audit.  Every current
// poison-generating flag is exercised on a shared instruction whose shorter
// carry-save form would retain that instruction, so leaf preservation alone
// cannot make these assertions pass.  Select and PHI cases pin traversal
// through both data and control-flow-shaped boundaries.
TEST(SymSimplifyGuard,
     HiddenDefinednessBelowOpaqueBoundariesRejectsCandidates) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto C32 = [](llvm::Value *X, uint64_t V) {
    return llvm::ConstantInt::get(X->getType(), V);
  };
  auto ExpectSharedUnchanged =
      [&](llvm::StringRef Name,
          llvm::function_ref<llvm::Value *(llvm::IRBuilder<> &, llvm::Value *)>
              MakeLeft) {
        llvm::Function *F = buildCarrySaveWithLeft(M, Name, MakeLeft);
        const std::string Before = printFunction(*F);
        EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u)
            << F->getName().str() << ":\n"
            << printFunction(*F);
        EXPECT_EQ(printFunction(*F), Before) << F->getName().str();
      };

  ExpectSharedUnchanged("hidden_add_nsw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNSWAdd(X, C32(X, 1));
                        });
  ExpectSharedUnchanged("hidden_add_nuw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNUWAdd(X, C32(X, 1));
                        });
  ExpectSharedUnchanged("hidden_sub_nsw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNSWSub(X, C32(X, 1));
                        });
  ExpectSharedUnchanged("hidden_sub_nuw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNUWSub(X, C32(X, 1));
                        });
  ExpectSharedUnchanged("hidden_mul_nsw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNSWMul(X, C32(X, 3));
                        });
  ExpectSharedUnchanged("hidden_mul_nuw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateNUWMul(X, C32(X, 3));
                        });
  ExpectSharedUnchanged("hidden_shl_nsw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateShl(X, C32(X, 3), "", /*HasNUW=*/false,
                                             /*HasNSW=*/true);
                        });
  ExpectSharedUnchanged("hidden_shl_nuw",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateShl(X, C32(X, 3), "", /*HasNUW=*/true,
                                             /*HasNSW=*/false);
                        });
  ExpectSharedUnchanged(
      "hidden_lshr_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateLShr(X, C32(X, 3), "", /*isExact=*/true);
      });
  ExpectSharedUnchanged(
      "hidden_ashr_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateAShr(X, C32(X, 3), "", /*isExact=*/true);
      });
  ExpectSharedUnchanged("hidden_or_disjoint",
                        [&](llvm::IRBuilder<> &B, llvm::Value *X) {
                          return B.CreateDisjointOr(X, C32(X, 1));
                        });
  ExpectSharedUnchanged(
      "hidden_trunc_nsw", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I64 = llvm::Type::getInt64Ty(B.getContext());
        return B.CreateTrunc(B.CreateZExt(X, I64), X->getType(), "",
                             /*IsNUW=*/false, /*IsNSW=*/true);
      });
  ExpectSharedUnchanged(
      "hidden_trunc_nuw", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I64 = llvm::Type::getInt64Ty(B.getContext());
        return B.CreateTrunc(B.CreateSExt(X, I64), X->getType(), "",
                             /*IsNUW=*/true, /*IsNSW=*/false);
      });
  ExpectSharedUnchanged(
      "hidden_zext_nneg", [](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *I16 = llvm::Type::getInt16Ty(B.getContext());
        return B.CreateZExt(B.CreateTrunc(X, I16), X->getType(), "",
                            /*IsNonNeg=*/true);
      });
  ExpectSharedUnchanged(
      "hidden_icmp_samesign", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        auto *Cmp = llvm::cast<llvm::ICmpInst>(B.CreateICmpSLT(X, C32(X, 0)));
        Cmp->setSameSign();
        return B.CreateZExt(Cmp, X->getType());
      });
  ExpectSharedUnchanged(
      "hidden_udiv_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateUDiv(X, C32(X, 3), "", /*isExact=*/true);
      });
  ExpectSharedUnchanged(
      "hidden_sdiv_exact", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateSDiv(X, C32(X, 3), "", /*isExact=*/true);
      });

  auto ExpectBoundaryUnchanged = [&](llvm::Function *F) {
    const std::string Before = printFunction(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << F->getName().str() << ":\n"
                                                 << printFunction(*F);
    EXPECT_EQ(printFunction(*F), Before) << F->getName().str();
  };
  ExpectBoundaryUnchanged(buildCarrySaveWithLeft(
      M, "hidden_select_divzero", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        llvm::Value *Unsafe = B.CreateUDiv(X, C32(X, 0));
        llvm::Value *Cond = B.CreateICmpEQ(X, C32(X, 0));
        return B.CreateSelect(Cond, Unsafe, X);
      }));
  ExpectBoundaryUnchanged(buildCarrySaveWithLeft(
      M, "hidden_select_freeze", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        llvm::Value *Frozen = B.CreateFreeze(X);
        llvm::Value *Cond = B.CreateICmpEQ(X, C32(X, 0));
        return B.CreateSelect(Cond, Frozen, X);
      }));
  ExpectBoundaryUnchanged(buildCarrySaveWithPhiLeft(
      M, "hidden_phi_overshift", [&](llvm::IRBuilder<> &B, llvm::Value *X) {
        return B.CreateShl(X, C32(X, 32));
      }));
  ExpectBoundaryUnchanged(buildCarrySaveWithPhiLeft(
      M, "hidden_phi_undef", [](llvm::IRBuilder<> &, llvm::Value *X) {
        return llvm::UndefValue::get(X->getType());
      }));
  ExpectBoundaryUnchanged(buildCarrySaveWithPhiLeft(
      M, "hidden_phi_poison", [](llvm::IRBuilder<> &, llvm::Value *X) {
        return llvm::PoisonValue::get(X->getType());
      }));

  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// The measured identity below does not depend on its middle input.  Replacing
// that input with an opaque load must not authorize erasing the instruction:
// opaque boundaries are part of the root's retained IR contract, independent
// of LLVM's generic dead-instruction classification.
TEST(SymSimplifyGuard, OpaqueLoadCannotBeErasedByARewrite) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::get(C, 0);
  auto *FT = llvm::FunctionType::get(I32, {Ptr, I32, I32},
                                     /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "opaque_load", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *Hidden = B.CreateLoad(I32, F->getArg(0));
  llvm::Value *X = F->getArg(1);
  llvm::Value *Z = F->getArg(2);
  llvm::Value *Or = B.CreateOr(B.CreateOr(X, Hidden), Z);
  llvm::Value *And = B.CreateAnd(B.CreateAnd(X, Hidden), Z);
  llvm::Value *Cross =
      B.CreateAnd(B.CreateXor(X, Hidden), B.CreateXor(Hidden, Z));
  B.CreateRet(B.CreateSub(B.CreateSub(Or, And), Cross));

  const std::string Before = printFunction(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_EQ(printFunction(*F), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// A call proven to return, not unwind and touch no memory is removable by
// generic DCE once it has no users.  That classification must not weaken this
// pass's stronger opaque-boundary contract: a semantic candidate that drops
// the call is declined before RAUW makes it dead.
TEST(SymSimplifyGuard, OpaqueReadNoneCallCannotBeErasedByARewrite) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *SourceTy = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *Source = llvm::Function::Create(
      SourceTy, llvm::Function::ExternalLinkage, "pure_source", &M);
  Source->setDoesNotAccessMemory();
  Source->setDoesNotThrow();
  Source->setWillReturn();

  auto *FT = llvm::FunctionType::get(I32, {I32, I32, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "opaque_call", &M);
  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Hidden = B.CreateCall(Source, {F->getArg(0)});
  ASSERT_TRUE(Hidden->willReturn());
  ASSERT_FALSE(Hidden->mayHaveSideEffects());
  llvm::Value *X = F->getArg(1);
  llvm::Value *Z = F->getArg(2);
  llvm::Value *Or = B.CreateOr(B.CreateOr(X, Hidden), Z);
  llvm::Value *And = B.CreateAnd(B.CreateAnd(X, Hidden), Z);
  llvm::Value *Cross =
      B.CreateAnd(B.CreateXor(X, Hidden), B.CreateXor(Hidden, Z));
  B.CreateRet(B.CreateSub(B.CreateSub(Or, And), Cross));

  const std::string Before = printFunction(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_EQ(printFunction(*F), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

// Leaf retention is a rewrite condition, not a blanket memory barrier.  The
// carry-save spelling can still become one add because its shorter form reads
// the exact same load instruction.
TEST(SymSimplifyGuard, OpaqueLoadMayBeRetainedWhileArithmeticAroundItShrinks) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::get(C, 0);
  auto *FT = llvm::FunctionType::get(I32, {Ptr, I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "retained_load", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *Hidden = B.CreateLoad(I32, F->getArg(0));
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Xor = B.CreateXor(Hidden, Y);
  llvm::Value *And = B.CreateAnd(Hidden, Y);
  llvm::Value *Two = B.CreateMul(And, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Xor, Two));
  const unsigned Before = instructionCount(*F);

  EXPECT_GT(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_LT(instructionCount(*F), Before);
  unsigned Loads = 0;
  for (const llvm::Instruction &I : llvm::instructions(*F))
    Loads += llvm::isa<llvm::LoadInst>(&I);
  EXPECT_EQ(Loads, 1u) << printFunction(*F);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

//===----------------------------------------------------------------------===//
// Proof-gated synthesis
//===----------------------------------------------------------------------===//

/// Two different IR spellings of the same logical shift are added together.
/// The derivational MBA engine deliberately treats the shifts as opaque, while
/// synthesis can discover the shorter `2 * (x lshr 4)` spelling.
llvm::Function *buildSynthesisShiftDouble(llvm::Module &M,
                                          llvm::StringRef Name) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);
  F->getArg(0)->setName("x");

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Direct = B.CreateLShr(X, llvm::ConstantInt::get(I32, 4));
  llvm::Value *ByTwo = B.CreateLShr(X, llvm::ConstantInt::get(I32, 2));
  llvm::Value *Nested = B.CreateLShr(ByTwo, llvm::ConstantInt::get(I32, 2));
  B.CreateRet(B.CreateAdd(Direct, Nested));
  return F;
}

/// A one-word equality indicator written without comparisons.  It is zero at
/// every input except p Needle.  With a cost-one synthesis grammar the sampled
/// zero candidate is therefore deterministic, and the built-in solver must
/// reject it with the concrete missing input rather than authorizing a rewrite.
llvm::Function *buildSparseEqualityIndicator(llvm::Module &M, uint32_t Needle) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "sparse_indicator", &M);
  F->getArg(0)->setName("x");

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *DifferenceForNegation =
      B.CreateXor(F->getArg(0), llvm::ConstantInt::get(I32, Needle));
  llvm::Value *DifferenceForOr =
      B.CreateXor(F->getArg(0), llvm::ConstantInt::get(I32, Needle));
  llvm::Value *Negated =
      B.CreateSub(llvm::ConstantInt::get(I32, 0), DifferenceForNegation);
  llvm::Value *HasHighBit = B.CreateLShr(B.CreateOr(DifferenceForOr, Negated),
                                         llvm::ConstantInt::get(I32, 31));
  B.CreateRet(B.CreateXor(HasHighBit, llvm::ConstantInt::get(I32, 1)));
  return F;
}

/// Two independent synthesis roots in deterministic instruction order.  The
/// volatile stores are opaque users, so neither root is absorbed into a larger
/// translatable tree and both produce their own proof disposition.
llvm::Function *buildTwoSynthesisShiftRoots(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), {I32, I32},
                                     /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "two_synthesis_roots", &M);
  F->getArg(0)->setName("x");
  F->getArg(1)->setName("y");

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto BuildRoot = [&](llvm::Value *Input, llvm::StringRef Name) {
    llvm::Value *Direct = B.CreateLShr(Input, llvm::ConstantInt::get(I32, 4));
    llvm::Value *ByTwo = B.CreateLShr(Input, llvm::ConstantInt::get(I32, 2));
    llvm::Value *Nested = B.CreateLShr(ByTwo, llvm::ConstantInt::get(I32, 2));
    llvm::Value *Root = B.CreateAdd(Direct, Nested);
    llvm::AllocaInst *Slot = B.CreateAlloca(I32, nullptr, Name);
    B.CreateStore(Root, Slot, /*isVolatile=*/true);
  };
  BuildRoot(F->getArg(0), "x.root");
  BuildRoot(F->getArg(1), "y.root");
  B.CreateRetVoid();
  return F;
}

SymSimplifyOptions synthesisOptions() {
  SymSimplifyOptions Opts;
  Opts.EnableSynthesis = true;
  Opts.Synthesis.UseStochasticFallback = false;
  return Opts;
}

TEST(SymSimplifyGuard, SynthesisEquivalent) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_equivalent");
  SymSimplifyOptions Opts = synthesisOptions();

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 1u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::Rewritten);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Equivalent);
  EXPECT_GT(Result.SearchWork, 0u);
  EXPECT_EQ(Result.ProofWork.Queries, 1u);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  unsigned Adds = 0;
  unsigned MultipliesByTwo = 0;
  unsigned LogicalShiftsByFour = 0;
  for (const llvm::Instruction &I : llvm::instructions(*F)) {
    Adds += I.getOpcode() == llvm::Instruction::Add;
    if (I.getOpcode() == llvm::Instruction::Mul)
      for (const llvm::Value *Op : I.operand_values())
        if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Op))
          MultipliesByTwo += CI->equalsInt(2);
    if (I.getOpcode() == llvm::Instruction::LShr)
      if (const auto *Amount =
              llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1)))
        LogicalShiftsByFour += Amount->equalsInt(4);
  }
  EXPECT_EQ(Adds, 0u) << printFunction(*F);
  EXPECT_EQ(MultipliesByTwo, 1u) << printFunction(*F);
  EXPECT_EQ(LogicalShiftsByFour, 1u) << printFunction(*F);
}

TEST(SymSimplifyGuard, SynthesisDifferentLeavesIRUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_different");
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Callback;
  Opts.ProofCallback = [](symbolic::SymContext &, symbolic::SymRef,
                          symbolic::SymRef) {
    return symbolic::SynthVerification::Different;
  };

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::Counterexample);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Different);
  EXPECT_GT(Result.SearchWork, 0u);
  EXPECT_GT(Result.ProofWork.Queries, 0u);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard, SynthesisUnknownLeavesIRUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_unknown");
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Callback;
  Opts.ProofCallback = [](symbolic::SymContext &, symbolic::SymRef,
                          symbolic::SymRef) {
    return symbolic::SynthVerification::Unknown;
  };

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::ProofIncomplete);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Unknown);
  EXPECT_GT(Result.SearchWork, 0u);
  EXPECT_EQ(Result.ProofWork.Queries, 1u);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard, SynthesisDisabledProviderLeavesIRUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_disabled");
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Disabled;

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::ProofIncomplete);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Unknown);
  EXPECT_GT(Result.SearchWork, 0u);
  EXPECT_EQ(Result.ProofWork.Queries, 0u);
  EXPECT_EQ(Result.ProofWork.Conflicts, 0u);
  EXPECT_EQ(Result.ProofWork.Propagations, 0u);
  EXPECT_EQ(Result.ProofWork.WatchVisits, 0u);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard, SynthesisEmptyCallbackLeavesIRUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_empty_callback");
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Callback;

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::ProofIncomplete);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Unknown);
  EXPECT_EQ(Result.ProofWork.Queries, 0u);
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard, SynthesisSearchBudgetLeavesIRUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSynthesisShiftDouble(M, "synthesis_budget");
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Synthesis.MaxWork = 1;

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::SearchBudgetExhausted);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::NotRun);
  EXPECT_EQ(Result.SearchWork, 1u);
  EXPECT_EQ(Result.ProofWork.Queries, 0u);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard, SynthesisCounterexampleIsCanonical) {
  constexpr uint32_t Needle = 0x13579bdfu;
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildSparseEqualityIndicator(M, Needle);
  const std::string Before = printFunction(*F);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Synthesis.MaxCost = 1;

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  ASSERT_EQ(Result.Rewrites, 0u);
  ASSERT_EQ(Result.Outcome, SymSimplifyOutcome::Counterexample);
  ASSERT_EQ(Result.Proof, solver::ProofStatus::Different);
  ASSERT_EQ(Result.ProofWork.Queries, 1u);
  ASSERT_TRUE(Result.Counterexample.has_value());
  ASSERT_EQ(Result.Counterexample->Candidate, "0");
  ASSERT_EQ(Result.Counterexample->Variables.size(), 1u);
  EXPECT_EQ(Result.Counterexample->Variables[0].Id, 0u);
  EXPECT_EQ(Result.Counterexample->Variables[0].Name, "x");
  EXPECT_EQ(Result.Counterexample->Variables[0].Width, 32u);
  EXPECT_EQ(Result.Counterexample->Variables[0].HexValue, "0x13579bdf");
  EXPECT_EQ(Result.Counterexample->toJson(),
            "{\"candidate\":\"0\",\"variables\":[{\"id\":0,\"name\":\"x\","
            "\"width\":32,\"value\":\"0x13579bdf\"}]}");
  EXPECT_EQ(printFunction(*F), Before);
}

TEST(SymSimplifyGuard,
     SynthesisMultipleRootsLaterEquivalentHasCoherentDisposition) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildTwoSynthesisShiftRoots(M);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Callback;
  unsigned DifferentCalls = 0;
  unsigned EquivalentCalls = 0;
  Opts.ProofCallback = [&](symbolic::SymContext &Ctx, symbolic::SymRef Original,
                           symbolic::SymRef) {
    const std::string Text = Ctx.toString(Original);
    if (Text.find("x") != std::string::npos) {
      ++DifferentCalls;
      return symbolic::SynthVerification::Different;
    }
    if (Text.find("y") != std::string::npos) {
      ++EquivalentCalls;
      return symbolic::SynthVerification::Equivalent;
    }
    ADD_FAILURE() << "unexpected synthesis root: " << Text;
    return symbolic::SynthVerification::Unknown;
  };

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 1u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::Rewritten);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Equivalent);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_GT(DifferentCalls, 0u);
  EXPECT_GT(EquivalentCalls, 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, SynthesisMultipleRootsFinalUnknownHasNoCounterexample) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildTwoSynthesisShiftRoots(M);
  SymSimplifyOptions Opts = synthesisOptions();
  Opts.Provider = ProofProvider::Callback;
  unsigned DifferentCalls = 0;
  unsigned UnknownCalls = 0;
  Opts.ProofCallback = [&](symbolic::SymContext &Ctx, symbolic::SymRef Original,
                           symbolic::SymRef) {
    const std::string Text = Ctx.toString(Original);
    if (Text.find("x") != std::string::npos) {
      ++DifferentCalls;
      return symbolic::SynthVerification::Different;
    }
    if (Text.find("y") != std::string::npos) {
      ++UnknownCalls;
      return symbolic::SynthVerification::Unknown;
    }
    ADD_FAILURE() << "unexpected synthesis root: " << Text;
    return symbolic::SynthVerification::Unknown;
  };

  SymSimplifyResult Result = SymSimplifyPass::simplifyWithResult(*F, Opts);

  EXPECT_EQ(Result.Rewrites, 0u);
  EXPECT_EQ(Result.Outcome, SymSimplifyOutcome::ProofIncomplete);
  EXPECT_EQ(Result.Proof, solver::ProofStatus::Unknown);
  EXPECT_FALSE(Result.Counterexample.has_value());
  EXPECT_GT(DifferentCalls, 0u);
  EXPECT_GT(UnknownCalls, 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

//===----------------------------------------------------------------------===//
// Observable semantic convergence
//===----------------------------------------------------------------------===//

TEST(SymSimplifyGuard, ConvergenceContinuesPastFourChangedRounds) {
  const std::vector<std::string> States = {"A", "B", "C", "D", "E", "F", "G"};
  auto Run = [&] {
    size_t Next = 0;
    std::string Current = "initial";
    return driveSemanticConvergence(
        /*MaxRounds=*/0,
        [&] {
          ConvergenceRound Round;
          if (Next < States.size()) {
            Current = States[Next];
            Round.Changed = true;
            Round.StructuralHash = static_cast<uint64_t>(++Next);
            Round.Semantic.Rewrites = 1;
            Round.Semantic.SearchWork = 2;
            Round.Semantic.ProofWork.Queries = 3;
            return Round;
          }
          ++Next;
          Round.StructuralHash = 7;
          return Round;
        },
        [&] { return Current; });
  };

  FunctionOptimizationResult Result = Run();
  FunctionOptimizationResult Repeated = Run();

  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Stop, OptimizationStopReason::Stable);
  EXPECT_EQ(Result.Rounds, 8u);
  EXPECT_EQ(Result.SemanticRewrites, 7u);
  EXPECT_EQ(Result.SearchWork, 14u);
  EXPECT_EQ(Result.ProofWork.Queries, 21u);
  EXPECT_EQ(Repeated.Changed, Result.Changed);
  EXPECT_EQ(Repeated.SemanticRewrites, Result.SemanticRewrites);
  EXPECT_EQ(Repeated.SearchWork, Result.SearchWork);
  EXPECT_EQ(Repeated.ProofWork.Queries, Result.ProofWork.Queries);
  EXPECT_EQ(Repeated.ProofWork.Conflicts, Result.ProofWork.Conflicts);
  EXPECT_EQ(Repeated.ProofWork.Propagations, Result.ProofWork.Propagations);
  EXPECT_EQ(Repeated.ProofWork.WatchVisits, Result.ProofWork.WatchVisits);
  EXPECT_EQ(Repeated.Rounds, Result.Rounds);
  EXPECT_EQ(Repeated.Stop, Result.Stop);
}

TEST(SymSimplifyGuard, ConvergenceFiniteRoundBudgetIsObservable) {
  unsigned Calls = 0;
  std::string Current;
  FunctionOptimizationResult Result = driveSemanticConvergence(
      /*MaxRounds=*/1,
      [&] {
        Current = std::to_string(++Calls);
        return ConvergenceRound{/*Changed=*/true, SymSimplifyResult(), Calls};
      },
      [&] { return Current; });

  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Stop, OptimizationStopReason::BudgetExhausted);
  EXPECT_EQ(Result.Rounds, 1u);
  EXPECT_EQ(Calls, 1u);
}

TEST(SymSimplifyGuard, ConvergenceDetectsABACycleAtZeroBasedRoundTwo) {
  const std::vector<std::string> States = {"A", "B", "A"};
  const uint64_t Hashes[] = {11, 22, 11};
  size_t Next = 0;
  std::string Current = "initial";
  FunctionOptimizationResult Result = driveSemanticConvergence(
      /*MaxRounds=*/0,
      [&] {
        if (Next >= States.size()) {
          ADD_FAILURE() << "cycle was not detected within the fixture";
          return ConvergenceRound();
        }
        Current = States[Next];
        return ConvergenceRound{/*Changed=*/true, SymSimplifyResult(),
                                Hashes[Next++]};
      },
      [&] { return Current; });

  // The driver cannot observe the caller's initial structural hash.  Its
  // checkpoint is the first changed state, so A -> B -> A is detected on the
  // third RunRound call: zero-based state index two, three reported rounds.
  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Stop, OptimizationStopReason::CycleDetected);
  EXPECT_EQ(Result.Rounds, 3u);
}

TEST(SymSimplifyGuard, ConvergenceHashCollisionRequiresExactSnapshot) {
  const std::vector<std::string> States = {"A", "B"};
  size_t Next = 0;
  std::string Current = "initial";
  FunctionOptimizationResult Result = driveSemanticConvergence(
      /*MaxRounds=*/0,
      [&] {
        ConvergenceRound Round;
        Round.StructuralHash = 42;
        if (Next < States.size()) {
          Current = States[Next++];
          Round.Changed = true;
        }
        return Round;
      },
      [&] { return Current; });

  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Stop, OptimizationStopReason::Stable);
  EXPECT_EQ(Result.Rounds, 3u);
}

TEST(SymSimplifyGuard, ConvergenceSaturatesDistinctTelemetryCounters) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  unsigned Calls = 0;
  std::string Current;
  FunctionOptimizationResult Result = driveSemanticConvergence(
      /*MaxRounds=*/2,
      [&] {
        Current = std::to_string(++Calls);
        ConvergenceRound Round;
        Round.Changed = true;
        Round.StructuralHash = Calls;
        const uint64_t Amount = Calls == 1 ? Max - 2 : 10;
        Round.Semantic.Rewrites = Amount;
        Round.Semantic.SearchWork = Amount;
        Round.Semantic.ProofWork.Queries = Amount;
        Round.Semantic.ProofWork.Conflicts = Amount;
        Round.Semantic.ProofWork.Propagations = Amount;
        Round.Semantic.ProofWork.WatchVisits = Amount;
        return Round;
      },
      [&] { return Current; });

  EXPECT_EQ(Result.Stop, OptimizationStopReason::BudgetExhausted);
  EXPECT_EQ(Result.SemanticRewrites, Max);
  EXPECT_EQ(Result.SearchWork, Max);
  EXPECT_EQ(Result.ProofWork.Queries, Max);
  EXPECT_EQ(Result.ProofWork.Conflicts, Max);
  EXPECT_EQ(Result.ProofWork.Propagations, Max);
  EXPECT_EQ(Result.ProofWork.WatchVisits, Max);
}

TEST(SymSimplifyGuard, ConvergenceProofIncompleteDoesNotAbortThePipeline) {
  FunctionOptimizationResult Result = driveSemanticConvergence(
      /*MaxRounds=*/1,
      [] {
        ConvergenceRound Round;
        Round.Changed = true;
        Round.StructuralHash = 1;
        Round.Semantic.Outcome = SymSimplifyOutcome::ProofIncomplete;
        Round.Semantic.Proof = solver::ProofStatus::Unknown;
        return Round;
      },
      [] { return std::string("proof-incomplete"); });

  EXPECT_EQ(Result.Stop, OptimizationStopReason::BudgetExhausted);
  EXPECT_EQ(Result.Rounds, 1u);
}

TEST(SymSimplifyGuard, ConvergenceAggregatesStopSeverityExactly) {
  OptimizationResult Module;
  auto Merge = [&](OptimizationStopReason Stop) {
    FunctionOptimizationResult Function;
    Function.Stop = Stop;
    mergeFunctionOptimizationResult(Module, Function);
  };

  Merge(OptimizationStopReason::CycleDetected);
  EXPECT_EQ(Module.Stop, OptimizationStopReason::CycleDetected);
  Merge(OptimizationStopReason::Stable);
  EXPECT_EQ(Module.Stop, OptimizationStopReason::CycleDetected);
  Merge(OptimizationStopReason::VerificationFailed);
  EXPECT_EQ(Module.Stop, OptimizationStopReason::VerificationFailed);
  Merge(OptimizationStopReason::BudgetExhausted);
  EXPECT_EQ(Module.Stop, OptimizationStopReason::VerificationFailed);
  Merge(OptimizationStopReason::InputInvalid);

  EXPECT_EQ(Module.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_EQ(Module.FunctionsVisited, 5u);
}

TEST(SymSimplifyGuard, ConvergenceAggregatesTwoFunctionsExactly) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);

  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *Stable =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "stable", &M);
  auto *BB = llvm::BasicBlock::Create(C, "entry", Stable);
  llvm::IRBuilder<> B(BB);
  B.CreateRet(Stable->getArg(0));
  buildCarrySaveAdd(M);

  OptimizationResult Result = Pipeline::optimizeModule(
      M, /*Conservative=*/false, Pipeline::OptStrength::Deep,
      /*MaxRounds=*/1);

  EXPECT_TRUE(Result.Changed);
  EXPECT_EQ(Result.Stop, OptimizationStopReason::BudgetExhausted);
  EXPECT_EQ(Result.FunctionsVisited, 2u);
  EXPECT_EQ(Result.Rounds, 1u);
  EXPECT_EQ(Result.SemanticRewrites, 1u);
  EXPECT_EQ(Result.SearchWork, 0u);
  EXPECT_EQ(Result.ProofWork.Queries, 0u);
  EXPECT_EQ(Result.ProofWork.Conflicts, 0u);
  EXPECT_EQ(Result.ProofWork.Propagations, 0u);
  EXPECT_EQ(Result.ProofWork.WatchVisits, 0u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

//===----------------------------------------------------------------------===//
// Transactional module optimization and LLVM default pipelines
//===----------------------------------------------------------------------===//

TEST(SymSimplifyGuard, DeepDefaultO2InlinesAndEliminatesInternalHelper) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Constant = new llvm::GlobalVariable(
      M, I32, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 40), "foldable_constant");

  auto *HelperTy = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *Helper = llvm::Function::Create(
      HelperTy, llvm::Function::InternalLinkage, "called_once", &M);
  llvm::IRBuilder<> HelperBuilder(llvm::BasicBlock::Create(C, "entry", Helper));
  llvm::Value *Loaded = HelperBuilder.CreateLoad(I32, Constant);
  HelperBuilder.CreateRet(HelperBuilder.CreateAdd(Loaded, Helper->getArg(0)));

  auto *EntryTy = llvm::FunctionType::get(I32, /*isVarArg=*/false);
  auto *Entry = llvm::Function::Create(EntryTy, llvm::Function::ExternalLinkage,
                                       "entry", &M);
  llvm::IRBuilder<> EntryBuilder(llvm::BasicBlock::Create(C, "entry", Entry));
  EntryBuilder.CreateRet(
      EntryBuilder.CreateCall(Helper, {llvm::ConstantInt::get(I32, 2)}));

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_TRUE(Result.Changed);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_EQ(M.getFunction("called_once"), nullptr);
  EXPECT_EQ(M.getNamedGlobal("foldable_constant"), nullptr);
  llvm::Function *OptimizedEntry = M.getFunction("entry");
  ASSERT_NE(OptimizedEntry, nullptr);
  auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
      OptimizedEntry->getEntryBlock().getTerminator());
  ASSERT_NE(Return, nullptr);
  auto *Value = llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
  ASSERT_NE(Value, nullptr) << printFunction(*OptimizedEntry);
  EXPECT_EQ(Value->getZExtValue(), 42u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, DeepPreservesDebugLocationAndExceptionMetadata) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  M.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                  llvm::DEBUG_METADATA_VERSION);

  llvm::DIBuilder Debug(M);
  llvm::DIFile *File = Debug.createFile("lifted.c", "/neverd/tests");
  llvm::DICompileUnit *Unit = Debug.createCompileUnit(
      llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C), File, "NeverD",
      /*isOptimized=*/true, /*Flags=*/"", /*RuntimeVersion=*/0);
  llvm::DISubroutineType *DebugType =
      Debug.createSubroutineType(Debug.getOrCreateTypeArray({}));

  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Global = new llvm::GlobalVariable(
      M, I32, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(I32, 0), "observable");
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "debug_contract", &M);
  llvm::DISubprogram *Subprogram =
      Debug.createFunction(Unit, F->getName(), F->getName(), File, 17,
                           DebugType, 17, llvm::DINode::FlagZero,
                           llvm::DISubprogram::SPFlagDefinition |
                               llvm::DISubprogram::SPFlagOptimized);
  F->setSubprogram(Subprogram);

  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  llvm::StoreInst *Store =
      B.CreateStore(llvm::ConstantInt::get(I32, 7), Global, true);
  Store->setDebugLoc(llvm::DILocation::get(C, 17, 3, Subprogram));
  B.CreateRetVoid();
  exception_rewrite::setContract(*F, exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::NotRequired);
  Debug.finalize();

  Pipeline::OptimizationOptions Options;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  llvm::Function *Optimized = M.getFunction("debug_contract");
  ASSERT_NE(Optimized, nullptr);
  EXPECT_NE(Optimized->getMetadata(exception_rewrite::FunctionAttachment),
            nullptr);
  const llvm::StoreInst *OptimizedStore = nullptr;
  for (const llvm::Instruction &I : llvm::instructions(*Optimized))
    if (const auto *Candidate = llvm::dyn_cast<llvm::StoreInst>(&I))
      if (Candidate->isVolatile())
        OptimizedStore = Candidate;
  ASSERT_NE(OptimizedStore, nullptr) << printFunction(*Optimized);
  ASSERT_TRUE(OptimizedStore->getDebugLoc());
  EXPECT_EQ(OptimizedStore->getDebugLoc().getLine(), 17u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, DeepO0ForwardsSemanticOptionsAndCountsDefinitionsOnce) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  buildVanishingVariable(M);

  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O0;
  Options.Semantic = synthesisOptions();
  // Make synthesis, rather than the derivational MBA engine, the only route to
  // the shorter x^z result.  InstCombine cannot remove the vanishing y input.
  Options.Semantic.MBA.MaxWork = 0;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.FunctionsVisited, 1u);
  EXPECT_EQ(Result.SemanticRewrites, 1u);
  EXPECT_GT(Result.SearchWork, 0u);
  EXPECT_GT(Result.ProofWork.Queries, 0u);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  llvm::Function *Optimized = M.getFunction("g");
  ASSERT_NE(Optimized, nullptr);
  EXPECT_TRUE(Optimized->getArg(1)->use_empty()) << printFunction(*Optimized);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, DeepRepairsFrameRelativeIntToPtrAfterO2) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *I64 = llvm::Type::getInt64Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  auto *ObserveTy = llvm::FunctionType::get(llvm::Type::getVoidTy(C), {I64},
                                            /*isVarArg=*/false);
  llvm::FunctionCallee Observe = M.getOrInsertFunction("observe_sp", ObserveTy);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), {Ptr},
                                     /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "frame_relative", &M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, kFrameSetupBlock, F));
  llvm::Value *FrameBase = B.CreatePtrToInt(F->getArg(0), I64, kRspInitValue);
  B.CreateCall(Observe, {FrameBase});
  llvm::Constant *NegativeAddress = llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::getSigned(I64, -16), Ptr);
  B.CreateStore(llvm::ConstantInt::get(I32, 7), NegativeAddress,
                /*isVolatile=*/true);
  B.CreateRetVoid();

  Pipeline::OptimizationOptions Options;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  llvm::Function *Optimized = M.getFunction("frame_relative");
  ASSERT_NE(Optimized, nullptr);
  const llvm::StoreInst *StackStore = nullptr;
  for (const llvm::Instruction &I : llvm::instructions(*Optimized))
    if (const auto *Candidate = llvm::dyn_cast<llvm::StoreInst>(&I))
      if (Candidate->isVolatile())
        StackStore = Candidate;
  ASSERT_NE(StackStore, nullptr) << printFunction(*Optimized);
  const auto *Fixed =
      llvm::dyn_cast<llvm::IntToPtrInst>(StackStore->getPointerOperand());
  ASSERT_NE(Fixed, nullptr) << printFunction(*Optimized);
  EXPECT_EQ(Fixed->getName(), "stackptr");
  EXPECT_TRUE(llvm::isa<llvm::BinaryOperator>(Fixed->getOperand(0)));
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
}

TEST(SymSimplifyGuard, VerifierRejectionIsTransactional) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "transaction", &M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRet(B.CreateAdd(F->getArg(0), llvm::ConstantInt::get(I32, 0)));
  const std::string Before = printModule(M);
  unsigned HookCalls = 0;

  Pipeline::OptimizationOptions Options;
  auto Reject = [&](const llvm::Module &) {
    ++HookCalls;
    return false;
  };
  Options.PostTransformVerifier = Reject;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.Stop, OptimizationStopReason::VerificationFailed);
  EXPECT_FALSE(Result.Changed);
  EXPECT_EQ(HookCalls, 1u);
  EXPECT_EQ(printModule(M), Before);
}

TEST(SymSimplifyGuard, InvalidInputIsTransactional) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "invalid", &M);
  llvm::BasicBlock::Create(C, "unterminated", F);
  const std::string Before = printModule(M);
  bool HookCalled = false;

  Pipeline::OptimizationOptions Options;
  auto Accept = [&](const llvm::Module &) {
    HookCalled = true;
    return true;
  };
  Options.PostTransformVerifier = Accept;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_FALSE(Result.Changed);
  EXPECT_FALSE(HookCalled);
  EXPECT_EQ(printModule(M), Before);
}

TEST(SymSimplifyGuard, InvalidOptimizationStrengthIsTransactional) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "invalid_strength", &M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRetVoid();
  const std::string Before = printModule(M);
  bool HookCalled = false;

  Pipeline::OptimizationOptions Options;
  Options.Strength = static_cast<Pipeline::OptStrength>(255);
  auto Accept = [&](const llvm::Module &) {
    HookCalled = true;
    return true;
  };
  Options.PostTransformVerifier = Accept;
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_FALSE(Result.Changed);
  EXPECT_FALSE(HookCalled);
  EXPECT_EQ(printModule(M), Before);
}

TEST(SymSimplifyGuard,
     ConservativeIgnoresInvalidStrengthAndPreservesNoOpIRHandles) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "identity", &M);
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(Entry);
  llvm::ReturnInst *Return = B.CreateRet(F->getArg(0));

  Pipeline::OptimizationOptions Options;
  Options.Conservative = true;
  Options.Strength = static_cast<Pipeline::OptStrength>(255);
  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.Stop, OptimizationStopReason::Stable);
  EXPECT_FALSE(Result.Changed);
  EXPECT_EQ(M.getFunction("identity"), F);
  EXPECT_EQ(&F->getEntryBlock(), Entry);
  EXPECT_EQ(F->getEntryBlock().getTerminator(), Return);
}

TEST(SymSimplifyGuard, InvalidExceptionRewriteContractIsTransactional) {
  using exception_rewrite::LoweringState;
  using exception_rewrite::SourceState;
  struct ContractCase {
    SourceState Source;
    LoweringState Lowering;
  };
  const ContractCase Cases[] = {
      {SourceState::Partial, LoweringState::Missing},
      {SourceState::Malformed, LoweringState::Missing},
      {SourceState::Complete, LoweringState::Missing},
      {SourceState::Complete, LoweringState::Incomplete},
  };

  for (const ContractCase &Case : Cases) {
    llvm::LLVMContext C;
    llvm::Module M("m", C);
    auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
    auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                     "contract", &M);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
    B.CreateRetVoid();
    exception_rewrite::setContract(*F, Case.Source, Case.Lowering);
    const std::string Before = printModule(M);
    bool HookCalled = false;

    Pipeline::OptimizationOptions Options;
    auto Accept = [&](const llvm::Module &) {
      HookCalled = true;
      return true;
    };
    Options.PostTransformVerifier = Accept;
    OptimizationResult Result = Pipeline::optimizeModule(M, Options);

    SCOPED_TRACE(static_cast<unsigned>(Case.Source));
    SCOPED_TRACE(static_cast<unsigned>(Case.Lowering));
    EXPECT_EQ(Result.Stop, OptimizationStopReason::InputInvalid);
    EXPECT_FALSE(Result.Changed);
    EXPECT_FALSE(HookCalled);
    EXPECT_EQ(printModule(M), Before);
  }

  llvm::LLVMContext C;
  llvm::Module M("missing-contract", C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "missing_contract", &M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRetVoid();
  exception_rewrite::markModule(M);
  const std::string Before = printModule(M);
  bool HookCalled = false;
  Pipeline::OptimizationOptions Options;
  auto Accept = [&](const llvm::Module &) {
    HookCalled = true;
    return true;
  };
  Options.PostTransformVerifier = Accept;

  OptimizationResult Result = Pipeline::optimizeModule(M, Options);

  EXPECT_EQ(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_FALSE(Result.Changed);
  EXPECT_FALSE(HookCalled);
  EXPECT_EQ(printModule(M), Before);
}

// The joint fixed point, checked through the real default pipeline rather than
// a pass list assembled for the test.  optimizeModule wires
// SemanticFixedPointPass exactly as production decompilation does; after it,
// the three-input MBA is `x ^ z` and `%y` -- the input the value never truly
// depended on -- has no uses left.  A single canonicalize/measure pairing would
// leave measurable residue; reaching this state is what the alternation is for.
TEST(SymSimplifyGuard, ReachesTheJointFixedPointThroughTheRealPipeline) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildVanishingVariable(M);

  Pipeline::optimizeModule(M, /*Conservative=*/false);

  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  F = M.getFunction("g");
  ASSERT_NE(F, nullptr);
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
  F = M.getFunction("f");
  ASSERT_NE(F, nullptr);
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
// Words wider than the machine's
//===----------------------------------------------------------------------===//

/// The carry-save spelling of `x + y` at \p Bits, plus \p Tail when one is
/// given, so a caller can pin what becomes of a literal no machine word holds.
llvm::Function *buildWideCarrySaveAdd(llvm::Module &M, llvm::StringRef Name,
                                      unsigned Bits,
                                      const llvm::APInt *Tail = nullptr) {
  llvm::LLVMContext &C = M.getContext();
  auto *Ty = llvm::IntegerType::get(C, Bits);
  auto *FT = llvm::FunctionType::get(Ty, {Ty, Ty}, /*isVarArg=*/false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Carry =
      B.CreateMul(B.CreateAnd(X, Y), llvm::ConstantInt::get(Ty, 2));
  llvm::Value *Sum = B.CreateAdd(B.CreateXor(X, Y), Carry);
  if (Tail)
    Sum = B.CreateAdd(Sum, llvm::ConstantInt::get(Ty, *Tail));
  B.CreateRet(Sum);
  return F;
}

// The same identity at a width no register has.  Nothing in the engine cares:
// its literals are arbitrary precision and corner measurement falls back to
// them once a value outgrows a machine word.  What this pins is the translator
// carrying an i128 across rather than declining the candidate, which is what
// lets the pass reach obfuscation written in a wide type.
TEST(SymSimplifyGuard, RewritesMixedBooleanArithmeticWiderThanAWord) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildWideCarrySaveAdd(M, "wide_mba", 128);
  unsigned Before = instructionCount(*F);

  ASSERT_GT(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_LT(instructionCount(*F), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  unsigned Bitwise = 0;
  for (llvm::Instruction &I : llvm::instructions(*F))
    if (I.getOpcode() == llvm::Instruction::Xor ||
        I.getOpcode() == llvm::Instruction::And)
      ++Bitwise;
  EXPECT_EQ(Bitwise, 0u) << printFunction(*F);
}

TEST(SymSimplifyGuard, RewritesI512CarrySaveIdentity) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildWideCarrySaveAdd(M, "wide_mba_i512", 512);
  const unsigned Before = instructionCount(*F);

  ASSERT_GT(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_LT(instructionCount(*F), Before);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  unsigned Bitwise = 0;
  for (llvm::Instruction &I : llvm::instructions(*F))
    if (I.getOpcode() == llvm::Instruction::Xor ||
        I.getOpcode() == llvm::Instruction::And)
      ++Bitwise;
  EXPECT_EQ(Bitwise, 0u) << printFunction(*F);
}

// A literal too large for a machine word has to come back bit for bit.  The
// engine keeps one in a side pool rather than inline, so a way back that read
// it as a `uint64_t` would hand the function the low sixty-four bits of the
// constant it started with -- fewer instructions computing something else.
TEST(SymSimplifyGuard, CarriesALiteralWiderThanAWordThroughUnchanged) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  const llvm::APInt Tail = llvm::APInt(128, 1).shl(100);
  llvm::Function *F = buildWideCarrySaveAdd(M, "wide_literal", 128, &Tail);

  ASSERT_GT(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  bool Survived = false;
  for (llvm::Instruction &I : llvm::instructions(*F))
    for (const llvm::Value *Op : I.operand_values())
      if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Op))
        Survived |= CI->getValue() == Tail;
  EXPECT_TRUE(Survived) << printFunction(*F);
}

//===----------------------------------------------------------------------===//
// How hard to look
//===----------------------------------------------------------------------===//

/// `@complement_sum(i32 %x)` returning `x + ~x`, which is -1 for every x.
/// Two instructions, but only three engine nodes -- the sum, the complement
/// and the one input both of them read -- which is below the size the default
/// policy considers worth measuring.
llvm::Function *buildComplementSum(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "complement_sum", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *X = F->getArg(0);
  B.CreateRet(B.CreateAdd(X, B.CreateNot(X)));
  return F;
}

// The two thresholds are policy rather than law, and the default is the
// optimizer's: an expression too small to be hiding anything is left alone, so
// a pipeline built on this IR is not churned for nothing.  A caller reading the
// result instead of compiling it wants the opposite, and asking for it has to
// reach the identity the default will not even measure.
TEST(SymSimplifyGuard, AggressiveOptionsReachATreeTheDefaultDeclinesToMeasure) {
  llvm::LLVMContext C;
  llvm::Module M("m", C);
  llvm::Function *F = buildComplementSum(M);
  const unsigned Before = instructionCount(*F);

  EXPECT_EQ(SymSimplifyPass::simplify(*F), 0u) << printFunction(*F);
  EXPECT_EQ(instructionCount(*F), Before);

  EXPECT_GT(SymSimplifyPass::simplify(*F, SymSimplifyOptions::aggressive()), 0u)
      << printFunction(*F);
  EXPECT_LT(instructionCount(*F), Before);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  const auto *Ret =
      llvm::cast<llvm::ReturnInst>(F->getEntryBlock().getTerminator());
  const auto *Returned =
      llvm::dyn_cast<llvm::ConstantInt>(Ret->getReturnValue());
  ASSERT_NE(Returned, nullptr) << printFunction(*F);
  EXPECT_TRUE(Returned->getValue().isAllOnes()) << printFunction(*F);
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
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, &M);

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
        llvm::Value *Carry = B.CreateMul(
            B.CreateAnd(X, Y), llvm::ConstantInt::get(X->getType(), 2));
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
      M, "genuine", [](llvm::IRBuilder<> &B, llvm::Value *X, llvm::Value *Y) {
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
  F = M.getFunction("opaque");
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(branchCondition(*F), nullptr) << printFunction(*F);
  EXPECT_FALSE(mentionsGuardedConstant(*F)) << printFunction(*F);
}

//===----------------------------------------------------------------------===//
// What the deeper lift order adds
//===----------------------------------------------------------------------===//

/// `@redundant_loads(ptr %p)` reading one address twice and adding the results.
/// Nothing writes memory in between, so the second read cannot see anything the
/// first did not -- but establishing that takes a walk over memory, which is
/// exactly what separates the two lift orders.  Promotion and SROA only reach
/// stack slots, and a peephole rewriter has no grounds on which to call two
/// loads one.
llvm::Function *buildRedundantLoads(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::get(C, 0);
  auto *FT = llvm::FunctionType::get(I32, {Ptr}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "redundant_loads", &M);

  auto *BB = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(BB);
  llvm::Value *P = F->getArg(0);
  llvm::Value *First = B.CreateLoad(I32, P);
  llvm::Value *Second = B.CreateLoad(I32, P);
  B.CreateRet(B.CreateAdd(First, Second));
  return F;
}

unsigned loadCount(const llvm::Function &F) {
  unsigned N = 0;
  for (const llvm::Instruction &I : llvm::instructions(F))
    if (llvm::isa<llvm::LoadInst>(&I))
      ++N;
  return N;
}

/// Build two identical integer expressions on opposite sides of an opaque
/// control-flow join.  InstCombine deliberately does not perform global value
/// numbering, while the calls keep SimplifyCFG from erasing the distinction
/// between the two paths.  The first sum dominates the second, so GVN can
/// replace the latter without making any assumption about memory.
llvm::Function *buildCrossBlockRedundancy(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *I1 = llvm::Type::getInt1Ty(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *FT = llvm::FunctionType::get(I32, {I32, I32, I1}, /*isVarArg=*/false);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   "cross_block_redundancy", &M);

  auto *HookTy = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  llvm::FunctionCallee LeftHook = M.getOrInsertFunction("left_hook", HookTy);
  llvm::FunctionCallee RightHook = M.getOrInsertFunction("right_hook", HookTy);

  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  auto *Left = llvm::BasicBlock::Create(C, "left", F);
  auto *Right = llvm::BasicBlock::Create(C, "right", F);
  auto *Merge = llvm::BasicBlock::Create(C, "merge", F);

  llvm::Value *X = F->getArg(0);
  llvm::Value *Y = F->getArg(1);
  llvm::Value *Condition = F->getArg(2);

  llvm::IRBuilder<> B(Entry);
  llvm::Value *First = B.CreateAdd(X, Y, "first");
  B.CreateCondBr(Condition, Left, Right);

  B.SetInsertPoint(Left);
  B.CreateCall(LeftHook);
  B.CreateBr(Merge);

  B.SetInsertPoint(Right);
  B.CreateCall(RightHook);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  llvm::Value *Second = B.CreateAdd(X, Y, "second");
  B.CreateRet(B.CreateAdd(First, Second));
  return F;
}

unsigned sourceAddCount(const llvm::Function &F) {
  auto Arg = F.arg_begin();
  const llvm::Value *X = &*Arg++;
  const llvm::Value *Y = &*Arg;
  unsigned N = 0;
  for (const llvm::Instruction &I : llvm::instructions(F)) {
    if (I.getOpcode() != llvm::Instruction::Add)
      continue;
    if ((I.getOperand(0) == X && I.getOperand(1) == Y) ||
        (I.getOperand(0) == Y && I.getOperand(1) == X))
      ++N;
  }
  return N;
}

// Why the deeper order is the default.  Both orders recover the same
// arithmetic; only the deeper one goes on to notice the function computes one
// thing twice.  Reading a decompiled listing means ruling out every apparent
// difference between two computations that turn out to be one, so a redundancy
// left standing in it is work handed to the reader.
TEST(SymSimplifyGuard, TheDeepOrderRemovesRedundancyTheThinOrderLeaves) {
  llvm::LLVMContext C;
  llvm::Module ThinModule("thin", C);
  llvm::Module DeepModule("deep", C);
  llvm::Function *Thin = buildCrossBlockRedundancy(ThinModule);
  llvm::Function *Deep = buildCrossBlockRedundancy(DeepModule);

  Pipeline::optimizeModule(ThinModule, /*Conservative=*/false,
                           Pipeline::OptStrength::Thin);
  Pipeline::optimizeModule(DeepModule, /*Conservative=*/false,
                           Pipeline::OptStrength::Deep);

  ASSERT_FALSE(llvm::verifyModule(ThinModule, &llvm::errs()));
  ASSERT_FALSE(llvm::verifyModule(DeepModule, &llvm::errs()));

  Thin = ThinModule.getFunction("cross_block_redundancy");
  Deep = DeepModule.getFunction("cross_block_redundancy");
  ASSERT_NE(Thin, nullptr);
  ASSERT_NE(Deep, nullptr);
  EXPECT_EQ(sourceAddCount(*Thin), 2u) << printFunction(*Thin);
  EXPECT_EQ(sourceAddCount(*Deep), 1u) << printFunction(*Deep);
}

// A bounded stabilization loop is only safe to leave on by default if what it
// settles on is a function of the input and of nothing else.  A listing that
// differed between runs of the same binary would make every diff taken against
// it meaningless, so two runs have to agree instruction for instruction.
TEST(SymSimplifyGuard, TheDeepOrderSettlesOnTheSameFunctionEveryRun) {
  llvm::LLVMContext C;
  llvm::Module First("first", C);
  llvm::Module Second("second", C);
  llvm::Function *One = buildVanishingVariable(First);
  llvm::Function *Two = buildVanishingVariable(Second);

  Pipeline::optimizeModule(First, /*Conservative=*/false,
                           Pipeline::OptStrength::Deep);
  Pipeline::optimizeModule(Second, /*Conservative=*/false,
                           Pipeline::OptStrength::Deep);

  ASSERT_FALSE(llvm::verifyModule(First, &llvm::errs()));
  ASSERT_FALSE(llvm::verifyModule(Second, &llvm::errs()));
  One = First.getFunction("g");
  Two = Second.getFunction("g");
  ASSERT_NE(One, nullptr);
  ASSERT_NE(Two, nullptr);
  EXPECT_EQ(printFunction(*One), printFunction(*Two));
}

// The stamp gates NeverD's semantic rewriter, not LLVM's default pipeline.
// Deep may infer attributes or choose another canonical order for commutative
// operands, but it must retain the carry-save payload instead of replacing it
// with the shorter add that semantic measurement proves equivalent.
TEST(SymSimplifyGuard, TheDeepOrderKeepsAStampedSemanticPayload) {
  llvm::LLVMContext C;
  llvm::Module ThinModule("thin", C);
  llvm::Module DeepModule("deep", C);
  llvm::Function *Thin = buildCarrySaveAdd(ThinModule);
  llvm::Function *Deep = buildCarrySaveAdd(DeepModule);
  Thin->addFnAttr(kObfuscatedFnAttr);
  Deep->addFnAttr(kObfuscatedFnAttr);

  Pipeline::optimizeModule(ThinModule, /*Conservative=*/false,
                           Pipeline::OptStrength::Thin);
  Pipeline::optimizeModule(DeepModule, /*Conservative=*/false,
                           Pipeline::OptStrength::Deep);

  ASSERT_FALSE(llvm::verifyModule(DeepModule, &llvm::errs()));
  Thin = ThinModule.getFunction("f");
  Deep = DeepModule.getFunction("f");
  ASSERT_NE(Thin, nullptr);
  ASSERT_NE(Deep, nullptr);
  EXPECT_TRUE(Deep->hasFnAttribute(kObfuscatedFnAttr));
  auto OpcodeCount = [](const llvm::Function &F, unsigned Opcode) {
    unsigned Count = 0;
    for (const llvm::Instruction &I : llvm::instructions(F))
      Count += I.getOpcode() == Opcode;
    return Count;
  };
  for (unsigned Opcode : {llvm::Instruction::Xor, llvm::Instruction::And,
                          llvm::Instruction::Shl}) {
    EXPECT_EQ(OpcodeCount(*Thin, Opcode), 1u) << printFunction(*Thin);
    EXPECT_EQ(OpcodeCount(*Deep, Opcode), 1u) << printFunction(*Deep);
  }
  EXPECT_EQ(instructionCount(*Deep), instructionCount(*Thin));
}

// The patch path's guarantee holds whatever lift strength the caller named.
// The two are separate arguments to one entry point, and the conservative
// branch is what keeps a patched binary's semantics byte for byte, so nothing
// but this stops a later edit from letting the strength reach past it.  Both
// spellings of the call have to leave the module character for character as it
// was given.
TEST(SymSimplifyGuard, TheConservativeOrderIgnoresTheStrength) {
  llvm::LLVMContext C;
  llvm::Module ThinModule("thin", C);
  llvm::Module DeepModule("deep", C);
  llvm::Function *ThinMBA = buildCarrySaveAdd(ThinModule);
  llvm::Function *DeepMBA = buildCarrySaveAdd(DeepModule);
  llvm::Function *ThinLoads = buildRedundantLoads(ThinModule);
  llvm::Function *DeepLoads = buildRedundantLoads(DeepModule);

  Pipeline::optimizeModule(ThinModule, /*Conservative=*/true,
                           Pipeline::OptStrength::Thin);
  Pipeline::optimizeModule(DeepModule, /*Conservative=*/true,
                           Pipeline::OptStrength::Deep);

  ASSERT_FALSE(llvm::verifyModule(DeepModule, &llvm::errs()));
  ThinMBA = ThinModule.getFunction("f");
  DeepMBA = DeepModule.getFunction("f");
  ThinLoads = ThinModule.getFunction("redundant_loads");
  DeepLoads = DeepModule.getFunction("redundant_loads");
  ASSERT_NE(ThinMBA, nullptr);
  ASSERT_NE(DeepMBA, nullptr);
  ASSERT_NE(ThinLoads, nullptr);
  ASSERT_NE(DeepLoads, nullptr);
  EXPECT_EQ(printFunction(*ThinMBA), printFunction(*DeepMBA));
  EXPECT_EQ(printFunction(*ThinLoads), printFunction(*DeepLoads));

  // Both of the things the deeper order is for are still here, so the two
  // agree because neither ran it rather than because both did.
  EXPECT_EQ(loadCount(*DeepLoads), 2u) << printFunction(*DeepLoads);
  unsigned Bitwise = 0;
  for (llvm::Instruction &I : llvm::instructions(*DeepMBA))
    if (I.getOpcode() == llvm::Instruction::Xor ||
        I.getOpcode() == llvm::Instruction::And)
      ++Bitwise;
  EXPECT_GT(Bitwise, 0u) << printFunction(*DeepMBA);
}

} // namespace
