//===- ControlFlowFlatteningPass.cpp - Control-flow flattening -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow flattening pass: rebuilds each function's CFG as a dispatcher
/// loop (state variable + central switch).  Demo-level sample transform — see
/// ControlFlowFlatteningPass.h.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/ControlFlowFlatteningPass.h"

#define DEBUG_TYPE "neverd-cff"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h" // DemoteRegToStack / DemotePHIToStack

#include <vector>

namespace neverd {

namespace {

// Spill PHIs and cross-block SSA values to stack slots in the entry block so
// the flattened CFG stays valid SSA: once every block is reached only through
// the dispatcher, no original block dominates another, so any value used across
// a block boundary must travel through memory.  This is the classic
// reg2mem-before-flatten step.  The entry block's allocas stay static (the
// allocas are inserted before the entry terminator), so no Windows __chkstk
// stack probe is emitted.
void fixStack(llvm::Function &F) {
  std::vector<llvm::PHINode *> Phis;
  std::vector<llvm::Instruction *> Regs;
  do {
    Phis.clear();
    Regs.clear();
    for (llvm::BasicBlock &BB : F)
      for (llvm::Instruction &I : BB) {
        if (auto *P = llvm::dyn_cast<llvm::PHINode>(&I)) {
          Phis.push_back(P);
          continue;
        }
        if (!llvm::isa<llvm::AllocaInst>(&I) && I.isUsedOutsideOfBlock(&BB))
          Regs.push_back(&I);
      }
    // Let the demotion helpers default the alloca to the entry block's start
    // (F.getEntryBlock().begin()).  That slot dominates every use AND, being in
    // the entry block, stays a static alloca (no Windows __chkstk probe).  We
    // must NOT anchor it at the entry terminator: a value defined in the entry
    // block gets its store placed right after the definition — which would land
    // *before* a terminator-anchored alloca and break dominance.
    for (llvm::PHINode *P : Phis)
      llvm::DemotePHIToStack(P);
    for (llvm::Instruction *I : Regs)
      llvm::DemoteRegToStack(*I, /*VolatileLoads=*/false);
  } while (!Phis.empty() || !Regs.empty());
}

// Flatten one function.  Returns the number of blocks moved into the dispatcher
// (0 if the function was left unchanged).
unsigned flattenFunction(llvm::Function &F) {
  if (F.isDeclaration() || F.size() < 2)
    return 0;

  // Only flatten functions whose every block ends in a (conditional or
  // unconditional) branch or a function-terminating instruction.  Anything else
  // (switch / invoke / indirectbr / callbr) is left untouched — this keeps the
  // demo robust without a switch-lowering pre-pass; the state-variable rewrite
  // below only knows how to re-target br terminators.
  for (llvm::BasicBlock &BB : F) {
    llvm::Instruction *T = BB.getTerminator();
    if (!T)
      return 0;
    if (!llvm::isa<llvm::UncondBrInst>(T) && !llvm::isa<llvm::CondBrInst>(T) &&
        !llvm::isa<llvm::ReturnInst>(T) && !llvm::isa<llvm::UnreachableInst>(T))
      return 0;
  }

  llvm::LLVMContext &Ctx = F.getContext();
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  llvm::BasicBlock *Entry = &F.getEntryBlock();

  // The entry block must end in a branch so we can redirect it to the
  // dispatcher.  (A function whose entry returns directly has nothing to
  // flatten.)
  if (!llvm::isa<llvm::UncondBrInst>(Entry->getTerminator()) &&
      !llvm::isa<llvm::CondBrInst>(Entry->getTerminator()))
    return 0;

  // If the entry ends in a conditional branch, split it so the entry ends in an
  // unconditional branch; the conditional part becomes an ordinary flattened
  // block.  splitBasicBlock updates successor PHIs to refer to the new block.
  if (llvm::isa<llvm::CondBrInst>(Entry->getTerminator()))
    Entry->splitBasicBlock(Entry->getTerminator()->getIterator(),
                           "nd_cff_first");

  // The block the (now unconditional) entry branch targets becomes the first
  // dispatched case.
  llvm::BasicBlock *FirstReal =
      llvm::cast<llvm::UncondBrInst>(Entry->getTerminator())->getSuccessor(0);

  // Every block except the entry is flattened.
  std::vector<llvm::BasicBlock *> Blocks;
  for (llvm::BasicBlock &BB : F)
    if (&BB != Entry)
      Blocks.push_back(&BB);
  if (Blocks.empty())
    return 0;

  // Assign each flattened block a case id.  A fixed per-block offset keeps the
  // ids from being a trivial 0,1,2,... sequence while remaining deterministic.
  llvm::DenseMap<llvm::BasicBlock *, uint32_t> CaseOf;
  for (uint32_t I = 0; I < Blocks.size(); ++I)
    CaseOf[Blocks[I]] = (I * 0x9E3779B1u) ^ 0xC0FFEE; // any 1:1 mapping is fine

  // State variable + dispatcher block.
  auto *Dispatch = llvm::BasicBlock::Create(Ctx, "nd_cff_dispatch", &F);

  llvm::Instruction *EntryTerm = Entry->getTerminator();
  // Allocate the state slot at the entry block's first insertion point so it
  // joins the leading static-alloca group.  An alloca emitted *after* other
  // entry-block instructions is still `isStaticAlloca()`, but if those
  // preceding instructions include atomics (atomicrmw / cmpxchg) the X86
  // Windows backend lowers this slot as a DYNAMIC alloca — which emits a
  // `__chkstk` stack probe.
  // `__chkstk` is an external symbol the rewrite image cannot resolve, so on
  // COFF it becomes a bad call target at runtime (x64) or a hard
  // "PC-relative fixup with a non-zero symbol offset" codegen error (AArch64 /
  // ARM32).  Anchoring it at entry-begin (like fixStack's reg2mem slots) keeps
  // it unambiguously static on every target.
  llvm::IRBuilder<> AB(&*Entry->getFirstInsertionPt());
  auto *StateSlot = AB.CreateAlloca(I32, nullptr, "nd_cff_state");
  llvm::IRBuilder<> EB(EntryTerm);
  EB.CreateStore(llvm::ConstantInt::get(I32, CaseOf[FirstReal]), StateSlot);
  EntryTerm->eraseFromParent();
  llvm::IRBuilder<>(Entry).CreateBr(Dispatch);

  // Dispatcher: load the state and switch to the matching block.  The default
  // (never taken — the state is always a valid case) points at the first block.
  llvm::IRBuilder<> DB(Dispatch);
  auto *StateVal = DB.CreateLoad(I32, StateSlot, "nd_cff_sv");
  auto *SW = DB.CreateSwitch(StateVal, FirstReal, Blocks.size());
  for (llvm::BasicBlock *BB : Blocks)
    SW->addCase(
        llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(I32, CaseOf[BB])),
        BB);

  // Rewrite each flattened block's branch terminator: set the state to the
  // successor's case id and jump back to the dispatcher.  ret / unreachable
  // terminators are left alone (they end the function / the dispatcher loop).
  unsigned Flattened = 0;
  for (llvm::BasicBlock *BB : Blocks) {
    llvm::Instruction *T = BB->getTerminator();
    // ret / unreachable terminators end the function (or the dispatcher loop):
    // leave them untouched, but still count the block as dispatched.
    if (llvm::isa<llvm::ReturnInst>(T) || llvm::isa<llvm::UnreachableInst>(T)) {
      ++Flattened;
      continue;
    }
    llvm::IRBuilder<> TB(T);
    llvm::Value *Next;
    if (auto *UB = llvm::dyn_cast<llvm::UncondBrInst>(T)) {
      Next = llvm::ConstantInt::get(I32, CaseOf[UB->getSuccessor(0)]);
    } else {
      auto *CB = llvm::cast<llvm::CondBrInst>(T);
      Next = TB.CreateSelect(
          CB->getCondition(),
          llvm::ConstantInt::get(I32, CaseOf[CB->getSuccessor(0)]),
          llvm::ConstantInt::get(I32, CaseOf[CB->getSuccessor(1)]),
          "nd_cff_next");
    }
    TB.CreateStore(Next, StateSlot);
    T->eraseFromParent();
    llvm::IRBuilder<>(BB).CreateBr(Dispatch);
    ++Flattened;
  }

  // Repair the SSA form the flattening broke.
  fixStack(F);

  LLVM_DEBUG(llvm::dbgs() << "neverd: flattened " << Flattened
                          << " block(s) in " << F.getName() << "\n");
  return Flattened;
}

unsigned flattenModule(llvm::Module &M) {
  unsigned Total = 0;
  for (llvm::Function &F : M)
    Total += flattenFunction(F);
  return Total;
}

} // namespace

llvm::PreservedAnalyses
ControlFlowFlatteningPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  unsigned N = flattenModule(M);
  return N ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

unsigned ControlFlowFlatteningPass::inject(llvm::Module &M) {
  return flattenModule(M);
}

} // namespace neverd
