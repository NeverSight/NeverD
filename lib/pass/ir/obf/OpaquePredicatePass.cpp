//===- OpaquePredicatePass.cpp - Opaque predicate pass -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Opaque-predicate pass: guards each basic block behind an always-true
/// predicate the backend cannot fold, with the dead false edge routed through a
/// harmless bogus block.  Demo-level sample transform — see
/// OpaquePredicatePass.h.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/OpaquePredicatePass.h"

#define DEBUG_TYPE "neverd-opaque-pred"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <vector>

namespace neverd {

namespace {

unsigned guardFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  llvm::LLVMContext &Ctx = F.getContext();
  auto *I32 = llvm::Type::getInt32Ty(Ctx);

  // Build the opaque value and always-true predicate once, at the top of the
  // entry block (it then dominates every block we guard).  cond ==
  //   ((x * (x + 1)) & 1) == 0
  // is true for every x (consecutive integers => even product, even under
  // modular wraparound); x is loaded from a volatile slot so the backend
  // cannot prove the predicate constant and fold the branch away.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::IRBuilder<> B(&Entry, Entry.getFirstInsertionPt());
  llvm::AllocaInst *Slot = B.CreateAlloca(I32, nullptr, "nd_op_slot");
  B.CreateStore(llvm::ConstantInt::get(I32, 0xC0DE), Slot, /*isVolatile=*/true);
  llvm::Value *X = B.CreateLoad(I32, Slot, /*isVolatile=*/true, "nd_op_x");
  llvm::Value *Prod = B.CreateMul(X, B.CreateAdd(X, B.getInt32(1)));
  llvm::Value *Lo = B.CreateAnd(Prod, B.getInt32(1));
  llvm::Value *Cond = B.CreateICmpEQ(Lo, B.getInt32(0), "nd_op_cond");
  auto *CondI = llvm::cast<llvm::Instruction>(Cond);

  // Snapshot the original blocks so the bogus/continuation blocks we create are
  // not themselves guarded.
  std::vector<llvm::BasicBlock *> Blocks;
  for (llvm::BasicBlock &BB : F)
    Blocks.push_back(&BB);

  unsigned Count = 0;
  for (llvm::BasicBlock *BB : Blocks) {
    if (BB->isEHPad())
      continue;
    // Choose the split point.  For non-entry blocks, after the PHIs.  For the
    // entry block, after BOTH the predicate setup (so cond dominates the guard)
    // AND every alloca (so they stay in the entry block).  Moving an alloca
    // into the split-off continuation would turn a static stack slot into a
    // *dynamic* allocation, which on Windows emits a `__chkstk` probe call to
    // an external routine the rewrite address model cannot resolve — and is
    // wasteful everywhere else.  This matters once other passes (e.g. constant
    // encryption) or the lifter put allocas in the entry block.
    llvm::Instruction *SplitPt;
    if (BB == &Entry) {
      llvm::Instruction *Anchor = CondI;
      for (llvm::Instruction &I : *BB)
        if (&I == CondI || llvm::isa<llvm::AllocaInst>(&I))
          Anchor = &I;
      SplitPt = Anchor->getNextNode();
    } else {
      SplitPt = &*BB->getFirstInsertionPt();
    }
    if (!SplitPt)
      continue;

    llvm::BasicBlock *Cont = llvm::SplitBlock(BB, SplitPt);

    // SplitBlock left BB ending in `br Cont`; replace it with an opaque branch
    // to a bogus block.  The bogus edge is never taken (cond is always true)
    // and is harmless even if it were — it only does a volatile store to the
    // dead slot and rejoins Cont — so the transform preserves semantics.
    auto *Bogus = llvm::BasicBlock::Create(Ctx, "nd_op_bogus", &F);
    {
      llvm::IRBuilder<> BB2(Bogus);
      BB2.CreateStore(llvm::ConstantInt::get(I32, 0xDEAD), Slot,
                      /*isVolatile=*/true);
      BB2.CreateBr(Cont);
    }
    BB->getTerminator()->eraseFromParent();
    llvm::IRBuilder<> BG(BB);
    BG.CreateCondBr(Cond, Cont, Bogus);
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: opaque predicate guarded " << Count
                          << " block(s) in " << F.getName() << "\n");
  return Count;
}

unsigned guardModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += guardFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses
OpaquePredicatePass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = guardModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned OpaquePredicatePass::inject(llvm::Module &M) { return guardModule(M); }

} // namespace neverd
