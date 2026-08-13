//===- MBAPass.cpp - Mixed boolean-arithmetic obfuscation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MBA pass: injects a provably-zero mixed-boolean-arithmetic term into every
/// eligible integer operator result.  Demo-level sample transform — see
/// MBAPass.h.
///
/// For `r = a OP b` (OP in add/sub/mul/and/or/xor, scalar or integer vector) we
/// emit:
///
///   z  = ((a ^ b) + ((a & b) << 1)) - a - b      ; ≡ 0 for all a, b
///   r' = r + z                                    ; ≡ r
///
/// and route every use of `r` through `r'`.  The added term is zero because the
/// carry-save identity `(a ^ b) + 2·(a & b) == a + b` holds for
/// two's-complement integers (element-wise for vectors), so `z == (a + b) - a -
/// b == 0`.
///
/// That carry-save sub-term is the same construct InstSubstitutionPass relies
/// on to survive the backend's algebraic simplification (DAGCombine does not
/// recognise `(a ^ b) + 2·(a & b)` as `a + b`), so the injected term reaches
/// the emitted machine code rather than being folded away — making the
/// obfuscation visible while keeping the program's behaviour identical.
///
/// Distinct from InstSubstitutionPass: that pass replaces an operator with an
/// equivalent expression tree; this one appends a zero-valued MBA polynomial.
/// A pure IR transform, orthogonal to the relocation/rewrite backend.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/MBAPass.h"

#define DEBUG_TYPE "neverd-mba"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace neverd {

namespace {

// Build the MBA zero-identity ((x ^ y) + ((x & y) << 1)) - x - y, which equals
// zero for all x, y.  The carry-save sub-term (x ^ y) + ((x & y) << 1) is the
// one InstSubstitutionPass proves survives the backend, so this whole term
// reaches the machine code instead of being folded to 0.
llvm::Value *buildMBAZero(llvm::IRBuilder<> &B, llvm::Value *X,
                          llvm::Value *Y) {
  llvm::Value *Carry =
      B.CreateAdd(B.CreateXor(X, Y), B.CreateShl(B.CreateAnd(X, Y), 1));
  return B.CreateSub(B.CreateSub(Carry, X), Y);
}

bool isEligible(llvm::BinaryOperator *BO) {
  if (!BO->getType()->isIntOrIntVectorTy())
    return false;
  switch (BO->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    return true;
  default:
    return false;
  }
}

unsigned mbaFunction(llvm::Function &F) {
  if (F.isDeclaration())
    return 0;

  // Snapshot the eligible operators up front so the freshly-created terms are
  // not themselves wrapped within the same run (keeps it finite: one wrap per
  // original operator).
  std::vector<llvm::BinaryOperator *> Worklist;
  for (llvm::Instruction &I : llvm::instructions(F))
    if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I))
      if (isEligible(BO))
        Worklist.push_back(BO);

  unsigned Count = 0;
  for (llvm::BinaryOperator *BO : Worklist) {
    // Insert the wrap right after BO (BinaryOperators are never terminators, so
    // getNextNode() is always a valid instruction to insert before).
    llvm::IRBuilder<> B(BO->getNextNode());
    llvm::Value *Z = buildMBAZero(B, BO->getOperand(0), BO->getOperand(1));
    llvm::Value *Wrapped = B.CreateAdd(BO, Z);

    // Route every existing use of BO through the wrapped value, then restore
    // the wrap's own use of BO (replaceAllUsesWith just rewrote it to itself).
    // BO's operands (used by Z) are defined before BO, so they are never BO
    // itself and are unaffected.
    BO->replaceAllUsesWith(Wrapped);
    llvm::cast<llvm::Instruction>(Wrapped)->setOperand(0, BO);
    ++Count;
  }
  return Count;
}

unsigned mbaModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += mbaFunction(F);
  LLVM_DEBUG(llvm::dbgs() << "neverd: MBA wrapped " << Total
                          << " operator(s)\n");
  return Total;
}

} // namespace

llvm::PreservedAnalyses MBAPass::run(llvm::Module &M,
                                     llvm::ModuleAnalysisManager &) {
  unsigned N = mbaModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned MBAPass::inject(llvm::Module &M) { return mbaModule(M); }

} // namespace neverd
