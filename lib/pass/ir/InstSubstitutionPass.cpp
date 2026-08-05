//===- InstSubstitutionPass.cpp - Instruction substitution pass ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction-substitution pass: replaces integer add/sub/and/or/
/// xor with semantically-equivalent instruction sequences.  Demo-level sample
/// transform — see InstSubstitutionPass.h.
///
/// The identities used (all exact for two's-complement / modular integers and
/// for integer vectors element-wise):
///
///   a + b  ==  (a ^ b) + ((a & b) << 1)      (carry-save / MBA)
///   a - b  ==  (a ^ b) - ((~a & b) << 1)     (borrow / MBA)
///   a & b  ==  ~(~a | ~b)                     (De Morgan)
///   a | b  ==  ~(~a & ~b)                     (De Morgan)
///   a ^ b  ==  (a & ~b) | (~a & b)
///
/// The add/sub identities deliberately use the carry/borrow MBA forms rather
/// than the simpler `a - (0 - b)` negation, because the backend's algebraic
/// simplification folds the latter straight back to a plain add/sub; the MBA
/// forms survive to the emitted machine code so every handled operator is
/// visibly substituted.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/InstSubstitutionPass.h"

#define DEBUG_TYPE "neverd-inst-subst"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

// Build the replacement value for a single eligible binary operator.  Returns
// nullptr if \p BO is not one of the handled integer opcodes.
llvm::Value *substituteOne(llvm::BinaryOperator *BO) {
  llvm::Type *Ty = BO->getType();
  if (!Ty->isIntOrIntVectorTy())
    return nullptr;

  llvm::IRBuilder<> B(BO);
  llvm::Value *A = BO->getOperand(0);
  llvm::Value *C = BO->getOperand(1);
  llvm::Value *Ones = llvm::Constant::getAllOnesValue(Ty); // ~x == xor(x, -1)

  auto Not = [&](llvm::Value *V) { return B.CreateXor(V, Ones); };

  switch (BO->getOpcode()) {
  case llvm::Instruction::Add: // a + b == (a ^ b) + ((a & b) << 1)
    return B.CreateAdd(B.CreateXor(A, C), B.CreateShl(B.CreateAnd(A, C), 1));
  case llvm::Instruction::Sub: // a - b == (a ^ b) - ((~a & b) << 1)
    return B.CreateSub(B.CreateXor(A, C),
                       B.CreateShl(B.CreateAnd(Not(A), C), 1));
  case llvm::Instruction::And: // a & b == ~(~a | ~b)
    return Not(B.CreateOr(Not(A), Not(C)));
  case llvm::Instruction::Or: // a | b == ~(~a & ~b)
    return Not(B.CreateAnd(Not(A), Not(C)));
  case llvm::Instruction::Xor: // a ^ b == (a & ~b) | (~a & b)
    return B.CreateOr(B.CreateAnd(A, Not(C)), B.CreateAnd(Not(A), C));
  default:
    return nullptr;
  }
}

// One substitution round over a snapshot of the currently-present operators, so
// the freshly-created replacement instructions are not re-substituted within
// the same round (that keeps a single round finite; more rounds compound on
// demand).
unsigned substituteRound(llvm::Module &M) {
  std::vector<llvm::BinaryOperator *> Worklist;
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (llvm::Instruction &I : llvm::instructions(F))
      if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I))
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
          if (BO->getType()->isIntOrIntVectorTy())
            Worklist.push_back(BO);
          break;
        default:
          break;
        }
  }

  unsigned Count = 0;
  for (llvm::BinaryOperator *BO : Worklist) {
    llvm::Value *Repl = substituteOne(BO);
    if (!Repl)
      continue;
    BO->replaceAllUsesWith(Repl);
    BO->eraseFromParent();
    ++Count;
  }
  return Count;
}

unsigned substituteModule(llvm::Module &M, unsigned Rounds) {
  if (Rounds == 0)
    Rounds = 1;
  unsigned Total = 0;
  for (unsigned R = 0; R < Rounds; ++R)
    Total += substituteRound(M);
  LLVM_DEBUG(llvm::dbgs() << "neverd: instruction substitution replaced "
                          << Total << " operator(s) over " << Rounds
                          << " round(s)\n");
  return Total;
}

} // namespace

llvm::PreservedAnalyses
InstSubstitutionPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = substituteModule(M, Rounds);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned InstSubstitutionPass::inject(llvm::Module &M, unsigned Rounds) {
  return substituteModule(M, Rounds);
}

} // namespace neverd
