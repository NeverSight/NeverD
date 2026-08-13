//===- BogusControlFlowPass.cpp - Bogus control flow --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bogus-control-flow pass: grows a dead, opaque-guarded fake control-flow
/// sub-graph (two junk blocks) around each basic block.  Demo-level sample
/// transform — see BogusControlFlowPass.h.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/obf/BogusControlFlowPass.h"

#define DEBUG_TYPE "neverd-bogus-cf"
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

// Emit a self-contained junk arithmetic chain seeded from \p X (a value that
// dominates the insertion point) and constants, then store the result to the
// volatile \p Sink slot.  The volatile store is a side effect the backend
// cannot remove, so the whole chain survives to machine code.  Because the
// chain only uses \p X and constants it is valid wherever \p X dominates,
// independent of the host function's CFG.  \p Salt varies the constants so the
// two bogus blocks differ.
void emitJunk(llvm::IRBuilder<> &B, llvm::Value *X, llvm::AllocaInst *Sink,
              uint32_t Salt) {
  llvm::Value *J = B.CreateAdd(X, B.getInt32(Salt));
  J = B.CreateMul(J, B.getInt32(0x9E3779B1u ^ Salt));
  J = B.CreateXor(J, B.CreateLShr(J, B.getInt32(13)));
  J = B.CreateSub(J, B.CreateShl(X, B.getInt32(3)));
  J = B.CreateOr(J, B.getInt32(Salt | 1u));
  J = B.CreateAnd(J, B.getInt32(0x7FFFFFFFu));
  B.CreateStore(J, Sink, /*isVolatile=*/true);
}

unsigned bogusFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  llvm::LLVMContext &Ctx = F.getContext();
  auto *I32 = llvm::Type::getInt32Ty(Ctx);

  // Build the opaque value and always-true predicate once, at the top of the
  // entry block (it then dominates every block we touch).  cond ==
  //   ((x * (x + 1)) & 1) == 0
  // is true for every x (consecutive integers => even product, even under
  // modular wraparound); x is loaded from a volatile slot so the backend cannot
  // prove the predicate constant and fold the fake edges away.
  llvm::BasicBlock &Entry = F.getEntryBlock();
  llvm::IRBuilder<> B(&Entry, Entry.getFirstInsertionPt());
  llvm::AllocaInst *Slot = B.CreateAlloca(I32, nullptr, "nd_bcf_slot");
  B.CreateStore(llvm::ConstantInt::get(I32, 0xB06D), Slot, /*isVolatile=*/true);
  llvm::Value *X = B.CreateLoad(I32, Slot, /*isVolatile=*/true, "nd_bcf_x");
  llvm::Value *Prod = B.CreateMul(X, B.CreateAdd(X, B.getInt32(1)));
  llvm::Value *Lo = B.CreateAnd(Prod, B.getInt32(1));
  llvm::Value *Cond = B.CreateICmpEQ(Lo, B.getInt32(0), "nd_bcf_cond");
  auto *CondI = llvm::cast<llvm::Instruction>(Cond);

  // Snapshot the original blocks so the bogus blocks we create are not
  // themselves processed.
  std::vector<llvm::BasicBlock *> Blocks;
  for (llvm::BasicBlock &BB : F)
    Blocks.push_back(&BB);

  unsigned Count = 0;
  for (llvm::BasicBlock *BB : Blocks) {
    if (BB->isEHPad())
      continue;
    // Choose the split point.  For non-entry blocks, after the PHIs.  For the
    // entry block, after BOTH the predicate setup (so cond dominates the guard)
    // AND every alloca (so they stay in the entry block as static allocations —
    // moving one into the split-off continuation would turn it dynamic and emit
    // a Windows `__chkstk` probe the rewrite address model cannot resolve).
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

    // Two bogus blocks forming a fake control-flow sub-graph on the dead path.
    // Both ultimately rejoin Cont, and each real/fake fork is taken via an
    // always-true opaque branch, so the bogus blocks never actually run and
    // only ever write to a dead volatile slot — semantics are preserved
    // regardless.
    //
    //   BB     --[cond=true]--> Cont
    //          --[dead]-------> Bogus1
    //   Bogus1 (junk) --[cond=true]--> Cont
    //                 --[dead]-------> Bogus2
    //   Bogus2 (junk) ----------------> Cont
    //
    // BB dominates Cont/Bogus1/Bogus2 on every path, so no new PHIs are needed
    // and the junk (which only uses the entry-dominating X) stays valid SSA.
    auto *Bogus1 = llvm::BasicBlock::Create(Ctx, "nd_bcf_b1", &F);
    auto *Bogus2 = llvm::BasicBlock::Create(Ctx, "nd_bcf_b2", &F);
    {
      llvm::IRBuilder<> JB(Bogus1);
      emitJunk(JB, X, Slot, 0x1111u);
      JB.CreateCondBr(Cond, Cont, Bogus2);
    }
    {
      llvm::IRBuilder<> JB(Bogus2);
      emitJunk(JB, X, Slot, 0x2222u);
      JB.CreateBr(Cont);
    }

    BB->getTerminator()->eraseFromParent();
    llvm::IRBuilder<>(BB).CreateCondBr(Cond, Cont, Bogus1);
    ++Count;
  }

  LLVM_DEBUG(llvm::dbgs() << "neverd: bogus control flow added to " << Count
                          << " block(s) in " << F.getName() << "\n");
  return Count;
}

unsigned bogusModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += bogusFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses
BogusControlFlowPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = bogusModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned BogusControlFlowPass::inject(llvm::Module &M) {
  return bogusModule(M);
}

} // namespace neverd
