//===- ControlFlowRecoveryPass.cpp - Undo dispatcher-loop flattening -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Threads a jump through the state slot a dispatcher loop switches on.
///
/// The whole transform rests on one observation, and it is worth stating
/// precisely because everything else here is the work of checking it holds.
///
/// Let P end by storing a known value to slot S and jumping to D, and let D
/// begin by loading S and switching on it.  If nothing between that store and
/// that load can change S, then the case D selects when it is entered from P is
/// already decided in P, and P may jump to it directly.
///
/// "Nothing can change S" is the part that has to be earned.  It is earned by
/// the slot never having its address taken -- then only the loads and stores
/// naming it can write it, and they can be enumerated -- together with taking
/// the *last* store in P and requiring D to load before it stores.  None of
/// this asks whether the code is obfuscated, which is why the result is a
/// rewrite that is simply correct rather than one that is correct if a guess
/// about the shape was right.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/ControlFlowRecoveryPass.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"

#include <optional>

namespace neverd {

namespace {

/// True when \p Slot is only ever loaded from and stored to.
///
/// This is what makes the writers to a slot enumerable.  Once its address
/// reaches anything else -- a call, a cast that is then stored, a comparison --
/// some other instruction may write it and the reasoning below has no basis.
bool isPlainSlot(const llvm::AllocaInst *Slot) {
  for (const llvm::User *U : Slot->users()) {
    if (const auto *L = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (L->isVolatile() || L->getPointerOperand() != Slot)
        return false;
      continue;
    }
    if (const auto *S = llvm::dyn_cast<llvm::StoreInst>(U)) {
      // Storing the pointer itself is the address escaping by another name.
      if (S->isVolatile() || S->getPointerOperand() != Slot ||
          S->getValueOperand() == Slot)
        return false;
      continue;
    }
    return false;
  }
  return true;
}

/// A block that does nothing but read a slot and switch on what it finds.
struct Dispatcher {
  llvm::SwitchInst *Switch = nullptr;
  llvm::AllocaInst *Slot = nullptr;
};

std::optional<Dispatcher> readDispatcher(llvm::BasicBlock &BB) {
  auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(BB.getTerminator());
  if (!Switch)
    return std::nullopt;

  auto *Load = llvm::dyn_cast<llvm::LoadInst>(Switch->getCondition());
  if (!Load || Load->getParent() != &BB || Load->isVolatile())
    return std::nullopt;

  auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Load->getPointerOperand());
  if (!Slot || !isPlainSlot(Slot))
    return std::nullopt;

  // A phi here would have to lose an incoming edge for every predecessor that
  // stops arriving, and its value would have to be made available where that
  // predecessor now jumps instead.  Nothing produces one in the shape this
  // exists for, so it is refused rather than guessed at.
  if (!BB.phis().empty())
    return std::nullopt;

  // The value the switch reads has to be the value a predecessor left, so
  // nothing may write the slot between the top of the block and the load.
  for (llvm::Instruction &I : BB) {
    if (&I == Load)
      break;
    if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
      if (S->getPointerOperand() == Slot)
        return std::nullopt;
  }
  return Dispatcher{Switch, Slot};
}

/// Where \p Switch goes when its condition is \p Value.  A value naming no case
/// takes the default, which is the same answer the switch itself would give.
llvm::BasicBlock *caseFor(llvm::SwitchInst *Switch, const llvm::APInt &Value) {
  for (const llvm::SwitchInst::CaseHandle &Case : Switch->cases())
    if (Case.getCaseValue()->getValue() == Value)
      return Case.getCaseSuccessor();
  return Switch->getDefaultDest();
}

/// The last value \p BB writes to \p Slot, which is the one that survives.
llvm::StoreInst *lastStoreTo(llvm::BasicBlock &BB, llvm::AllocaInst *Slot) {
  llvm::StoreInst *Last = nullptr;
  for (llvm::Instruction &I : BB)
    if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
      if (S->getPointerOperand() == Slot)
        Last = S;
  return Last;
}

/// True when an edge into \p Target may be added from somewhere new.
bool canReceiveANewEdge(const llvm::BasicBlock *Target) {
  // A phi would need an incoming value for the new predecessor, and the value
  // it holds for the dispatcher is not one that is available there.
  return Target->phis().empty();
}

/// Rewrite \p Pred's jump into the dispatcher as a jump to the case it selects.
bool threadThroughSlot(llvm::BasicBlock &Pred, const Dispatcher &D) {
  auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Pred.getTerminator());
  if (!Br || Br->getSuccessor(0) != D.Switch->getParent())
    return false;

  llvm::StoreInst *Store = lastStoreTo(Pred, D.Slot);
  if (!Store)
    return false;

  llvm::Value *Written = Store->getValueOperand();
  llvm::Value *Condition = nullptr;
  llvm::BasicBlock *IfTrue = nullptr;
  llvm::BasicBlock *IfFalse = nullptr;

  if (auto *Number = llvm::dyn_cast<llvm::ConstantInt>(Written)) {
    IfTrue = caseFor(D.Switch, Number->getValue());
  } else if (auto *Choice = llvm::dyn_cast<llvm::SelectInst>(Written)) {
    // What a conditional branch becomes when it is flattened: the two block
    // numbers, picked between by the condition the branch used to test.
    auto *Taken = llvm::dyn_cast<llvm::ConstantInt>(Choice->getTrueValue());
    auto *NotTaken = llvm::dyn_cast<llvm::ConstantInt>(Choice->getFalseValue());
    if (!Taken || !NotTaken)
      return false;
    Condition = Choice->getCondition();
    IfTrue = caseFor(D.Switch, Taken->getValue());
    IfFalse = caseFor(D.Switch, NotTaken->getValue());
  } else {
    return false;
  }

  if (!canReceiveANewEdge(IfTrue) || (IfFalse && !canReceiveANewEdge(IfFalse)))
    return false;

  llvm::IRBuilder<> B(Br);
  B.SetCurrentDebugLocation(Br->getDebugLoc());
  if (Condition)
    B.CreateCondBr(Condition, IfTrue, IfFalse);
  else
    B.CreateBr(IfTrue);
  Br->eraseFromParent();

  // The store fed nothing but the dispatcher, and the select fed nothing but
  // the store; with the jump decided here neither has a reader left.
  llvm::Value *Fed = Store->getValueOperand();
  Store->eraseFromParent();
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(Fed))
    llvm::RecursivelyDeleteTriviallyDeadInstructions(I);
  return true;
}

} // namespace

unsigned ControlFlowRecoveryPass::recover(llvm::Function &F) {
  unsigned Threaded = 0;
  // Collected first: threading rewrites terminators, and a dispatcher found
  // while the graph is being changed underneath the walk is not worth the care
  // it would take to keep valid.
  llvm::SmallVector<Dispatcher, 4> Dispatchers;
  for (llvm::BasicBlock &BB : F)
    if (std::optional<Dispatcher> D = readDispatcher(BB))
      Dispatchers.push_back(*D);

  for (const Dispatcher &D : Dispatchers) {
    llvm::BasicBlock *Block = D.Switch->getParent();
    // Snapshot the predecessors: each one that is threaded stops being one.
    llvm::SmallVector<llvm::BasicBlock *, 8> Preds(llvm::predecessors(Block));
    for (llvm::BasicBlock *Pred : Preds)
      if (threadThroughSlot(*Pred, D))
        ++Threaded;
  }
  return Threaded;
}

llvm::PreservedAnalyses
ControlFlowRecoveryPass::run(llvm::Function &F, llvm::FunctionAnalysisManager &) {
  return recover(F) == 0 ? llvm::PreservedAnalyses::all()
                         : llvm::PreservedAnalyses::none();
}

} // namespace neverd
