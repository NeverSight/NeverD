//===- LLVMCFuncWriter.cpp - LLVM IR function-level rendering --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function-level orchestration for the LLVM IR C emitter: analysis pass
/// scheduling, local variable declaration emission, block scanning, and
/// function signature rendering.  Instruction-level rendering lives in
/// LLVMCStmtWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/MsvcAtlCallee.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionEncoding.h"
#include "neverd/loader/ExceptionPersonality.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool isLifetimeOrDbgUser(const llvm::User *U) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(U))
    return true;
  const auto *CB = llvm::dyn_cast<llvm::CallBase>(U);
  if (!CB)
    return false;
  const auto IID = CB->getIntrinsicID();
  return IID == llvm::Intrinsic::lifetime_start ||
         IID == llvm::Intrinsic::lifetime_end ||
         IID == llvm::Intrinsic::donothing;
}

bool isKnownWindowsPersonality(llvm::StringRef Name) {
  Name.consume_front("#");
  return Name.contains("C_specific_handler") ||
         Name.contains("CxxFrameHandler") || Name.contains("GSHandlerCheck") ||
         Name.contains("except_handler") ||
         Name.ends_with("_personality_seh0") ||
         Name == "_GCC_specific_handler" || Name == "__gnat_personality_imp" ||
         Name == "ProcessCLRException";
}

bool isKnownWindowsPersonality(const llvm::Value *Personality) {
  llvm::SmallPtrSet<const llvm::Value *, 8> Visited;
  const llvm::Value *Current = Personality;
  while (Current) {
    Current = Current->stripPointerCasts();
    if (!Visited.insert(Current).second)
      return true;

    if (const auto *Named = llvm::dyn_cast<llvm::GlobalValue>(Current))
      if (isKnownWindowsPersonality(Named->getName()))
        return true;

    if (const auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Current)) {
      Current = Alias->getAliasee();
      continue;
    }

    // An ifunc selects its implementation by executing arbitrary resolver
    // code.  C source cannot prove which personality it returns, so the only
    // safe executable projection is the analysis-only trap.
    if (llvm::isa<llvm::GlobalIFunc>(Current))
      return true;

    switch (llvm::classifyEHPersonality(Current)) {
    case llvm::EHPersonality::MSVC_X86SEH:
    case llvm::EHPersonality::MSVC_TableSEH:
    case llvm::EHPersonality::MSVC_CXX:
    case llvm::EHPersonality::CoreCLR:
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool isWindowsEHIntrinsic(llvm::StringRef Name) {
  return Name == "llvm.eh.actions" || Name == "llvm.eh.exceptioncode" ||
         Name == "llvm.eh.recoverfp" || Name == "llvm.localescape" ||
         Name == "llvm.localrecover";
}

bool metadataReferencesFunction(
    const llvm::Metadata *MD, const llvm::Function &Fn,
    llvm::SmallPtrSetImpl<const llvm::Metadata *> &Visited) {
  if (!MD || !Visited.insert(MD).second)
    return false;
  if (const auto *ValueMD = llvm::dyn_cast<llvm::ValueAsMetadata>(MD))
    return ValueMD->getValue()->stripPointerCasts() == &Fn;
  const auto *Node = llvm::dyn_cast<llvm::MDNode>(MD);
  if (!Node)
    return false;
  for (const llvm::MDOperand &Operand : Node->operands())
    if (metadataReferencesFunction(Operand.get(), Fn, Visited))
      return true;
  return false;
}

} // anonymous namespace

bool LLVMCWriter::isAnalysisOnlyFunction(const llvm::Function &Fn) const {
  if (Fn.getMetadata(windows_eh_md::FunctionAttachment) ||
      Fn.getMetadata(windows_eh_md::NativeAttachment))
    return true;

  if (const llvm::Module *Module = Fn.getParent()) {
    if (const llvm::NamedMDNode *Table =
            Module->getNamedMetadata(windows_eh_md::FunctionTable)) {
      for (const llvm::MDNode *Row : Table->operands()) {
        llvm::SmallPtrSet<const llvm::Metadata *, 16> Visited;
        if (metadataReferencesFunction(Row, Fn, Visited))
          return true;
      }
    }
  }

  bool HasWindowsIREvidence = false;
  for (const llvm::BasicBlock &BB : Fn) {
    for (const llvm::Instruction &Inst : BB) {
      HasWindowsIREvidence |=
          llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CatchReturnInst, llvm::CleanupPadInst,
                    llvm::CleanupReturnInst>(&Inst);
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        HasWindowsIREvidence |= Call->countOperandBundlesOfType(
                                    windows_eh_md::ProvenanceBundle) != 0;
        if (const llvm::Function *Callee = Call->getCalledFunction())
          HasWindowsIREvidence |= isWindowsEHIntrinsic(Callee->getName());
      }
    }
  }
  if (HasWindowsIREvidence)
    return true;

  if (!Fn.hasPersonalityFn())
    return false;
  if (isKnownWindowsPersonality(Fn.getPersonalityFn()))
    return true;

  const llvm::Module *Module = Fn.getParent();
  return Module && !Module->getTargetTriple().empty() &&
         llvm::Triple(Module->getTargetTriple()).isOSWindows();
}

bool LLVMCWriter::isReferencedByExecutableProjection(
    const llvm::Function &Fn) const {
  llvm::SmallVector<const llvm::User *, 16> Worklist;
  llvm::SmallPtrSet<const llvm::User *, 32> Visited;
  for (const llvm::User *User : Fn.users())
    Worklist.push_back(User);

  while (!Worklist.empty()) {
    const llvm::User *User = Worklist.pop_back_val();
    if (!Visited.insert(User).second)
      continue;
    if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(User)) {
      const llvm::Function *Owner = Inst->getFunction();
      if (Owner && !isAnalysisOnlyFunction(*Owner))
        return true;
      continue;
    }
    if (const auto *Owner = llvm::dyn_cast<llvm::Function>(User)) {
      if (!Owner->isDeclaration() && !isAnalysisOnlyFunction(*Owner))
        return true;
      continue;
    }
    if (llvm::isa<llvm::GlobalAlias, llvm::GlobalIFunc>(User)) {
      for (const llvm::User *Next : User->users())
        Worklist.push_back(Next);
      continue;
    }
    if (llvm::isa<llvm::GlobalValue>(User))
      return true;
    for (const llvm::User *Next : User->users())
      Worklist.push_back(Next);
  }
  return false;
}

void LLVMCWriter::scanReferencedBlocks(llvm::Function &Fn) {
  ReferencedBlocks.clear();
  ConditionChainBodies.clear();
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> FoldedArms;
  for (auto &BB : Fn) {
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator());
    if (!Br)
      continue;
    const llvm::BasicBlock *Then = Br->getSuccessor(0);
    const llvm::BasicBlock *Else = Br->getSuccessor(1);
    const llvm::BasicBlock *ThenJoin = straightAssignArmJoin(Then);
    const llvm::BasicBlock *ElseJoin = straightAssignArmJoin(Else);
    if (Then && Else && Then != Else && ThenJoin && ThenJoin == ElseJoin) {
      FoldedArms.insert(Then);
      FoldedArms.insert(Else);
    }
  }
  auto Note = [&](const llvm::BasicBlock *Dest) {
    if (const llvm::BasicBlock *Printed = printBranchTarget(Dest))
      ReferencedBlocks.insert(Printed);
  };
  for (auto &BB : Fn) {
    llvm::Instruction *Term = BB.getTerminator();
    if (!Term)
      continue;
    if (auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
      if (ConditionChainBodies.count(&BB) ||
          DuplicatedAssignBlocks.count(&BB) ||
          InlinedFallthroughBlocks.count(&BB))
        continue;
      const llvm::BasicBlock *Target = Br->getSuccessor(0);
      if (const llvm::BasicBlock *Pred = BB.getSinglePredecessor()) {
        if (elseBodyFallsIntoNext(Pred, &BB))
          continue;
        if (straightLineTrueArm(Pred, &BB, true)) {
          if (const auto *Out =
                  llvm::dyn_cast<llvm::UncondBrInst>(BB.getTerminator()))
            if (!joinPrintsNext(Pred, Out->getSuccessor(0)))
              Note(Out->getSuccessor(0));
          continue;
        }
        if (!FoldedArms.count(&BB) && straightLineFalseSkip(Pred, &BB)) {
          if (const auto *Out =
                  llvm::dyn_cast<llvm::UncondBrInst>(BB.getTerminator())) {
            const auto *FromBr =
                llvm::cast<llvm::CondBrInst>(Pred->getTerminator());
            const llvm::BasicBlock *Target = Out->getSuccessor(0);
            const auto *Nested =
                Target ? llvm::dyn_cast<llvm::CondBrInst>(
                             Target->getTerminator())
                       : nullptr;
            const bool InlinedCheck =
                Nested && Target->getSinglePredecessor() == &BB &&
                straightLineFalseSkip(Target, Nested->getSuccessor(1)) &&
                printBranchTarget(Nested->getSuccessor(0)) ==
                    printBranchTarget(FromBr->getSuccessor(0));
            if (!InlinedCheck &&
                printBranchTarget(Target) !=
                    printBranchTarget(FromBr->getSuccessor(0)))
              Note(Target);
          }
          continue;
        }
      }
      if (FoldedArms.count(&BB) || isPrintPassthrough(&BB) ||
          uncondBranchFallsIntoEHBoundary(&BB, Target) ||
          joinPrintsNext(&BB, Target))
        continue;
      if (const llvm::BasicBlock *Printed = printBranchTarget(Target))
        if (InlinedFallthroughBlocks.count(Printed) ||
            DuplicatedAssignBlocks.count(Printed))
          continue;
      Note(Target);
      continue;
    }
    if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Term)) {
      if (ConditionChainBodies.count(&BB) ||
          InlinedFallthroughBlocks.count(&BB))
        continue;
      std::vector<const llvm::BasicBlock *> ChainConds;
      const llvm::BasicBlock *ChainArm = nullptr;
      const llvm::BasicBlock *ChainDef = nullptr;
      const llvm::BasicBlock *ChainJoin = nullptr;
      bool ChainInvert = false;
      if (conditionChainElse(&BB, ChainConds, ChainArm, ChainDef, ChainJoin,
                             ChainInvert)) {
        for (size_t I = 1; I < ChainConds.size(); ++I)
          ConditionChainBodies.insert(ChainConds[I]);
        ConditionChainBodies.insert(ChainArm);
        ConditionChainBodies.insert(ChainDef);
        if (ChainJoin && !joinPrintsNext(&BB, ChainJoin))
          Note(ChainJoin);
        continue;
      }
      if (const llvm::BasicBlock *Pred = BB.getSinglePredecessor())
        if (andRejoinTest(Pred, &BB))
          continue;
      const llvm::BasicBlock *Then = Br->getSuccessor(0);
      const llvm::BasicBlock *Else = Br->getSuccessor(1);
      const llvm::BasicBlock *ThenJoin = straightAssignArmJoin(Then);
      const llvm::BasicBlock *ElseJoin = straightAssignArmJoin(Else);
      if (Then && Else && Then != Else && ThenJoin && ThenJoin == ElseJoin) {
        FoldedJoinArms.insert(Then);
        FoldedJoinArms.insert(Else);
        if (!joinPrintsNext(&BB, ThenJoin))
          Note(ThenJoin);
        FoldedJoinArms.erase(Then);
        FoldedJoinArms.erase(Else);
        continue;
      }
      auto NoteTrue = [&](const llvm::BasicBlock *Edge) {
        if (const llvm::BasicBlock *Body = straightLineTrueArm(&BB, Edge, true)) {
          if (const auto *Out = llvm::dyn_cast<llvm::UncondBrInst>(
                  Body->getTerminator())) {
            llvm::SmallVector<const llvm::BasicBlock *, 4> Chain;
            if (singleEntryUncondTail(&BB, Body, Out->getSuccessor(0), Chain)) {
              for (const llvm::BasicBlock *Tail : Chain)
                InlinedFallthroughBlocks.insert(Tail);
              if (const auto *TailOut = llvm::dyn_cast<llvm::UncondBrInst>(
                      Chain.back()->getTerminator()))
                if (!joinPrintsNext(&BB, TailOut->getSuccessor(0)))
                  Note(TailOut->getSuccessor(0));
              return;
            }
            if (!joinPrintsNext(&BB, Out->getSuccessor(0)))
              Note(Out->getSuccessor(0));
          }
          return;
        }
        const llvm::BasicBlock *Region = nullptr;
        llvm::SmallVector<const llvm::BasicBlock *, 4> TrueChain;
        llvm::SmallVector<const llvm::BasicBlock *, 4> FalseChain;
        if (singleEntryCondRegion(&BB, Edge, Region, TrueChain, FalseChain)) {
          DuplicatedAssignBlocks.insert(Region);
          for (const llvm::BasicBlock *Tail : TrueChain)
            DuplicatedAssignBlocks.insert(Tail);
          for (const llvm::BasicBlock *Tail : FalseChain)
            DuplicatedAssignBlocks.insert(Tail);
          if (const auto *TailOut = llvm::dyn_cast<llvm::UncondBrInst>(
                  TrueChain.back()->getTerminator()))
            if (!joinPrintsNext(&BB, TailOut->getSuccessor(0)))
              Note(TailOut->getSuccessor(0));
          return;
        }
        Note(Edge);
      };
      if (elseBodyFallsIntoNext(&BB, Else)) {
        NoteTrue(Then);
        continue;
      }
      if (Else && Then != Else && joinPrintsNext(&BB, Else) &&
          !edgePrintsPhiCopy(&BB, Else)) {
        if (const llvm::BasicBlock *SkipBody =
                straightLineFalseSkip(&BB, Else)) {
          llvm::SmallVector<const llvm::BasicBlock *, 4> SelConds;
          llvm::SmallVector<const llvm::BasicBlock *, 4> SelArms;
          const llvm::BasicBlock *SelDef = nullptr;
          const llvm::BasicBlock *SelJoin = nullptr;
          if (assignSelectElseIf(&BB, SkipBody, SelConds, SelArms, SelDef,
                                 SelJoin))
            continue;
          const llvm::BasicBlock *Alt = nullptr;
          const llvm::BasicBlock *Def = nullptr;
          const llvm::BasicBlock *Join = nullptr;
          if (sharedAssignElse(&BB, SkipBody, Alt, Def, Join) ||
              DuplicatedAssignBlocks.count(SkipBody)) {
            if (Alt)
              DuplicatedAssignBlocks.insert(Alt);
            if (Def)
              DuplicatedAssignBlocks.insert(Def);
            continue;
          }
          if (const auto *Out =
                  llvm::dyn_cast<llvm::UncondBrInst>(Else->getTerminator()))
            if (printBranchTarget(Out->getSuccessor(0)) !=
                printBranchTarget(Then))
              Note(Out->getSuccessor(0));
          continue;
        }
        if (const llvm::BasicBlock *And = andRejoinTest(&BB, Else)) {
          const auto *AndBr =
              llvm::cast<llvm::CondBrInst>(And->getTerminator());
          if (const llvm::BasicBlock *Body =
                  straightLineTrueArm(And, AndBr->getSuccessor(0))) {
            if (const auto *Out = llvm::dyn_cast<llvm::UncondBrInst>(
                    Body->getTerminator()))
              if (!joinPrintsNext(And, Out->getSuccessor(0)))
                Note(Out->getSuccessor(0));
          }
          continue;
        }
        NoteTrue(Then);
        continue;
      }
      NoteTrue(Then);
      if (Else && Else != Then)
        Note(Else);
      continue;
    }
    if (auto *SW = llvm::dyn_cast<llvm::SwitchInst>(Term)) {
      Note(SW->getDefaultDest());
      for (const auto &C : SW->cases())
        Note(C.getCaseSuccessor());
      continue;
    }
    if (auto *CR = llvm::dyn_cast<llvm::CatchReturnInst>(Term)) {
      Note(CR->getSuccessor());
      continue;
    }
    if (auto *Clean = llvm::dyn_cast<llvm::CleanupReturnInst>(Term)) {
      llvm::BasicBlock *Dest = Clean->getUnwindDest();
      if (Dest && !llvm::isa<llvm::CatchSwitchInst>(Dest->getTerminator()))
        Note(Dest);
    }
  }
}

void LLVMCWriter::markInlinable(llvm::Function &Fn) {
  Analysis.Inlinable.clear();
  InlineCache.clear();
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (Inst.getType()->isVoidTy())
        continue;
      if (foldImmediate(&Inst)) {
        Analysis.Inlinable.insert(&Inst);
        continue;
      }
      if (!Inst.hasOneUse())
        continue;
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (llvm::isa<llvm::CallInst>(&Inst))
        continue;
      if (llvm::isa<llvm::LoadInst>(&Inst))
        continue;
      if (llvm::isa<llvm::PHINode>(&Inst))
        continue;
      if (llvm::isa<llvm::ExtractValueInst>(&Inst))
        continue;
      if (llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CleanupPadInst, llvm::CatchReturnInst,
                    llvm::CleanupReturnInst, llvm::InvokeInst>(&Inst))
        continue;
      if (Analysis.IntrinsicStructVals.count(&Inst))
        continue;
      Analysis.Inlinable.insert(&Inst);
    }
  }
}

namespace {

struct SlotMention {
  const llvm::AllocaInst *Slot = nullptr;
  uint64_t Off = 0;
};

void collectSlotMentions(const LLVMCWriter &Writer, const llvm::Value *V,
                         llvm::SmallVectorImpl<SlotMention> &Out,
                         llvm::SmallPtrSetImpl<const llvm::Value *> &Seen,
                         int Depth) {
  if (!V || Depth > 6 || !Seen.insert(V).second)
    return;
  if (const auto *AI =
          llvm::dyn_cast<llvm::AllocaInst>(V->stripPointerCasts())) {
    Out.push_back({AI, 0});
    return;
  }
  if (const auto Peeled = Writer.peelPointerOffset(V)) {
    if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(
            Peeled->first->stripPointerCasts())) {
      Out.push_back({AI, Peeled->second});
      return;
    }
  }
  const auto *Call = llvm::dyn_cast<llvm::CallBase>(V);
  if (!Call)
    return;
  for (const llvm::Use &Arg : Call->args())
    collectSlotMentions(Writer, Arg.get(), Out, Seen, Depth + 1);
}

bool mentionsSameSlot(llvm::ArrayRef<SlotMention> Left,
                      llvm::ArrayRef<SlotMention> Right) {
  for (const SlotMention &A : Left)
    for (const SlotMention &B : Right)
      if (A.Slot == B.Slot && A.Off == B.Off)
        return true;
  return false;
}

} // namespace

bool LLVMCWriter::sameSlotDestructorBetween(
    const llvm::CallBase &Call, const llvm::Instruction &Sink) const {
  llvm::SmallVector<SlotMention, 4> Slots;
  llvm::SmallPtrSet<const llvm::Value *, 16> Seen;
  for (const llvm::Use &Arg : Call.args())
    collectSlotMentions(*this, Arg.get(), Slots, Seen, 0);
  if (Slots.empty())
    return false;

  auto IsSameSlotDtor = [&](const llvm::Instruction &I) {
    const auto *Other = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!Other || Other == &Call)
      return false;
    const MsvcAtlCallee *Atl = msvcAtlCallee(printedCalleeName(*Other));
    if (!Atl || Atl->Kind != MsvcAtlCalleeKind::Dtor)
      return false;
    llvm::SmallVector<SlotMention, 2> DtorSlots;
    llvm::SmallPtrSet<const llvm::Value *, 8> DtorSeen;
    for (const llvm::Use &Arg : Other->args())
      collectSlotMentions(*this, Arg.get(), DtorSlots, DtorSeen, 0);
    return mentionsSameSlot(Slots, DtorSlots);
  };

  bool After = false;
  for (const llvm::Instruction &Cur : *Call.getParent()) {
    if (&Cur == &Call) {
      After = true;
      continue;
    }
    if (!After)
      continue;
    if (&Cur == &Sink)
      return false;
    if (IsSameSlotDtor(Cur))
      return true;
  }
  return false;
}

void LLVMCWriter::markSinglePrintedUseCalls(llvm::Function &Fn) {
  for (llvm::BasicBlock &BB : Fn) {
    for (llvm::Instruction &Inst : BB) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!Call || Call->getType()->isVoidTy() || Call->use_empty())
        continue;
      if (llvm::isa<llvm::IntrinsicInst>(Call) ||
          llvm::isa<llvm::InlineAsm>(Call->getCalledOperand()) ||
          callDoesNotReturn(*Call))
        continue;
      const std::string Name = printedCalleeName(*Call);
      if (isMsvcCxxThrowCallName(Name))
        continue;
      if (const MsvcAtlCallee *Atl = msvcAtlCallee(Name))
        if (Atl->Kind == MsvcAtlCalleeKind::Ctor)
          continue;

      llvm::SmallPtrSet<const llvm::Instruction *, 32> Seen;
      llvm::SmallPtrSet<const llvm::Instruction *, 4> Sinks;
      bool Blocked = false;
      std::function<void(const llvm::Value *)> Walk =
          [&](const llvm::Value *V) {
            if (!V || Blocked)
              return;
            for (const llvm::User *U : V->users()) {
              if (Blocked)
                return;
              const auto *I = llvm::dyn_cast<llvm::Instruction>(U);
              if (!I || llvm::isa<llvm::PHINode, llvm::InvokeInst>(I)) {
                Blocked = true;
                return;
              }
              if (!Seen.insert(I).second)
                continue;
              if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(I)) {
                if (Analysis.DeadFrameStores.count(SI))
                  continue;
                if (SI->getValueOperand() == V) {
                  if (const llvm::AllocaInst *Slot =
                          asAllocaPointer(SI->getPointerOperand())) {
                    if (isJoinCallArgAlloca(Slot)) {
                      Blocked = true;
                      return;
                    }
                    // A store that is not printed is not a second use. An
                    // unread copy into a typed slot otherwise blocks the
                    // one printed condition.
                    const bool Forward =
                        !instructionIsPrinted(*SI) ||
                        (!allocaAddressTaken(Slot) && !isJoinArmStore(SI) &&
                         !isTypedRecordCursorSlot(Slot));
                    if (Forward) {
                      llvm::SmallVector<const llvm::LoadInst *, 4> Window;
                      bool After = false;
                      bool Killed = false;
                      for (const llvm::Instruction &Cur : *SI->getParent()) {
                        if (&Cur == SI) {
                          After = true;
                          continue;
                        }
                        if (!After)
                          continue;
                        if (const auto *Next =
                                llvm::dyn_cast<llvm::StoreInst>(&Cur)) {
                          if (asAllocaPointer(Next->getPointerOperand()) ==
                              Slot) {
                            Killed = true;
                            break;
                          }
                        }
                        if (const auto *LI =
                                llvm::dyn_cast<llvm::LoadInst>(&Cur))
                          if (asAllocaPointer(LI->getPointerOperand()) == Slot)
                            Window.push_back(LI);
                      }
                      if (!Killed) {
                        for (const llvm::User *SU : Slot->users()) {
                          const auto *LI = llvm::dyn_cast<llvm::LoadInst>(SU);
                          if (!LI || LI->getParent() == SI->getParent())
                            continue;
                          bool Sees = true;
                          for (const llvm::Instruction &Cur : *LI->getParent()) {
                            if (&Cur == LI)
                              break;
                            if (const auto *Next =
                                    llvm::dyn_cast<llvm::StoreInst>(&Cur)) {
                              if (asAllocaPointer(Next->getPointerOperand()) ==
                                  Slot) {
                                Sees = false;
                                break;
                              }
                            }
                          }
                          if (Sees)
                            Window.push_back(LI);
                        }
                      }
                      for (const llvm::LoadInst *LI : Window)
                        Walk(LI);
                      continue;
                    }
                  }
                }
              }
              if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(I)) {
                bool Used = CB->getCalledOperand() == V;
                const unsigned Limit =
                    printedCallArgLimit(*CB, printedCalleeName(*CB));
                for (unsigned Arg = 0; Arg < Limit && Arg < CB->arg_size();
                     ++Arg)
                  if (CB->getArgOperand(Arg) == V)
                    Used = true;
                if (Used)
                  Sinks.insert(CB);
                continue;
              }
              if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::LoadInst>(
                      I) ||
                  Analysis.Inlinable.count(I)) {
                Walk(I);
                continue;
              }
              Sinks.insert(I);
            }
          };
      Walk(Call);
      if (!Blocked && Sinks.size() == 1 &&
          !sameSlotDestructorBetween(*Call, **Sinks.begin()))
        Analysis.Inlinable.insert(Call);
    }
  }
}

bool LLVMCWriter::functionHasWindowsEHPads(const llvm::Function &Fn) const {
  for (const llvm::BasicBlock &BB : Fn) {
    const llvm::Instruction *Term = BB.getTerminator();
    if (Term && llvm::isa<llvm::CatchSwitchInst, llvm::CatchReturnInst,
                          llvm::CleanupReturnInst>(Term))
      return true;
    for (const llvm::Instruction &Inst : BB)
      if (llvm::isa<llvm::CatchPadInst, llvm::CleanupPadInst>(&Inst))
        return true;
  }
  return false;
}

namespace {

bool isEHTokenNone(const llvm::Value *V) {
  return !V || llvm::isa<llvm::ConstantTokenNone>(V);
}

const llvm::CatchPadInst *
firstCatchPad(const llvm::CatchSwitchInst &CS) {
  if (CS.getNumHandlers() == 0)
    return nullptr;
  const llvm::BasicBlock *PadBB = *CS.handler_begin();
  if (!PadBB)
    return nullptr;
  for (const llvm::Instruction &Inst : *PadBB)
    if (!llvm::isa<llvm::PHINode>(&Inst))
      return llvm::dyn_cast<llvm::CatchPadInst>(&Inst);
  return nullptr;
}

bool cleanupBelongsToSwitch(const llvm::CleanupPadInst &Pad,
                            const llvm::CatchSwitchInst &CS) {
  const llvm::Value *Parent = Pad.getParentPad();
  if (Parent == &CS)
    return true;
  if (const auto *Catch = firstCatchPad(CS); Catch && Parent == Catch)
    return true;
  return false;
}

const llvm::BasicBlock *catchRetDest(const llvm::CatchSwitchInst &CS) {
  if (const auto *Pad = firstCatchPad(CS))
    for (const llvm::User *User : Pad->users())
      if (const auto *Ret = llvm::dyn_cast<llvm::CatchReturnInst>(User))
        return Ret->getSuccessor();
  if (CS.getNumHandlers() != 0)
    return *CS.handler_begin();
  return nullptr;
}

void collectNormalReachable(const llvm::Function &Fn,
                            llvm::SmallPtrSet<const llvm::BasicBlock *, 16> &Out) {
  llvm::SmallVector<const llvm::BasicBlock *, 16> Work;
  Work.push_back(&Fn.getEntryBlock());
  Out.insert(&Fn.getEntryBlock());
  while (!Work.empty()) {
    const llvm::BasicBlock *BB = Work.pop_back_val();
    const llvm::Instruction *Term = BB->getTerminator();
    if (!Term)
      continue;
    if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term)) {
      const llvm::BasicBlock *Normal = Invoke->getNormalDest();
      if (Normal && Out.insert(Normal).second)
        Work.push_back(Normal);
      continue;
    }
    if (llvm::isa<llvm::CatchSwitchInst, llvm::CatchReturnInst,
                  llvm::CleanupReturnInst>(Term))
      continue;
    for (unsigned I = 0, N = Term->getNumSuccessors(); I < N; ++I) {
      const llvm::BasicBlock *Succ = Term->getSuccessor(I);
      if (Succ && Out.insert(Succ).second)
        Work.push_back(Succ);
    }
  }
}

bool isCatchPadBlock(const llvm::BasicBlock &BB) {
  for (const llvm::Instruction &Inst : BB)
    if (!llvm::isa<llvm::PHINode>(&Inst))
      return llvm::isa<llvm::CatchPadInst>(&Inst);
  return false;
}

bool isEHDispatchOrPad(const llvm::BasicBlock &BB) {
  if (llvm::isa<llvm::CatchSwitchInst>(BB.getTerminator()))
    return true;
  return isCatchPadBlock(BB);
}

bool hasCleanupPad(const llvm::BasicBlock &BB) {
  for (const llvm::Instruction &Inst : BB)
    if (llvm::isa<llvm::CleanupPadInst>(&Inst))
      return true;
  return false;
}

void collectUntilNormal(
    const llvm::BasicBlock *Start,
    const llvm::SmallPtrSet<const llvm::BasicBlock *, 16> &Normal,
    std::vector<const llvm::BasicBlock *> &Out) {
  if (!Start || Normal.contains(Start))
    return;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
  llvm::SmallVector<const llvm::BasicBlock *, 8> Work;
  Work.push_back(Start);
  Seen.insert(Start);
  while (!Work.empty()) {
    const llvm::BasicBlock *BB = Work.pop_back_val();
    Out.push_back(BB);
    const llvm::Instruction *Term = BB->getTerminator();
    if (!Term)
      continue;
    for (unsigned I = 0, N = Term->getNumSuccessors(); I < N; ++I) {
      const llvm::BasicBlock *Succ = Term->getSuccessor(I);
      if (!Succ || Normal.contains(Succ) || isEHDispatchOrPad(*Succ) ||
          !Seen.insert(Succ).second)
        continue;
      if (Succ != Start && hasCleanupPad(*Succ))
        continue;
      Work.push_back(Succ);
    }
  }
}

bool isNamedPointerDisplay(const TypeRef &Ty) {
  return Ty && Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
         Ty->Pointee->Kind == NdTypeKind::Struct &&
         !Ty->Pointee->SourceName.empty() && !Ty->Pointee->IsEnum;
}

bool isNamedRecordDisplay(const TypeRef &Ty) {
  return Ty && Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum &&
         !Ty->SourceName.empty();
}

bool hasDisplayField(const TypeRef &Ty, int64_t Off, llvm::StringRef Name) {
  if (!Ty || Ty->FieldDisplayOffsets.size() != Ty->FieldDisplayNames.size())
    return false;
  for (size_t I = 0; I < Ty->FieldDisplayOffsets.size(); ++I) {
    if (Ty->FieldDisplayOffsets[I] == Off &&
        Ty->FieldDisplayNames[I] == Name)
      return true;
  }
  return false;
}

bool isArgListRecord(const TypeRef &Ty) {
  return isNamedRecordDisplay(Ty) &&
         (Ty->SourceName == "ArgList" ||
          (hasDisplayField(Ty, 0, "types_") &&
           hasDisplayField(Ty, 8, "values_")));
}

TypeRef debugCallArgPointer(const FunctionSym &FS, unsigned Index) {
  const bool Indirect = isMsvcIndirectReturn(FS.ReturnType);
  const bool Member =
      Indirect && !FS.Params.empty() && FS.Params[0].first == "this";
  TypeRef Sret;
  if (Indirect)
    if (TypeRef Record = msvcIndirectReturnRecordType(FS.ReturnType))
      Sret = NdType::makePtr(Record);
  TypeRef Arg;
  if (Member) {
    if (Index == 0)
      Arg = FS.Params[0].second;
    else if (Index == 1)
      Arg = Sret;
    else if (Index - 1 < FS.Params.size())
      Arg = FS.Params[Index - 1].second;
  } else if (Indirect) {
    if (Index == 0)
      Arg = Sret;
    else if (Index - 1 < FS.Params.size())
      Arg = FS.Params[Index - 1].second;
  } else if (Index < FS.Params.size()) {
    Arg = FS.Params[Index].second;
  }
  return isNamedPointerDisplay(Arg) ? Arg : TypeRef{};
}

} // namespace

std::vector<LLVMCWriter::EHWrapClause>
LLVMCWriter::collectWindowsEHWraps(const llvm::Function &Fn) {
  std::vector<const llvm::CatchSwitchInst *> Switches;
  std::vector<const llvm::CleanupPadInst *> Cleanups;
  for (const llvm::BasicBlock &BB : Fn) {
    if (const auto *CS =
            llvm::dyn_cast<llvm::CatchSwitchInst>(BB.getTerminator()))
      Switches.push_back(CS);
    for (const llvm::Instruction &Inst : BB)
      if (const auto *Pad = llvm::dyn_cast<llvm::CleanupPadInst>(&Inst))
        Cleanups.push_back(Pad);
  }

  const bool Cxx = functionIsCxxEH(Fn);
  auto Except = [&](const llvm::CatchSwitchInst &CS) {
    EHWrapClause Clause;
    Clause.Switch = &CS;
    if (Cxx) {
      Clause.Kind = EHWrapClause::Kind::CxxCatch;
      if (const auto *Pad = firstCatchPad(CS))
        Clause.Clause = windowsCxxCatchType(*Pad);
      else
        Clause.Clause = "...";
    } else {
      Clause.Kind = EHWrapClause::Kind::SEHExcept;
      Clause.Clause = windowsEHFilterExpr(CS);
    }
    return Clause;
  };
  auto Finally = [&](const llvm::CleanupPadInst *Pad = nullptr) {
    EHWrapClause Clause;
    Clause.Cleanup = Pad;
    Clause.Kind = Cxx ? EHWrapClause::Kind::CxxCleanup
                      : EHWrapClause::Kind::SEHFinally;
    return Clause;
  };
  auto AttachBodies = [&](std::vector<EHWrapClause> Wraps) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Normal;
    collectNormalReachable(Fn, Normal);
    for (EHWrapClause &Wrap : Wraps) {
      if (Wrap.Switch)
        if (const llvm::BasicBlock *Start = catchRetDest(*Wrap.Switch))
          collectUntilNormal(Start, Normal, Wrap.Body);
      if (Wrap.Cleanup)
        collectUntilNormal(Wrap.Cleanup->getParent(), Normal, Wrap.Body);
    }
    return Wraps;
  };

  std::vector<const llvm::CatchSwitchInst *> RootSwitches;
  for (const auto *CS : Switches)
    if (isEHTokenNone(CS->getParentPad()))
      RootSwitches.push_back(CS);
  std::vector<const llvm::CleanupPadInst *> RootCleanups;
  for (const auto *Pad : Cleanups)
    if (isEHTokenNone(Pad->getParentPad()))
      RootCleanups.push_back(Pad);

  // Nested C++ pads emitted with token-none parents: one cleanup per try,
  // plus a root cleanup for actions below every try.  The list is outermost
  // first so each inner funclet closes before the enclosing catch.
  if (Cxx && !Switches.empty() && !Cleanups.empty()) {
    bool Shape = true;
    for (const llvm::CatchSwitchInst *CS : Switches)
      Shape = Shape && isEHTokenNone(CS->getParentPad());
    for (const llvm::CleanupPadInst *Pad : Cleanups)
      Shape = Shape && isEHTokenNone(Pad->getParentPad());
    const llvm::CleanupPadInst *RootPad = nullptr;
    std::map<const llvm::BasicBlock *, const llvm::CleanupPadInst *> LocalOf;
    if (Shape) {
      for (const llvm::CleanupPadInst *Pad : Cleanups) {
        const auto *Ret = llvm::dyn_cast<llvm::CleanupReturnInst>(
            Pad->getParent()->getTerminator());
        const llvm::BasicBlock *Dest = Ret ? Ret->getUnwindDest() : nullptr;
        if (!Ret) {
          Shape = false;
          break;
        }
        if (!Dest) {
          if (RootPad) {
            Shape = false;
            break;
          }
          RootPad = Pad;
          continue;
        }
        if (!llvm::isa<llvm::CatchSwitchInst>(Dest->getTerminator()) ||
            LocalOf.count(Dest) != 0) {
          Shape = false;
          break;
        }
        LocalOf.emplace(Dest, Pad);
      }
    }
    const llvm::BasicBlock *RootBB = RootPad ? RootPad->getParent() : nullptr;
    int Outermost = 0;
    if (Shape) {
      for (const llvm::CatchSwitchInst *CS : Switches)
        if (CS->getUnwindDest() == RootBB)
          ++Outermost;
      if (Outermost != 1 ||
          LocalOf.size() + (RootPad ? 1u : 0u) != Cleanups.size())
        Shape = false;
    }
    if (Shape) {
      llvm::SmallPtrSet<const llvm::CatchSwitchInst *, 8> Seen;
      std::vector<EHWrapClause> Nested;
      if (RootPad)
        Nested.push_back(Finally(RootPad));
      std::function<bool(const llvm::CatchSwitchInst *)> Walk =
          [&](const llvm::CatchSwitchInst *CS) -> bool {
        if (!Seen.insert(CS).second)
          return false;
        Nested.push_back(Except(*CS));
        const llvm::BasicBlock *Block = CS->getParent();
        auto It = LocalOf.find(Block);
        const llvm::BasicBlock *Entry = Block;
        if (It != LocalOf.end()) {
          Nested.push_back(Finally(It->second));
          Entry = It->second->getParent();
        }
        for (const llvm::CatchSwitchInst *Child : Switches) {
          if (Child->getUnwindDest() != Entry)
            continue;
          if (!Walk(Child))
            return false;
        }
        return true;
      };
      bool Walked = false;
      for (const llvm::CatchSwitchInst *CS : Switches) {
        if (CS->getUnwindDest() != RootBB)
          continue;
        Walked = true;
        if (!Walk(CS))
          Shape = false;
      }
      if (Shape && Walked && Seen.size() == Switches.size())
        return AttachBodies(std::move(Nested));
    }
  }
  if (Cxx && Switches.size() == 1 && Cleanups.size() == 2 &&
      isEHTokenNone(Switches.front()->getParentPad())) {
    const llvm::BasicBlock *OuterDest = Switches.front()->getUnwindDest();
    const llvm::CleanupPadInst *Outer = nullptr;
    const llvm::CleanupPadInst *Inner = nullptr;
    bool Paired = OuterDest != nullptr;
    for (const llvm::CleanupPadInst *Pad : Cleanups) {
      if (!Paired || !isEHTokenNone(Pad->getParentPad())) {
        Paired = false;
        break;
      }
      if (Pad->getParent() == OuterDest)
        Outer = Pad;
      else if (!Inner)
        Inner = Pad;
      else
        Paired = false;
    }
    if (Paired && Outer && Inner)
      return AttachBodies(
          {Finally(Outer), Except(*Switches.front()), Finally(Inner)});
  }
  if (Switches.size() == 1 && Cleanups.size() == 1) {
    const auto *CS = Switches.front();
    const auto *Pad = Cleanups.front();
    if (isEHTokenNone(CS->getParentPad()) && isEHTokenNone(Pad->getParentPad()))
      return AttachBodies({Except(*CS), Finally(Pad)});
    if (isEHTokenNone(CS->getParentPad()) && cleanupBelongsToSwitch(*Pad, *CS))
      return AttachBodies({Except(*CS), Finally(Pad)});
    if (isEHTokenNone(Pad->getParentPad()) && CS->getParentPad() == Pad)
      return AttachBodies({Finally(Pad), Except(*CS)});
  }
  if (RootSwitches.size() == 1 && RootCleanups.empty() && Switches.size() == 1)
    return AttachBodies({Except(*RootSwitches.front())});
  if (RootSwitches.empty() && RootCleanups.size() == 1 && Cleanups.size() == 1)
    return AttachBodies({Finally(RootCleanups.front())});

  if (Switches.empty() && Cleanups.empty())
    return {};
  EHWrapClause Fallback;
  if (Cxx) {
    Fallback.Kind = EHWrapClause::Kind::CxxCatch;
    Fallback.Clause = "...";
  } else {
    Fallback.Kind = EHWrapClause::Kind::SEHExcept;
    Fallback.Clause = "EXCEPTION_EXECUTE_HANDLER";
  }
  return AttachBodies({Fallback});
}

void LLVMCWriter::writeEHWrapOpen(const EHWrapClause &Clause, int Indent) {
  emitIndent(Indent);
  switch (Clause.Kind) {
  case EHWrapClause::Kind::SEHExcept:
  case EHWrapClause::Kind::SEHFinally:
    OS << "__try {\n";
    break;
  case EHWrapClause::Kind::CxxCatch:
  case EHWrapClause::Kind::CxxCleanup:
    OS << "try {\n";
    break;
  }
}

void LLVMCWriter::writeBasicBlock(const llvm::BasicBlock &BB, int Indent) {
  if (FoldedJoinArms.count(&BB) || InlinedFallthroughBlocks.count(&BB) ||
      ConditionChainBodies.count(&BB) || DuplicatedAssignBlocks.count(&BB))
    return;
  if (BB.getParent() && isPrintPassthrough(&BB))
    return;
  const llvm::BasicBlock *LoopLatch = nullptr;
  const llvm::BasicBlock *LoopExit = nullptr;
  const llvm::BasicBlock *LoopBody = nullptr;
  const llvm::AllocaInst *LoopSlot = nullptr;
  if (cursorForLoop(&BB, LoopLatch, LoopExit, LoopBody, LoopSlot)) {
    writeCursorFor(&BB, LoopLatch, LoopExit, LoopBody, LoopSlot, Indent);
    return;
  }
  const llvm::BasicBlock *SplitLatch = nullptr;
  const llvm::BasicBlock *SplitStep = nullptr;
  const llvm::AllocaInst *SplitSlot = nullptr;
  if (splitPhiCursorLoop(&BB, SplitLatch, SplitStep, SplitSlot)) {
    writeSplitPhiCursorFor(&BB, SplitLatch, SplitStep, SplitSlot, Indent);
    return;
  }
  AfterCxxThrow = false;
  {
    std::map<const llvm::Value *, std::string> Keep;
    for (const llvm::Instruction &Inst : BB) {
      const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
      if (!Phi)
        break;
      if (auto It = KnownImmediates.find(Phi); It != KnownImmediates.end())
        Keep[Phi] = It->second;
    }
    KnownImmediates.swap(Keep);
    AllocaImmediates.clear();
    AllocaLastValues.clear();
  }
  if (ReferencedBlocks.count(&BB))
    OS << blockLabel(&BB) << ":\n";
  for (const llvm::Instruction &Inst : BB) {
    if (llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    writeInstruction(const_cast<llvm::Instruction &>(Inst), Indent);
  }
}

void LLVMCWriter::writeEHWrapClose(const EHWrapClause &Clause, int Indent) {
  auto WriteBody = [&]() {
    if (Clause.Body.empty()) {
      emitIndent(Indent + 1);
      OS << "/* recovered handler labels remain in the protected body */\n";
      return;
    }
    for (const llvm::BasicBlock *BB : Clause.Body)
      writeBasicBlock(*BB, Indent + 1);
  };
  emitIndent(Indent);
  switch (Clause.Kind) {
  case EHWrapClause::Kind::SEHExcept:
    OS << "} __except (" << Clause.Clause << ") {\n";
    WriteBody();
    emitIndent(Indent);
    OS << "}\n";
    break;
  case EHWrapClause::Kind::SEHFinally:
    OS << "} __finally {\n";
    WriteBody();
    emitIndent(Indent);
    OS << "}\n";
    break;
  case EHWrapClause::Kind::CxxCatch:
    OS << "} catch (" << Clause.Clause << ") {\n";
    WriteBody();
    emitIndent(Indent);
    OS << "}\n";
    break;
  case EHWrapClause::Kind::CxxCleanup:
    if (Clause.Body.empty()) {
      OS << "} /* unwind cleanup */\n";
      break;
    }
    OS << "} /* unwind cleanup */ {\n";
    WriteBody();
    emitIndent(Indent);
    OS << "}\n";
    break;
  }
}

bool LLVMCWriter::functionIsCxxEH(const llvm::Function &Fn) const {
  if (!Fn.hasPersonalityFn())
    return false;
  switch (llvm::classifyEHPersonality(Fn.getPersonalityFn())) {
  case llvm::EHPersonality::MSVC_CXX:
    return true;
  default:
    break;
  }
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(
          Fn.getPersonalityFn()->stripPointerCasts())) {
    llvm::StringRef Name = GV->getName();
    return Name.contains("CxxFrameHandler") || Name.contains("CxxFrame");
  }
  return false;
}

bool LLVMCWriter::functionNeedsAnalysisOnlyEHWrap(
    const llvm::Function &Fn) const {
  auto LanguageEHName = [](llvm::StringRef Name) {
    Name.consume_front("#");
    return Name.contains("CxxFrame") || Name.contains("C_specific_handler") ||
           Name.contains("except_handler");
  };
  if (functionIsCxxEH(Fn))
    return true;
  if (Fn.hasPersonalityFn()) {
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(
            Fn.getPersonalityFn()->stripPointerCasts())) {
      if (LanguageEHName(GV->getName()))
        return true;
    }
  }
  const llvm::MDNode *Payload =
      Fn.getMetadata(windows_eh_md::FunctionAttachment);
  if (!Payload)
    Payload = Fn.getMetadata(windows_eh_md::NativeAttachment);
  if (Payload &&
      windows_eh_md::PersonalityName < Payload->getNumOperands()) {
    if (const auto *Name = llvm::dyn_cast<llvm::MDString>(
            Payload->getOperand(windows_eh_md::PersonalityName)))
      if (LanguageEHName(Name->getString()))
        return true;
  }
  return false;
}

void LLVMCWriter::writeExceptionAnnotation(const llvm::Function &Fn) {
  if (!Opts.EmitComments)
    return;
  const llvm::MDNode *Payload =
      Fn.getMetadata(windows_eh_md::FunctionAttachment);
  if (!Payload)
    Payload = Fn.getMetadata(windows_eh_md::NativeAttachment);
  if (!Payload)
    return;
  auto MdU64 = [&](unsigned Index) -> uint64_t {
    if (Index >= Payload->getNumOperands())
      return 0;
    if (const auto *C = llvm::dyn_cast<llvm::ConstantAsMetadata>(
            Payload->getOperand(Index)))
      if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C->getValue()))
        return CI->getZExtValue();
    return 0;
  };
  auto MdStr = [&](unsigned Index) -> std::string {
    if (Index >= Payload->getNumOperands())
      return {};
    if (const auto *S =
            llvm::dyn_cast<llvm::MDString>(Payload->getOperand(Index)))
      return S->getString().str();
    return {};
  };
  OS << "/* neverd.exception: encoding="
     << getExceptionEncodingName(
            static_cast<ExceptionEncoding>(MdU64(windows_eh_md::Encoding)))
     << ", status="
     << getExceptionParseStatusName(static_cast<ExceptionParseStatus>(
            MdU64(windows_eh_md::ParseStatus)))
     << ", personality=" << MdStr(windows_eh_md::PersonalityName) << "\n";
  OS << " * code=[0x" << llvm::utohexstr(MdU64(windows_eh_md::CodeBegin))
     << ", 0x" << llvm::utohexstr(MdU64(windows_eh_md::CodeEnd)) << ")";
  if (uint64_t Unwind = MdU64(windows_eh_md::UnwindInfoVA))
    OS << ", unwind=0x" << llvm::utohexstr(Unwind);
  OS << " */\n";
}

bool LLVMCWriter::isUnknownCopyInst(const llvm::Instruction &Inst) const {
  if (isCallClobberName(Inst.getName()))
    return true;
  if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst)) {
    if (Phi->getNumIncomingValues() == 0)
      return false;
    for (const llvm::Value *Inc : Phi->incoming_values()) {
      if (isUnknownPlaceholder(Inc) || isCallClobberValue(Inc) ||
          OmittedUnknowns.count(Inc))
        continue;
      return false;
    }
    return true;
  }
  if (Inst.getNumOperands() != 1)
    return false;
  if (!llvm::isa<llvm::CastInst>(Inst) && !llvm::isa<llvm::FreezeInst>(Inst))
    return false;
  const llvm::Value *Op = Inst.getOperand(0);
  return isUnknownPlaceholder(Op) || OmittedUnknowns.count(Op);
}

bool LLVMCWriter::allocaAddressTaken(const llvm::AllocaInst *AI) const {
  if (!AI)
    return false;
  if (auto It = AllocaAddressTakenCache.find(AI);
      It != AllocaAddressTakenCache.end())
    return It->second;
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Escapes =
      [&](const llvm::Value *V) -> bool {
    if (!V || !Seen.insert(V).second)
      return false;
    for (const llvm::User *U : V->users()) {
      if (llvm::isa<llvm::LoadInst>(U))
        continue;
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
        if (SI->getPointerOperand()->stripPointerCasts() ==
            V->stripPointerCasts())
          continue;
        return true;
      }
      if (llvm::isa<llvm::GetElementPtrInst, llvm::BitCastInst,
                    llvm::AddrSpaceCastInst>(U)) {
        if (Escapes(U))
          return true;
        continue;
      }
      return true;
    }
    return false;
  };
  const bool Result = Escapes(AI);
  AllocaAddressTakenCache.insert({AI, Result});
  return Result;
}

bool LLVMCWriter::usersAreDeadCopies(const llvm::Value *V) const {
  if (!V)
    return true;
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Dead =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur || !Seen.insert(Cur).second)
      return true;
    for (const llvm::User *U : Cur->users()) {
      const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI)
        return false;
      if (OmittedUnknowns.count(UI) || OmittedInlined.count(UI) ||
          Analysis.Inlinable.count(UI))
        continue;
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode>(UI)) {
        if (!Dead(UI))
          return false;
        continue;
      }
      return false;
    }
    return true;
  };
  return Dead(V);
}

bool LLVMCWriter::usersOnlySeeImmediate(const llvm::Value *V) const {
  if (!V)
    return true;
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Only =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur || !Seen.insert(Cur).second)
      return true;
    for (const llvm::User *U : Cur->users()) {
      const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI)
        return false;
      if (OmittedUnknowns.count(UI) || OmittedInlined.count(UI))
        continue;
      if (llvm::isa<llvm::CallBase>(UI)) {
        if (foldImmediate(Cur))
          continue;
        return false;
      }
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode>(UI)) {
        if (!Only(UI))
          return false;
        continue;
      }
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI)) {
        if (const llvm::AllocaInst *Slot =
                asAllocaPointer(SI->getPointerOperand())) {
          if (!allocaAddressTaken(Slot))
            continue;
        }
        return false;
      }
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(UI)) {
        if (foldImmediate(LI) ||
            asAllocaPointer(LI->getPointerOperand()))
          continue;
        return false;
      }
      return false;
    }
    return true;
  };
  return Only(V);
}

void LLVMCWriter::collectOmittedInlinedImmediates(llvm::Function &Fn) {
  OmittedInlined.clear();
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
        if (allocaAddressTaken(AI))
          continue;
        bool AllImm = true;
        bool HasStore = false;
        std::optional<std::string> Common;
        for (const llvm::User *U : AI->users()) {
          if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
            HasStore = true;
            auto Imm = foldImmediate(SI->getValueOperand());
            if (!Imm) {
              AllImm = false;
              continue;
            }
            if (!Common)
              Common = std::move(Imm);
            else if (*Common != *Imm)
              AllImm = false;
            continue;
          }
          if (llvm::isa<llvm::LoadInst>(U) || isLifetimeOrDbgUser(U))
            continue;
          AllImm = false;
        }
        if (!AllImm || !HasStore || !Common)
          continue;
        OmittedAllocaImmediates[AI] = *Common;
        AllocaImmediates[AI] = *Common;
        OmittedInlined.insert(AI);
        for (const llvm::User *U : AI->users())
          if (const auto *I = llvm::dyn_cast<llvm::Instruction>(U))
            OmittedInlined.insert(I);
      }
    }
  }
  bool Grew = true;
  unsigned Guard = 0;
  while (Grew && Guard++ < 32) {
    Grew = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (OmittedInlined.count(&Inst) || Inst.use_empty())
          continue;
        if (!foldImmediate(&Inst))
          continue;
        if (!usersOnlySeeImmediate(&Inst))
          continue;
        OmittedInlined.insert(&Inst);
        Grew = true;
      }
    }
  }
}

void LLVMCWriter::collectOmittedUnknowns(llvm::Function &Fn) {
  OmittedUnknowns.clear();
  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (isCallClobberName(Inst.getName()))
        OmittedUnknowns.insert(&Inst);
  bool Grew = true;
  unsigned Guard = 0;
  while (Grew && Guard++ < 32) {
    Grew = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (OmittedUnknowns.count(&Inst) || !isUnknownCopyInst(Inst))
          continue;
        bool Live = false;
        for (const llvm::User *U : Inst.users()) {
          const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
          if (!UI) {
            Live = true;
            break;
          }
          if (OmittedUnknowns.count(UI) || isUnknownCopyInst(*UI) ||
              usersAreDeadCopies(UI))
            continue;
          Live = true;
          break;
        }
        if (!Live) {
          OmittedUnknowns.insert(&Inst);
          Grew = true;
        }
      }
    }
  }
}

void LLVMCWriter::setupFunction(llvm::Function &Fn) {
  NextVar = 0;
  ValNames.clear();
  UsedNames.clear();
  BlockLabels.clear();
  ReferencedBlocks.clear();
  KnownImmediates.clear();
  AllocaImmediates.clear();
  UnknownPlaceholderCache.clear();
  KilledEntryZeroCache.clear();
  AllocaAddressTakenCache.clear();
  UniqueImmediateUsers.clear();
  AllocaLastValues.clear();
  AllocaHomeValues.clear();
  UniqueHomes.clear();
  JoinCallArgAllocas.clear();
  JoinArmBlocks.clear();
  FoldedJoinArms.clear();
  InlinedFallthroughBlocks.clear();
  ConditionChainBodies.clear();
  DuplicatedAssignBlocks.clear();
  SelectUseText.clear();
  ForwardedFieldArgs.clear();
  FoldedLocalNames.clear();
  OmittedAllocaImmediates.clear();
  OmittedLeftoverNarrow.clear();
  LeftoverSiblingFill.clear();
  DebugFn.reset();
  DebugThisRecord = nullptr;
  DebugThisArg = nullptr;
  FunctionEntry = 0;
  SyntheticFrame = nullptr;
  FrameBaseOffset = 0;
  SpLocalOwner.clear();
  SpLocalNameCandidates.clear();
  SpLocalAmbiguousNames.clear();
  FrameSlots.clear();
  FrameDebugCache.clear();
  ThisHomes.clear();
  ValueTypes.clear();
  AllocaTypes.clear();
  ValueTexts.clear();
  AllocaTexts.clear();
  EHTryDepth = 0;
  EHWrapIsCxx = false;
  EHBoundaryBlocks.clear();
  EHSkippedMainBlocks.clear();
  EHMainBlock = nullptr;
  OmitCleanupRetTo.clear();
  PhiTailSlot = nullptr;

  if (auto VAOr = rewrite_source::getOriginalVA(Fn)) {
    if (*VAOr && **VAOr)
      FunctionEntry = static_cast<va_t>(**VAOr);
  }
  if (Dbg && FunctionEntry) {
    if (auto FS = Dbg->resolveFunction(FunctionEntry)) {
      DebugFn = std::move(*FS);
      if (!Fn.arg_empty() && !DebugFn->Params.empty() &&
          DebugFn->Params[0].second) {
        TypeRef Ty = DebugFn->Params[0].second;
        TypeRef Rec = Ty;
        while (Rec && Rec->Kind == NdTypeKind::Ptr)
          Rec = Rec->Pointee;
        if (Rec && Rec->Kind == NdTypeKind::Struct &&
            !Rec->SourceName.empty()) {
          DebugThisRecord = Rec;
          DebugThisArg = Fn.getArg(0);
          std::string ThisName = "this";
          if (!DebugFn->Params[0].first.empty())
            ThisName = DebugFn->Params[0].first;
          ValNames[DebugThisArg] = ThisName;
          if (Fn.arg_size() > 1 && DebugFn->Params.size() > 1 &&
              !DebugFn->Params[1].first.empty())
            ValNames[Fn.getArg(1)] = DebugFn->Params[1].first;
        }
      }
    }
  }
  if (DebugFn) {
    const bool Indirect = isMsvcIndirectReturn(DebugFn->ReturnType);
    const bool Member = Indirect && !DebugFn->Params.empty() &&
                        DebugFn->Params[0].first == "this";
    const int Sret = !Indirect ? -1 : (Member ? 1 : 0);
    unsigned ParamIdx = 0;
    for (llvm::Argument &Arg : Fn.args()) {
      std::string Raw;
      if (Sret >= 0 && static_cast<int>(ParamIdx) == Sret)
        Raw = "result";
      else {
        size_t DebugIdx = ParamIdx;
        if (Sret >= 0 && static_cast<int>(ParamIdx) > Sret)
          DebugIdx = static_cast<size_t>(ParamIdx - 1);
        if (DebugIdx < DebugFn->Params.size() &&
            !DebugFn->Params[DebugIdx].first.empty())
          Raw = DebugFn->Params[DebugIdx].first;
      }
      if (!Raw.empty())
        ValNames[&Arg] = std::move(Raw);
      ++ParamIdx;
    }
  }

  discoverSyntheticFrame(Fn);

  scanReferencedBlocks(Fn);
  analyzeIntrinsicStructs(Analysis, Fn);
  markInlinable(Fn);
  analyzeDeadFrameStores(Analysis, Fn);
  analyzeStoreForwarding(Analysis, Fn);
  InferredVoid = analyzeVoidReturn(Analysis, Fn);

  if (InferredVoid)
    analyzeVoidDeadChain(Analysis, Fn);
  collectOmittedUnknowns(Fn);
  collectOmittedInlinedImmediates(Fn);
  collectTypedHomes(Fn);
  markSinglePrintedUseCalls(Fn);
  scanReferencedBlocks(Fn);
}

std::optional<FunctionSym>
LLVMCWriter::debugCallee(const llvm::CallBase &Call) const {
  if (!Dbg)
    return std::nullopt;
  if (const auto *Callee = Call.getCalledFunction()) {
    if (auto VAOr = rewrite_source::getOriginalVA(*Callee)) {
      if (*VAOr && **VAOr)
        if (auto FS = Dbg->resolveFunction(static_cast<va_t>(**VAOr)))
          return FS;
    }
    if (auto Slot = parseNdCodePtrSymbol(Callee->getName()))
      if (auto FS = Dbg->resolveFunction(*Slot))
        return FS;
    llvm::StringRef Hex = Callee->getName();
    if (Hex.consume_front(kAutoFuncPrefix) ||
        Hex.consume_front(kAutoFuncPrefixEVM)) {
      uint64_t Addr = 0;
      if (!Hex.empty() && !Hex.getAsInteger(16, Addr) && Addr)
        if (auto FS = Dbg->resolveFunction(static_cast<va_t>(Addr)))
          return FS;
    }
  }
  if (auto Slot = imageDataVA(Call.getCalledOperand()))
    if (auto FS = Dbg->resolveFunction(*Slot))
      return FS;
  return std::nullopt;
}

bool LLVMCWriter::looksLikeHiddenSretOperand(const llvm::Value *Arg) const {
  if (!Arg)
    return false;
  const llvm::Value *Inner = peelIntegerView(Arg);
  if (!Inner)
    Inner = Arg;
  if (Inner->getType()->isPointerTy())
    return true;
  if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Inner))
    return !CI->isZero();
  if (imageDataVA(Inner))
    return true;
  if (const auto Peeled = peelPointerOffset(Inner);
      Peeled && Peeled->first == SyntheticFrame)
    return true;
  return asAllocaPointer(Inner) != nullptr;
}

std::string LLVMCWriter::printedCalleeName(const llvm::CallBase &Call) const {
  if (const auto *Callee = Call.getCalledFunction()) {
    const std::string Raw = Callee->getName().str();
    if (const char *CN = llvmIntrinsicToCName(Raw.c_str()))
      return CN;
    return functionIdentifier(*Callee);
  }
  return resolveImportCalleeName(Call.getCalledOperand());
}

bool LLVMCWriter::valueFeedsPrintedUse(const llvm::Value *V) {
  if (!V)
    return false;
  auto WriteOnlyAlloca = [](const llvm::AllocaInst *Slot) {
    if (!Slot)
      return false;
    for (const llvm::User *User : Slot->users()) {
      if (isLifetimeOrDbgUser(User))
        continue;
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
      if (!Store || Store->getPointerOperand() != Slot ||
          Store->isVolatile() || Store->isAtomic())
        return false;
    }
    return true;
  };
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Walk =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur || !Seen.insert(Cur).second)
      return false;
    for (const llvm::User *U : Cur->users()) {
      const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI)
        return true;
      if (OmittedUnknowns.count(UI) || OmittedInlined.count(UI))
        continue;
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode,
                    llvm::SelectInst>(UI)) {
        if (Walk(UI))
          return true;
        continue;
      }
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(UI)) {
        if (CB->getCalledOperand() == Cur)
          return true;
        const unsigned Limit =
            printedCallArgLimit(*CB, printedCalleeName(*CB));
        for (unsigned I = 0; I < CB->arg_size(); ++I) {
          if (CB->getArgOperand(I) != Cur || I >= Limit)
            continue;
          if (!JoinCallArgAllocas.empty()) {
            if (const llvm::AllocaInst *Join =
                    joinAllocaForCallArg(CB->getArgOperand(I), *CB)) {
              const llvm::Value *Peeled = Cur;
              std::set<const llvm::Value *> PeelSeen;
              while (Peeled && PeelSeen.insert(Peeled).second) {
                if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Peeled)) {
                  Peeled = Fr->getOperand(0);
                  continue;
                }
                if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Peeled)) {
                  Peeled = Cast->getOperand(0);
                  continue;
                }
                break;
              }
              const llvm::AllocaInst *Home = nullptr;
              if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Peeled))
                Home = asAllocaPointer(LI->getPointerOperand());
              if (Home && Home != Join)
                continue;
            }
          }
          return true;
        }
        continue;
      }
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI)) {
        if (SI->getValueOperand() != Cur)
          return true;
        const llvm::AllocaInst *Dest =
            asAllocaPointer(SI->getPointerOperand());
        // A typed cursor may still be a register copy sink.  Type inference
        // does not make a slot observable when every use only writes it.
        if (WriteOnlyAlloca(Dest))
          continue;
        if (!Dest || allocaAddressTaken(Dest) || isJoinCallArgAlloca(Dest) ||
            isThisFieldValueHome(Dest) || isFrameFieldValueHome(Dest) ||
            isTypedRecordCursorSlot(Dest))
          return true;
        for (const llvm::User *DU : Dest->users()) {
          if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(DU)) {
            if (Walk(LI))
              return true;
          }
        }
        continue;
      }
      return true;
    }
    return false;
  };
  return Walk(V);
}

bool LLVMCWriter::loadOnlyUsedAsCallArgs(const llvm::Value *V) const {
  if (!V || V->use_empty())
    return false;
  std::set<const llvm::Value *> Seen;
  std::function<bool(const llvm::Value *)> Only =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur || !Seen.insert(Cur).second)
      return true;
    if (Cur->use_empty())
      return false;
    for (const llvm::User *U : Cur->users()) {
      const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI)
        return false;
      if (OmittedUnknowns.count(UI) || OmittedInlined.count(UI))
        continue;
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode,
                    llvm::SelectInst>(UI)) {
        if (!Only(UI))
          return false;
        continue;
      }
      if (llvm::isa<llvm::CallBase>(UI))
        continue;
      return false;
    }
    return true;
  };
  return Only(V);
}

bool LLVMCWriter::isResultParamValue(const llvm::Value *V) const {
  if (!V)
    return false;
  const llvm::Value *Inner = peelIntegerView(V);
  if (!Inner)
    Inner = V;
  auto NamedResult = [&](const llvm::Value *Cur) {
    auto It = ValNames.find(Cur);
    return It != ValNames.end() && It->second == "result";
  };
  if (NamedResult(V) || NamedResult(Inner))
    return true;
  if (!DebugFn || DebugFn->Params.size() < 2 ||
      DebugFn->Params[1].first != "result")
    return false;
  const llvm::Function *Fn = nullptr;
  if (const auto *A = llvm::dyn_cast<llvm::Argument>(Inner))
    Fn = A->getParent();
  else if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Inner))
    Fn = I->getFunction();
  return Fn && Fn->arg_size() > 1 && Inner == Fn->getArg(1);
}

unsigned LLVMCWriter::printedCallArgLimit(const llvm::CallBase &Call,
                                          llvm::StringRef CalleeName) const {
  const unsigned Have = Call.arg_size();
  const MsvcAtlCallee *Atl = msvcAtlCallee(CalleeName);
  auto Clamp = [&](size_t Limit) {
    return static_cast<unsigned>(
        std::min(Limit, static_cast<size_t>(Have)));
  };
  if (Atl && Atl->Kind == MsvcAtlCalleeKind::Format && Have >= 2) {
    if (auto VA = imageDataVA(Call.getArgOperand(1))) {
      if (auto Lit = imageStringLiteral(Img, *VA, /*AllowEmpty=*/false)) {
        llvm::StringRef Body = *Lit;
        if (Body.consume_front("L"))
          Body.consume_front("\"");
        Body.consume_back("\"");
        unsigned Slots = 0;
        for (size_t I = 0; I < Body.size(); ++I) {
          if (Body[I] != '%')
            continue;
          if (I + 1 < Body.size() && Body[I + 1] == '%') {
            ++I;
            continue;
          }
          ++Slots;
          for (++I; I < Body.size(); ++I) {
            const char C = Body[I];
            if (C == '*')
              ++Slots;
            if (llvm::StringRef("diouxXfFeEgGaAcspn").contains(C))
              break;
          }
        }
        return Clamp(static_cast<size_t>(2 + Slots));
      }
    }
  }
  auto UnknownAt = [&](size_t I) {
    llvm::Value *Arg = Call.getArgOperand(static_cast<unsigned>(I));
    return isUnknownPlaceholder(Arg) || OmittedUnknowns.count(Arg);
  };
  auto KeepExtra = [&](size_t I) {
    if (I == 0)
      return true;
    llvm::Value *Arg = Call.getArgOperand(static_cast<unsigned>(I));
    if (!Arg)
      return false;
    if (Arg->getType()->isPointerTy())
      return true;
    if (llvm::isa<llvm::ConstantInt>(Arg))
      return true;
    if (imageDataVA(Arg))
      return true;
    // Copy/string ctors pass the source as an integer address (`lea rdx`).
    // That is not an LLVM pointer and not a constant, so CtorDrop was
    // treating `&name` / the name/value join as a leftover clobber.
    if (Atl && Atl->Kind == MsvcAtlCalleeKind::Ctor && I == 1) {
      std::set<const llvm::Value *> Seen;
      auto addressShaped = [&](auto &&self, const llvm::Value *V,
                               int Depth) -> bool {
        if (!V || Depth > 4 || !Seen.insert(V).second)
          return false;
        if (llvm::isa<llvm::CallBase>(V))
          return true;
        if (const auto Peeled = peelPointerOffset(V)) {
          if (Peeled->first == DebugThisArg || Peeled->first == SyntheticFrame)
            return true;
        }
        const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V);
        if (!LI)
          return false;
        const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
        if (!Slot)
          return false;
        for (const llvm::User *U : Slot->users()) {
          const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
          if (!SI || SI->getPointerOperand() != Slot)
            continue;
          if (self(self, SI->getValueOperand(), Depth + 1))
            return true;
        }
        return false;
      };
      if (addressShaped(addressShaped, Arg, 0))
        return true;
    }
    return false;
  };
  if (auto FS = debugCallee(Call)) {
    // A pointer-encoded class return may be a real RAX pointer rather than
    // an indirect result. With no debug parameters, live integer machine
    // arguments do not establish a hidden result address.
    if (Have != 0 && !Atl && FS->Params.empty() &&
        isMsvcPointerEncodedClassReturn(FS->ReturnType) &&
        !looksLikeHiddenSretOperand(Call.getArgOperand(0)))
      return 0;
    const bool Indirect = isMsvcIndirectReturn(FS->ReturnType);
    const bool Member =
        Indirect && !FS->Params.empty() && FS->Params[0].first == "this";
    size_t Limit = 0;
    if (Member)
      Limit = FS->Params.empty() ? 1 : FS->Params.size() + 1;
    else if (Indirect)
      Limit = FS->Params.size() + 1;
    else
      Limit = FS->Params.size();
    if (Limit == 0 && Atl && Atl->Kind == MsvcAtlCalleeKind::Dtor)
      Limit = Atl->MaxArgs;
    if (Limit > 0) {
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::Keep)
        return Have;
      // Ctor overloads share one C stem. Keep recovered wchar/copy extras
      // the same way ATL CtorDrop does; do not cap them to a 1-param
      // default-ctor FunctionSym.
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::CtorDrop)
        return static_cast<unsigned>(
            msvcAtlPrintedArgLimit(*Atl, Have, UnknownAt, KeepExtra));
      if (Indirect && Member) {
        const unsigned SretIdx = 1;
        if (SretIdx < Have &&
            !looksLikeHiddenSretOperand(Call.getArgOperand(SretIdx)))
          --Limit;
      }
      const unsigned Clamped = Clamp(Limit);
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::Fixed)
        return std::min(Clamped, Atl->MaxArgs);
      return Clamped;
    }
  }
  if (Atl)
    return static_cast<unsigned>(
        msvcAtlPrintedArgLimit(*Atl, Have, UnknownAt, KeepExtra));
  if (auto Arity = libc::libcArity(CalleeName.str());
      Arity && Arity->FpArgs == 0 && Arity->IntArgs >= 0)
    return Clamp(static_cast<size_t>(Arity->IntArgs));
  if (!Call.getCalledFunction()) {
    unsigned Lim = Have;
    while (Lim > 1 && isTrailingLiveInCopy(Call.getArgOperand(Lim - 1)))
      --Lim;
    return Lim;
  }
  return Have;
}

bool LLVMCWriter::isTrailingLiveInCopy(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (llvm::isa<llvm::Constant>(V) || llvm::isa<llvm::CallBase>(V) ||
        llvm::isa<llvm::BinaryOperator>(V))
      return false;
    if (isUnknownPlaceholder(V))
      return true;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (OmittedAllocaImmediates.count(Home) || AllocaImmediates.count(Home))
          return false;
        if (const llvm::Value *Prev = allocaStoredValue(Home);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
        return true;
      }
      return false;
    }
    return false;
  }
  return false;
}

void LLVMCWriter::collectTypedHomes(llvm::Function &Fn) {
  AllocaLastValues.clear();
  AllocaImmediates.clear();

  auto expectedCallArgType =
      [&](const llvm::CallBase &Call, unsigned Index) -> TypeRef {
    if (auto FS = debugCallee(Call))
      if (TypeRef Ty = cDisplayType(debugCallArgPointer(*FS, Index)))
        return Ty;
    std::string Name;
    if (const auto *Callee = Call.getCalledFunction()) {
      const std::string Raw = Callee->getName().str();
      if (const char *CN = llvmIntrinsicToCName(Raw.c_str()))
        Name = CN;
      else
        Name = functionIdentifier(*Callee);
    } else {
      Name = resolveImportCalleeName(Call.getCalledOperand());
    }
    if (Name.empty())
      return {};
    if (const MsvcAtlCallee *Atl = msvcAtlCallee(Name)) {
      if (Index == 0 || msvcAtlTypesCallArgAsPointer(*Atl, Index) ||
          (Atl->Kind == MsvcAtlCalleeKind::Ctor && Index == 1)) {
        TypeRef Ty = cDisplayType(msvcAtlSyntheticThis(Name, *Atl));
        if (isNamedPointerDisplay(Ty))
          return Ty;
      }
    }
    return {};
  };
  auto applyCallSlotType = [&](llvm::Value *Arg, const TypeRef &Expected) {
    if (!isNamedPointerDisplay(Expected))
      return;
    const auto Peeled = peelPointerOffset(Arg);
    if (!Peeled || Peeled->first != SyntheticFrame)
      return;
    auto It = FrameSlots.find(Peeled->second);
    if (It == FrameSlots.end() || It->second.Name.empty())
      return;
    TypeRef Next = cDisplayType(Expected->Pointee);
    if (!Next)
      return;
    if (Dbg)
      Dbg->completeType(Next);
    if (!It->second.CallType ||
        Next->FieldDisplayNames.size() >=
            It->second.CallType->FieldDisplayNames.size())
      It->second.CallType = Next;
    if (It->second.Type &&
        It->second.Type->FieldDisplayNames.size() >
            Next->FieldDisplayNames.size())
      return;
    It->second.Type = std::move(Next);
  };
  auto frameSlotOff = [&](const llvm::Value *V) -> std::optional<uint64_t> {
    llvm::SmallPtrSet<const llvm::Value *, 8> Seen;
    while (V && Seen.insert(V).second) {
      if (const auto Peeled = peelPointerOffset(V);
          Peeled && Peeled->first == SyntheticFrame)
        return Peeled->second;
      const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V);
      if (!LI)
        return std::nullopt;
      if (const auto Peeled = peelPointerOffset(LI->getPointerOperand());
          Peeled && Peeled->first == SyntheticFrame)
        return Peeled->second;
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        auto It = AllocaHomeValues.find(Home);
        if (It != AllocaHomeValues.end() && It->second && It->second != V) {
          V = It->second;
          continue;
        }
      }
      return std::nullopt;
    }
    return std::nullopt;
  };
  auto findOwnerSlot = [&](uint64_t Off) -> NamedFrameSlot * {
    auto It = FrameSlots.find(Off);
    if (It != FrameSlots.end())
      return &It->second;
    for (auto &Slot : FrameSlots) {
      uint16_t Size = Slot.second.Type && Slot.second.Type->Size
                          ? Slot.second.Type->Size
                          : 0;
      if (!Size && Slot.second.CallType)
        Size = Slot.second.CallType->Size;
      if (Size < 8 || Slot.first > Off ||
          Off >= Slot.first + static_cast<uint64_t>(Size))
        continue;
      return &Slot.second;
    }
    return nullptr;
  };
  auto applyCallTypeOnly = [&](uint64_t Off, const TypeRef &Next) {
    if (!isArgListRecord(Next))
      return;
    NamedFrameSlot *Slot = findOwnerSlot(Off);
    if (!Slot || Slot->Name.empty())
      return;
    if (Dbg)
      Dbg->completeType(Next);
    if (!Slot->CallType || Next->FieldDisplayNames.size() >=
                               Slot->CallType->FieldDisplayNames.size())
      Slot->CallType = Next;
    if (!isNamedRecordDisplay(Slot->Type))
      Slot->Type = Next;
  };
  auto argListOf = [&](const NamedFrameSlot &Slot) -> TypeRef {
    if (isArgListRecord(Slot.CallType))
      return Slot.CallType;
    if (isArgListRecord(Slot.Type))
      return Slot.Type;
    return {};
  };
  auto calleeDisplayName = [&](const llvm::CallBase &Call) -> std::string {
    if (const auto *Callee = Call.getCalledFunction()) {
      const std::string Raw = Callee->getName().str();
      if (const char *CN = llvmIntrinsicToCName(Raw.c_str()))
        return CN;
      return functionIdentifier(*Callee);
    }
    return resolveImportCalleeName(Call.getCalledOperand());
  };

  std::set<va_t> StoredImageAddrs;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
        if (auto VA = imageDataVA(SI->getPointerOperand()))
          StoredImageAddrs.insert(*VA);
    }
  }
  const uint16_t PtrWidth =
      (Img && Img->Bits == Bitness::Bits32) ? 4u : 8u;

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
        const uint16_t Size = llvmAccessSize(LI->getType());
        if (auto Acc = typedRecordAccess(LI->getPointerOperand(), Size)) {
          ValueTypes[LI] = Acc->Type;
          ValueTexts[LI] = Acc->Text;
        } else if (auto Acc = frameSlotAccess(LI->getPointerOperand(), Size,
                                              /*AddressOf=*/false,
                                              /*Synthesize=*/Size >= 16)) {
          ValueTypes[LI] = Acc->Type;
          ValueTexts[LI] = Acc->Text;
        } else if (auto Acc = typedIndexAccess(LI->getPointerOperand())) {
          ValueTypes[LI] = Acc->Type;
          ValueTexts[LI] = Acc->Text;
        } else if (const llvm::AllocaInst *Slot =
                       asAllocaPointer(LI->getPointerOperand())) {
          if (auto It = AllocaTypes.find(Slot); It != AllocaTypes.end()) {
            ValueTypes[LI] = It->second;
            if (!isTypedRecordCursorSlot(Slot)) {
              if (auto Text = AllocaTexts.find(Slot);
                  Text != AllocaTexts.end())
                ValueTexts[LI] = Text->second;
            }
          }
        } else if (auto VA = imageDataVA(LI->getPointerOperand());
                   VA && Size == PtrWidth && !StoredImageAddrs.count(*VA) &&
                   !foldReadonlyScalar(*VA, Size)) {
          if (std::string Image = imageDataCName(LI->getPointerOperand());
              !Image.empty() &&
              !llvm::StringRef(Image).starts_with(kSyntheticGlobalPrefix))
            ValueTexts[LI] = std::move(Image);
        }
        continue;
      }
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst>(&Inst)) {
        if (auto Text = ValueTexts.find(Inst.getOperand(0));
            Text != ValueTexts.end() && !Text->second.empty())
          ValueTexts[&Inst] = Text->second;
        continue;
      }
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
        if (const llvm::AllocaInst *Slot =
                asAllocaPointer(SI->getPointerOperand())) {
          const llvm::Value *Stored = SI->getValueOperand();
          noteAllocaStore(Slot, Stored);
          if (foldImmediate(Stored))
            continue;
          if (auto It = AllocaHomeValues.find(Slot);
              It != AllocaHomeValues.end()) {
            if (It->second != Stored)
              UniqueHomes.erase(Slot);
          } else
            UniqueHomes.insert(Slot);
          AllocaHomeValues[Slot] = Stored;
        } else {
          const uint16_t Size =
              llvmAccessSize(SI->getValueOperand()->getType());
          const bool Wide = Size >= 16 && !Analysis.DeadFrameStores.count(SI);
          (void)frameSlotAccess(SI->getPointerOperand(), Size,
                                /*AddressOf=*/false, /*Synthesize=*/Wide);
        }
      }
      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        const std::string Name = calleeDisplayName(*CB);
        const unsigned Limit = printedCallArgLimit(*CB, Name);
        TypeRef Packed;
        for (unsigned I = 0, E = CB->arg_size(); I < E; ++I) {
          llvm::Value *Arg = CB->getArgOperand(I);
          if (auto Acc = frameSlotAccess(Arg, 0, /*AddressOf=*/true,
                                         /*Synthesize=*/true)) {
            (void)Acc;
            if (auto *AI = llvm::dyn_cast<llvm::Instruction>(Arg))
              Analysis.Inlinable.insert(AI);
          }
          if (I < Limit) {
            TypeRef Expected = expectedCallArgType(*CB, I);
            applyCallSlotType(Arg, Expected);
            if (Expected && isArgListRecord(Expected->Pointee))
              Packed = cDisplayType(Expected->Pointee);
          }
          if (auto Off = frameSlotOff(Arg)) {
            auto It = FrameSlots.find(*Off);
            if (It != FrameSlots.end())
              if (TypeRef Src = argListOf(It->second))
                Packed = Src;
          }
        }
        if (Packed)
          for (unsigned I = Limit, E = CB->arg_size(); I < E; ++I)
            if (auto Off = frameSlotOff(CB->getArgOperand(I)))
              applyCallTypeOnly(*Off, Packed);
      }
    }
  }

  auto NamedStructPtr = [](const TypeRef &Ty) {
    return Ty && Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
           Ty->Pointee->Kind == NdTypeKind::Struct &&
           !Ty->Pointee->SourceName.empty() && !Ty->Pointee->IsEnum;
  };
  bool Changed = true;
  for (int Guard = 0; Changed && Guard < 6; ++Guard) {
    Changed = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        if (!SI)
          continue;
        const llvm::AllocaInst *Slot =
            asAllocaPointer(SI->getPointerOperand());
        if (!Slot)
          continue;
        TypeRef Ty = typeOfValue(SI->getValueOperand());
        if (const auto *LI =
                llvm::dyn_cast<llvm::LoadInst>(SI->getValueOperand())) {
          if (const llvm::AllocaInst *Src =
                  asAllocaPointer(LI->getPointerOperand())) {
            if (auto SrcTy = AllocaTypes.find(Src);
                SrcTy != AllocaTypes.end() && NamedStructPtr(SrcTy->second))
              Ty = SrcTy->second;
          }
        }
        if (!NamedStructPtr(Ty))
          continue;
        auto It = AllocaTypes.find(Slot);
        if (It != AllocaTypes.end() && NamedStructPtr(It->second))
          continue;
        AllocaTypes[Slot] = Ty;
        Changed = true;
      }
    }
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst);
        if (!LI)
          continue;
        const uint16_t Size = llvmAccessSize(LI->getType());
        if (auto Acc = typedRecordAccess(LI->getPointerOperand(), Size)) {
          if (ValueTexts[LI] != Acc->Text) {
            ValueTypes[LI] = Acc->Type;
            ValueTexts[LI] = Acc->Text;
            Changed = true;
          }
          continue;
        }
        if (ValueTexts.count(LI) && !ValueTexts[LI].empty())
          continue;
        if (auto Acc = frameSlotAccess(LI->getPointerOperand(), Size,
                                       /*AddressOf=*/false,
                                       /*Synthesize=*/Size >= 16)) {
          ValueTypes[LI] = Acc->Type;
          ValueTexts[LI] = Acc->Text;
          Changed = true;
          continue;
        }
        if (auto Acc = typedIndexAccess(LI->getPointerOperand())) {
          ValueTypes[LI] = Acc->Type;
          ValueTexts[LI] = Acc->Text;
          Changed = true;
          continue;
        }
        const llvm::AllocaInst *Slot =
            asAllocaPointer(LI->getPointerOperand());
        if (!Slot)
          continue;
        if (auto Text = AllocaTexts.find(Slot);
            Text != AllocaTexts.end() && !Text->second.empty()) {
          ValueTexts[LI] = Text->second;
          Changed = true;
        }
      }
    }
  }
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (!llvm::isa<llvm::CastInst, llvm::FreezeInst>(&Inst))
        continue;
      if (auto Text = ValueTexts.find(Inst.getOperand(0));
          Text != ValueTexts.end() && !Text->second.empty())
        ValueTexts[&Inst] = Text->second;
    }
  }

  std::set<uint64_t> TakenArgListOffs;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      const auto Dest = peelPointerOffset(SI->getPointerOperand());
      if (!Dest || Dest->first != SyntheticFrame)
        continue;
      const auto SrcOff = frameSlotOff(SI->getValueOperand());
      if (!SrcOff)
        continue;
      NamedFrameSlot *Owner = findOwnerSlot(Dest->second);
      if (!Owner)
        continue;
      const uint64_t Rel =
          Dest->second >= Owner->Off ? Dest->second - Owner->Off : 0;
      TypeRef Src = argListOf(*Owner);
      if (!Src || (!hasDisplayField(Src, static_cast<int64_t>(Rel), "values_") &&
                   !hasDisplayField(Owner->CallType, static_cast<int64_t>(Rel),
                                    "values_")))
        continue;
      applyCallTypeOnly(*SrcOff, Src);
      if (NamedFrameSlot *Taken = findOwnerSlot(*SrcOff)) {
        Taken->AddressTaken = true;
        TakenArgListOffs.insert(Taken->Off);
      }
    }
  }
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || llvmAccessSize(SI->getValueOperand()->getType()) < 16)
        continue;
      const auto Dest = peelPointerOffset(SI->getPointerOperand());
      if (!Dest || Dest->first != SyntheticFrame)
        continue;
      const auto SrcOff = frameSlotOff(SI->getValueOperand());
      if (!SrcOff || *SrcOff == Dest->second)
        continue;
      auto DestIt = FrameSlots.find(Dest->second);
      auto SrcIt = FrameSlots.find(*SrcOff);
      if (DestIt == FrameSlots.end() || SrcIt == FrameSlots.end() ||
          DestIt->second.Name.empty())
        continue;
      TypeRef SrcTy = isNamedRecordDisplay(SrcIt->second.Type)
                          ? SrcIt->second.Type
                          : SrcIt->second.CallType;
      if (!isNamedRecordDisplay(SrcTy))
        continue;
      if (!isNamedRecordDisplay(DestIt->second.Type))
        DestIt->second.Type = SrcTy;
      TypeRef SrcCall = argListOf(SrcIt->second);
      if (!SrcCall && isArgListRecord(SrcTy))
        SrcCall = SrcTy;
      if (SrcCall &&
          (!DestIt->second.CallType ||
           SrcCall->FieldDisplayNames.size() >=
               DestIt->second.CallType->FieldDisplayNames.size()))
        DestIt->second.CallType = SrcCall;
    }
  }
  for (uint64_t Taken : TakenArgListOffs) {
    auto TakenIt = FrameSlots.find(Taken);
    if (TakenIt == FrameSlots.end())
      continue;
    TypeRef Src = argListOf(TakenIt->second);
    if (!Src)
      continue;
    uint16_t Size = TakenIt->second.Type && TakenIt->second.Type->Size
                        ? TakenIt->second.Type->Size
                        : (Src->Size ? Src->Size : 16);
    if (Size < 8)
      continue;
    const uint64_t Sibling = Taken + Size;
    auto SibIt = FrameSlots.find(Sibling);
    if (SibIt == FrameSlots.end() || SibIt->second.Name.empty() ||
        SibIt->second.AddressTaken)
      continue;
    if (!isNamedRecordDisplay(SibIt->second.Type) ||
        (SibIt->second.Type->SourceName != Src->SourceName &&
         SibIt->second.Type->Size == Src->Size)) {
      SibIt->second.Type = Src;
      SibIt->second.CallType = Src;
    }
  }
  std::map<std::pair<std::string, unsigned>, std::vector<uint64_t>> Groups;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!CB)
        continue;
      const std::string Name = calleeDisplayName(*CB);
      if (Name.empty())
        continue;
      for (unsigned I = 0, E = CB->arg_size(); I < E; ++I) {
        const auto Peeled = peelPointerOffset(CB->getArgOperand(I));
        if (!Peeled || Peeled->first != SyntheticFrame)
          continue;
        Groups[{Name, I}].push_back(Peeled->second);
      }
    }
  }
  for (auto &[_, Offs] : Groups) {
    TypeRef Donor;
    for (uint64_t Off : Offs) {
      auto It = FrameSlots.find(Off);
      if (It == FrameSlots.end() || !It->second.Type)
        continue;
      if (It->second.Type->Kind == NdTypeKind::Struct &&
          It->second.Type->SourceName == "CStringT") {
        Donor = It->second.Type;
        break;
      }
    }
    if (!Donor)
      continue;
    for (uint64_t Off : Offs) {
      auto It = FrameSlots.find(Off);
      if (It == FrameSlots.end() || It->second.Name.empty() || It->second.Type)
        continue;
      It->second.Type = Donor;
    }
  }

  llvm::SmallPtrSet<const llvm::AllocaInst *, 8> JoinDefaults;
  for (const llvm::BasicBlock &BB : Fn) {
    for (const llvm::Instruction &Inst : BB) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
      if (!Slot || !JoinDefaults.insert(Slot).second)
        continue;
      if (!joinFieldDefaultText(Slot))
        continue;
      UniqueHomes.erase(Slot);
      AllocaHomeValues.erase(Slot);
      AllocaTexts.erase(Slot);
      for (const llvm::User *U : Slot->users()) {
        const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
        if (!LI)
          continue;
        ValueTexts.erase(LI);
        Analysis.Inlinable.erase(LI);
        for (const llvm::User *LU : LI->users()) {
          const auto *Cast = llvm::dyn_cast<llvm::CastInst>(LU);
          if (!Cast)
            continue;
          ValueTexts.erase(Cast);
          Analysis.Inlinable.erase(Cast);
        }
      }
    }
  }

  collectLeftoverNarrowCallStores(Fn);
  AllocaLastValues.clear();
  AllocaImmediates.clear();
  markComposedPrints(Fn);
  markRedundantPhiCopies(Fn);
  omitSingleCopyCursorTemp(Fn);
  markIndirectCalleeChains(Fn);
  JoinCallArgAllocas.clear();
  JoinArmBlocks.clear();
  llvm::SmallVector<std::pair<const llvm::CallBase *, const llvm::AllocaInst *>,
                    8>
      FieldJoins;
  for (const llvm::BasicBlock &BB : Fn) {
    for (const llvm::Instruction &Inst : BB) {
      const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!CB)
        continue;
      const unsigned Limit =
          printedCallArgLimit(*CB, printedCalleeName(*CB));
      for (unsigned I = 0; I < Limit; ++I) {
        if (const llvm::AllocaInst *Slot =
                joinAllocaForCallArg(CB->getArgOperand(I), *CB)) {
          JoinCallArgAllocas.insert(Slot);
          FieldJoins.emplace_back(CB, Slot);
          for (const llvm::BasicBlock *Pred :
               llvm::predecessors(CB->getParent()))
            JoinArmBlocks[Slot].insert(Pred);
        }
      }
    }
  }
  forwardDeadNullFieldArg(FieldJoins);
}

bool LLVMCWriter::isRedundantFlagAnd(const llvm::Instruction &Inst) const {
  const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&Inst);
  if (!BO || BO->getOpcode() != llvm::Instruction::And)
    return false;
  const llvm::Value *LHS = BO->getOperand(0);
  const llvm::Value *RHS = BO->getOperand(1);
  if (LHS == RHS)
    return true;
  auto LT = ValueTexts.find(LHS);
  auto RT = ValueTexts.find(RHS);
  return LT != ValueTexts.end() && RT != ValueTexts.end() &&
         !LT->second.empty() && LT->second == RT->second;
}

bool LLVMCWriter::usersOnlyFeedInlinable(const llvm::Value *V) const {
  if (!V || V->use_empty())
    return false;
  for (const llvm::User *U : V->users()) {
    const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
    if (!UI)
      return false;
    if (Analysis.Inlinable.count(UI) || OmittedUnknowns.count(UI) ||
        OmittedInlined.count(UI))
      continue;
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(UI)) {
      if (auto Text = ValueTexts.find(LI);
          Text != ValueTexts.end() && !Text->second.empty())
        continue;
      return false;
    }
    if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI)) {
      const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
      if (Slot && !allocaAddressTaken(Slot))
        continue;
      return false;
    }
    return false;
  }
  return true;
}

void LLVMCWriter::markComposedPrints(llvm::Function &Fn) {
  auto Mark = [&](const llvm::Instruction &Inst) {
    return Analysis.Inlinable.insert(&Inst).second;
  };
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (Inst.getType()->isVoidTy())
        continue;
      if (auto Text = ValueTexts.find(&Inst);
          Text != ValueTexts.end() && !Text->second.empty())
        Mark(Inst);
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst>(&Inst)) {
        if (auto Text = ValueTexts.find(Inst.getOperand(0));
            Text != ValueTexts.end() && !Text->second.empty()) {
          ValueTexts[&Inst] = Text->second;
          Mark(Inst);
        }
      }
      if (isComposedRemValue(&Inst) || isRedundantFlagAnd(Inst) ||
          (llvm::isa<llvm::ICmpInst>(&Inst) && !Inst.use_empty()))
        Mark(Inst);
    }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (Inst.getType()->isVoidTy() || Analysis.Inlinable.count(&Inst))
          continue;
        const bool Chain = llvm::isa<llvm::CastInst, llvm::FreezeInst>(&Inst) ||
                           (llvm::isa<llvm::BinaryOperator>(&Inst) &&
                            (Inst.getOpcode() == llvm::Instruction::Add ||
                             Inst.getOpcode() == llvm::Instruction::Sub ||
                             Inst.getOpcode() == llvm::Instruction::Mul ||
                             Inst.getOpcode() == llvm::Instruction::Shl ||
                             Inst.getOpcode() == llvm::Instruction::UDiv ||
                             Inst.getOpcode() == llvm::Instruction::SDiv ||
                             Inst.getOpcode() == llvm::Instruction::URem ||
                             Inst.getOpcode() == llvm::Instruction::SRem ||
                             Inst.getOpcode() == llvm::Instruction::And ||
                             Inst.getOpcode() == llvm::Instruction::Or ||
                             Inst.getOpcode() == llvm::Instruction::Xor));
        if (Chain && usersOnlyFeedInlinable(&Inst))
          Changed |= Mark(Inst);
      }
    }
  }
}

std::string LLVMCWriter::composedReprintText(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  std::function<std::string(const llvm::Value *)> Rec =
      [&](const llvm::Value *Cur) -> std::string {
    if (!Cur || !Seen.insert(Cur).second)
      return {};
    if (auto Text = ValueTexts.find(Cur);
        Text != ValueTexts.end() && !Text->second.empty())
      return Text->second;
    if (const auto *Arg = llvm::dyn_cast<llvm::Argument>(Cur)) {
      if (auto It = ValNames.find(Arg); It != ValNames.end())
        return It->second;
      if (DebugFn && Arg->getArgNo() < DebugFn->Params.size() &&
          !DebugFn->Params[Arg->getArgNo()].first.empty())
        return DebugFn->Params[Arg->getArgNo()].first;
      if (DebugThisArg == Arg)
        return "this";
      return {};
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur))
      return Rec(Cast->getOperand(0));
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cur))
      return Rec(Fr->getOperand(0));
    if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Cur)) {
      if (Phi->getNumIncomingValues() == 0)
        return {};
      std::string Common;
      for (const llvm::Value *Inc : Phi->incoming_values()) {
        const std::string Text = Rec(Inc);
        if (Text.empty())
          return {};
        if (Common.empty())
          Common = Text;
        else if (Common != Text)
          return {};
      }
      return Common;
    }
    const llvm::AllocaInst *Slot = llvm::dyn_cast<llvm::AllocaInst>(Cur);
    if (!Slot)
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur))
        Slot = asAllocaPointer(LI->getPointerOperand());
    if (Slot && isTypedRecordCursorSlot(Slot)) {
      if (auto Text = AllocaTexts.find(Slot);
          Text != AllocaTexts.end() && !Text->second.empty() &&
          (isThisFieldValueHome(Slot) || !cursorSlotIsObserved(Slot)))
        return Text->second;
      if (cursorSlotIsObserved(Slot) && !isThisFieldValueHome(Slot))
        return getName(Slot);
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (UniqueHomes.count(Home) && !joinFieldDefaultText(Home)) {
          if (auto Stored = AllocaHomeValues.find(Home);
              Stored != AllocaHomeValues.end() && Stored->second != Cur)
            return Rec(Stored->second);
        }
        if (ThisHomes.count(Home) && DebugThisArg)
          return Rec(DebugThisArg);
      }
    }
    return {};
  };
  return Rec(V);
}

void LLVMCWriter::forwardDeadNullFieldArg(
    llvm::ArrayRef<std::pair<const llvm::CallBase *, const llvm::AllocaInst *>>
        Uses) {
  ForwardedFieldArgs.clear();
  if (Uses.empty())
    return;
  auto ZeroStore = [&](const llvm::Value *Stored) {
    auto Imm = foldImmediate(Stored);
    return Imm && *Imm == "0";
  };
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Dead;
  const llvm::Function *Fn = Uses.front().first->getFunction();
  if (!Fn)
    return;
  for (const llvm::BasicBlock &BB : *Fn) {
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB.getTerminator());
    if (!Br)
      continue;
    const llvm::BasicBlock *Taken = impliedPointerSuccessor(Br);
    if (!Taken)
      continue;
    const llvm::BasicBlock *Other = Taken == Br->getSuccessor(0)
                                        ? Br->getSuccessor(1)
                                        : Br->getSuccessor(0);
    const llvm::BasicBlock *Cur = Other;
    const llvm::BasicBlock *Pred = Br->getParent();
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Chain;
    bool Ok = true;
    bool Stored = false;
    while (Cur && Seen.insert(Cur).second) {
      if (Cur != Other && Cur->getSinglePredecessor() != Pred) {
        Ok = false;
        break;
      }
      for (const llvm::Instruction &Inst : *Cur) {
        if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        if (llvm::isa<llvm::LoadInst, llvm::CastInst, llvm::FreezeInst,
                      llvm::ICmpInst, llvm::BinaryOperator>(&Inst))
          continue;
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        const llvm::AllocaInst *Slot =
            SI ? asAllocaPointer(SI->getPointerOperand()) : nullptr;
        if (Slot && ZeroStore(SI->getValueOperand())) {
          Stored = true;
          continue;
        }
        Ok = false;
        break;
      }
      if (!Ok)
        break;
      Chain.push_back(Cur);
      const auto *Go = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
      if (!Go)
        break;
      const llvm::BasicBlock *Next = Go->getSuccessor(0);
      if (!Next || Next == Cur || Next->getSinglePredecessor() != Cur)
        break;
      Pred = Cur;
      Cur = Next;
    }
    if (Ok && Stored)
      for (const llvm::BasicBlock *Block : Chain)
        Dead.insert(Block);
  }

  auto OneCall = [&](const llvm::AllocaInst *Slot,
                     const llvm::CallBase *Expect) {
    llvm::SmallVector<const llvm::Value *, 8> Work;
    llvm::SmallPtrSet<const llvm::Value *, 16> Seen;
    for (const llvm::User *U : Slot->users())
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U))
        Work.push_back(LI);
    const llvm::CallBase *Found = nullptr;
    while (!Work.empty()) {
      const llvm::Value *V = Work.pop_back_val();
      if (!Seen.insert(V).second)
        continue;
      for (const llvm::User *U : V->users()) {
        if (isLifetimeOrDbgUser(U))
          continue;
        if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::ICmpInst,
                      llvm::BinaryOperator>(U)) {
          Work.push_back(llvm::cast<llvm::Value>(U));
          continue;
        }
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
        if (SI && SI->getValueOperand() == V) {
          const llvm::AllocaInst *Dest =
              asAllocaPointer(SI->getPointerOperand());
          if (!Dest || Dest == Slot || allocaAddressTaken(Dest) ||
              isTypedRecordCursorSlot(Dest))
            return false;
          for (const llvm::User *DU : Dest->users())
            if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(DU))
              Work.push_back(LI);
          continue;
        }
        const auto *CB = llvm::dyn_cast<llvm::CallBase>(U);
        if (!CB || CB != Expect)
          return false;
        if (Found && Found != CB)
          return false;
        Found = CB;
      }
    }
    return Found == Expect;
  };

  std::map<const llvm::AllocaInst *, const llvm::CallBase *> CallOf;
  for (const auto &Use : Uses) {
    if (!Use.first || !Use.second)
      continue;
    auto It = CallOf.find(Use.second);
    if (It == CallOf.end())
      CallOf.emplace(Use.second, Use.first);
    else if (It->second != Use.first)
      It->second = nullptr;
  }
  for (const auto &Entry : CallOf) {
    const llvm::AllocaInst *Slot = Entry.first;
    const llvm::CallBase *Call = Entry.second;
    if (!Call || allocaAddressTaken(Slot) || isTypedRecordCursorSlot(Slot))
      continue;
    if (!OneCall(Slot, Call))
      continue;
    std::string Text;
    bool Field = false;
    bool Bad = false;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(Call->getParent())) {
      const llvm::Value *Last = nullptr;
      for (const llvm::Instruction &Inst : *Pred) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        if (!SI || asAllocaPointer(SI->getPointerOperand()) != Slot)
          continue;
        Last = SI->getValueOperand();
      }
      if (Dead.count(Pred)) {
        if (Last && !ZeroStore(Last))
          Bad = true;
        continue;
      }
      if (!Last || ZeroStore(Last)) {
        Bad = true;
        continue;
      }
      const std::string Next = ultimateComposedText(Last);
      if (Next.empty() || Next.find("->") == std::string::npos ||
          (Field && Next != Text)) {
        Bad = true;
        continue;
      }
      Text = Next;
      Field = true;
    }
    if (!Bad && Field)
      ForwardedFieldArgs.emplace(Slot, Text);
  }
}

void LLVMCWriter::omitSingleCopyCursorTemp(llvm::Function &Fn) {
  for (const llvm::BasicBlock &BB : Fn) {
    for (const llvm::Instruction &Inst : BB) {
      const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!AI || allocaAddressTaken(AI))
        continue;
      const llvm::StoreInst *Def = nullptr;
      bool Bad = false;
      const llvm::AllocaInst *CopyDest = nullptr;
      for (const llvm::User *U : AI->users()) {
        if (isLifetimeOrDbgUser(U))
          continue;
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U)) {
          if (LI->getPointerOperand() != AI &&
              asAllocaPointer(LI->getPointerOperand()) != AI) {
            Bad = true;
            break;
          }
          for (const llvm::User *LU : LI->users()) {
            if (llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::ICmpInst,
                          llvm::BinaryOperator>(LU))
              continue;
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(LU);
            const llvm::AllocaInst *Dest =
                SI && SI->getValueOperand() == LI
                    ? asAllocaPointer(SI->getPointerOperand())
                    : nullptr;
            if (!Dest || Dest == AI) {
              Bad = true;
              break;
            }
            if (CopyDest && CopyDest != Dest) {
              Bad = true;
              break;
            }
            CopyDest = Dest;
          }
          if (Bad)
            break;
          continue;
        }
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
        if (!SI || asAllocaPointer(SI->getPointerOperand()) != AI) {
          Bad = true;
          break;
        }
        const llvm::Value *Stored = SI->getValueOperand();
        if (foldImmediate(Stored))
          continue;
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Stored))
          if (asAllocaPointer(LI->getPointerOperand()) == AI)
            continue;
        if (Def) {
          Bad = true;
          break;
        }
        Def = SI;
      }
      if (Bad || !Def || !CopyDest)
        continue;
      const std::string Text = ultimateComposedText(Def->getValueOperand());
      if (Text.empty() || (Text.find('[') == std::string::npos &&
                           Text.find("->") == std::string::npos &&
                           Text.find('(') == std::string::npos))
        continue;
      OmittedInlined.insert(AI);
    }
  }
}

void LLVMCWriter::markRedundantPhiCopies(llvm::Function &Fn) {
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 32) {
    Changed = false;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi || Analysis.Inlinable.count(Phi) ||
            Phi->getNumIncomingValues() == 0)
          continue;
        std::string Common;
        bool Same = true;
        for (const llvm::Value *Inc : Phi->incoming_values()) {
          const std::string Text = composedReprintText(Inc);
          if (Text.empty()) {
            Same = false;
            break;
          }
          if (Common.empty())
            Common = Text;
          else if (Common != Text) {
            Same = false;
            break;
          }
        }
        if (!Same || Common.empty())
          continue;
        ValueTexts[Phi] = Common;
        Analysis.Inlinable.insert(Phi);
        Changed = true;
      }
    }
  }
}

bool LLVMCWriter::isNamedParamValue(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (llvm::isa<llvm::Argument>(V))
      return true;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(V)) {
      V = Fr->getOperand(0);
      continue;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
      if (Slot && UniqueHomes.count(Slot)) {
        if (auto Home = AllocaHomeValues.find(Slot);
            Home != AllocaHomeValues.end() && Home->second != V) {
          V = Home->second;
          continue;
        }
      }
      if (Slot && ThisHomes.count(Slot) && DebugThisArg) {
        V = DebugThisArg;
        continue;
      }
    }
    break;
  }
  return false;
}

bool LLVMCWriter::allocaHomeIsNamedParam(const llvm::AllocaInst *Slot) const {
  if (!Slot || !UniqueHomes.count(Slot))
    return false;
  auto Home = AllocaHomeValues.find(Slot);
  return Home != AllocaHomeValues.end() && isNamedParamValue(Home->second);
}

bool LLVMCWriter::allocaOnlyHoldsImmediates(
    const llvm::AllocaInst *Slot) const {
  if (!Slot || allocaAddressTaken(Slot))
    return false;
  bool HasStore = false;
  for (const llvm::User *U : Slot->users()) {
    if (llvm::isa<llvm::LoadInst>(U) || isLifetimeOrDbgUser(U))
      continue;
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
    if (!SI || !foldImmediate(SI->getValueOperand()))
      return false;
    HasStore = true;
  }
  return HasStore;
}

bool LLVMCWriter::isKilledEntryZeroStore(
    const llvm::StoreInst *Store, const llvm::AllocaInst *Slot) const {
  if (!Store || !Slot ||
      Store->getParent() != &Slot->getFunction()->getEntryBlock() ||
      Store->getPointerOperand() != Slot || Store->isVolatile() ||
      Store->isAtomic() || allocaAddressTaken(Slot))
    return false;
  const auto *Zero = llvm::dyn_cast<llvm::Constant>(Store->getValueOperand());
  if (!Zero || !Zero->isNullValue() ||
      Store->getValueOperand()->getType() != Slot->getAllocatedType())
    return false;
  if (auto It = KilledEntryZeroCache.find(Store);
      It != KilledEntryZeroCache.end())
    return It->second;
  auto Remember = [&](bool Result) {
    KilledEntryZeroCache.insert({Store, Result});
    return Result;
  };
  for (const llvm::User *U : Slot->users()) {
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (LI->getPointerOperand() == Slot)
        continue;
    } else if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
      if (SI->getPointerOperand() == Slot)
        continue;
    } else if (isLifetimeOrDbgUser(U)) {
      continue;
    }
    return Remember(false);
  }

  // Follow the value written by this store, stopping each path at a complete
  // overwrite. A load on any bypass path keeps the entry zero observable.
  llvm::SmallVector<const llvm::BasicBlock *, 8> Pending;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Visited;
  enum class ScanResult { Open, Killed, Read };
  auto Scan = [&](const llvm::Instruction *Start) -> ScanResult {
    for (const llvm::Instruction *I = Start; I; I = I->getNextNode()) {
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(I)) {
        if (LI->getPointerOperand() == Slot)
          return ScanResult::Read;
      } else if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(I)) {
        if (SI->getPointerOperand() == Slot) {
          if (SI->getValueOperand()->getType() != Slot->getAllocatedType())
            return ScanResult::Read;
          return ScanResult::Killed;
        }
      }
    }
    return ScanResult::Open;
  };
  const llvm::Instruction *Start = Store->getNextNode();
  const ScanResult First = Scan(Start);
  if (First == ScanResult::Read)
    return Remember(false);
  if (First == ScanResult::Killed)
    return Remember(true);
  for (const llvm::BasicBlock *Succ : llvm::successors(Store->getParent()))
    Pending.push_back(Succ);
  while (!Pending.empty()) {
    const llvm::BasicBlock *BB = Pending.pop_back_val();
    if (!Visited.insert(BB).second)
      continue;
    const ScanResult Result = Scan(&BB->front());
    if (Result == ScanResult::Read)
      return Remember(false);
    if (Result == ScanResult::Killed)
      continue;
    for (const llvm::BasicBlock *Succ : llvm::successors(BB))
      Pending.push_back(Succ);
  }
  return Remember(true);
}

std::optional<std::string>
LLVMCWriter::uniqueAllocaImmediate(const llvm::AllocaInst *Slot) const {
  if (!Slot || allocaAddressTaken(Slot))
    return std::nullopt;

  // foldImmediate follows loads back to their alloca, then comes back here
  // for every store.  Keep one result per slot for this walk: a chain of
  // copied homes otherwise rescans all of its predecessors at every branch.
  // An active slot is a cycle, so it cannot establish one unique constant.
  struct ImmediateWalk {
    const LLVMCWriter *Owner = nullptr;
    unsigned Depth = 0;
    llvm::SmallPtrSet<const llvm::AllocaInst *, 32> Active;
    llvm::DenseMap<const llvm::AllocaInst *, std::optional<std::string>> Memo;
  };
  static thread_local ImmediateWalk Walk;
  if (Walk.Depth == 0) {
    Walk.Owner = this;
    Walk.Active.clear();
    Walk.Memo.clear();
  } else if (Walk.Owner != this) {
    return std::nullopt;
  }
  if (auto It = Walk.Memo.find(Slot); It != Walk.Memo.end())
    return It->second;
  if (!Walk.Active.insert(Slot).second)
    return std::nullopt;
  ++Walk.Depth;

  auto Users = UniqueImmediateUsers.find(Slot);
  if (Users == UniqueImmediateUsers.end()) {
    llvm::SmallVector<ImmediateUser, 2> Entries;
    for (const llvm::User *U : Slot->users()) {
      if (llvm::isa<llvm::LoadInst>(U) || isLifetimeOrDbgUser(U))
        continue;
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      bool IsSelfCopy = false;
      if (SI) {
        const llvm::Value *Val = SI->getValueOperand();
        llvm::SmallPtrSet<const llvm::Value *, 8> Seen;
        while (Val && Seen.insert(Val).second) {
          if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Val)) {
            Val = Fr->getOperand(0);
            continue;
          }
          if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Val)) {
            Val = Cast->getOperand(0);
            continue;
          }
          break;
        }
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Val))
          IsSelfCopy = asAllocaPointer(LI->getPointerOperand()) == Slot;
      }
      Entries.push_back({SI, IsSelfCopy});
      if (!SI)
        break;
    }
    Users = UniqueImmediateUsers.insert({Slot, std::move(Entries)}).first;
  }

  auto Collect = [&]() -> std::optional<std::string> {
    std::optional<std::string> Common;
    bool HasStore = false;
    for (const ImmediateUser &Entry : Users->second) {
      const auto *SI = Entry.Store;
      if (!SI)
        return std::nullopt;
      // A store of this slot's own load is a copy, not another value.
      if (Entry.IsSelfCopy || isKilledEntryZeroStore(SI, Slot))
        continue;
      auto Imm = foldImmediate(SI->getValueOperand());
      if (!Imm)
        return std::nullopt;
      if (!Common)
        Common = std::move(Imm);
      else if (*Common != *Imm)
        return std::nullopt;
      HasStore = true;
    }
    if (!HasStore)
      return std::nullopt;
    return Common;
  };
  auto Result = Collect();
  Walk.Active.erase(Slot);
  Walk.Memo.insert({Slot, Result});
  if (--Walk.Depth == 0) {
    Walk.Owner = nullptr;
    Walk.Active.clear();
    Walk.Memo.clear();
  }
  return Result;
}

bool LLVMCWriter::allocaLoadsComposeImmediate(
    const llvm::AllocaInst *Slot) const {
  // Return/compare users may compose a constant directly only when every
  // store supplies that same constant. A conditional overwrite of an entry
  // zero still leaves the zero visible on its bypass edge.
  if (!Slot || !uniqueAllocaImmediate(Slot))
    return false;
  std::function<bool(const llvm::Value *)> Compose =
      [&](const llvm::Value *Cur) -> bool {
    if (!Cur)
      return true;
    for (const llvm::User *U : Cur->users()) {
      const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
      if (!UI)
        return false;
      if (Analysis.Inlinable.count(UI) || OmittedUnknowns.count(UI) ||
          OmittedInlined.count(UI))
        continue;
      if (llvm::isa<llvm::ReturnInst, llvm::ICmpInst, llvm::CondBrInst,
                    llvm::SelectInst>(UI))
        continue;
      if (llvm::isa<llvm::CastInst, llvm::FreezeInst>(UI)) {
        if (!Compose(UI))
          return false;
        continue;
      }
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI)) {
        const llvm::AllocaInst *Dest = asAllocaPointer(SI->getPointerOperand());
        if (Dest && !allocaAddressTaken(Dest))
          continue;
      }
      return false;
    }
    return true;
  };
  for (const llvm::User *U : Slot->users()) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
    if (!LI)
      continue;
    if (!Compose(LI))
      return false;
  }
  return true;
}

bool LLVMCWriter::isDeadImmediateInit(const llvm::AllocaInst *Slot,
                                      const llvm::Value *Stored) const {
  if (!Slot || !Stored || allocaAddressTaken(Slot) || !foldImmediate(Stored))
    return false;
  auto Home = AllocaHomeValues.find(Slot);
  if (Home == AllocaHomeValues.end() || !Home->second)
    return false;
  return !foldImmediate(Home->second);
}

bool LLVMCWriter::storedTypedMemberLoad(const llvm::Value *Stored) const {
  const llvm::Value *Src = Stored;
  std::set<const llvm::Value *> Seen;
  while (Src && Seen.insert(Src).second) {
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Src)) {
      Src = Fr->getOperand(0);
      continue;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Src)) {
      Src = Cast->getOperand(0);
      continue;
    }
    break;
  }
  const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Src);
  if (!LI)
    return false;
  const uint16_t Size = llvmAccessSize(LI->getType());
  if (typedRecordAccess(LI->getPointerOperand(), Size))
    return true;
  return frameSlotAccess(LI->getPointerOperand(), Size, /*AddressOf=*/false)
      .has_value();
}

std::optional<std::string>
LLVMCWriter::joinFieldDefaultText(const llvm::AllocaInst *Slot) const {
  if (!Slot)
    return std::nullopt;
  // Field recognition loads the cursor home, which asks whether this slot
  // is a join default. A nested question is not itself that default.
  thread_local int Depth = 0;
  if (Depth)
    return std::nullopt;
  struct DepthGuard {
    int &Depth;
    explicit DepthGuard(int &Depth) : Depth(Depth) { ++Depth; }
    ~DepthGuard() { --Depth; }
  } Guard(Depth);
  if (allocaAddressTaken(Slot) || isTypedRecordCursorSlot(Slot) ||
      isThisFieldValueHome(Slot) || isFrameFieldValueHome(Slot))
    return std::nullopt;
  if (llvm::isa<llvm::ArrayType>(Slot->getAllocatedType()))
    return std::nullopt;
  auto FieldStore = [&](const llvm::Value *Stored) {
    const llvm::Value *Src = Stored;
    llvm::SmallPtrSet<const llvm::Value *, 8> Seen;
    while (Src && Seen.insert(Src).second) {
      if (storedTypedMemberLoad(Src))
        return true;
      if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Src)) {
        Src = Fr->getOperand(0);
        continue;
      }
      if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Src)) {
        Src = Cast->getOperand(0);
        continue;
      }
      const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Src);
      if (!LI)
        return false;
      auto Text = ValueTexts.find(LI);
      if (Text != ValueTexts.end() && Text->second.find("->") != std::string::npos)
        return true;
      if (auto Peeled = peelPointerOffset(LI->getPointerOperand());
          Peeled && Peeled->second != 0)
        return true;
      // The field address is spilled (`add` stored, then inttoptr/load).
      // peelPointerOffset stops on that slot when it looks like the cursor
      // itself, so the +field offset is only on the slot's home.
      const llvm::Value *Addr = LI->getPointerOperand();
      llvm::SmallPtrSet<const llvm::Value *, 8> AddrSeen;
      while (Addr && AddrSeen.insert(Addr).second) {
        if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Addr)) {
          Addr = Fr->getOperand(0);
          continue;
        }
        if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Addr)) {
          Addr = Cast->getOperand(0);
          continue;
        }
        break;
      }
      if (const auto *AddrLI = llvm::dyn_cast<llvm::LoadInst>(Addr)) {
        if (const llvm::AllocaInst *AddrSlot =
                asAllocaPointer(AddrLI->getPointerOperand())) {
          // `cursor+field` folds to a plain integer when the cursor load
          // looks like zero, so that store never becomes AllocaHomeValues.
          // The store instruction still carries the add.
          for (const llvm::User *AU : AddrSlot->users()) {
            const auto *ASI = llvm::dyn_cast<llvm::StoreInst>(AU);
            if (!ASI || asAllocaPointer(ASI->getPointerOperand()) != AddrSlot)
              continue;
            if (auto Peeled = peelPointerOffset(ASI->getValueOperand());
                Peeled && Peeled->second != 0 &&
                isTypedRecordCursorValue(Peeled->first))
              return true;
          }
        }
      }
      const llvm::AllocaInst *Home = asAllocaPointer(LI->getPointerOperand());
      auto HomeVal = Home ? AllocaHomeValues.find(Home) : AllocaHomeValues.end();
      if (HomeVal == AllocaHomeValues.end() || !HomeVal->second ||
          HomeVal->second == Src)
        return false;
      if (auto Peeled = peelPointerOffset(HomeVal->second);
          Peeled && Peeled->second != 0 &&
          isTypedRecordCursorValue(Peeled->first))
        return true;
      Src = HomeVal->second;
    }
    return false;
  };
  std::optional<std::string> ImmText;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> ImmBlocks;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> FieldBlocks;
  for (const llvm::User *U : Slot->users()) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
    if (!SI || !SI->getParent())
      continue;
    const llvm::Value *Stored = SI->getValueOperand();
    if (FieldStore(Stored)) {
      FieldBlocks.insert(SI->getParent());
      continue;
    }
    auto Imm = foldImmediate(Stored);
    if (!Imm || *Imm == "0")
      continue;
    if (ImmText && *ImmText != *Imm)
      return std::nullopt;
    ImmText = std::move(*Imm);
    ImmBlocks.insert(SI->getParent());
  }
  if (!ImmText || FieldBlocks.empty() || ImmBlocks.empty())
    return std::nullopt;
  for (const llvm::BasicBlock *BB : FieldBlocks)
    if (ImmBlocks.count(BB))
      return std::nullopt;
  for (const llvm::BasicBlock *Start : FieldBlocks) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
    llvm::SmallVector<const llvm::BasicBlock *, 8> Work{Start};
    while (!Work.empty()) {
      const llvm::BasicBlock *Cur = Work.pop_back_val();
      if (!Cur || !Seen.insert(Cur).second)
        continue;
      if (Cur != Start && ImmBlocks.count(Cur))
        return std::nullopt;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
        Work.push_back(Succ);
    }
  }
  return ImmText;
}

bool LLVMCWriter::isJoinFieldVsImmediateHome(
    const llvm::AllocaInst *Slot) const {
  return joinFieldDefaultText(Slot).has_value();
}

bool LLVMCWriter::isJoinCallArgAlloca(const llvm::AllocaInst *Slot) const {
  return Slot && JoinCallArgAllocas.count(Slot);
}

bool LLVMCWriter::isJoinArmStored(const llvm::AllocaInst *Slot,
                                  const llvm::Value *Stored) const {
  if (!Slot || !Stored)
    return false;
  auto Blocks = JoinArmBlocks.find(Slot);
  if (Blocks == JoinArmBlocks.end())
    return false;
  for (const llvm::BasicBlock *BB : Blocks->second) {
    const llvm::Value *Last = nullptr;
    for (const llvm::Instruction &Inst : *BB) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || asAllocaPointer(SI->getPointerOperand()) != Slot)
        continue;
      Last = SI->getValueOperand();
    }
    if (Last == Stored)
      return true;
  }
  return false;
}

bool LLVMCWriter::isJoinArmStore(const llvm::StoreInst *SI) {
  if (!SI)
    return false;
  const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
  return isJoinArmStored(Slot, SI->getValueOperand());
}

bool LLVMCWriter::allocaStoreIsHidden(const llvm::AllocaInst *Slot,
                                      const llvm::Value *Stored) {
  if (!Slot || !Stored)
    return true;
  if (std::optional<std::string> Def = joinFieldDefaultText(Slot)) {
    if (auto Imm = foldImmediate(Stored); Imm && *Imm == *Def)
      return true;
    // Only an observed slot prints the field over the default. A parallel
    // copy that nothing reads stays hidden, so it is not an undeclared name.
    if (allocaHasPrintedLoad(Slot))
      return false;
  }
  if (isJoinArmStored(Slot, Stored))
    return false;
  const bool ExceptionalJoin = Slot->getFunction()->hasPersonalityFn();
  if (ExceptionalJoin) {
    const llvm::BasicBlock *FirstStoreBlock = nullptr;
    bool HasSeparateStoreBlocks = false;
    for (const llvm::User *U : Slot->users()) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      if (!SI || SI->getPointerOperand() != Slot)
        continue;
      if (FirstStoreBlock && FirstStoreBlock != SI->getParent()) {
        HasSeparateStoreBlocks = true;
        break;
      }
      FirstStoreBlock = SI->getParent();
    }
    if (HasSeparateStoreBlocks) {
      for (const llvm::User *U : Slot->users())
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U))
          if (llvm::pred_size(LI->getParent()) > 1 &&
              valueFeedsPrintedUse(LI))
            return false;
    }
  }
  // Reg2mem also uses an alloca for a value selected by control flow.  A
  // path-local immediate does not make another predecessor's store dead: the
  // load after the join still observes whichever value reached it.
  if (ExceptionalJoin && allocaOnlyHoldsImmediates(Slot) &&
      !uniqueAllocaImmediate(Slot)) {
    for (const llvm::User *U : Slot->users())
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U))
        if (valueFeedsPrintedUse(LI))
          return false;
  }
  if (OmittedInlined.count(Slot))
    return true;
  if (allocaAddressTaken(Slot))
    return false;
  const llvm::Value *Src = Stored;
  std::set<const llvm::Value *> Seen;
  while (Src && Seen.insert(Src).second) {
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Src)) {
      Src = Fr->getOperand(0);
      continue;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Src)) {
      Src = Cast->getOperand(0);
      continue;
    }
    break;
  }
  if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(Src)) {
    if (!CB->getType()->isVoidTy() && !isCallClobberValue(CB) &&
        !isUnusedCallClobber(*CB))
      return true;
  }
  if (!UniqueHomes.count(Slot) && !isTypedRecordCursorSlot(Slot) &&
      storedTypedMemberLoad(Src))
    return false;
  if (!isTypedRecordCursorSlot(Slot)) {
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Src)) {
      if (Analysis.Inlinable.count(I))
        return true;
    }
    if (auto Text = ValueTexts.find(Src);
        Text != ValueTexts.end() && !Text->second.empty())
      return true;
  }
  if (!allocaHasLoad(Slot) || imageDataVA(Stored) || isComposedRemValue(Stored) ||
      (foldImmediate(Stored) && UniqueHomes.count(Slot) &&
       allocaOnlyHoldsImmediates(Slot)) ||
      (llvm::isa<llvm::Instruction>(Stored) && foldImmediate(Stored) &&
       usersOnlySeeImmediate(Stored) &&
       (!ExceptionalJoin || !allocaHasPrintedLoad(Slot))) ||
      allocaHomeIsNamedParam(Slot) ||
      isThisFieldAddress(Stored) || isThisFieldValueHome(Slot) ||
      (allocaOnlyHoldsImmediates(Slot) && allocaLoadsComposeImmediate(Slot)) ||
      isDeadImmediateInit(Slot, Stored) ||
      (!isJoinCallArgAlloca(Slot) && !isTypedRecordCursorSlot(Slot) &&
       !allocaHasPrintedLoad(Slot)) ||
      (isTypedRecordCursorSlot(Slot) && !cursorSlotIsObserved(Slot)) ||
      (!isTypedRecordCursorSlot(Slot) &&
       ((llvm::isa<llvm::Instruction>(Stored) &&
         Analysis.Inlinable.count(llvm::cast<llvm::Instruction>(Stored))) ||
        (ValueTexts.count(Stored) &&
         !ValueTexts.find(Stored)->second.empty()))))
    return true;
  return false;
}

bool LLVMCWriter::instructionIsPrinted(const llvm::Instruction &Inst) {
  if (llvm::isa<llvm::DbgInfoIntrinsic, llvm::AllocaInst, llvm::PHINode,
                llvm::CatchPadInst, llvm::CleanupPadInst, llvm::CatchSwitchInst>(
          &Inst))
    return false;
  if (Analysis.Inlinable.count(&Inst) || Analysis.DeadFrameStores.count(&Inst) ||
      isUnusedCallClobber(Inst))
    return false;
  if ((llvm::isa<llvm::CastInst, llvm::FreezeInst>(&Inst) ||
       isUnknownCopyInst(Inst)) &&
      usersAreDeadCopies(&Inst))
    return false;
  if (auto Imm = foldImmediate(&Inst)) {
    (void)Imm;
    if (!Inst.use_empty() || imageDataVA(&Inst))
      return false;
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
      if (imageDataVA(LI->getPointerOperand()) || foldImmediate(LI))
        return false;
    }
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
    if (isImportCalleeOnlyLoad(LI) || computedAllocaLoadIsForwarded(LI) ||
        typedRecordAccess(LI->getPointerOperand(),
                          llvmAccessSize(LI->getType())) ||
        typedIndexAccess(LI->getPointerOperand()) ||
        isTypedRecordCursorSlot(asAllocaPointer(LI->getPointerOperand())) ||
        isJoinCallArgAlloca(asAllocaPointer(LI->getPointerOperand())) ||
        joinFieldDefaultText(asAllocaPointer(LI->getPointerOperand())))
      return false;
    if (!valueFeedsPrintedUse(LI))
      return false;
    if (auto Text = ValueTexts.find(LI);
        Text != ValueTexts.end() && !Text->second.empty())
      return false;
    if (const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
        Slot && !allocaAddressTaken(Slot) &&
        (allocaHomeIsNamedParam(Slot) ||
         (allocaOnlyHoldsImmediates(Slot) &&
          allocaLoadsComposeImmediate(Slot))))
      return false;
    return true;
  }
  if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
    if (const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand())) {
      if (isKilledEntryZeroStore(SI, Slot))
        return false;
      if (std::optional<std::string> Def = joinFieldDefaultText(Slot)) {
        if (auto Imm = foldImmediate(SI->getValueOperand()); Imm && *Imm == *Def)
          return false;
      }
      if (isJoinArmStore(SI))
        return true;
      return !allocaStoreIsHidden(Slot, SI->getValueOperand());
    }
    return true;
  }
  return true;
}

bool LLVMCWriter::isPrintPassthrough(const llvm::BasicBlock *BB) {
  if (!BB)
    return false;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
  if (!Br || Br->getNumSuccessors() != 1)
    return false;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Br)
      continue;
    if (instructionIsPrinted(Inst))
      return false;
  }
  return true;
}

const llvm::BasicBlock *
LLVMCWriter::printBranchTarget(const llvm::BasicBlock *To) {
  std::set<const llvm::BasicBlock *> Seen;
  while (To && Seen.insert(To).second && isPrintPassthrough(To))
    To = To->getTerminator()->getSuccessor(0);
  return To;
}

const llvm::BasicBlock *
LLVMCWriter::straightAssignArmJoin(const llvm::BasicBlock *Arm) const {
  if (!Arm || !Arm->getSinglePredecessor())
    return nullptr;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Arm->getTerminator());
  if (!Br || Br->getSuccessor(0) == Arm)
    return nullptr;
  bool SawStore = false;
  for (const llvm::Instruction &Inst : *Arm) {
    if (&Inst == Arm->getTerminator())
      break;
    if (llvm::isa<llvm::PHINode>(Inst))
      return nullptr;
    if (llvm::isa<llvm::StoreInst>(Inst)) {
      SawStore = true;
      continue;
    }
    if (llvm::isa<llvm::LoadInst, llvm::CastInst, llvm::GetElementPtrInst,
                  llvm::FreezeInst, llvm::BinaryOperator, llvm::CmpInst,
                  llvm::SelectInst>(Inst))
      continue;
    return nullptr;
  }
  if (!SawStore)
    return nullptr;
  return Br->getSuccessor(0);
}

bool LLVMCWriter::joinPrintsNext(const llvm::BasicBlock *From,
                                 const llvm::BasicBlock *Join) {
  if (!From || !Join || !From->getParent())
    return false;
  // A bridge that only branches is not printed, so the else target after
  // that bridge is still the next statement.
  const llvm::BasicBlock *Printed = printBranchTarget(Join);
  if (!Printed)
    return false;
  const llvm::Function *Fn = From->getParent();
  bool SeenFrom = false;
  for (const llvm::BasicBlock &BB : *Fn) {
    if (!SeenFrom) {
      if (&BB == From)
        SeenFrom = true;
      continue;
    }
    if (FoldedJoinArms.count(&BB) || InlinedFallthroughBlocks.count(&BB) ||
        ConditionChainBodies.count(&BB))
      continue;
    if (&BB != &Fn->getEntryBlock() && llvm::pred_empty(&BB))
      continue;
    if (isPrintPassthrough(&BB))
      continue;
    if (llvm::isa<llvm::CatchSwitchInst>(BB.getTerminator()) ||
        isCatchPadBlock(BB) || hasCleanupPad(BB))
      continue;
    return &BB == Printed;
  }
  return false;
}

const llvm::BasicBlock *
LLVMCWriter::elseBodyFallsIntoNext(const llvm::BasicBlock *From,
                                   const llvm::BasicBlock *Else) {
  if (!From || !Else || From == Else || !From->getParent() ||
      Else->getParent() != From->getParent())
    return nullptr;
  if (Else->getSinglePredecessor() != From)
    return nullptr;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(1) != Else)
    return nullptr;
  if (Else->isLandingPad() || isPrintPassthrough(Else) ||
      FoldedJoinArms.count(Else) || isCatchPadBlock(*Else) ||
      hasCleanupPad(*Else) ||
      llvm::isa<llvm::CatchSwitchInst>(Else->getTerminator()))
    return nullptr;
  for (const llvm::Instruction &Inst : *Else)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return nullptr;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Else->getTerminator());
  if (!Br || Br->getSuccessor(0) == Else)
    return nullptr;
  bool SeenFrom = false;
  bool ElseAfter = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (&BB == From)
      SeenFrom = true;
    else if (SeenFrom && &BB == Else)
      ElseAfter = true;
  }
  if (!ElseAfter)
    return nullptr;
  if (joinPrintsNext(From, Else))
    return nullptr;
  if (!joinPrintsNext(From, Br->getSuccessor(0)))
    return nullptr;
  return Else;
}

const llvm::BasicBlock *
LLVMCWriter::straightLineTrueArm(const llvm::BasicBlock *From,
                                 const llvm::BasicBlock *Then,
                                 bool InlineAssignArm) {
  if (!From || !Then || From == Then || !From->getParent() ||
      Then->getParent() != From->getParent())
    return nullptr;
  if (Then->getSinglePredecessor() != From)
    return nullptr;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(0) != Then)
    return nullptr;
  const llvm::BasicBlock *Pred = From;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Hops;
  while (isPrintPassthrough(Then)) {
    if (Then->isLandingPad() || FoldedJoinArms.count(Then) ||
        isCatchPadBlock(*Then) || hasCleanupPad(*Then) ||
        !Hops.insert(Then).second || Then->getSinglePredecessor() != Pred)
      return nullptr;
    const auto *HopBr = llvm::cast<llvm::UncondBrInst>(Then->getTerminator());
    if (!HopBr->getSuccessor(0) || HopBr->getSuccessor(0) == Then)
      return nullptr;
    Pred = Then;
    Then = HopBr->getSuccessor(0);
    if (!Then || Then->getParent() != From->getParent())
      return nullptr;
  }
  if (Then == From || Then->getSinglePredecessor() != Pred ||
      Then == FromBr->getSuccessor(1))
    return nullptr;
  if (Then->isLandingPad() || isPrintPassthrough(Then) ||
      FoldedJoinArms.count(Then) || isCatchPadBlock(*Then) ||
      hasCleanupPad(*Then))
    return nullptr;
  for (const llvm::Instruction &Inst : *Then)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return nullptr;
  const llvm::Instruction *Term = Then->getTerminator();
  if (const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
    if (!Br->getSuccessor(0) || Br->getSuccessor(0) == Then)
      return nullptr;
  } else if (!llvm::isa<llvm::ReturnInst>(Term) &&
             !llvm::isa<llvm::UnreachableInst>(Term)) {
    return nullptr;
  }
  bool SeenFrom = false;
  bool ThenAfter = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (&BB == From)
      SeenFrom = true;
    else if (SeenFrom && &BB == Then)
      ThenAfter = true;
  }
  if (!ThenAfter)
    return nullptr;
  // The block that already prints next stays in place so the false edge can
  // fall into it.
  if (joinPrintsNext(From, Then))
    return nullptr;
  if (const llvm::BasicBlock *Join = straightAssignArmJoin(Then)) {
    // Callers that only ask "is this already inside the if?" still treat a
    // store arm as uninlined, so a false-skip keeps that arm outside.
    if (!InlineAssignArm)
      return nullptr;
    const llvm::BasicBlock *Else = FromBr->getSuccessor(1);
    // Both arms of an assign diamond are printed by that fold.
    if (Else && straightAssignArmJoin(Else) == Join)
      return nullptr;
    // Falling into the join would also run the other edge's statements.
    // A cursor-loop exit is past the for, so the arm still prints inside
    // the test and breaks.
    if (joinPrintsNext(From, Join) && !cursorForExitsAt(Join))
      return nullptr;
    if (Else && straightLineFalseSkip(From, Else))
      return nullptr;
  }
  return Then;
}

bool LLVMCWriter::singleEntryUncondTail(
    const llvm::BasicBlock *From, const llvm::BasicBlock *Arm,
    const llvm::BasicBlock *Edge,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Chain) {
  Chain.clear();
  if (!From || !Arm || !Edge || !From->getParent() ||
      Arm->getParent() != From->getParent() ||
      Edge->getParent() != From->getParent())
    return false;
  auto OtherEntry = [&](const llvm::BasicBlock *Join,
                        const llvm::BasicBlock *Coming) {
    if (!Join || Join == Coming)
      return false;
    for (const llvm::BasicBlock *JoinPred : llvm::predecessors(Join))
      if (JoinPred != Coming)
        return true;
    return false;
  };
  const llvm::BasicBlock *Pred = Arm;
  const llvm::BasicBlock *Cur = Edge;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
  for (int Step = 0; Step < 4 && Cur; ++Step) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 4> Hops;
    while (Cur && isPrintPassthrough(Cur)) {
      if (Cur->isLandingPad() || FoldedJoinArms.count(Cur) ||
          isCatchPadBlock(*Cur) || hasCleanupPad(*Cur) ||
          !Hops.insert(Cur).second || Cur->getSinglePredecessor() != Pred)
        return false;
      const auto *Hop = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
      if (!Hop || !Hop->getSuccessor(0) || Hop->getSuccessor(0) == Cur)
        return false;
      Pred = Cur;
      Cur = Hop->getSuccessor(0);
    }
    if (!Cur || Cur == Arm || Cur == From || !Seen.insert(Cur).second)
      return false;
    const llvm::BasicBlock *JoinNow = printBranchTarget(Cur);
    if (JoinNow && JoinNow != Cur && OtherEntry(JoinNow, Pred))
      return !Chain.empty();
    // Scan records a cond-region chain before print. Membership in
    // DuplicatedAssignBlocks must not hide that same chain.
    if (Cur->getSinglePredecessor() != Pred || Cur->isLandingPad() ||
        isPrintPassthrough(Cur) || FoldedJoinArms.count(Cur) ||
        isCatchPadBlock(*Cur) || hasCleanupPad(*Cur)) {
      if (!Chain.empty() && OtherEntry(printBranchTarget(Cur), Pred))
        return true;
      Chain.clear();
      return false;
    }
    for (const llvm::Instruction &Inst : *Cur)
      if (llvm::isa<llvm::PHINode>(Inst)) {
        Chain.clear();
        return false;
      }
    const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == Cur ||
        edgePrintsPhiCopy(Pred, Cur) ||
        edgePrintsPhiCopy(Cur, Br->getSuccessor(0))) {
      Chain.clear();
      return false;
    }
    bool Printed = false;
    bool After = false;
    bool SeenFrom = false;
    for (const llvm::Instruction &Inst : *Cur) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(Inst))
        continue;
      if (instructionIsPrinted(Inst))
        Printed = true;
    }
    for (const llvm::BasicBlock &BB : *From->getParent()) {
      if (&BB == From)
        SeenFrom = true;
      else if (SeenFrom && &BB == Cur)
        After = true;
    }
    if (!Printed || !After) {
      Chain.clear();
      return false;
    }
    Chain.push_back(Cur);
    const llvm::BasicBlock *Join = printBranchTarget(Br->getSuccessor(0));
    if (OtherEntry(Join, Cur))
      return true;
    Pred = Cur;
    Cur = Br->getSuccessor(0);
  }
  Chain.clear();
  return false;
}

bool LLVMCWriter::singleEntryCondRegion(
    const llvm::BasicBlock *From, const llvm::BasicBlock *Edge,
    const llvm::BasicBlock *&Region,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &TrueChain,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &FalseChain) {
  Region = nullptr;
  TrueChain.clear();
  FalseChain.clear();
  if (!From || !Edge || Edge == From || !From->getParent() ||
      Edge->getParent() != From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(0) != Edge)
    return false;
  const llvm::BasicBlock *Pred = From;
  const llvm::BasicBlock *Cur = Edge;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 4> Hops;
  while (Cur && isPrintPassthrough(Cur)) {
    if (!Hops.insert(Cur).second || Cur->getSinglePredecessor() != Pred)
      return false;
    const auto *Hop = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!Hop || !Hop->getSuccessor(0) || Hop->getSuccessor(0) == Cur)
      return false;
    Pred = Cur;
    Cur = Hop->getSuccessor(0);
  }
  if (!Cur || Cur->getSinglePredecessor() != Pred || Cur->isLandingPad() ||
      isCatchPadBlock(*Cur) || hasCleanupPad(*Cur) ||
      FoldedJoinArms.count(Cur))
    return false;
  const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
  if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
    return false;
  for (const llvm::Instruction &Inst : *Cur)
    if (llvm::isa<llvm::PHINode>(Inst))
      return false;
  if (joinPrintsNext(From, Cur) || edgePrintsPhiCopy(Pred, Cur))
    return false;
  if (!singleEntryUncondTail(From, Cur, Br->getSuccessor(0), TrueChain) ||
      !singleEntryUncondTail(From, Cur, Br->getSuccessor(1), FalseChain))
    return false;
  const auto *TrueBr =
      llvm::dyn_cast<llvm::UncondBrInst>(TrueChain.back()->getTerminator());
  const auto *FalseBr =
      llvm::dyn_cast<llvm::UncondBrInst>(FalseChain.back()->getTerminator());
  if (!TrueBr || !FalseBr ||
      printBranchTarget(TrueBr->getSuccessor(0)) !=
          printBranchTarget(FalseBr->getSuccessor(0)))
    return false;
  Region = Cur;
  return true;
}

bool LLVMCWriter::cursorForLoop(const llvm::BasicBlock *Header,
                                 const llvm::BasicBlock *&Latch,
                                 const llvm::BasicBlock *&Exit,
                                 const llvm::BasicBlock *&Body,
                                 const llvm::AllocaInst *&Slot) {
  Latch = nullptr;
  Exit = nullptr;
  Body = nullptr;
  Slot = nullptr;
  if (!Header || !Header->getParent())
    return false;
  const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Header->getTerminator());
  if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
    return false;
  auto ReachesBlock = [&](const llvm::BasicBlock *Start,
                          const llvm::BasicBlock *Target) {
    if (!Start || !Target)
      return false;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
    llvm::SmallVector<const llvm::BasicBlock *, 8> Work{Start};
    while (!Work.empty()) {
      const llvm::BasicBlock *Cur = Work.pop_back_val();
      if (!Cur || Cur == Header || !Seen.insert(Cur).second)
        continue;
      if (Cur == Target)
        return true;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
        Work.push_back(Succ);
    }
    return false;
  };
  const llvm::BasicBlock *Back = nullptr;
  int Preds = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Header)) {
    ++Preds;
    bool FromLoop = false;
    for (const llvm::BasicBlock *Succ : llvm::successors(Header))
      if (ReachesBlock(Succ, Pred))
        FromLoop = true;
    if (!FromLoop)
      continue;
    if (Back)
      return false;
    Back = Pred;
  }
  if (Preds != 2 || !Back)
    return false;
  const llvm::StoreInst *CursorStore = nullptr;
  for (const llvm::Instruction &Inst : *Back) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI)
      continue;
    const auto *A = asAllocaPointer(SI->getPointerOperand());
    if (!A)
      continue;
    Slot = A;
    CursorStore = SI;
  }
  if (!Slot || !CursorStore)
    return false;
  bool LoadsSlot = false;
  for (const llvm::Instruction &Inst : *Header) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst);
    if (LI && asAllocaPointer(LI->getPointerOperand()) == Slot)
      LoadsSlot = true;
  }
  if (!LoadsSlot)
    return false;
  auto Reaches = [&](const llvm::Value *From, const llvm::Value *Target) {
    llvm::SmallPtrSet<const llvm::Value *, 16> Seen;
    llvm::SmallVector<const llvm::Value *, 8> Work{From};
    while (!Work.empty()) {
      const llvm::Value *Cur = Work.pop_back_val();
      if (!Cur || !Seen.insert(Cur).second)
        continue;
      if (Cur == Target)
        return true;
      for (const llvm::User *U : Cur->users())
        Work.push_back(U);
    }
    return false;
  };
  for (const llvm::Instruction &Inst : *Back) {
    if (&Inst == CursorStore || Inst.isTerminator())
      continue;
    if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
      if (instructionIsPrinted(Inst) &&
          asAllocaPointer(SI->getPointerOperand()) != Slot)
        return false;
      continue;
    }
    if (llvm::isa<llvm::CallBase>(Inst) && !Reaches(&Inst, CursorStore))
      return false;
  }
  const bool TrueHits = ReachesBlock(Br->getSuccessor(0), Back);
  const bool FalseHits = ReachesBlock(Br->getSuccessor(1), Back);
  if (TrueHits == FalseHits)
    return false;
  Body = TrueHits ? Br->getSuccessor(0) : Br->getSuccessor(1);
  Exit = TrueHits ? Br->getSuccessor(1) : Br->getSuccessor(0);
  Latch = Back;
  return Body && Exit && Body != Latch && Exit != Latch;
}

std::string LLVMCWriter::cursorFieldIncText(const llvm::BasicBlock *BB,
                                             const llvm::Instruction *Stop) {
  if (!BB)
    return {};
  // Field peels walk address temps. Replay this block's stores so a later
  // detection pass sees the same pointer as the statement printer.
  const auto SavedLast = AllocaLastValues;
  AllocaLastValues.clear();
  std::string Field;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Stop)
      break;
    if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(SI->getPointerOperand()))
        AllocaLastValues[Home] = SI->getValueOperand();
    }
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst);
    if (!LI)
      continue;
    if (auto Acc = typedRecordAccess(LI->getPointerOperand(),
                                     llvmAccessSize(LI->getType()));
        Acc && Acc->Text.find("->") != std::string::npos) {
      Field = Acc->Text;
      continue;
    }
    if (auto Text = ValueTexts.find(LI);
        Text != ValueTexts.end() &&
        Text->second.find("->") != std::string::npos)
      Field = Text->second;
  }
  AllocaLastValues = SavedLast;
  return Field;
}

void LLVMCWriter::writeCursorFor(const llvm::BasicBlock *Header,
                                 const llvm::BasicBlock *Latch,
                                 const llvm::BasicBlock *Exit,
                                 const llvm::BasicBlock *Body,
                                 const llvm::AllocaInst *Slot, int Indent) {
  const auto *Br = llvm::cast<llvm::CondBrInst>(Header->getTerminator());
  const auto SavedLast = AllocaLastValues;
  const auto SavedImm = AllocaImmediates;
  AllocaLastValues.clear();
  AllocaImmediates.clear();
  for (const llvm::Instruction &Inst : *Header) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI || SI->getParent() != Header)
      continue;
    if (const llvm::AllocaInst *Home =
            asAllocaPointer(SI->getPointerOperand()))
      AllocaLastValues[Home] = SI->getValueOperand();
  }
  std::string Cond = condStr(Br->getCondition());
  const llvm::StoreInst *CursorStore = nullptr;
  for (const llvm::Instruction &Inst : *Latch) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI || asAllocaPointer(SI->getPointerOperand()) != Slot)
      continue;
    CursorStore = SI;
  }
  AllocaLastValues.clear();
  for (const llvm::Instruction &Inst : *Latch) {
    if (&Inst == CursorStore)
      break;
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI)
      continue;
    if (const llvm::AllocaInst *Home =
            asAllocaPointer(SI->getPointerOperand()))
      AllocaLastValues[Home] = SI->getValueOperand();
  }
  std::string Rhs =
      CursorStore ? valueStr(CursorStore->getValueOperand()) : std::string();
  auto IncText = [](const std::string &Text) {
    return Text.find('(') != std::string::npos ||
           Text.find("->") != std::string::npos;
  };
  // A copied next-pointer load names the cursor slot. The field load that
  // produced it is earlier in the latch.
  if (CursorStore && !IncText(Rhs)) {
    if (std::string Field = cursorFieldIncText(Latch, CursorStore);
        IncText(Field))
      Rhs = std::move(Field);
    else if (std::string Walk =
                 ultimateComposedText(CursorStore->getValueOperand());
             IncText(Walk))
      Rhs = std::move(Walk);
  }
  const std::string Lhs = getName(const_cast<llvm::AllocaInst *>(Slot));
  AllocaLastValues = SavedLast;
  AllocaImmediates = SavedImm;
  const bool ExitOnTrue = printBranchTarget(Br->getSuccessor(0)) ==
                          printBranchTarget(Exit);
  std::string ForCond = Cond;
  if (ExitOnTrue) {
    if (Cond.size() >= 3 && Cond[0] == '!' && Cond[1] == '(' &&
        Cond.back() == ')')
      ForCond = Cond.substr(2, Cond.size() - 3);
    else if (!Cond.empty())
      ForCond = "!(" + Cond + ")";
  }
  if (ForCond.empty()) {
    for (const llvm::Instruction &Inst : *Header)
      if (!llvm::isa<llvm::AllocaInst>(Inst))
        writeInstruction(const_cast<llvm::Instruction &>(Inst), Indent);
    return;
  }
  const bool IncExpr = !Lhs.empty() && !Rhs.empty() &&
                       (Rhs.find('(') != std::string::npos ||
                        Rhs.find("->") != std::string::npos);
  emitIndent(Indent);
  if (IncExpr)
    OS << "for (; " << ForCond << "; " << Lhs << " = " << Rhs << ") {\n";
  else
    OS << "for (; " << ForCond << ";) {\n";
  writeBasicBlock(*Body, Indent + 1);
  if (!IncExpr) {
    for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Latch)) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      writeInstruction(Inst, Indent + 1);
    }
  }
  InlinedFallthroughBlocks.insert(Body);
  InlinedFallthroughBlocks.insert(Latch);
  emitIndent(Indent);
  OS << "}\n";
  (void)Exit;
}

namespace {
struct CursorForContinue {
  const llvm::BasicBlock *Header = nullptr;
  const llvm::BasicBlock *Latch = nullptr;
  const llvm::BasicBlock *Step = nullptr;
  const llvm::BasicBlock *Exit = nullptr;
  int Depth = 0;
};
CursorForContinue &cursorForContinue() {
  static CursorForContinue State;
  return State;
}
} // namespace

bool LLVMCWriter::cursorCopyLatch(const llvm::BasicBlock *BB) {
  if (!BB || !BB->getParent())
    return false;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
  if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == BB)
    return false;
  const llvm::BasicBlock *Succ = Br->getSuccessor(0);
  const llvm::BasicBlock *Pred = BB->getSinglePredecessor();
  if (!Pred)
    return false;
  bool Reaches = false;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
  llvm::SmallVector<const llvm::BasicBlock *, 8> Work{Succ};
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || !Seen.insert(Cur).second)
      continue;
    if (Cur == Pred) {
      Reaches = true;
      break;
    }
    if (Cur == BB)
      continue;
    for (const llvm::BasicBlock *Next : llvm::successors(Cur))
      Work.push_back(Next);
  }
  if (!Reaches)
    return false;
  const llvm::AllocaInst *Slot = nullptr;
  const llvm::StoreInst *Store = nullptr;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Br)
      continue;
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI)
      continue;
    const llvm::AllocaInst *Home = asAllocaPointer(SI->getPointerOperand());
    if (!Home)
      continue;
    if (Slot)
      return false;
    Slot = Home;
    Store = SI;
  }
  if (!Slot || !Store ||
      !llvm::isa<llvm::LoadInst>(Store->getValueOperand()))
    return false;
  for (const llvm::Instruction &Inst : *Succ) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst);
    if (LI && asAllocaPointer(LI->getPointerOperand()) == Slot)
      return true;
  }
  return false;
}

bool LLVMCWriter::splitPhiCursorLoop(const llvm::BasicBlock *Header,
                                     const llvm::BasicBlock *&Latch,
                                     const llvm::BasicBlock *&Step,
                                     const llvm::AllocaInst *&Slot) {
  Latch = nullptr;
  Step = nullptr;
  Slot = nullptr;
  if (!Header)
    return false;
  const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Header->getTerminator());
  if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
    return false;
  const llvm::BasicBlock *Back = nullptr;
  int Preds = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Header)) {
    ++Preds;
    if (cursorCopyLatch(Pred) &&
        Pred->getTerminator()->getSuccessor(0) == Header)
      Back = Pred;
  }
  if (Preds != 2 || !Back)
    return false;
  auto Reaches = [&](const llvm::BasicBlock *Start,
                     const llvm::BasicBlock *Target) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
    llvm::SmallVector<const llvm::BasicBlock *, 8> Work{Start};
    while (!Work.empty()) {
      const llvm::BasicBlock *Cur = Work.pop_back_val();
      if (!Cur || Cur == Header || !Seen.insert(Cur).second)
        continue;
      if (Cur == Target)
        return true;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
        Work.push_back(Succ);
    }
    return false;
  };
  if (!Reaches(Br->getSuccessor(0), Back) ||
      !Reaches(Br->getSuccessor(1), Back))
    return false;
  const llvm::AllocaInst *Found = nullptr;
  for (const llvm::Instruction &Inst : *Back) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
    if (!SI)
      continue;
    if (const llvm::AllocaInst *Home = asAllocaPointer(SI->getPointerOperand()))
      Found = Home;
  }
  const llvm::BasicBlock *NextBB = Back->getSinglePredecessor();
  if (!Found || !NextBB)
    return false;
  if (cursorFieldIncText(NextBB, nullptr).empty())
    return false;
  Latch = Back;
  Step = NextBB;
  Slot = Found;
  return true;
}

void LLVMCWriter::writeSplitPhiCursorFor(const llvm::BasicBlock *Header,
                                         const llvm::BasicBlock *Latch,
                                         const llvm::BasicBlock *Step,
                                         const llvm::AllocaInst *Slot,
                                         int Indent) {
  const std::string Inc = cursorFieldIncText(Step, nullptr);
  if (Inc.empty() || !Slot)
    return;
  const std::string Name = getName(const_cast<llvm::AllocaInst *>(Slot));
  emitIndent(Indent);
  OS << "for (; " << Name << "; " << Name << " = " << Inc << ") {\n";
  CursorForContinue &State = cursorForContinue();
  const llvm::BasicBlock *SavedHeader = State.Header;
  const llvm::BasicBlock *SavedLatch = State.Latch;
  const llvm::BasicBlock *SavedStep = State.Step;
  const llvm::BasicBlock *SavedExit = State.Exit;
  const int SavedDepth = State.Depth;
  State.Header = Header;
  State.Latch = Latch;
  State.Step = Step;
  State.Exit = nullptr;
  ++State.Depth;
  InlinedFallthroughBlocks.insert(Latch);
  InlinedFallthroughBlocks.insert(Step);
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Region;
  llvm::SmallVector<const llvm::BasicBlock *, 8> Work{Header};
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || Cur == Latch || Cur == Step || !Region.insert(Cur).second)
      continue;
    for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
      Work.push_back(Succ);
  }
  // A block also reached from outside the loop is the shared exit (the
  // format tail). Leave it after the for so the hit arm can branch to it.
  bool Dropped = true;
  while (Dropped) {
    Dropped = false;
    llvm::SmallVector<const llvm::BasicBlock *, 8> Kill;
    for (const llvm::BasicBlock *BB : Region) {
      if (BB == Header)
        continue;
      for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
        if (!Pred || Pred == Header || Pred == Latch || Pred == Step ||
            Region.count(Pred))
          continue;
        Kill.push_back(BB);
        break;
      }
    }
    for (const llvm::BasicBlock *BB : Kill) {
      Region.erase(BB);
      Dropped = true;
    }
  }
  const llvm::BasicBlock *LoopExit = nullptr;
  bool SplitExit = false;
  for (const llvm::BasicBlock *BB : Region) {
    for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
      if (!Succ || Succ == Latch || Succ == Step || Region.count(Succ))
        continue;
      const llvm::BasicBlock *Target = printBranchTarget(Succ);
      if (!Target || Target == Latch || Target == Step || Region.count(Target))
        continue;
      if (!LoopExit)
        LoopExit = Target;
      else if (LoopExit != Target)
        SplitExit = true;
    }
  }
  bool ExitFollows = LoopExit && !SplitExit;
  if (ExitFollows) {
    bool SeenHeader = false;
    for (const llvm::BasicBlock &BB : *Header->getParent()) {
      if (&BB == Header) {
        SeenHeader = true;
        continue;
      }
      if (!SeenHeader)
        continue;
      if (&BB == LoopExit)
        break;
      if (Region.count(&BB) || &BB == Latch || &BB == Step ||
          isPrintPassthrough(&BB) || InlinedFallthroughBlocks.count(&BB) ||
          FoldedJoinArms.count(&BB))
        continue;
      ExitFollows = false;
      break;
    }
  }
  State.Exit = ExitFollows ? LoopExit : nullptr;
  for (const llvm::BasicBlock &BB : *Header->getParent()) {
    if (!Region.count(&BB))
      continue;
    if (&BB == Header) {
      for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Header)) {
        if (llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        writeInstruction(Inst, Indent + 1);
      }
    } else {
      writeBasicBlock(BB, Indent + 1);
    }
    InlinedFallthroughBlocks.insert(&BB);
  }
  State.Header = SavedHeader;
  State.Latch = SavedLatch;
  State.Step = SavedStep;
  State.Exit = SavedExit;
  State.Depth = SavedDepth;
  emitIndent(Indent);
  OS << "}\n";
}

bool LLVMCWriter::assignSelectElseIf(
    const llvm::BasicBlock *From, const llvm::BasicBlock *FirstArm,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Conds,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Arms,
    const llvm::BasicBlock *&Default, const llvm::BasicBlock *&Join) {
  Conds.clear();
  Arms.clear();
  Default = nullptr;
  Join = nullptr;
  auto AssignTo = [&](const llvm::BasicBlock *BB,
                      const llvm::BasicBlock *Want) {
    if (!BB || !Want || BB == Want || BB->isLandingPad() ||
        isCatchPadBlock(*BB) || hasCleanupPad(*BB))
      return false;
    const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
    if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == BB ||
        printBranchTarget(Br->getSuccessor(0)) != Want ||
        edgePrintsPhiCopy(BB, Br->getSuccessor(0)))
      return false;
    bool SawStore = false;
    bool Printed = false;
    for (const llvm::Instruction &Inst : *BB) {
      if (&Inst == Br)
        break;
      if (llvm::isa<llvm::PHINode>(Inst))
        return false;
      if (llvm::isa<llvm::StoreInst>(Inst)) {
        SawStore = true;
        if (instructionIsPrinted(Inst))
          Printed = true;
        continue;
      }
      if (llvm::isa<llvm::LoadInst, llvm::CastInst, llvm::GetElementPtrInst,
                    llvm::FreezeInst, llvm::BinaryOperator, llvm::CmpInst,
                    llvm::SelectInst>(Inst)) {
        if (instructionIsPrinted(Inst))
          Printed = true;
        continue;
      }
      return false;
    }
    return SawStore && Printed;
  };
  if (!From || !FirstArm || !From->getParent() ||
      FirstArm->getParent() != From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(0) == FromBr->getSuccessor(1))
    return false;
  const llvm::BasicBlock *Cont = printBranchTarget(FromBr->getSuccessor(0));
  const auto *FirstBr =
      llvm::dyn_cast<llvm::UncondBrInst>(FirstArm->getTerminator());
  if (!Cont || !FirstBr || !FirstBr->getSuccessor(0))
    return false;
  Join = printBranchTarget(FirstBr->getSuccessor(0));
  if (!Join || Join == FirstArm || Join == From || !AssignTo(FirstArm, Join))
    return false;
  Conds.push_back(From);
  Arms.push_back(FirstArm);
  const llvm::BasicBlock *Prev = From;
  for (int Step = 0; Step < 3 && Cont; ++Step) {
    if (Cont->getSinglePredecessor() != Prev || Cont->isLandingPad() ||
        isCatchPadBlock(*Cont) || hasCleanupPad(*Cont))
      return false;
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cont->getTerminator());
    if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
      return false;
    if (edgePrintsPhiCopy(Cont, Br->getSuccessor(0)) ||
        edgePrintsPhiCopy(Cont, Br->getSuccessor(1)))
      return false;
    const llvm::BasicBlock *Arm =
        straightLineTrueArm(Cont, Br->getSuccessor(0), true);
    if (!Arm || !AssignTo(Arm, Join))
      return false;
    Conds.push_back(Cont);
    Arms.push_back(Arm);
    const llvm::BasicBlock *FalseT = printBranchTarget(Br->getSuccessor(1));
    if (FalseT && AssignTo(FalseT, Join) && FalseT != Arm && FalseT != Join) {
      Default = FalseT;
      return Conds.size() >= 2;
    }
    if (!FalseT || FalseT->getSinglePredecessor() != Cont)
      return false;
    Prev = Cont;
    Cont = FalseT;
  }
  return false;
}

bool LLVMCWriter::blockOnlyFeedsBranch(const llvm::BasicBlock *BB) {
  if (!BB || !BB->getTerminator())
    return false;
  const llvm::Instruction *Term = BB->getTerminator();
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Term || llvm::isa<llvm::AllocaInst>(Inst))
      continue;
    llvm::SmallPtrSet<const llvm::Instruction *, 16> Seen;
    std::function<bool(const llvm::Instruction *)> Feeds =
        [&](const llvm::Instruction *I) -> bool {
      if (!I || I == Term)
        return true;
      if (I->getParent() != BB)
        return false;
      if (!Seen.insert(I).second)
        return true;
      if (I->use_empty())
        return !instructionIsPrinted(*I);
      for (const llvm::User *U : I->users()) {
        const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
        if (!UI || !Feeds(UI))
          return false;
      }
      return true;
    };
    if (!Feeds(&Inst))
      return false;
  }
  return true;
}

bool LLVMCWriter::conditionChainElse(
    const llvm::BasicBlock *From,
    std::vector<const llvm::BasicBlock *> &Conds, const llvm::BasicBlock *&Arm,
    const llvm::BasicBlock *&Def, const llvm::BasicBlock *&Join,
    bool &InvertCond) {
  auto Attempt = [&](bool ContinueOnFalse) -> bool {
  Conds.clear();
  Arm = nullptr;
  Def = nullptr;
  Join = nullptr;
  InvertCond = ContinueOnFalse;
  if (!From || !From->getParent())
    return false;
  const llvm::BasicBlock *Cur = From;
  const llvm::BasicBlock *Prev = nullptr;
  const llvm::BasicBlock *Default = nullptr;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
  while (Cur && Seen.insert(Cur).second && Conds.size() < 4) {
    if (Prev && Cur->getSinglePredecessor() != Prev)
      return false;
    if (Cur->isLandingPad() || isCatchPadBlock(*Cur) || hasCleanupPad(*Cur))
      return false;
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
    if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
      return false;
    if (!blockOnlyFeedsBranch(Cur))
      return false;
    const unsigned ContIdx = ContinueOnFalse ? 1 : 0;
    const unsigned DefIdx = ContinueOnFalse ? 0 : 1;
    const llvm::BasicBlock *ContEdge = Br->getSuccessor(ContIdx);
    const llvm::BasicBlock *DefT = printBranchTarget(Br->getSuccessor(DefIdx));
    if (!ContEdge || !DefT || DefT == ContEdge)
      return false;
    if (edgePrintsPhiCopy(Cur, Br->getSuccessor(0)) ||
        edgePrintsPhiCopy(Cur, Br->getSuccessor(1)))
      return false;
    if (!Default)
      Default = DefT;
    else if (DefT != Default)
      return false;
    Conds.push_back(Cur);
    // The arm may be the next block. These other edges go to Default.
    const llvm::BasicBlock *Body = nullptr;
    if (ContEdge->getSinglePredecessor() == Cur &&
        !ContEdge->isLandingPad() && !isPrintPassthrough(ContEdge) &&
        !FoldedJoinArms.count(ContEdge) && !isCatchPadBlock(*ContEdge) &&
        !hasCleanupPad(*ContEdge)) {
      bool Phi = false;
      bool Printed = false;
      for (const llvm::Instruction &Inst : *ContEdge) {
        if (llvm::isa<llvm::PHINode>(Inst))
          Phi = true;
        else if (!Inst.isTerminator() && instructionIsPrinted(Inst))
          Printed = true;
      }
      const auto *Out =
          llvm::dyn_cast<llvm::UncondBrInst>(ContEdge->getTerminator());
      bool Later = false;
      bool SeenCur = false;
      for (const llvm::BasicBlock &BB : *Cur->getParent()) {
        if (&BB == Cur)
          SeenCur = true;
        else if (SeenCur && &BB == ContEdge)
          Later = true;
      }
      if (!Phi && Printed && Later && Out && Out->getSuccessor(0) &&
          Out->getSuccessor(0) != ContEdge &&
          !edgePrintsPhiCopy(ContEdge, Out->getSuccessor(0)))
        Body = ContEdge;
    }
    if (Body) {
      const auto *Out = llvm::cast<llvm::UncondBrInst>(Body->getTerminator());
      Arm = Body;
      Join = printBranchTarget(Out->getSuccessor(0));
      break;
    }
    const llvm::BasicBlock *Next = printBranchTarget(ContEdge);
    if (!Next || Next == Cur ||
        !llvm::isa<llvm::CondBrInst>(Next->getTerminator()))
      return false;
    Prev = Cur;
    Cur = Next;
  }
  if (!Arm || !Default || !Join || Conds.size() < 2 || Default == Join ||
      Default == Arm || Join == Arm)
    return false;
  if (Default->isLandingPad() || isCatchPadBlock(*Default) ||
      hasCleanupPad(*Default))
    return false;
  const auto *DefBr = llvm::dyn_cast<llvm::UncondBrInst>(Default->getTerminator());
  if (!DefBr || printBranchTarget(DefBr->getSuccessor(0)) != Join)
    return false;
  if (edgePrintsPhiCopy(Default, DefBr->getSuccessor(0)))
    return false;
  bool DefPrints = false;
  for (const llvm::Instruction &Inst : *Default) {
    if (Inst.isTerminator())
      continue;
    if (instructionIsPrinted(Inst))
      DefPrints = true;
  }
  if (!DefPrints)
    return false;
  bool ArmPrints = false;
  for (const llvm::Instruction &Inst : *Arm) {
    if (Inst.isTerminator())
      continue;
    if (instructionIsPrinted(Inst))
      ArmPrints = true;
  }
  if (!ArmPrints)
    return false;
  auto FromChain = [&](const llvm::BasicBlock *Pred) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Hop;
    while (Pred && Hop.insert(Pred).second) {
      if (llvm::is_contained(Conds, Pred))
        return true;
      if (!isPrintPassthrough(Pred))
        return false;
      Pred = Pred->getSinglePredecessor();
    }
    return false;
  };
  unsigned Preds = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Default)) {
    ++Preds;
    if (!FromChain(Pred))
      return false;
  }
  if (Preds == 0)
    return false;
  bool SeenFrom = false;
  bool DefAfter = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (&BB == From)
      SeenFrom = true;
    else if (SeenFrom && &BB == Default)
      DefAfter = true;
  }
  if (!DefAfter)
    return false;
  Def = Default;
  return true;
  };
  return Attempt(false) || Attempt(true);
}

bool LLVMCWriter::sharedAssignElse(const llvm::BasicBlock *From,
                                   const llvm::BasicBlock *Body,
                                   const llvm::BasicBlock *&Alt,
                                   const llvm::BasicBlock *&Def,
                                   const llvm::BasicBlock *&Join) {
  Alt = nullptr;
  Def = nullptr;
  Join = nullptr;
  if (!From || !Body || From == Body)
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  const auto *Inner = llvm::dyn_cast<llvm::CondBrInst>(Body->getTerminator());
  if (!FromBr || !Inner || FromBr->getSuccessor(0) == FromBr->getSuccessor(1) ||
      Inner->getSuccessor(0) == Inner->getSuccessor(1))
    return false;
  Alt = straightLineFalseSkip(Body, Inner->getSuccessor(1));
  if (!Alt)
    return false;
  Def = printBranchTarget(Inner->getSuccessor(0));
  if (!Def || Def == Alt || Def == Body || Def == From ||
      Def != printBranchTarget(FromBr->getSuccessor(0)))
    return false;
  auto AssignJoin = [&](const llvm::BasicBlock *Arm) -> const llvm::BasicBlock * {
    if (!Arm || Arm->isLandingPad() || isCatchPadBlock(*Arm) ||
        hasCleanupPad(*Arm))
      return nullptr;
    const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Arm->getTerminator());
    if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == Arm)
      return nullptr;
    bool SawStore = false;
    bool Printed = false;
    for (const llvm::Instruction &Inst : *Arm) {
      if (&Inst == Arm->getTerminator())
        break;
      if (llvm::isa<llvm::PHINode>(Inst))
        return nullptr;
      if (llvm::isa<llvm::StoreInst>(Inst)) {
        SawStore = true;
        if (instructionIsPrinted(Inst))
          Printed = true;
        continue;
      }
      if (llvm::isa<llvm::LoadInst, llvm::CastInst, llvm::GetElementPtrInst,
                    llvm::FreezeInst, llvm::BinaryOperator, llvm::CmpInst,
                    llvm::SelectInst>(Inst)) {
        if (instructionIsPrinted(Inst))
          Printed = true;
        continue;
      }
      return nullptr;
    }
    if (!SawStore || !Printed)
      return nullptr;
    return Br->getSuccessor(0);
  };
  const llvm::BasicBlock *DefJoin = AssignJoin(Def);
  const llvm::BasicBlock *AltJoin = AssignJoin(Alt);
  if (!DefJoin || DefJoin != AltJoin)
    return false;
  auto UsesBlock = [](const llvm::BasicBlock *Arm,
                      const llvm::BasicBlock *Home) {
    if (!Arm || !Home)
      return false;
    for (const llvm::Instruction &Inst : *Arm)
      for (const llvm::Use &Op : Inst.operands()) {
        const auto *Origin = llvm::dyn_cast<llvm::Instruction>(Op.get());
        if (Origin && Origin->getParent() == Home)
          return true;
      }
    return false;
  };
  if (UsesBlock(Def, Body) || UsesBlock(Def, Alt))
    return false;
  if (edgePrintsPhiCopy(From, FromBr->getSuccessor(0)) ||
      edgePrintsPhiCopy(Body, Inner->getSuccessor(0)) ||
      edgePrintsPhiCopy(Body, Inner->getSuccessor(1)) ||
      edgePrintsPhiCopy(Def, DefJoin) || edgePrintsPhiCopy(Alt, AltJoin))
    return false;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Origins;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
  llvm::SmallVector<const llvm::BasicBlock *, 8> Work;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Def))
    Work.push_back(Pred);
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || !Seen.insert(Cur).second)
      continue;
    if (Cur == From || Cur == Body) {
      Origins.insert(Cur);
      continue;
    }
    const auto *Hop = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    bool Call = false;
    if (Hop) {
      for (const llvm::Instruction &Inst : *Cur)
        if (llvm::isa<llvm::CallBase>(Inst)) {
          Call = true;
          break;
        }
    }
    if (Hop && !Call) {
      bool Any = false;
      for (const llvm::BasicBlock *Pred : llvm::predecessors(Cur)) {
        Any = true;
        Work.push_back(Pred);
      }
      if (!Any)
        Origins.insert(Cur);
      continue;
    }
    Origins.insert(Cur);
  }
  if (Origins.size() != 2 || !Origins.count(From) || !Origins.count(Body))
    return false;
  Join = DefJoin;
  return true;
}

const llvm::BasicBlock *
LLVMCWriter::straightLineFalseSkip(const llvm::BasicBlock *From,
                                   const llvm::BasicBlock *Else) {
  if (!From || !Else || From == Else || !From->getParent() ||
      Else->getParent() != From->getParent())
    return nullptr;
  if (Else->getSinglePredecessor() != From)
    return nullptr;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(1) != Else)
    return nullptr;
  const llvm::BasicBlock *Then = FromBr->getSuccessor(0);
  if (!Then || Then == Else)
    return nullptr;
  // An empty branch is not printed. The body is the block it falls into
  // when that block has no other entry.
  const llvm::BasicBlock *Pred = From;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Hops;
  while (isPrintPassthrough(Else)) {
    if (!Hops.insert(Else).second || Else->getSinglePredecessor() != Pred)
      return nullptr;
    const auto *Hop = llvm::cast<llvm::UncondBrInst>(Else->getTerminator());
    Pred = Else;
    Else = Hop->getSuccessor(0);
    if (!Else || Else == Pred)
      return nullptr;
  }
  if (Else->getSinglePredecessor() != Pred)
    return nullptr;
  if (Else->isLandingPad() || FoldedJoinArms.count(Else) ||
      isCatchPadBlock(*Else) || hasCleanupPad(*Else) ||
      llvm::isa<llvm::CatchSwitchInst>(Else->getTerminator()))
    return nullptr;
  for (const llvm::Instruction &Inst : *Else)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return nullptr;
  const llvm::Instruction *Term = Else->getTerminator();
  const auto *InnerBr = llvm::dyn_cast<llvm::CondBrInst>(Term);
  if (const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
    if (!Br->getSuccessor(0) || Br->getSuccessor(0) == Else)
      return nullptr;
  } else if (InnerBr) {
    if (InnerBr->getSuccessor(0) == InnerBr->getSuccessor(1))
      return nullptr;
  } else if (!llvm::isa<llvm::ReturnInst>(Term)) {
    return nullptr;
  }
  bool SeenFrom = false;
  bool ElseAfter = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (&BB == From)
      SeenFrom = true;
    else if (SeenFrom && &BB == Else)
      ElseAfter = true;
  }
  if (!ElseAfter)
    return nullptr;
  // A nested branch whose false edge is this skip target still belongs
  // inside the inverted if. Its straight true arm is not the next block.
  if (InnerBr &&
      printBranchTarget(InnerBr->getSuccessor(1)) == printBranchTarget(Then)) {
    // A condition-only successor is one conjunction, not a nested if.
    if (andRejoinTest(From, Else))
      return nullptr;
    const llvm::BasicBlock *Arm = InnerBr->getSuccessor(0);
    if (!straightLineTrueArm(Else, Arm) || !joinPrintsNext(From, Else))
      return nullptr;
    if (edgePrintsPhiCopy(From, Else) || edgePrintsPhiCopy(From, Then) ||
        edgePrintsPhiCopy(Else, Arm) ||
        edgePrintsPhiCopy(Else, InnerBr->getSuccessor(1)))
      return nullptr;
    if (straightLineTrueArm(From, Then))
      return nullptr;
    FoldedJoinArms.insert(Else);
    FoldedJoinArms.insert(Arm);
    const bool NextIsThen = joinPrintsNext(From, Then);
    FoldedJoinArms.erase(Else);
    FoldedJoinArms.erase(Arm);
    if (!NextIsThen)
      return nullptr;
    return Else;
  }
  // This body is already a skip of the same target. Print it inside.
  if (InnerBr &&
      printBranchTarget(InnerBr->getSuccessor(0)) == printBranchTarget(Then) &&
      straightLineFalseSkip(Else, InnerBr->getSuccessor(1)) &&
      !andRejoinTest(From, Else) && joinPrintsNext(From, Else) &&
      !edgePrintsPhiCopy(From, Else) && !edgePrintsPhiCopy(From, Then) &&
      !edgePrintsPhiCopy(Else, InnerBr->getSuccessor(0)) &&
      !edgePrintsPhiCopy(Else, InnerBr->getSuccessor(1)) &&
      !straightLineTrueArm(From, Then))
    return Else;
  // Work, then an unconditional branch into a skip of this same target.
  if (const auto *Bridge = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
    const llvm::BasicBlock *Check = Bridge->getSuccessor(0);
    const auto *CheckBr =
        Check ? llvm::dyn_cast<llvm::CondBrInst>(Check->getTerminator())
              : nullptr;
    if (Check && CheckBr && Check->getSinglePredecessor() == Else &&
        !Check->isLandingPad() && !isCatchPadBlock(*Check) &&
        !hasCleanupPad(*Check) && !isPrintPassthrough(Check) &&
        printBranchTarget(CheckBr->getSuccessor(0)) ==
            printBranchTarget(Then) &&
        straightLineFalseSkip(Check, CheckBr->getSuccessor(1)) &&
        joinPrintsNext(From, Else) && joinPrintsNext(Else, Check) &&
        !edgePrintsPhiCopy(From, Else) && !edgePrintsPhiCopy(From, Then) &&
        !edgePrintsPhiCopy(Else, Check) &&
        !edgePrintsPhiCopy(Check, CheckBr->getSuccessor(0)) &&
        !edgePrintsPhiCopy(Check, CheckBr->getSuccessor(1)) &&
        !straightLineTrueArm(From, Then)) {
      const llvm::BasicBlock *Arm = CheckBr->getSuccessor(1);
      FoldedJoinArms.insert(Else);
      FoldedJoinArms.insert(Check);
      FoldedJoinArms.insert(Arm);
      const llvm::BasicBlock *Want = printBranchTarget(Then);
      bool NextIsThen = false;
      bool SeenFrom = false;
      for (const llvm::BasicBlock &BB : *From->getParent()) {
        if (!SeenFrom) {
          if (&BB == From)
            SeenFrom = true;
          continue;
        }
        if (FoldedJoinArms.count(&BB) || InlinedFallthroughBlocks.count(&BB))
          continue;
        if (&BB != &From->getParent()->getEntryBlock() &&
            llvm::pred_empty(&BB))
          continue;
        if (isPrintPassthrough(&BB) || isCatchPadBlock(BB) ||
            hasCleanupPad(BB) ||
            llvm::isa<llvm::CatchSwitchInst>(BB.getTerminator()))
          continue;
        NextIsThen = &BB == Want || printBranchTarget(&BB) == Want;
        break;
      }
      FoldedJoinArms.erase(Else);
      FoldedJoinArms.erase(Check);
      FoldedJoinArms.erase(Arm);
      if (NextIsThen)
        return Else;
    }
  }
  // The false body is the next printed block and the true target is the
  // block after that, so inverting the condition lets the target fall through.
  if (!joinPrintsNext(From, Else) || !joinPrintsNext(Else, Then))
    return nullptr;
  if (edgePrintsPhiCopy(From, Else) || edgePrintsPhiCopy(From, Then))
    return nullptr;
  // A straight true arm is already printed inside the original if.
  // Both store arms of an assign diamond are printed by that fold.
  if (straightLineTrueArm(From, Then))
    return nullptr;
  const llvm::BasicBlock *ElseJoin = straightAssignArmJoin(Else);
  if (ElseJoin && ElseJoin == straightAssignArmJoin(Then))
    return nullptr;
  return Else;
}

bool LLVMCWriter::sameTargetReleaseRegion(
    const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
    const llvm::BasicBlock *&Join) {
  Region.clear();
  Join = nullptr;
  if (!From || !Else || From == Else || !From->getParent() ||
      Else->getParent() != From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(1) != Else ||
      FromBr->getSuccessor(0) == Else)
    return false;
  const llvm::BasicBlock *Then = FromBr->getSuccessor(0);
  if (!Then || edgePrintsPhiCopy(From, Then) || edgePrintsPhiCopy(From, Else))
    return false;
  Join = printBranchTarget(Then);
  if (!Join || Join == From || Join == Else || Join->getParent() != From->getParent())
    return false;
  if (!joinPrintsNext(From, Else) || straightLineFalseSkip(From, Else) ||
      straightLineTrueArm(From, Then) || andRejoinTest(From, Else))
    return false;
  if (InlinedFallthroughBlocks.count(Else) || FoldedJoinArms.count(Else) ||
      ConditionChainBodies.count(Else) || DuplicatedAssignBlocks.count(Else))
    return false;

  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> In;
  llvm::SmallVector<const llvm::BasicBlock *, 16> Work{Else};
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || Cur == From || Cur == Join || In.count(Cur))
      continue;
    if (isPrintPassthrough(Cur) && printBranchTarget(Cur) == Join)
      continue;
    if (In.size() >= 24 || Cur->getParent() != From->getParent())
      return false;
    if (Cur->isLandingPad() || isCatchPadBlock(*Cur) || hasCleanupPad(*Cur))
      return false;
    const llvm::Instruction *Term = Cur->getTerminator();
    if (!Term || (!llvm::isa<llvm::UncondBrInst>(Term) &&
                  !llvm::isa<llvm::CondBrInst>(Term)))
      return false;
    if (InlinedFallthroughBlocks.count(Cur) || FoldedJoinArms.count(Cur) ||
        ConditionChainBodies.count(Cur) || DuplicatedAssignBlocks.count(Cur))
      return false;
    for (const llvm::Instruction &Inst : *Cur)
      if (llvm::isa<llvm::PHINode>(Inst))
        return false;
    In.insert(Cur);
    for (const llvm::BasicBlock *Succ : llvm::successors(Cur)) {
      if (!Succ || Succ == From)
        return false;
      if (Succ == Join || printBranchTarget(Succ) == Join)
        continue;
      Work.push_back(Succ);
    }
  }
  if (!In.count(Else))
    return false;

  auto PredOk = [&](const llvm::BasicBlock *Pred) {
    if (!Pred)
      return false;
    if (Pred == From || In.count(Pred))
      return true;
    if (!isPrintPassthrough(Pred) || printBranchTarget(Pred) == Join)
      return false;
    bool Any = false;
    for (const llvm::BasicBlock *Hop : llvm::predecessors(Pred)) {
      Any = true;
      if (Hop != From && !In.count(Hop))
        return false;
    }
    return Any;
  };
  for (const llvm::BasicBlock *BB : In) {
    bool Any = false;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
      Any = true;
      if (!PredOk(Pred))
        return false;
    }
    if (!Any)
      return false;
  }
  auto IsSkipPath = [&](const llvm::BasicBlock *Pred) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    const llvm::BasicBlock *Cur = Pred;
    while (Cur && Seen.insert(Cur).second) {
      if (Cur == From)
        return true;
      if (In.count(Cur) || !isPrintPassthrough(Cur))
        return false;
      Cur = Cur->getSinglePredecessor();
    }
    return false;
  };
  bool SharedTail = false;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Join)) {
    if (In.count(Pred) || IsSkipPath(Pred))
      continue;
    SharedTail = true;
    break;
  }
  if (!SharedTail)
    return false;

  const llvm::BasicBlock *Exit = nullptr;
  for (const llvm::BasicBlock *BB : In) {
    if (isPrintPassthrough(BB))
      continue;
    for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
      if (Succ != Join && printBranchTarget(Succ) != Join)
        continue;
      const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
      if (!Br || printBranchTarget(Br->getSuccessor(0)) != Join)
        return false;
      if (Exit && Exit != BB)
        return false;
      Exit = BB;
    }
  }
  if (!Exit)
    return false;
  const llvm::BasicBlock *LastPrinted = nullptr;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (!In.count(&BB) || isPrintPassthrough(&BB))
      continue;
    LastPrinted = &BB;
  }
  if (LastPrinted != Exit)
    return false;
  for (const llvm::BasicBlock *BB : In)
    Region.push_back(BB);
  return true;
}

bool LLVMCWriter::exclusiveSkipElseIf(
    const llvm::BasicBlock *From,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Headers,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Arms,
    llvm::SmallVectorImpl<char> &ArmOnTrue, const llvm::BasicBlock *&Tail,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Owned,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &FlatConds,
    llvm::SmallVectorImpl<char> &FlatTaken,
    llvm::SmallVectorImpl<unsigned> &FlatCounts,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Elided) {
  Headers.clear();
  Arms.clear();
  ArmOnTrue.clear();
  Owned.clear();
  FlatConds.clear();
  FlatTaken.clear();
  FlatCounts.clear();
  Elided.clear();
  Tail = nullptr;
  if (!From || !From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(0) == FromBr->getSuccessor(1))
    return false;

  auto After = [](const llvm::BasicBlock *Earlier,
                  const llvm::BasicBlock *Later) {
    if (!Earlier || !Later || Earlier->getParent() != Later->getParent())
      return false;
    bool Seen = false;
    for (const llvm::BasicBlock &BB : *Earlier->getParent()) {
      if (&BB == Earlier)
        Seen = true;
      else if (Seen && &BB == Later)
        return true;
    }
    return false;
  };
  auto BadBlock = [&](const llvm::BasicBlock *BB) {
    if (!BB || BB->isLandingPad() || isCatchPadBlock(*BB) || hasCleanupPad(*BB))
      return true;
    const llvm::Instruction *Term = BB->getTerminator();
    if (!Term || llvm::isa<llvm::CatchSwitchInst>(Term) ||
        llvm::isa<llvm::SwitchInst, llvm::InvokeInst, llvm::IndirectBrInst,
                  llvm::CallBrInst, llvm::ResumeInst, llvm::CatchReturnInst,
                  llvm::CleanupReturnInst>(Term))
      return true;
    for (const llvm::Instruction &Inst : *BB)
      if (llvm::isa<llvm::PHINode>(Inst))
        return true;
    return false;
  };
  auto ReturnExit = [](const llvm::BasicBlock *BB) {
    if (!BB || !BB->getTerminator())
      return false;
    return llvm::isa<llvm::ReturnInst>(BB->getTerminator()) ||
           llvm::isa<llvm::UnreachableInst>(BB->getTerminator());
  };
  auto PrintedCall = [&](const llvm::BasicBlock *BB) {
    if (!BB)
      return false;
    for (const llvm::Instruction &Inst : *BB) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!Call || Call->isInlineAsm() || !instructionIsPrinted(Inst))
        continue;
      if (const llvm::Function *Callee = Call->getCalledFunction())
        if (Callee->isIntrinsic())
          continue;
      return true;
    }
    return false;
  };
  auto DirectShared =
      [&](const llvm::BasicBlock *Header, const llvm::BasicBlock *Edge,
          llvm::SmallVectorImpl<const llvm::BasicBlock *> &Hops)
      -> const llvm::BasicBlock * {
    Hops.clear();
    if (!Header || !Edge || edgePrintsPhiCopy(Header, Edge))
      return nullptr;
    const llvm::BasicBlock *Pred = Header;
    const llvm::BasicBlock *Cur = Edge;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    while (Cur && isPrintPassthrough(Cur) &&
           Cur->getSinglePredecessor() == Pred && Seen.insert(Cur).second) {
      Hops.push_back(Cur);
      Pred = Cur;
      const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
      if (!Br)
        return nullptr;
      Cur = Br->getSuccessor(0);
    }
    if (!Cur || Cur == Header || Cur->getSinglePredecessor() ||
        edgePrintsPhiCopy(Pred, Cur) || !PrintedCall(Cur))
      return nullptr;
    return Cur;
  };

  struct ArmInfo {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Blocks;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Hops;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Exits;
    const llvm::BasicBlock *Start = nullptr;
  };
  auto CollectArm = [&](const llvm::BasicBlock *Header,
                        const llvm::BasicBlock *Edge, ArmInfo &Arm,
                        const llvm::BasicBlock *LayoutLimit) -> bool {
    Arm = {};
    if (!Header || !Edge || edgePrintsPhiCopy(Header, Edge) || BadBlock(Edge))
      return false;
    const llvm::BasicBlock *Pred = Header;
    const llvm::BasicBlock *Start = Edge;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> HopSeen;
    while (isPrintPassthrough(Start) && Start->getSinglePredecessor() == Pred &&
           HopSeen.insert(Start).second) {
      Arm.Hops.push_back(Start);
      Pred = Start;
      const auto *Br =
          llvm::dyn_cast<llvm::UncondBrInst>(Start->getTerminator());
      if (!Br)
        return false;
      Start = Br->getSuccessor(0);
      if (!Start || Start == Pred)
        return false;
    }
    if (!Start || Start == Header || Start->getSinglePredecessor() != Pred ||
        BadBlock(Start))
      return false;
    Arm.Start = Start;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> In;
    if (In.size() >= 160 || BadBlock(Start))
      return false;
    In.insert(Start);
    bool Grew = true;
    while (Grew) {
      Grew = false;
      llvm::SmallVector<const llvm::BasicBlock *, 32> Cands;
      for (const llvm::BasicBlock *BB : In) {
        const llvm::Instruction *Term = BB->getTerminator();
        if (!Term || llvm::isa<llvm::ReturnInst>(Term) ||
            llvm::isa<llvm::UnreachableInst>(Term))
          continue;
        if (!llvm::isa<llvm::UncondBrInst>(Term) &&
            !llvm::isa<llvm::CondBrInst>(Term))
          return false;
        for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
          if (!Succ || edgePrintsPhiCopy(BB, Succ))
            return false;
          if (!In.count(Succ))
            Cands.push_back(Succ);
        }
      }
      for (const llvm::BasicBlock *Cand : Cands) {
        if (In.count(Cand))
          continue;
        if (isPrintPassthrough(Cand)) {
          const auto *HopBr =
              llvm::dyn_cast<llvm::UncondBrInst>(Cand->getTerminator());
          const llvm::BasicBlock *Dest = HopBr ? HopBr->getSuccessor(0) : nullptr;
          bool Shared = false;
          if (Dest) {
            for (const llvm::BasicBlock *Pred : llvm::predecessors(Dest)) {
              if (Pred == Cand || In.count(Pred) || Pred == Header)
                continue;
              Shared = true;
              break;
            }
          }
          if (Shared)
            continue;
        }
        bool Any = false;
        bool Ok = true;
        for (const llvm::BasicBlock *Pred : llvm::predecessors(Cand)) {
          Any = true;
          if (Pred == Header || In.count(Pred))
            continue;
          if (isPrintPassthrough(Pred)) {
            const llvm::BasicBlock *Origin = Pred;
            llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
            while (Origin && isPrintPassthrough(Origin) &&
                   Seen.insert(Origin).second) {
              const llvm::BasicBlock *Prev = Origin->getSinglePredecessor();
              if (!Prev)
                break;
              Origin = Prev;
            }
            if (Origin == Header || In.count(Origin))
              continue;
          }
          Ok = false;
          break;
        }
        if (Any && Ok) {
          if (In.size() >= 160 || BadBlock(Cand))
            return false;
          if (InlinedFallthroughBlocks.count(Cand))
            continue;
          if (LayoutLimit && !After(Cand, LayoutLimit))
            continue;
          In.insert(Cand);
          Grew = true;
        }
      }
    }
    bool Pulled = true;
    while (Pulled) {
      Pulled = false;
      llvm::SmallVector<const llvm::BasicBlock *, 16> Extra;
      for (const llvm::BasicBlock *BB : In) {
        const llvm::Instruction *Term = BB->getTerminator();
        if (!Term || llvm::isa<llvm::ReturnInst>(Term) ||
            llvm::isa<llvm::UnreachableInst>(Term))
          continue;
        for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
          if (!Succ || In.count(Succ) || BadBlock(Succ))
            continue;
          bool Any = false;
          bool Ok = true;
          for (const llvm::BasicBlock *Pred : llvm::predecessors(Succ)) {
            Any = true;
            const llvm::BasicBlock *Origin = Pred;
            llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
            while (Origin && !In.count(Origin) && isPrintPassthrough(Origin) &&
                   Origin->getSinglePredecessor() &&
                   Seen.insert(Origin).second)
              Origin = Origin->getSinglePredecessor();
            if (In.count(Origin) || Origin == Header)
              continue;
            Ok = false;
            break;
          }
          if (Any && Ok && (!LayoutLimit || After(Succ, LayoutLimit)))
            Extra.push_back(Succ);
        }
      }
      for (const llvm::BasicBlock *BB : Extra) {
        if (In.count(BB) || In.size() >= 160)
          continue;
        In.insert(BB);
        Pulled = true;
      }
    }
    {
      llvm::DenseMap<const llvm::BasicBlock *, int> Color;
      std::function<bool(const llvm::BasicBlock *)> Cyclic =
          [&](const llvm::BasicBlock *BB) {
            Color[BB] = 1;
            const llvm::Instruction *Term = BB->getTerminator();
            if (Term && !llvm::isa<llvm::ReturnInst>(Term) &&
                !llvm::isa<llvm::UnreachableInst>(Term)) {
              for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
                if (!In.count(Succ))
                  continue;
                const int Seen = Color.lookup(Succ);
                if (Seen == 1 || (Seen == 0 && Cyclic(Succ)))
                  return true;
              }
            }
            Color[BB] = 2;
            return false;
          };
      for (const llvm::BasicBlock *BB : In)
        if (Color.lookup(BB) == 0 && Cyclic(BB))
          return false;
    }
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> HopSet(Arm.Hops.begin(),
                                                          Arm.Hops.end());
    auto PredOk = [&](const llvm::BasicBlock *BB) {
      bool Any = false;
      for (const llvm::BasicBlock *PredBB : llvm::predecessors(BB)) {
        Any = true;
        if (PredBB == Header || In.count(PredBB) || HopSet.count(PredBB))
          continue;
        if (isPrintPassthrough(PredBB)) {
          const llvm::BasicBlock *Origin = PredBB;
          llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
          while (Origin && isPrintPassthrough(Origin) &&
                 Seen.insert(Origin).second) {
            const llvm::BasicBlock *Prev = Origin->getSinglePredecessor();
            if (!Prev)
              break;
            Origin = Prev;
          }
          if (Origin == Header || In.count(Origin) || HopSet.count(Origin))
            continue;
        }
        return false;
      }
      return Any;
    };
    bool Changed = true;
    while (Changed) {
      Changed = false;
      llvm::SmallVector<const llvm::BasicBlock *, 32> Drop;
      for (const llvm::BasicBlock *BB : In)
        if (!PredOk(BB))
          Drop.push_back(BB);
      for (const llvm::BasicBlock *BB : Drop) {
        In.erase(BB);
        Changed = true;
      }
      if (!In.count(Start))
        return false;
      llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Reach;
      llvm::SmallVector<const llvm::BasicBlock *, 32> Left{Start};
      while (!Left.empty()) {
        const llvm::BasicBlock *Cur = Left.pop_back_val();
        if (!Cur || !In.count(Cur) || !Reach.insert(Cur).second)
          continue;
        for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
          if (In.count(Succ))
            Left.push_back(Succ);
      }
      if (Reach.size() != In.size()) {
        In = std::move(Reach);
        Changed = true;
      }
    }
    if (!In.count(Start))
      return false;
    if (LayoutLimit) {
      llvm::SmallVector<const llvm::BasicBlock *, 16> Drop;
      for (const llvm::BasicBlock *BB : In)
        if (!After(Header, BB) || !After(BB, LayoutLimit))
          Drop.push_back(BB);
      for (const llvm::BasicBlock *BB : Drop)
        In.erase(BB);
      if (!In.count(Start))
        return false;
    }
    bool SawCall = false;
    for (const llvm::BasicBlock *BB : In)
      if (PrintedCall(BB))
        SawCall = true;
    if (!SawCall)
      return false;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 4> ExitSet;
    for (const llvm::BasicBlock *BB : In) {
      const llvm::Instruction *Term = BB->getTerminator();
      if (!Term || llvm::isa<llvm::ReturnInst>(Term) ||
          llvm::isa<llvm::UnreachableInst>(Term))
        continue;
      for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
        if (!Succ || In.count(Succ))
          continue;
        const llvm::BasicBlock *PredBB = BB;
        const llvm::BasicBlock *Cur = Succ;
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
        while (Cur && !In.count(Cur) && isPrintPassthrough(Cur) &&
               Cur->getSinglePredecessor() == PredBB &&
               Seen.insert(Cur).second) {
          PredBB = Cur;
          const auto *Br =
              llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
          if (!Br)
            break;
          Cur = Br->getSuccessor(0);
        }
        if (!Cur || In.count(Cur))
          continue;
        ExitSet.insert(Cur);
      }
    }
    if (ExitSet.empty()) {
      bool SawLeave = false;
      for (const llvm::BasicBlock *BB : In) {
        const llvm::Instruction *Term = BB->getTerminator();
        if (llvm::isa<llvm::ReturnInst>(Term) ||
            llvm::isa<llvm::UnreachableInst>(Term))
          SawLeave = true;
      }
      if (!SawLeave)
        return false;
    }
    Arm.Blocks = std::move(In);
    for (const llvm::BasicBlock *Exit : ExitSet)
      Arm.Exits.push_back(Exit);
    return true;
  };
  auto ExitsOk = [&](const ArmInfo &Arm, const llvm::BasicBlock *Want) {
    if (!Want)
      return false;
    for (const llvm::BasicBlock *Exit : Arm.Exits)
      if (Exit != Want && !ReturnExit(Exit))
        return false;
    return true;
  };
  auto LayoutOk = [&](const llvm::BasicBlock *Header, const ArmInfo &Arm,
                      const llvm::BasicBlock *Limit) {
    if (!Limit || !After(Header, Limit))
      return false;
    for (const llvm::BasicBlock *BB : Arm.Hops)
      if (!After(Header, BB) || !After(BB, Limit))
        return false;
    for (const llvm::BasicBlock *BB : Arm.Blocks)
      if (!After(Header, BB) || !After(BB, Limit))
        return false;
    return true;
  };

  struct Chain {
    llvm::SmallVector<const llvm::BasicBlock *, 4> Headers;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Arms;
    llvm::SmallVector<char, 4> ArmOnTrue;
    llvm::SmallVector<const llvm::BasicBlock *, 64> Owned;
    llvm::SmallVector<const llvm::BasicBlock *, 8> Elided;
    llvm::SmallVector<const llvm::BasicBlock *, 8> FlatConds;
    llvm::SmallVector<char, 8> FlatTaken;
    llvm::SmallVector<unsigned, 4> FlatCounts;
    const llvm::BasicBlock *Tail = nullptr;
    bool AndChain = false;
  };
  std::function<bool(const llvm::BasicBlock *, int, Chain, Chain &)> Walk;
  Walk = [&](const llvm::BasicBlock *Cur, int Depth, Chain St,
             Chain &Out) -> bool {
    if (!Cur || Depth >= 5 || BadBlock(Cur))
      return false;
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
    if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
      return false;
    if (edgePrintsPhiCopy(Cur, Br->getSuccessor(0)) ||
        edgePrintsPhiCopy(Cur, Br->getSuccessor(1)))
      return false;
    for (const llvm::BasicBlock *Header : St.Headers)
      if (Header == Cur)
        return false;
    const llvm::BasicBlock *TrueEdge = Br->getSuccessor(0);
    const llvm::BasicBlock *FalseEdge = Br->getSuccessor(1);
    llvm::SmallVector<const llvm::BasicBlock *, 4> TrueHops;
    llvm::SmallVector<const llvm::BasicBlock *, 4> FalseHops;
    const llvm::BasicBlock *TrueTail = DirectShared(Cur, TrueEdge, TrueHops);
    const llvm::BasicBlock *FalseTail = DirectShared(Cur, FalseEdge, FalseHops);

    auto AddOwned = [](Chain &Next, const ArmInfo &Arm,
                       llvm::ArrayRef<const llvm::BasicBlock *> Extra) {
      auto Push = [&](const llvm::BasicBlock *BB) {
        if (!BB || BB == Next.Tail)
          return;
        if (llvm::is_contained(Next.Owned, BB) ||
            llvm::is_contained(Next.Headers, BB))
          return;
        Next.Owned.push_back(BB);
      };
      for (const llvm::BasicBlock *BB : Arm.Blocks)
        Push(BB);
      for (const llvm::BasicBlock *BB : Arm.Hops)
        Push(BB);
      for (const llvm::BasicBlock *BB : Extra)
        Push(BB);
    };
    auto TryLast = [&](bool ArmTrue, const llvm::BasicBlock *Direct,
                       llvm::ArrayRef<const llvm::BasicBlock *> DirectHops,
                       const llvm::BasicBlock *ArmEdge) -> bool {
      if (!Direct)
        return false;
      if (St.Tail && St.Tail != Direct)
        return false;
      ArmInfo Arm;
      if (!CollectArm(Cur, ArmEdge, Arm, nullptr) || !Arm.Start)
        return false;
      if (!LayoutOk(Cur, Arm, Direct))
        return false;
      Chain Next = St;
      if (!Next.Tail)
        Next.Tail = Direct;
      if (!ExitsOk(Arm, Next.Tail))
        return false;
      if (Cur != From)
        Next.Owned.push_back(Cur);
      Next.Headers.push_back(Cur);
      Next.Arms.push_back(Arm.Start);
      Next.ArmOnTrue.push_back(ArmTrue ? 1 : 0);
      AddOwned(Next, Arm, DirectHops);
      if (llvm::is_contained(Next.Owned, Next.Tail))
        return false;
      Out = std::move(Next);
      return true;
    };
    if (TryLast(true, FalseTail, FalseHops, TrueEdge) ||
        TryLast(false, TrueTail, TrueHops, FalseEdge))
      return true;

    auto TryCont = [&](bool ArmTrue, const llvm::BasicBlock *ArmEdge,
                       const llvm::BasicBlock *ContEdge) -> bool {
      if (DirectShared(Cur, ContEdge, ArmTrue ? FalseHops : TrueHops))
        return false;
      const llvm::BasicBlock *NextHeader = ContEdge;
      llvm::SmallVector<const llvm::BasicBlock *, 4> Hops;
      const llvm::BasicBlock *Pred = Cur;
      const llvm::BasicBlock *Hop = ContEdge;
      llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
      while (Hop && isPrintPassthrough(Hop) &&
             Hop->getSinglePredecessor() == Pred && Seen.insert(Hop).second) {
        Hops.push_back(Hop);
        Pred = Hop;
        const auto *HopBr =
            llvm::dyn_cast<llvm::UncondBrInst>(Hop->getTerminator());
        if (!HopBr)
          return false;
        Hop = HopBr->getSuccessor(0);
      }
      NextHeader = Hop;
      if (!NextHeader || NextHeader == Cur ||
          NextHeader->getSinglePredecessor() != Pred ||
          !llvm::isa<llvm::CondBrInst>(NextHeader->getTerminator()) ||
          BadBlock(NextHeader))
        return false;
      ArmInfo Arm;
      if (!CollectArm(Cur, ArmEdge, Arm, nullptr) || !Arm.Start ||
          Arm.Blocks.count(NextHeader))
        return false;
      if (!LayoutOk(Cur, Arm, NextHeader))
        return false;
      const llvm::BasicBlock *Fall = nullptr;
      for (const llvm::BasicBlock *Exit : Arm.Exits) {
        if (ReturnExit(Exit))
          continue;
        if (Fall && Fall != Exit)
          return false;
        Fall = Exit;
      }
      Chain Next = St;
      if (Fall) {
        if (Next.Tail && Next.Tail != Fall)
          return false;
        if (!Next.Tail)
          Next.Tail = Fall;
        if (!PrintedCall(Next.Tail))
          return false;
      }
      if (Next.Tail && !ExitsOk(Arm, Next.Tail) && Fall)
        return false;
      if (Cur != From)
        Next.Owned.push_back(Cur);
      Next.Headers.push_back(Cur);
      Next.Arms.push_back(Arm.Start);
      Next.ArmOnTrue.push_back(ArmTrue ? 1 : 0);
      AddOwned(Next, Arm, Hops);
      Chain Child;
      if (!Walk(NextHeader, Depth + 1, Next, Child))
        return false;
      Out = std::move(Child);
      return true;
    };
    return TryCont(true, TrueEdge, FalseEdge) ||
           TryCont(false, FalseEdge, TrueEdge);
  };

  auto StraightReturn =
      [&](const llvm::BasicBlock *Start,
          llvm::SmallVectorImpl<const llvm::BasicBlock *> &ChainOut) {
        ChainOut.clear();
        const llvm::BasicBlock *Cur = Start;
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
        while (Cur && ChainOut.size() < 4 && Seen.insert(Cur).second) {
          if (BadBlock(Cur) && Cur != Start)
            return false;
          ChainOut.push_back(Cur);
          const llvm::Instruction *Term = Cur->getTerminator();
          if (!Term)
            return false;
          if (llvm::isa<llvm::ReturnInst>(Term) ||
              llvm::isa<llvm::UnreachableInst>(Term))
            return true;
          const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Term);
          if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == Cur)
            return false;
          Cur = Br->getSuccessor(0);
        }
        return false;
      };
  auto ResolveShared =
      [&](const llvm::BasicBlock *Header, const llvm::BasicBlock *Edge,
          llvm::SmallVectorImpl<const llvm::BasicBlock *> &Hops)
      -> const llvm::BasicBlock * {
    Hops.clear();
    if (!Header || !Edge || edgePrintsPhiCopy(Header, Edge))
      return nullptr;
    const llvm::BasicBlock *Pred = Header;
    const llvm::BasicBlock *Cur = Edge;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    while (Cur && isPrintPassthrough(Cur) &&
           Cur->getSinglePredecessor() == Pred && Seen.insert(Cur).second) {
      Hops.push_back(Cur);
      Pred = Cur;
      const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
      if (!Br)
        return nullptr;
      Cur = Br->getSuccessor(0);
    }
    return Cur;
  };

  Chain Done;
  bool AndOk = false;
  {
    const llvm::BasicBlock *Cursor = From;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> SeenClause;
    while (Done.Headers.size() < 4 && Cursor && SeenClause.insert(Cursor).second) {
      llvm::SmallVector<const llvm::BasicBlock *, 4> Conds;
      llvm::SmallVector<char, 4> Taken;
      llvm::SmallVector<const llvm::BasicBlock *, 4> Hops;
      const llvm::BasicBlock *Miss = nullptr;
      const llvm::BasicBlock *ArmEdge = nullptr;
      const llvm::BasicBlock *Cur = Cursor;
      bool ClauseOk = true;
      while (Conds.size() < 4 && Cur) {
        const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
        if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1) || BadBlock(Cur) ||
            edgePrintsPhiCopy(Cur, Br->getSuccessor(0)) ||
            edgePrintsPhiCopy(Cur, Br->getSuccessor(1))) {
          ClauseOk = false;
          break;
        }
        llvm::SmallVector<const llvm::BasicBlock *, 4> TrueHops;
        llvm::SmallVector<const llvm::BasicBlock *, 4> FalseHops;
        const llvm::BasicBlock *TrueDest =
            ResolveShared(Cur, Br->getSuccessor(0), TrueHops);
        const llvm::BasicBlock *FalseDest =
            ResolveShared(Cur, Br->getSuccessor(1), FalseHops);
        auto Private = [&](const llvm::BasicBlock *Header,
                           const llvm::BasicBlock *Dest,
                           const llvm::SmallVector<const llvm::BasicBlock *, 4>
                               &EdgeHops) {
          if (!Header || !Dest)
            return false;
          const llvm::BasicBlock *Pred =
              EdgeHops.empty() ? Header : EdgeHops.back();
          return Dest->getSinglePredecessor() == Pred;
        };
        const bool TrueShared = TrueDest && !TrueDest->getSinglePredecessor();
        const bool FalseShared = FalseDest && !FalseDest->getSinglePredecessor();
        const llvm::BasicBlock *Share = nullptr;
        const llvm::BasicBlock *Priv = nullptr;
        bool TakenTrue = false;
        const llvm::SmallVector<const llvm::BasicBlock *, 4> *ShareHops = nullptr;
        if (TrueShared && Private(Cur, FalseDest, FalseHops) && !FalseShared) {
          Share = TrueDest;
          Priv = FalseDest;
          ShareHops = &TrueHops;
        } else if (FalseShared && Private(Cur, TrueDest, TrueHops) && !TrueShared) {
          Share = FalseDest;
          Priv = TrueDest;
          TakenTrue = true;
          ShareHops = &FalseHops;
        } else {
          ClauseOk = false;
          break;
        }
        if (Miss && Share != Miss) {
          ArmEdge = Cur;
          break;
        }
        Miss = Share;
        Conds.push_back(Cur);
        Taken.push_back(TakenTrue ? 1 : 0);
        if (ShareHops)
          Hops.append(*ShareHops);
        bool Extend = false;
        if (Priv && llvm::isa<llvm::CondBrInst>(Priv->getTerminator()) &&
            !BadBlock(Priv)) {
          const auto *PrivBr =
              llvm::cast<llvm::CondBrInst>(Priv->getTerminator());
          llvm::SmallVector<const llvm::BasicBlock *, 4> HopA;
          llvm::SmallVector<const llvm::BasicBlock *, 4> HopB;
          const llvm::BasicBlock *DestA =
              ResolveShared(Priv, PrivBr->getSuccessor(0), HopA);
          const llvm::BasicBlock *DestB =
              ResolveShared(Priv, PrivBr->getSuccessor(1), HopB);
          const bool AShare = DestA && !DestA->getSinglePredecessor();
          const bool BShare = DestB && !DestB->getSinglePredecessor();
          if ((AShare && DestA == Miss && Private(Priv, DestB, HopB)) ||
              (BShare && DestB == Miss && Private(Priv, DestA, HopA)))
            Extend = true;
        }
        if (!Extend) {
          ArmEdge = Priv;
          break;
        }
        Cur = Priv;
      }
      if (!ClauseOk || !Miss || !ArmEdge || Conds.empty() || ArmEdge == Miss)
        break;
      const llvm::BasicBlock *ArmHeader = Conds.back();
      ArmInfo Arm;
      if (ArmEdge == ArmHeader ||
          !CollectArm(ArmHeader == ArmEdge ? Conds.back() : ArmHeader, ArmEdge,
                      Arm, Miss) ||
          !Arm.Start) {
        // ArmEdge is the private successor of the last consumed cond.
        if (Conds.back() == ArmEdge ||
            !CollectArm(Conds.back(), ArmEdge, Arm, Miss) ||
            !Arm.Start)
          break;
      }
      const llvm::BasicBlock *Fall = nullptr;
      bool ExitOk = true;
      llvm::SmallVector<const llvm::BasicBlock *, 4> ReturnExits;
      for (const llvm::BasicBlock *Exit : Arm.Exits) {
        llvm::SmallVector<const llvm::BasicBlock *, 4> RetChain;
        if (StraightReturn(Exit, RetChain)) {
          ReturnExits.push_back(Exit);
          continue;
        }
        if (Fall && Fall != Exit) {
          ExitOk = false;
          break;
        }
        Fall = Exit;
      }
      if (!ExitOk || !Fall)
        break;
      const bool Last = Fall == Miss;
      if (!Last &&
          (!llvm::isa<llvm::CondBrInst>(Miss->getTerminator()) ||
           BadBlock(Miss)))
        break;
      const llvm::BasicBlock *Limit = Last ? Fall : Miss;
      if (!LayoutOk(Conds.back(), Arm, Limit) || !After(Conds.front(), Limit))
        break;
      if (Arm.Blocks.count(Miss) || Arm.Blocks.count(Fall) && Fall != Miss &&
                                         Arm.Blocks.count(Fall))
        break;
      if (!Last && !After(Arm.Start, Miss))
        break;
      if (Done.Tail && Done.Tail != Fall)
        break;
      Done.Tail = Fall;
      Done.Headers.push_back(Conds.front());
      Done.Arms.push_back(Arm.Start);
      Done.ArmOnTrue.push_back(Taken.front());
      Done.FlatCounts.push_back(static_cast<unsigned>(Conds.size()));
      Done.FlatConds.append(Conds.begin(), Conds.end());
      Done.FlatTaken.append(Taken.begin(), Taken.end());
      auto PushOwned = [&](const llvm::BasicBlock *BB) {
        if (!BB || BB == From || BB == Done.Tail ||
            llvm::is_contained(Done.Owned, BB) ||
            llvm::is_contained(Done.Headers, BB))
          return;
        Done.Owned.push_back(BB);
      };
      for (const llvm::BasicBlock *BB : Conds)
        PushOwned(BB);
      for (const llvm::BasicBlock *BB : Hops)
        PushOwned(BB);
      for (const llvm::BasicBlock *BB : Arm.Blocks)
        PushOwned(BB);
      for (const llvm::BasicBlock *BB : Arm.Hops)
        PushOwned(BB);
      for (const llvm::BasicBlock *Exit : ReturnExits) {
        if (!llvm::is_contained(Done.Elided, Exit) && Exit != Done.Tail)
          Done.Elided.push_back(Exit);
      }
      Done.AndChain = true;
      if (Last)
        break;
      Cursor = Miss;
    }
    bool Wide = false;
    for (unsigned Count : Done.FlatCounts)
      if (Count >= 2)
        Wide = true;
    if (Done.AndChain && Wide && Done.Tail) {
      llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Have(
          Done.Owned.begin(), Done.Owned.end());
      for (const llvm::BasicBlock *Header : Done.Headers)
        Have.insert(Header);
      Have.insert(From);
      llvm::SmallVector<const llvm::BasicBlock *, 4> Keep;
      for (const llvm::BasicBlock *Exit : Done.Elided) {
        if (!Exit || Exit == Done.Tail)
          continue;
        bool Ok = true;
        bool Any = false;
        for (const llvm::BasicBlock *Pred : llvm::predecessors(Exit)) {
          Any = true;
          const llvm::BasicBlock *Origin = Pred;
          llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
          while (Origin && isPrintPassthrough(Origin) &&
                 Seen.insert(Origin).second) {
            const llvm::BasicBlock *Prev = Origin->getSinglePredecessor();
            if (!Prev)
              break;
            Origin = Prev;
          }
          if (!Have.count(Origin) && !Have.count(Pred)) {
            Ok = false;
            break;
          }
        }
        if (Ok && Any)
          Keep.push_back(Exit);
      }
      Done.Elided = std::move(Keep);
    }
    AndOk = Done.AndChain && Wide && Done.Headers.size() >= 2 && Done.Tail &&
            PrintedCall(Done.Tail) && Done.Headers.front() == From;
    if (!AndOk)
      Done = Chain{};
  }
  if (!AndOk && !Walk(From, 0, Chain{}, Done))
    return false;
  if (!Done.AndChain) {
    Done.FlatConds.append(Done.Headers.begin(), Done.Headers.end());
    Done.FlatTaken.append(Done.ArmOnTrue.begin(), Done.ArmOnTrue.end());
    Done.FlatCounts.assign(Done.Headers.size(), 1u);
  }
  if (Done.Headers.size() < 2 || !Done.Tail || !PrintedCall(Done.Tail) ||
      Done.Headers.size() != Done.Arms.size() ||
      Done.Headers.size() != Done.ArmOnTrue.size())
    return false;
  if (Done.Headers.front() != From ||
      llvm::is_contained(Done.Owned, Done.Tail) ||
      llvm::is_contained(Done.Owned, From))
    return false;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> OwnedSet(Done.Owned.begin(),
                                                           Done.Owned.end());
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> HeaderSet(Done.Headers.begin(),
                                                           Done.Headers.end());
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> ElidedSet(Done.Elided.begin(),
                                                           Done.Elided.end());
  if (!Done.AndChain) {
    for (size_t I = 0; I < Done.Headers.size(); ++I) {
      const bool OnTrue = Done.ArmOnTrue[I] != 0;
      const auto *Br =
          llvm::cast<llvm::CondBrInst>(Done.Headers[I]->getTerminator());
      ArmInfo Arm;
      if (!CollectArm(Done.Headers[I], Br->getSuccessor(OnTrue ? 0 : 1), Arm,
                      nullptr) ||
          Arm.Start != Done.Arms[I] || !ExitsOk(Arm, Done.Tail))
        return false;
      const llvm::BasicBlock *Limit =
          I + 1 < Done.Headers.size() ? Done.Headers[I + 1] : Done.Tail;
      if (!LayoutOk(Done.Headers[I], Arm, Limit))
        return false;
    }
  }
  auto OriginOf = [&](const llvm::BasicBlock *BB) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    const llvm::BasicBlock *Cur = BB;
    while (Cur && isPrintPassthrough(Cur) && Seen.insert(Cur).second) {
      const llvm::BasicBlock *Prev = Cur->getSinglePredecessor();
      if (!Prev)
        break;
      Cur = Prev;
    }
    return Cur;
  };
  for (const llvm::BasicBlock *BB : Done.Owned) {
    bool Any = false;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
      Any = true;
      const llvm::BasicBlock *Origin = OriginOf(Pred);
      if (Origin == From || HeaderSet.count(Origin) || OwnedSet.count(Origin) ||
          HeaderSet.count(Pred) || OwnedSet.count(Pred))
        continue;
      return false;
    }
    if (!Any)
      return false;
  }
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Done.Tail)) {
    const llvm::BasicBlock *Origin = OriginOf(Pred);
    if (Origin == From || HeaderSet.count(Origin) || OwnedSet.count(Origin) ||
        HeaderSet.count(Pred) || OwnedSet.count(Pred))
      continue;
    return false;
  }
  bool SeenFrom = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (!SeenFrom) {
      if (&BB == From)
        SeenFrom = true;
      continue;
    }
    if (OwnedSet.count(&BB) || HeaderSet.count(&BB) || ElidedSet.count(&BB) ||
        isPrintPassthrough(&BB) || InlinedFallthroughBlocks.count(&BB) ||
        FoldedJoinArms.count(&BB) || ConditionChainBodies.count(&BB) ||
        DuplicatedAssignBlocks.count(&BB))
      continue;
    if (&BB != &From->getParent()->getEntryBlock() && llvm::pred_empty(&BB))
      continue;
    if (isCatchPadBlock(BB) || hasCleanupPad(BB) ||
        llvm::isa<llvm::CatchSwitchInst>(BB.getTerminator()))
      continue;
    if (&BB != Done.Tail)
      return false;
    break;
  }
  Headers.append(Done.Headers.begin(), Done.Headers.end());
  Arms.append(Done.Arms.begin(), Done.Arms.end());
  ArmOnTrue.append(Done.ArmOnTrue.begin(), Done.ArmOnTrue.end());
  Owned.append(Done.Owned.begin(), Done.Owned.end());
  FlatConds.append(Done.FlatConds.begin(), Done.FlatConds.end());
  FlatTaken.append(Done.FlatTaken.begin(), Done.FlatTaken.end());
  FlatCounts.append(Done.FlatCounts.begin(), Done.FlatCounts.end());
  Elided.append(Done.Elided.begin(), Done.Elided.end());
  Tail = Done.Tail;
  return true;
}

bool LLVMCWriter::privateJoinElseIf(
    const llvm::BasicBlock *From,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Conds,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &TrueEdges,
    const llvm::BasicBlock *&ElseEdge, const llvm::BasicBlock *&Join,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Skip) {
  Conds.clear();
  TrueEdges.clear();
  Skip.clear();
  ElseEdge = nullptr;
  Join = nullptr;
  if (!From || !From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(0) == FromBr->getSuccessor(1))
    return false;
  if (!joinPrintsNext(From, FromBr->getSuccessor(1)))
    return false;

  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Owned;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Exits;
  const llvm::BasicBlock *JoinCand = nullptr;
  const llvm::BasicBlock *Cur = From;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> SeenHead;

  auto Claim = [&](const llvm::BasicBlock *BB) {
    if (!BB || BB == JoinCand)
      return false;
    // Scan records tails before print. Membership in those sets is not an
    // outside entry.
    if (BB->isLandingPad() || isCatchPadBlock(*BB) || hasCleanupPad(*BB))
      return false;
    Owned.insert(BB);
    return true;
  };
  auto HasPrintedCall = [&](llvm::ArrayRef<const llvm::BasicBlock *> Blocks) {
    for (const llvm::BasicBlock *BB : Blocks) {
      for (const llvm::Instruction &Inst : *BB) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
        if (Call && !Call->isInlineAsm() && instructionIsPrinted(Inst))
          return true;
      }
    }
    return false;
  };
  auto AsArm = [&](const llvm::BasicBlock *Header, const llvm::BasicBlock *Edge,
                   const llvm::BasicBlock *&OutJoin) {
    OutJoin = nullptr;
    if (!Header || !Edge || edgePrintsPhiCopy(Header, Edge))
      return false;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Blocks;
    llvm::SmallVector<const llvm::BasicBlock *, 4> Chain;
    if (singleEntryUncondTail(Header, Header, Edge, Chain)) {
      const auto *Br =
          llvm::dyn_cast<llvm::UncondBrInst>(Chain.back()->getTerminator());
      if (!Br)
        return false;
      OutJoin = printBranchTarget(Br->getSuccessor(0));
      Blocks.append(Chain.begin(), Chain.end());
      Exits.insert(Chain.back());
    } else {
      const auto *HBr = llvm::dyn_cast<llvm::CondBrInst>(Header->getTerminator());
      const llvm::BasicBlock *Region = nullptr;
      llvm::SmallVector<const llvm::BasicBlock *, 4> TrueChain;
      llvm::SmallVector<const llvm::BasicBlock *, 4> FalseChain;
      if (!HBr || HBr->getSuccessor(0) != Edge ||
          !singleEntryCondRegion(Header, Edge, Region, TrueChain, FalseChain))
        return false;
      const auto *TrueBr =
          llvm::dyn_cast<llvm::UncondBrInst>(TrueChain.back()->getTerminator());
      const auto *FalseBr = llvm::dyn_cast<llvm::UncondBrInst>(
          FalseChain.back()->getTerminator());
      if (!TrueBr || !FalseBr)
        return false;
      OutJoin = printBranchTarget(TrueBr->getSuccessor(0));
      if (!OutJoin || OutJoin != printBranchTarget(FalseBr->getSuccessor(0)))
        return false;
      Blocks.push_back(Region);
      Blocks.append(TrueChain.begin(), TrueChain.end());
      Blocks.append(FalseChain.begin(), FalseChain.end());
      Exits.insert(TrueChain.back());
      Exits.insert(FalseChain.back());
    }
    if (!OutJoin || !HasPrintedCall(Blocks))
      return false;
    for (const llvm::BasicBlock *BB : Blocks)
      if (!Claim(BB))
        return false;
    return true;
  };

  for (int Step = 0; Step < 4 && Cur; ++Step) {
    if (!SeenHead.insert(Cur).second)
      return false;
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator());
    if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
      return false;
    if (edgePrintsPhiCopy(Cur, Br->getSuccessor(0)) ||
        edgePrintsPhiCopy(Cur, Br->getSuccessor(1)))
      return false;
    const llvm::BasicBlock *TrueJoin = nullptr;
    const llvm::BasicBlock *FalseJoin = nullptr;
    const bool TrueArm = AsArm(Cur, Br->getSuccessor(0), TrueJoin);
    const bool FalseArm = AsArm(Cur, Br->getSuccessor(1), FalseJoin);
    if (TrueArm && FalseArm) {
      if (TrueJoin != FalseJoin || (JoinCand && JoinCand != TrueJoin))
        return false;
      JoinCand = TrueJoin;
      Conds.push_back(Cur);
      TrueEdges.push_back(Br->getSuccessor(0));
      ElseEdge = Br->getSuccessor(1);
      if (Cur != From && !Claim(Cur))
        return false;
      break;
    }
    if (!TrueArm || FalseArm || (JoinCand && JoinCand != TrueJoin))
      return false;
    JoinCand = TrueJoin;
    Conds.push_back(Cur);
    TrueEdges.push_back(Br->getSuccessor(0));
    if (Cur != From && !Claim(Cur))
      return false;
    const llvm::BasicBlock *Next = printBranchTarget(Br->getSuccessor(1));
    if (!Next || Next == Cur || Next == JoinCand ||
        !llvm::isa<llvm::CondBrInst>(Next->getTerminator()))
      return false;
    if (!Claim(Next) || (Next != Br->getSuccessor(1) &&
                         !Claim(Br->getSuccessor(1))))
      return false;
    Cur = Next;
  }
  if (!JoinCand || !ElseEdge || Conds.empty())
    return false;
  if (Conds.size() < 2) {
    // One header folds only when its true edge is already a conditional
    // region. A plain arm stays with the existing if.
    const llvm::BasicBlock *Region = nullptr;
    llvm::SmallVector<const llvm::BasicBlock *, 4> TrueChain;
    llvm::SmallVector<const llvm::BasicBlock *, 4> FalseChain;
    if (!singleEntryCondRegion(From, TrueEdges[0], Region, TrueChain,
                               FalseChain))
      return false;
  }
  Join = JoinCand;
  if (Owned.count(Join))
    return false;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Join)) {
    if (Exits.count(Pred) || Owned.count(Pred))
      continue;
    if (isPrintPassthrough(Pred)) {
      bool Any = false;
      bool Ok = true;
      for (const llvm::BasicBlock *Hop : llvm::predecessors(Pred)) {
        Any = true;
        if (!Owned.count(Hop) && !Exits.count(Hop))
          Ok = false;
      }
      if (Ok && Any)
        continue;
    }
    return false;
  }
  for (const llvm::BasicBlock *BB : Owned)
    if (BB != From)
      Skip.push_back(BB);
  return !Skip.empty();
}

bool LLVMCWriter::regionBeforeSkipTarget(
    const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
    const llvm::BasicBlock *&SkipTarget) {
  Region.clear();
  SkipTarget = nullptr;
  if (!From || !Else || !From->getParent() || Else->getParent() != From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(1) != Else ||
      FromBr->getSuccessor(0) == Else)
    return false;
  if (edgePrintsPhiCopy(From, Else))
    return false;
  SkipTarget = printBranchTarget(FromBr->getSuccessor(0));
  if (!SkipTarget || SkipTarget == Else || SkipTarget == From ||
      edgePrintsPhiCopy(From, FromBr->getSuccessor(0)))
    return false;
  auto Before = [](const llvm::BasicBlock *Earlier,
                   const llvm::BasicBlock *Later) {
    if (!Earlier || !Later || Earlier->getParent() != Later->getParent())
      return false;
    bool SeenEarlier = false;
    for (const llvm::BasicBlock &BB : *Earlier->getParent()) {
      if (&BB == Earlier)
        SeenEarlier = true;
      if (&BB == Later)
        return SeenEarlier && &BB != Earlier;
    }
    return false;
  };
  // A pure skip that the or-chain already inlined is not the printed body.
  // Follow it only when the header reaches the skip through a passthrough
  // and the skip itself is not the shared assign those skips jump to.
  const llvm::BasicBlock *RegionStart = Else;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> SkipChain;
  bool SkipAssigns = false;
  if (const auto *SkipBr =
          llvm::dyn_cast<llvm::UncondBrInst>(SkipTarget->getTerminator())) {
    for (const llvm::Instruction &Inst : *SkipTarget) {
      if (&Inst == SkipBr)
        break;
      if (llvm::isa<llvm::StoreInst>(Inst) && instructionIsPrinted(Inst))
        SkipAssigns = true;
    }
  }
  const bool ThroughHop =
      isPrintPassthrough(FromBr->getSuccessor(0)) && !SkipAssigns;
  while (ThroughHop && RegionStart &&
         InlinedFallthroughBlocks.count(RegionStart) &&
         SkipChain.size() < 4) {
    const auto *ChainBr =
        llvm::dyn_cast<llvm::CondBrInst>(RegionStart->getTerminator());
    if (!ChainBr ||
        printBranchTarget(ChainBr->getSuccessor(0)) != SkipTarget ||
        edgePrintsPhiCopy(RegionStart, ChainBr->getSuccessor(0)) ||
        edgePrintsPhiCopy(RegionStart, ChainBr->getSuccessor(1)))
      break;
    SkipChain.insert(RegionStart);
    RegionStart = ChainBr->getSuccessor(1);
  }
  if (!RegionStart || RegionStart == SkipTarget || RegionStart == From ||
      !joinPrintsNext(From, RegionStart) ||
      edgePrintsPhiCopy(From, RegionStart) ||
      !Before(RegionStart, SkipTarget))
    return false;
  {
    // A true edge that is already a conditional region is printed by that
    // fold. Treating it as the later skip drops its arms.
    const llvm::BasicBlock *RegionBlock = nullptr;
    llvm::SmallVector<const llvm::BasicBlock *, 4> TrueChain;
    llvm::SmallVector<const llvm::BasicBlock *, 4> FalseChain;
    if (singleEntryCondRegion(From, FromBr->getSuccessor(0), RegionBlock,
                              TrueChain, FalseChain))
      return false;
  }
  // The header's edge to the skip may be a print passthrough, and the
  // false-edge region may take another passthrough to that same skip.
  // A successor that also has an entry from outside the region stays
  // outside, so a shared block is not pulled into this test.
  auto PassthroughOrigin =
      [&](const llvm::BasicBlock *Pred) -> const llvm::BasicBlock * {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    const llvm::BasicBlock *Cur = Pred;
    while (Cur && isPrintPassthrough(Cur) && Seen.insert(Cur).second) {
      const llvm::BasicBlock *Prev = Cur->getSinglePredecessor();
      if (!Prev)
        return nullptr;
      Cur = Prev;
    }
    return Cur;
  };
  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> In;
  llvm::SmallVector<const llvm::BasicBlock *, 16> Work{RegionStart};
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || Cur == SkipTarget || Cur == From || In.count(Cur) ||
        SkipChain.count(Cur))
      continue;
    // An empty hop, or a single-entry block with real instructions, may be
    // laid out after the shared exit and still fall into a block before it.
    const bool LateFromRegion =
        !Before(Cur, SkipTarget) && Cur->getSinglePredecessor() &&
        In.count(Cur->getSinglePredecessor());
    const bool LateFallthrough = LateFromRegion && isPrintPassthrough(Cur);
    const bool LateBody = LateFromRegion && !isPrintPassthrough(Cur) &&
                          llvm::isa<llvm::UncondBrInst>(Cur->getTerminator());
    const bool LateCond = LateFromRegion && !isPrintPassthrough(Cur) &&
                          llvm::isa<llvm::CondBrInst>(Cur->getTerminator());
    const bool LateUnreachable =
        LateFromRegion && Cur->getTerminator() &&
        llvm::isa<llvm::UnreachableInst>(Cur->getTerminator());
    if (In.size() >= 48)
      return false;
    if (!Before(Cur, SkipTarget) && !LateFallthrough && !LateBody &&
        !LateCond && !LateUnreachable)
      continue;
    if (Cur->isLandingPad() || isCatchPadBlock(*Cur) || hasCleanupPad(*Cur))
      return false;
    const llvm::Instruction *Term = Cur->getTerminator();
    if (!Term || llvm::isa<llvm::SwitchInst, llvm::InvokeInst,
                           llvm::IndirectBrInst, llvm::CallBrInst,
                           llvm::ResumeInst, llvm::CatchReturnInst,
                           llvm::CleanupReturnInst>(Term))
      return false;
    In.insert(Cur);
    for (const llvm::BasicBlock *Succ : llvm::successors(Cur)) {
      if (!Succ || Succ == From)
        return false;
      if (Succ == SkipTarget || printBranchTarget(Succ) == SkipTarget ||
          SkipChain.count(Succ))
        continue;
      if (!Before(Succ, SkipTarget)) {
        const bool HopOk =
            Cur == RegionStart && Succ->getSinglePredecessor() == Cur &&
            !edgePrintsPhiCopy(Cur, Succ) && !Succ->isLandingPad() &&
            !isCatchPadBlock(*Succ) && !hasCleanupPad(*Succ);
        const auto *HopBr =
            llvm::dyn_cast<llvm::UncondBrInst>(Succ->getTerminator());
        const llvm::BasicBlock *HopSucc =
            HopBr ? HopBr->getSuccessor(0) : nullptr;
        const bool HopTargetOk =
            HopSucc && Before(Cur, HopSucc) && Before(HopSucc, SkipTarget) &&
            HopSucc != From && !edgePrintsPhiCopy(Succ, HopSucc);
        if (HopOk && HopTargetOk &&
            ((isPrintPassthrough(Succ) && joinPrintsNext(Cur, Succ)) ||
             (!isPrintPassthrough(Succ) && joinPrintsNext(Cur, HopSucc))))
          Work.push_back(Succ);
        else if (HopOk && !HopBr) {
          const auto *CondHop =
              llvm::dyn_cast<llvm::CondBrInst>(Succ->getTerminator());
          if (CondHop && !edgePrintsPhiCopy(Succ, CondHop->getSuccessor(0)) &&
              !edgePrintsPhiCopy(Succ, CondHop->getSuccessor(1))) {
            const llvm::BasicBlock *TrueT =
                printBranchTarget(CondHop->getSuccessor(0));
            const llvm::BasicBlock *FalseT =
                printBranchTarget(CondHop->getSuccessor(1));
            const llvm::BasicBlock *WorkEdge = nullptr;
            if (TrueT == SkipTarget && FalseT && Before(FalseT, SkipTarget))
              WorkEdge = FalseT;
            else if (FalseT == SkipTarget && TrueT && Before(TrueT, SkipTarget))
              WorkEdge = TrueT;
            const auto *WorkBr =
                WorkEdge ? llvm::dyn_cast<llvm::UncondBrInst>(
                               WorkEdge->getTerminator())
                         : nullptr;
            if (WorkEdge && WorkEdge->getSinglePredecessor() == Succ &&
                joinPrintsNext(Cur, WorkEdge) && WorkBr &&
                printBranchTarget(WorkBr->getSuccessor(0)) == SkipTarget &&
                !edgePrintsPhiCopy(Succ, WorkEdge) &&
                !edgePrintsPhiCopy(WorkEdge, WorkBr->getSuccessor(0)))
              Work.push_back(Succ);
          }
        }
        // A division check or its trap may be laid out after the shared
        // exit and still belong to this test. A block that falls into a
        // different successor would make the exit check reject the region.
        if (Succ->getSinglePredecessor() == Cur) {
          const llvm::Instruction *SuccTerm = Succ->getTerminator();
          bool Keep = SuccTerm && llvm::isa<llvm::UnreachableInst>(SuccTerm);
          if (!Keep && SuccTerm) {
            bool AnySucc = false;
            bool Leaves = false;
            for (const llvm::BasicBlock *Next : llvm::successors(Succ)) {
              if (!Next)
                continue;
              AnySucc = true;
              if (Next == SkipTarget || printBranchTarget(Next) == SkipTarget ||
                  Before(Next, SkipTarget))
                continue;
              const llvm::Instruction *NextTerm = Next->getTerminator();
              if (NextTerm && llvm::isa<llvm::UnreachableInst>(NextTerm))
                continue;
              Leaves = true;
            }
            Keep = AnySucc && !Leaves;
          }
          if (Keep)
            Work.push_back(Succ);
        }
        continue;
      }
      Work.push_back(Succ);
    }
  }
  auto RegionPredOk = [&](const llvm::BasicBlock *Pred) {
    const llvm::BasicBlock *Origin = PassthroughOrigin(Pred);
    return Origin == From ||
           (Origin && (In.count(Origin) || SkipChain.count(Origin)));
  };
  bool Changed = true;
  while (Changed) {
    Changed = false;
    llvm::SmallVector<const llvm::BasicBlock *, 16> Drop;
    for (const llvm::BasicBlock *BB : In) {
      bool Any = false;
      bool Ok = true;
      for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
        Any = true;
        if (!RegionPredOk(Pred)) {
          Ok = false;
          break;
        }
      }
      if (!Any || !Ok)
        Drop.push_back(BB);
    }
    for (const llvm::BasicBlock *BB : Drop) {
      In.erase(BB);
      Changed = true;
    }
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Reach;
    llvm::SmallVector<const llvm::BasicBlock *, 16> Left{RegionStart};
    while (!Left.empty()) {
      const llvm::BasicBlock *Cur = Left.pop_back_val();
      if (!Cur || !In.count(Cur) || !Reach.insert(Cur).second)
        continue;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur)) {
        if (!Succ || Succ == SkipTarget || printBranchTarget(Succ) == SkipTarget)
          continue;
        if (In.count(Succ))
          Left.push_back(Succ);
      }
    }
    if (Reach.size() != In.size()) {
      In = std::move(Reach);
      Changed = true;
    }
  }
  if (In.size() < 2 || !In.count(RegionStart))
    return false;
  auto SkipOriginOwned = [&](const llvm::BasicBlock *Origin) {
    if (!Origin)
      return false;
    if (Origin == From || In.count(Origin) || SkipChain.count(Origin))
      return true;
    const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Origin->getTerminator());
    if (!Br)
      return false;
    const llvm::BasicBlock *TrueTarget = printBranchTarget(Br->getSuccessor(0));
    const llvm::BasicBlock *FalseTarget = printBranchTarget(Br->getSuccessor(1));
    if (TrueTarget != SkipTarget && FalseTarget != SkipTarget)
      return false;
    const llvm::BasicBlock *Other =
        TrueTarget == SkipTarget ? FalseTarget : TrueTarget;
    return Other == From || In.count(Other);
  };
  bool OnlyFrom = false;
  auto OwnOrigin = [&](const llvm::BasicBlock *Pred) -> bool {
    if (Pred && isPrintPassthrough(Pred) && !Pred->getSinglePredecessor()) {
      bool Any = false;
      for (const llvm::BasicBlock *Hop : llvm::predecessors(Pred)) {
        Any = true;
        const llvm::BasicBlock *Origin = PassthroughOrigin(Hop);
        const llvm::BasicBlock *Use = Origin ? Origin : Hop;
        if (!SkipOriginOwned(Use))
          return false;
        if (Use == From)
          OnlyFrom = true;
      }
      return Any;
    }
    const llvm::BasicBlock *Origin = PassthroughOrigin(Pred);
    if (!SkipOriginOwned(Origin))
      return false;
    if (Origin == From)
      OnlyFrom = true;
    return true;
  };
  for (const llvm::BasicBlock *Pred : llvm::predecessors(SkipTarget)) {
    if (!OwnOrigin(Pred))
      return false;
  }
  if (!OnlyFrom)
    return false;
  bool SawCall = false;
  for (const llvm::BasicBlock *BB : In) {
    bool Any = false;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
      Any = true;
      if (!RegionPredOk(Pred))
        return false;
    }
    if (!Any)
      return false;
    for (const llvm::Instruction &Inst : *BB) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (Call && !Call->isInlineAsm()) {
        if (const llvm::Function *Callee = Call->getCalledFunction()) {
          if (Callee->isIntrinsic())
            continue;
        }
        SawCall = true;
      }
    }
  }
  bool SawLoop = false;
  if (!SawCall) {
    for (const llvm::BasicBlock *BB : In) {
      const llvm::BasicBlock *Latch = nullptr;
      const llvm::BasicBlock *LoopExit = nullptr;
      const llvm::BasicBlock *LoopBody = nullptr;
      const llvm::AllocaInst *LoopSlot = nullptr;
      if (cursorForLoop(BB, Latch, LoopExit, LoopBody, LoopSlot)) {
        SawLoop = true;
        break;
      }
      const llvm::BasicBlock *PhiLatch = nullptr;
      const llvm::BasicBlock *PhiStep = nullptr;
      const llvm::AllocaInst *PhiSlot = nullptr;
      if (splitPhiCursorLoop(BB, PhiLatch, PhiStep, PhiSlot)) {
        SawLoop = true;
        break;
      }
    }
  }
  if (!SawCall && !SawLoop)
    return false;
  // An unconditional exit that prints next is not given its own goto. A
  // trailing goto to a different skip would run that exit as well.
  if (!joinPrintsNext(From, SkipTarget)) {
    for (const llvm::BasicBlock *BB : In) {
      // A passthrough is not printed, so its branch is not a fallthrough
      // the trailing skip goto can steal.
      if (isPrintPassthrough(BB))
        continue;
      const auto *ExitBr =
          llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
      if (!ExitBr)
        continue;
      const llvm::BasicBlock *ExitTarget =
          printBranchTarget(ExitBr->getSuccessor(0));
      if (!ExitTarget || ExitTarget == SkipTarget || In.count(ExitTarget) ||
          SkipChain.count(ExitTarget))
        continue;
      if (joinPrintsNext(BB, ExitTarget))
        return false;
    }
  }
  for (const llvm::BasicBlock *BB : In)
    Region.push_back(BB);
  return true;
}

bool LLVMCWriter::laterTrueArmJoinsFallthrough(
    const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Side,
    const llvm::BasicBlock *&Arm, const llvm::BasicBlock *&Join) {
  Region.clear();
  Side.clear();
  Arm = nullptr;
  Join = nullptr;
  if (!From || !Else || !From->getParent() ||
      Else->getParent() != From->getParent())
    return false;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  if (!FromBr || FromBr->getSuccessor(1) != Else ||
      FromBr->getSuccessor(0) == Else)
    return false;
  if (!joinPrintsNext(From, Else) || edgePrintsPhiCopy(From, Else) ||
      edgePrintsPhiCopy(From, FromBr->getSuccessor(0)))
    return false;
  const llvm::BasicBlock *Body =
      straightLineTrueArm(From, FromBr->getSuccessor(0), true);
  if (!Body)
    return false;
  const auto *BodyBr = llvm::dyn_cast<llvm::UncondBrInst>(Body->getTerminator());
  if (!BodyBr || !BodyBr->getSuccessor(0) || BodyBr->getSuccessor(0) == Body)
    return false;
  if (edgePrintsPhiCopy(Body, BodyBr->getSuccessor(0)))
    return false;
  Join = printBranchTarget(BodyBr->getSuccessor(0));
  if (!Join || Join == Body || Join == Else || Join == From ||
      Join->getParent() != From->getParent())
    return false;
  Arm = Body;
  auto Before = [](const llvm::BasicBlock *Earlier,
                   const llvm::BasicBlock *Later) {
    if (!Earlier || !Later || Earlier->getParent() != Later->getParent())
      return false;
    bool SeenEarlier = false;
    for (const llvm::BasicBlock &BB : *Earlier->getParent()) {
      if (&BB == Earlier)
        SeenEarlier = true;
      if (&BB == Later)
        return SeenEarlier && &BB != Earlier;
    }
    return false;
  };
  if (!Before(Else, Join))
    return false;

  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> In;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> SideSet;
  llvm::SmallVector<const llvm::BasicBlock *, 16> Work{Else};
  while (!Work.empty()) {
    const llvm::BasicBlock *Cur = Work.pop_back_val();
    if (!Cur || Cur == Join || Cur == From || Cur == Arm || In.count(Cur))
      continue;
    if (In.size() >= 24 || Cur->getParent() != From->getParent() ||
        Cur->isLandingPad() || isCatchPadBlock(*Cur) || hasCleanupPad(*Cur) ||
        isPrintPassthrough(Cur))
      return false;
    const llvm::Instruction *Term = Cur->getTerminator();
    if (!Term ||
        llvm::isa<llvm::SwitchInst, llvm::InvokeInst, llvm::IndirectBrInst,
                  llvm::CallBrInst, llvm::ResumeInst, llvm::CatchReturnInst,
                  llvm::CleanupReturnInst, llvm::ReturnInst>(Term))
      return false;
    for (const llvm::Instruction &Inst : *Cur)
      if (llvm::isa<llvm::PHINode>(&Inst))
        return false;
    if (!Before(Cur, Join)) {
      const llvm::BasicBlock *ArmJoin = straightAssignArmJoin(Cur);
      if (!ArmJoin || ArmJoin == Cur)
        return false;
      if (ArmJoin != Join && !Before(ArmJoin, Join) && ArmJoin != Else)
        return false;
      const auto *SideBr = llvm::dyn_cast<llvm::UncondBrInst>(Term);
      if (!SideBr || printBranchTarget(SideBr->getSuccessor(0)) != ArmJoin)
        return false;
      if (!Cur->getSinglePredecessor())
        return false;
      In.insert(Cur);
      SideSet.insert(Cur);
      Side.push_back(Cur);
      if (ArmJoin != Join && !In.count(ArmJoin))
        Work.push_back(ArmJoin);
      continue;
    }
    In.insert(Cur);
    for (const llvm::BasicBlock *Succ : llvm::successors(Cur)) {
      if (!Succ || Succ == From || Succ == Arm)
        return false;
      if (Succ == Join || printBranchTarget(Succ) == Join)
        continue;
      Work.push_back(Succ);
    }
  }
  if (!In.count(Else))
    return false;
  for (const llvm::BasicBlock *BB : In) {
    bool Any = false;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
      Any = true;
      if (Pred != From && !In.count(Pred))
        return false;
    }
    if (!Any)
      return false;
  }
  auto FromArm = [&](const llvm::BasicBlock *Pred) {
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    const llvm::BasicBlock *Cur = Pred;
    while (Cur && Seen.insert(Cur).second) {
      if (Cur == Arm)
        return true;
      if (!isPrintPassthrough(Cur))
        return false;
      Cur = Cur->getSinglePredecessor();
    }
    return false;
  };
  bool SawArm = false;
  bool SawRegion = false;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Join)) {
    if (FromArm(Pred)) {
      SawArm = true;
      continue;
    }
    if (In.count(Pred)) {
      SawRegion = true;
      continue;
    }
    if (isPrintPassthrough(Pred)) {
      const llvm::BasicBlock *Hop = Pred->getSinglePredecessor();
      if (Hop && (In.count(Hop) || Hop == From)) {
        SawRegion = Hop != From || SawRegion;
        if (Hop == From)
          SawArm = true;
        continue;
      }
    }
    return false;
  }
  if (!SawArm || !SawRegion)
    return false;
  bool SawCall = false;
  for (const llvm::BasicBlock *BB : In) {
    if (SideSet.count(BB))
      continue;
    if (!Before(BB, Join))
      return false;
    for (const llvm::Instruction &Inst : *BB) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!Call || Call->isInlineAsm())
        continue;
      if (const llvm::Function *Callee = Call->getCalledFunction())
        if (Callee->isIntrinsic())
          continue;
      SawCall = true;
    }
    Region.push_back(BB);
  }
  if (!SawCall || Region.empty())
    return false;
  const llvm::BasicBlock *Last = nullptr;
  for (const llvm::BasicBlock &BB : *From->getParent())
    if (In.count(&BB) && !SideSet.count(&BB))
      Last = &BB;
  return Last && joinPrintsNext(Last, Join);
}

bool LLVMCWriter::pureSameTargetSkip(const llvm::BasicBlock *BB,
                                     const llvm::BasicBlock *Target,
                                     const llvm::BasicBlock *Pred) {
  if (!BB || !Target || !Pred || BB->getSinglePredecessor() != Pred)
    return false;
  if (BB->isLandingPad() || isCatchPadBlock(*BB) || hasCleanupPad(*BB) ||
      FoldedJoinArms.count(BB) || InlinedFallthroughBlocks.count(BB))
    return false;
  for (const llvm::Instruction &Inst : *BB)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return false;
  const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(BB->getTerminator());
  if (!Br || Br->getSuccessor(0) == Br->getSuccessor(1))
    return false;
  if (printBranchTarget(Br->getSuccessor(0)) != Target)
    return false;
  if (edgePrintsPhiCopy(BB, Br->getSuccessor(0)) ||
      edgePrintsPhiCopy(BB, Br->getSuccessor(1)))
    return false;
  if (straightLineTrueArm(BB, Br->getSuccessor(0)) ||
      straightLineFalseSkip(BB, Br->getSuccessor(1)))
    return false;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Br)
      continue;
    if (instructionIsPrinted(Inst))
      return false;
  }
  return true;
}

const llvm::BasicBlock *
LLVMCWriter::andRejoinTest(const llvm::BasicBlock *From,
                           const llvm::BasicBlock *Else) {
  if (!From || !Else || Else->getSinglePredecessor() != From)
    return nullptr;
  if (Else->isLandingPad() || isCatchPadBlock(*Else) || hasCleanupPad(*Else) ||
      FoldedJoinArms.count(Else) || InlinedFallthroughBlocks.count(Else))
    return nullptr;
  for (const llvm::Instruction &Inst : *Else)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return nullptr;
  const auto *FromBr = llvm::dyn_cast<llvm::CondBrInst>(From->getTerminator());
  const auto *ElseBr = llvm::dyn_cast<llvm::CondBrInst>(Else->getTerminator());
  if (!FromBr || !ElseBr || FromBr->getSuccessor(1) != Else)
    return nullptr;
  if (ElseBr->getSuccessor(0) == ElseBr->getSuccessor(1))
    return nullptr;
  if (printBranchTarget(ElseBr->getSuccessor(1)) !=
      printBranchTarget(FromBr->getSuccessor(0)))
    return nullptr;
  if (edgePrintsPhiCopy(From, Else) ||
      edgePrintsPhiCopy(From, FromBr->getSuccessor(0)) ||
      edgePrintsPhiCopy(Else, ElseBr->getSuccessor(0)) ||
      edgePrintsPhiCopy(Else, ElseBr->getSuccessor(1)))
    return nullptr;
  if (!straightLineTrueArm(Else, ElseBr->getSuccessor(0)))
    return nullptr;
  // Loads and casts that only feed this condition are not statements.
  // `instructionIsPrinted` still reports them before a predecessor store
  // has been forwarded, which would keep a label nothing jumps to.
  llvm::SmallPtrSet<const llvm::Instruction *, 8> Seen;
  std::function<bool(const llvm::Instruction *)> OnlyCond =
      [&](const llvm::Instruction *I) -> bool {
    if (!I || I == ElseBr)
      return true;
    if (!Seen.insert(I).second)
      return true;
    if (I->getParent() != Else)
      return false;
    if (!llvm::isa<llvm::LoadInst, llvm::CastInst, llvm::CmpInst,
                   llvm::FreezeInst, llvm::BinaryOperator,
                   llvm::GetElementPtrInst>(I))
      return false;
    for (const llvm::User *U : I->users())
      if (!OnlyCond(llvm::dyn_cast<llvm::Instruction>(U)))
        return false;
    return true;
  };
  for (const llvm::Instruction &Inst : *Else) {
    if (&Inst == ElseBr || OnlyCond(&Inst))
      continue;
    if (instructionIsPrinted(Inst))
      return nullptr;
  }
  return Else;
}

bool LLVMCWriter::uncondBranchFallsIntoEHBoundary(
    const llvm::BasicBlock *From, const llvm::BasicBlock *To) const {
  if (!From || !To || !EHBoundaryBlocks.count(To) || !From->getParent() ||
      To->getParent() != From->getParent() || EHMainBlock != From)
    return false;
  bool SeenFrom = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (!SeenFrom) {
      if (&BB == From)
        SeenFrom = true;
      continue;
    }
    if (&BB == To)
      return true;
    // Only blocks held for the later handler clause may intervene.  Any
    // other main-walk block or EH marker would print before this target.
    if (EHBoundaryBlocks.count(&BB) || !EHSkippedMainBlocks.count(&BB))
      return false;
  }
  return false;
}

bool LLVMCWriter::backEdgeCallBeforeHeader(const llvm::BasicBlock *BB) {
  if (!BB || !BB->getParent() || BB->isLandingPad() || isCatchPadBlock(*BB) ||
      hasCleanupPad(*BB))
    return false;
  const llvm::BasicBlock *Pred = BB->getSinglePredecessor();
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
  if (!Pred || !Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == BB)
    return false;
  const llvm::BasicBlock *Succ = Br->getSuccessor(0);
  if (edgePrintsPhiCopy(BB, Succ))
    return false;
  const llvm::Function *Fn = BB->getParent();
  auto Next = std::next(BB->getIterator());
  if (Next == Fn->end() || &*Next != Succ)
    return false;
  bool SeenBB = false;
  bool PredAfter = false;
  for (const llvm::BasicBlock &Block : *Fn) {
    if (&Block == BB)
      SeenBB = true;
    else if (SeenBB && &Block == Pred)
      PredAfter = true;
  }
  if (!PredAfter)
    return false;
  bool OtherPred = false;
  for (const llvm::BasicBlock *Other : llvm::predecessors(Succ)) {
    if (Other != BB)
      OtherPred = true;
  }
  if (!OtherPred)
    return false;
  const llvm::Instruction *Printed = nullptr;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Br || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    if (!instructionIsPrinted(Inst))
      continue;
    if (Printed || !llvm::isa<llvm::CallInst>(&Inst))
      return false;
    Printed = &Inst;
  }
  return Printed != nullptr;
}

const llvm::BasicBlock *
LLVMCWriter::sharedReturnEpilogue(const llvm::BasicBlock *Tail) {
  if (!Tail || !Tail->getParent() || Tail->isLandingPad() ||
      isCatchPadBlock(*Tail) || hasCleanupPad(*Tail))
    return nullptr;
  const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Tail->getTerminator());
  if (!Ret)
    return nullptr;
  for (const llvm::Instruction &Inst : *Tail)
    if (llvm::isa<llvm::PHINode>(&Inst))
      return nullptr;
  unsigned Returns = 0;
  for (const llvm::BasicBlock &BB : *Tail->getParent())
    if (llvm::isa<llvm::ReturnInst>(BB.getTerminator()))
      ++Returns;
  if (Returns != 1)
    return nullptr;
  const llvm::CallInst *Call = nullptr;
  unsigned Calls = 0;
  for (const llvm::Instruction &Inst : *Tail) {
    if (&Inst == Ret || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    if (!instructionIsPrinted(Inst))
      continue;
    const auto *C = llvm::dyn_cast<llvm::CallInst>(&Inst);
    if (!C || ++Calls > 1)
      return nullptr;
    Call = C;
  }
  if (Calls != 1 || !Call)
    return nullptr;
  const std::string Name = printedCalleeName(*Call);
  const MsvcAtlCallee *Atl = msvcAtlCallee(Name);
  if (!Atl || Atl->Kind != MsvcAtlCalleeKind::Dtor)
    return nullptr;
  unsigned Preds = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Tail)) {
    if (edgePrintsPhiCopy(Pred, Tail))
      return nullptr;
    ++Preds;
  }
  if (Preds < 2)
    return nullptr;
  return Tail;
}

const llvm::BasicBlock *
LLVMCWriter::phiFedCallTail(const llvm::BasicBlock *Tail) {
  if (!Tail || Tail->isLandingPad() || isCatchPadBlock(*Tail) ||
      hasCleanupPad(*Tail))
    return nullptr;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Tail->getTerminator());
  if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == Tail)
    return nullptr;
  if (edgePrintsPhiCopy(Tail, Br->getSuccessor(0)))
    return nullptr;
  const llvm::CallInst *Call = nullptr;
  for (const llvm::Instruction &Inst : *Tail) {
    if (&Inst == Br || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    if (!instructionIsPrinted(Inst))
      continue;
    const auto *C = llvm::dyn_cast<llvm::CallInst>(&Inst);
    if (!C || Call)
      return nullptr;
    Call = C;
  }
  if (!Call)
    return nullptr;
  // A second printed argument is the value the later call still needs.
  // Hiding those edge stores leaves the join call with an unset slot.
  const unsigned Limit = printedCallArgLimit(*Call, printedCalleeName(*Call));
  if (Limit != 1 || Call->arg_size() < 1)
    return nullptr;
  const llvm::AllocaInst *Slot =
      joinAllocaForCallArg(Call->getArgOperand(0), *Call);
  if (!Slot || allocaAddressTaken(Slot))
    return nullptr;
  unsigned Preds = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(Tail)) {
    if (edgePrintsPhiCopy(Pred, Tail))
      return nullptr;
    const auto *PredBr =
        llvm::dyn_cast<llvm::UncondBrInst>(Pred->getTerminator());
    if (!PredBr || PredBr->getSuccessor(0) != Tail)
      return nullptr;
    bool Stored = false;
    for (const llvm::Instruction &Inst : *Pred) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (SI && asAllocaPointer(SI->getPointerOperand()) == Slot)
        Stored = true;
    }
    if (!Stored)
      return nullptr;
    ++Preds;
  }
  if (Preds < 2)
    return nullptr;
  return Tail;
}

bool LLVMCWriter::storeFeedsPhiCallTail(const llvm::StoreInst *SI) {
  if (!SI)
    return false;
  const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
  const auto *Br =
      llvm::dyn_cast<llvm::UncondBrInst>(SI->getParent()->getTerminator());
  if (!Slot || !Br)
    return false;
  const llvm::BasicBlock *Tail = phiFedCallTail(Br->getSuccessor(0));
  if (!Tail)
    return false;
  for (const llvm::Instruction &Inst : *Tail) {
    const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
    if (!Call || !instructionIsPrinted(Inst))
      continue;
    const unsigned Limit =
        printedCallArgLimit(*Call, printedCalleeName(*Call));
    for (unsigned I = 0; I < Limit && I < Call->arg_size(); ++I)
      if (joinAllocaForCallArg(Call->getArgOperand(I), *Call) == Slot)
        return true;
  }
  return false;
}

const llvm::BasicBlock *
LLVMCWriter::duplicatedCallTail(const llvm::BasicBlock *Edge) {
  const llvm::BasicBlock *BB = printBranchTarget(Edge);
  if (!BB || isPrintPassthrough(BB) || BB->isLandingPad() ||
      isCatchPadBlock(*BB) || hasCleanupPad(*BB))
    return nullptr;
  const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
  if (!Br || !Br->getSuccessor(0) || Br->getSuccessor(0) == BB)
    return nullptr;
  if (edgePrintsPhiCopy(BB, Br->getSuccessor(0)))
    return nullptr;
  const llvm::Instruction *Printed = nullptr;
  for (const llvm::Instruction &Inst : *BB) {
    if (&Inst == Br || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    if (!instructionIsPrinted(Inst))
      continue;
    if (Printed || !llvm::isa<llvm::CallInst>(&Inst))
      return nullptr;
    Printed = &Inst;
  }
  if (!Printed)
    return nullptr;
  unsigned Arms = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
    if (!isPrintPassthrough(Pred) || edgePrintsPhiCopy(Pred, BB))
      return nullptr;
    const llvm::BasicBlock *Origin = Pred->getSinglePredecessor();
    const auto *Cond = Origin ? llvm::dyn_cast<llvm::CondBrInst>(
                                    Origin->getTerminator())
                              : nullptr;
    if (!Cond || (printBranchTarget(Cond->getSuccessor(0)) != BB &&
                  printBranchTarget(Cond->getSuccessor(1)) != BB))
      return nullptr;
    ++Arms;
  }
  if (Arms < 2)
    return nullptr;
  return BB;
}

bool LLVMCWriter::cursorForExitsAt(const llvm::BasicBlock *Succ) {
  const CursorForContinue &State = cursorForContinue();
  if (!State.Depth || !State.Exit || !Succ)
    return false;
  return Succ == State.Exit || printBranchTarget(Succ) == State.Exit;
}

bool LLVMCWriter::redundantLoopElseContinue(const llvm::BasicBlock *From,
                                           const llvm::BasicBlock *Else) {
  const CursorForContinue &State = cursorForContinue();
  if (!State.Depth || !From || !Else || !From->getParent())
    return false;
  auto IsLatch = [&](const llvm::BasicBlock *BB) {
    if (!BB)
      return false;
    const llvm::BasicBlock *Printed = printBranchTarget(BB);
    return BB == State.Latch || BB == State.Step || Printed == State.Latch ||
           Printed == State.Step;
  };
  const llvm::BasicBlock *Cur = Else;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
  bool Reached = false;
  while (Cur && Seen.insert(Cur).second) {
    if (IsLatch(Cur)) {
      Reached = true;
      break;
    }
    for (const llvm::Instruction &Inst : *Cur) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (instructionIsPrinted(Inst))
        return false;
    }
    const auto *Go = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!Go || !Go->getSuccessor(0) || Go->getSuccessor(0) == Cur)
      return false;
    Cur = Go->getSuccessor(0);
  }
  if (!Reached)
    return false;
  bool SeenFrom = false;
  for (const llvm::BasicBlock &BB : *From->getParent()) {
    if (!SeenFrom) {
      if (&BB == From)
        SeenFrom = true;
      continue;
    }
    if (IsLatch(&BB) || InlinedFallthroughBlocks.count(&BB) ||
        FoldedJoinArms.count(&BB) || DuplicatedAssignBlocks.count(&BB) ||
        ConditionChainBodies.count(&BB) || isPrintPassthrough(&BB))
      continue;
    if (State.Exit &&
        (&BB == State.Exit || printBranchTarget(&BB) == State.Exit))
      break;
    return false;
  }
  return true;
}

void LLVMCWriter::writeGoto(const llvm::BasicBlock *From,
                            const llvm::BasicBlock *To, int Indent,
                            bool EmitGoto) {
  if (!From || !To)
    return;
  std::set<const llvm::BasicBlock *> Seen;
  const llvm::BasicBlock *CurFrom = From;
  const llvm::BasicBlock *CurTo = To;
  auto EmptyUncondBridge = [&](const llvm::BasicBlock *BB) {
    if (!cursorForContinue().Depth || !BB)
      return false;
    const CursorForContinue &State = cursorForContinue();
    if (BB == State.Latch || BB == State.Step)
      return false;
    const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
    if (!Br || Br->getNumSuccessors() != 1 || Br->getSuccessor(0) == BB ||
        BB->getSinglePredecessor() != CurFrom)
      return false;
    // Record field texts the statement printer would record, then fold the
    // block away when that recording emits no statement.
    for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(BB)) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      writeInstruction(Inst, Indent);
    }
    InlinedFallthroughBlocks.insert(BB);
    return true;
  };
  while (CurTo && Seen.insert(CurTo).second &&
         (isPrintPassthrough(CurTo) || EmptyUncondBridge(CurTo))) {
    writePhiCopies(CurFrom, CurTo, Indent);
    CurFrom = CurTo;
    CurTo = CurTo->getTerminator()->getSuccessor(0);
  }
  if (CurTo)
    writePhiCopies(CurFrom, CurTo, Indent);
  if (const llvm::BasicBlock *Tail = phiFedCallTail(CurTo)) {
    const llvm::CallInst *Call = nullptr;
    for (const llvm::Instruction &Inst : *Tail) {
      const auto *C = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!C || !instructionIsPrinted(Inst))
        continue;
      Call = C;
      break;
    }
    const llvm::AllocaInst *Slot = nullptr;
    if (Call) {
      const unsigned Limit =
          printedCallArgLimit(*Call, printedCalleeName(*Call));
      for (unsigned I = 0; I < Limit && I < Call->arg_size(); ++I) {
        if (const llvm::AllocaInst *Home =
                joinAllocaForCallArg(Call->getArgOperand(I), *Call)) {
          Slot = Home;
          break;
        }
      }
    }
    if (Call && Slot) {
      PhiTailSlot = Slot;
      writeInstruction(const_cast<llvm::CallInst &>(*Call), Indent);
      PhiTailSlot = nullptr;
      FoldedLocalNames.push_back(getName(Slot));
      InlinedFallthroughBlocks.insert(Tail);
      ReferencedBlocks.erase(Tail);
      const auto *TailBr =
          llvm::cast<llvm::UncondBrInst>(Tail->getTerminator());
      writeGoto(Tail, TailBr->getSuccessor(0), Indent,
                !joinPrintsNext(From, TailBr->getSuccessor(0)));
      return;
    }
  }
  if (EmitGoto) {
    if (const llvm::BasicBlock *Epi = sharedReturnEpilogue(CurTo)) {
      for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Epi)) {
        if (llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        if (&Inst != Epi->getTerminator() &&
            (!llvm::isa<llvm::CallInst>(&Inst) || !instructionIsPrinted(Inst)))
          continue;
        writeInstruction(Inst, Indent);
      }
      ReferencedBlocks.erase(Epi);
      bool Falls = false;
      for (const llvm::BasicBlock *Pred : llvm::predecessors(Epi)) {
        if (joinPrintsNext(Pred, Epi))
          Falls = true;
      }
      if (!Falls)
        InlinedFallthroughBlocks.insert(Epi);
      return;
    }
  }
  if (!EmitGoto)
    return;
  if (const CursorForContinue &State = cursorForContinue();
      State.Depth && CurTo &&
      (CurTo == State.Latch || CurTo == State.Step ||
       printBranchTarget(CurTo) == State.Latch ||
       printBranchTarget(CurTo) == State.Step)) {
    emitIndent(Indent);
    OS << "continue;\n";
    return;
  }
  if (const CursorForContinue &State = cursorForContinue();
      State.Depth && State.Exit && CurTo &&
      (CurTo == State.Exit || printBranchTarget(CurTo) == State.Exit)) {
    emitIndent(Indent);
    OS << "break;\n";
    return;
  }
  if (CurTo && backEdgeCallBeforeHeader(CurTo) &&
      !InlinedFallthroughBlocks.count(CurTo) &&
      !FoldedJoinArms.count(CurTo) && !DuplicatedAssignBlocks.count(CurTo) &&
      !ConditionChainBodies.count(CurTo)) {
    InlinedFallthroughBlocks.insert(CurTo);
    for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(CurTo)) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (!instructionIsPrinted(Inst))
        continue;
      writeInstruction(Inst, Indent);
    }
    const auto *BackBr =
        llvm::cast<llvm::UncondBrInst>(CurTo->getTerminator());
    writeGoto(CurTo, BackBr->getSuccessor(0), Indent, true);
    return;
  }
  if (const llvm::BasicBlock *Tail = duplicatedCallTail(CurTo ? CurTo : To)) {
    for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Tail)) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (!instructionIsPrinted(Inst))
        continue;
      writeInstruction(Inst, Indent);
    }
    InlinedFallthroughBlocks.insert(Tail);
    ReferencedBlocks.erase(Tail);
    const auto *Br = llvm::cast<llvm::UncondBrInst>(Tail->getTerminator());
    writeGoto(Tail, Br->getSuccessor(0), Indent, true);
    return;
  }
  emitIndent(Indent);
  const llvm::BasicBlock *LabelBB = CurTo ? CurTo : To;
  ReferencedBlocks.insert(LabelBB);
  OS << "goto " << blockLabel(LabelBB) << ";\n";
}

bool LLVMCWriter::allocaHasPrintedLoad(const llvm::AllocaInst *Slot) {
  if (!Slot)
    return false;
  for (const llvm::User *U : Slot->users()) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
    if (!LI)
      continue;
    if (Analysis.DeadFrameStores.count(LI) || Analysis.Inlinable.count(LI) ||
        isImportCalleeOnlyLoad(LI) || computedAllocaLoadIsForwarded(LI) ||
        typedRecordAccess(LI->getPointerOperand(),
                          llvmAccessSize(LI->getType())) ||
        typedIndexAccess(LI->getPointerOperand()) ||
        isTypedRecordCursorSlot(Slot))
      continue;
    if (allocaHomeIsNamedParam(Slot) ||
        (allocaOnlyHoldsImmediates(Slot) &&
         allocaLoadsComposeImmediate(Slot)))
      continue;
    // A value folded from the current print path cannot prove that every
    // predecessor stores the same value into this home. Normal branches and
    // EH joins both need their distinct stores visible in C.
    if (foldImmediate(LI) && !LI->use_empty() &&
        uniqueAllocaImmediate(Slot))
      continue;
    if (!valueFeedsPrintedUse(LI))
      continue;
    if (allocaOnlyHoldsImmediates(Slot) && loadOnlyUsedAsCallArgs(LI))
      continue;
    return true;
  }
  return false;
}

void LLVMCWriter::emitFunctionDecls(llvm::Function &Fn) {
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || Analysis.DeadFrameStores.count(SI))
        continue;
      const uint16_t Size = llvmAccessSize(SI->getValueOperand()->getType());
      if (!Size || Size > 4)
        continue;
      (void)frameSlotAccess(SI->getPointerOperand(), Size,
                            /*AddressOf=*/false, /*Synthesize=*/true,
                            /*Overlay=*/false);
    }
  }
  for (const auto &[_, Slot] : FrameSlots) {
    if (Slot.Name.empty())
      continue;
    UsedNames.insert(Slot.Name);
  }

  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst))
        if (isTypedRecordCursorSlot(AI) && !isThisFieldValueHome(AI) &&
            (cursorSlotIsObserved(AI) || allocaAddressTaken(AI)))
          (void)getName(AI);

  std::set<const llvm::Value *> NeedsDecl;
  // Two views share one freshVar sequence. The cross-block view is what
  // already-named temps used. writeBasicBlock also forgets stores at each
  // block, and a load printed from that view needs a declaration too.
  const auto SavedLastValues = AllocaLastValues;
  auto collectDecls = [&](bool ClearEachBlock) {
    AllocaLastValues.clear();
    for (auto &BB : Fn) {
      if (ClearEachBlock)
        AllocaLastValues.clear();
      for (auto &Inst : BB) {
        if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
          if (const llvm::AllocaInst *Slot =
                  asAllocaPointer(SI->getPointerOperand()))
            AllocaLastValues[Slot] = SI->getValueOperand();
        }
      // writeInstruction still emits `name = ...` for unused non-calls
      // (they are not inlinable when use_empty).  Skip only values that
      // the statement writer does not assign.
      if (Inst.getType()->isVoidTy() || Inst.getType()->isTokenTy())
        continue;
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CleanupPadInst>(&Inst))
        continue;
      if (Analysis.IntrinsicStructVals.count(&Inst))
        continue;
      if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(&Inst))
        if (Analysis.IntrinsicStructNames.count(EV->getAggregateOperand()))
          continue;
      if (Analysis.Inlinable.count(&Inst))
        continue;
      if (Analysis.DeadFrameStores.count(&Inst))
        continue;
      if (isUnusedCallClobber(Inst))
        continue;
      if ((llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode>(&Inst) ||
           isUnknownCopyInst(Inst)) &&
          usersAreDeadCopies(&Inst)) {
        const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi || !phiPrintedAsJoinCallArg(Phi))
          continue;
      }
      if (auto Imm = foldImmediate(&Inst)) {
        (void)Imm;
        if (!Inst.use_empty() || imageDataVA(&Inst))
          continue;
      }
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
        if (isImportCalleeOnlyLoad(LI) || computedAllocaLoadIsForwarded(LI) ||
            typedRecordAccess(LI->getPointerOperand(),
                              llvmAccessSize(LI->getType())) ||
            frameSlotAccess(LI->getPointerOperand(),
                            llvmAccessSize(LI->getType()),
                            /*AddressOf=*/false) ||
            typedIndexAccess(LI->getPointerOperand()) ||
            joinFieldDefaultText(asAllocaPointer(LI->getPointerOperand())) ||
            isTypedRecordCursorSlot(
                asAllocaPointer(LI->getPointerOperand())) ||
            isJoinCallArgAlloca(asAllocaPointer(LI->getPointerOperand())))
          continue;
        if (!valueFeedsPrintedUse(LI))
          continue;
        if (const llvm::AllocaInst *ImmSlot =
                asAllocaPointer(LI->getPointerOperand());
            ImmSlot && allocaOnlyHoldsImmediates(ImmSlot) &&
            loadOnlyUsedAsCallArgs(LI))
          continue;
        if (auto Text = ValueTexts.find(LI);
            Text != ValueTexts.end() && !Text->second.empty())
          continue;
        if (const llvm::AllocaInst *Slot =
                asAllocaPointer(LI->getPointerOperand());
            Slot && !allocaAddressTaken(Slot) &&
            (OmittedInlined.count(Slot) || allocaHomeIsNamedParam(Slot) ||
             (allocaOnlyHoldsImmediates(Slot) &&
              allocaLoadsComposeImmediate(Slot))))
          continue;
      }
      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        if (callDoesNotReturn(*CB) || !ctorThisAddress(*CB).empty() ||
            unreadAtlThisReturn(*CB))
          continue;
      }
      if (InferredVoid) {
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst))
          if (!isCallResultLive(Analysis, CI))
            continue;
      }
      NeedsDecl.insert(&Inst);
      }
    }
  };
  collectDecls(false);
  collectDecls(true);
  AllocaLastValues = SavedLastValues;

  // Name temps in instruction order. NeedsDecl is a pointer set, so
  // iterating it assigns freshVar suffixes in an ASLR-dependent order.
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (!NeedsDecl.count(&Inst))
        continue;
      auto DeclName = getName(&Inst);
      // A later print can hide this value. Drop the declaration when the
      // name never appears in the body, and keep a printed unused assign.
      CallDeclNames.push_back(DeclName);
      emitIndent(1);
      OS << typeToCLLVM(Inst.getType()) << " " << DeclName << ";\n";
    }
  }

  bool EmittedNamedSlot = false;
  std::set<std::string> DeclaredSlots;
  for (const auto &[_, Slot] : FrameSlots) {
    if (Slot.Name.empty() || !DeclaredSlots.insert(Slot.Name).second)
      continue;
    emitIndent(1);
    if (Slot.Type)
      OS << declarationToC(Slot.Type, Slot.Name) << ";\n";
    else
      OS << "uint64_t " << Slot.Name << ";\n";
    CallDeclNames.push_back(Slot.Name);
    EmittedNamedSlot = true;
  }

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
        const bool RawFrame =
            !Analysis.RawFrameLocations.empty() && AI == SyntheticFrame;
        if (Analysis.DeadFrameAllocas.count(AI) && !RawFrame)
          continue;
        if (!RawFrame && !allocaAddressTaken(AI) &&
            !(isTypedRecordCursorSlot(AI) && cursorSlotIsObserved(AI) &&
              !isThisFieldValueHome(AI)) &&
            !isJoinCallArgAlloca(AI) && !allocaHasPrintedLoad(AI))
          continue;
        auto AllocName = getName(AI);
        // Same rule as instruction temps: an alloca declared for a load that
        // the statement walk never prints is not a live local.
        CallDeclNames.push_back(AllocName);
        emitIndent(1);
        auto *AllocTy = AI->getAllocatedType();
        if (auto *ArrTy = llvm::dyn_cast<llvm::ArrayType>(AllocTy)) {
          OS << typeToCLLVM(ArrTy->getElementType()) << " " << AllocName << "["
             << ArrTy->getNumElements() << "];\n";
        } else {
          if (auto It = AllocaTypes.find(AI); It != AllocaTypes.end() &&
              It->second && It->second->Kind == NdTypeKind::Ptr &&
              It->second->Pointee &&
              It->second->Pointee->Kind == NdTypeKind::Struct &&
              !It->second->Pointee->SourceName.empty()) {
            const std::string Tag =
                cNamedTypeSpelling(It->second->Pointee->SourceName);
            if (!Tag.empty()) {
              OS << Tag << "* " << AllocName << ";\n";
              continue;
            }
          }
          OS << typeToCLLVM(AllocTy) << " " << AllocName << ";\n";
        }
      }
    }
  }

  if (!NeedsDecl.empty() || EmittedNamedSlot)
    OS << "\n";
}

namespace {

bool isCIdentChar(unsigned char C) {
  return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'Z') ||
         (C >= 'a' && C <= 'z') || C == '_';
}

bool lineDeclaresName(llvm::StringRef Line, llvm::StringRef Name) {
  if (Name.empty() || Line.find('=') != llvm::StringRef::npos)
    return false;
  bool Found = false;
  size_t Pos = 0;
  while (Pos < Line.size()) {
    const size_t At = Line.find(Name, Pos);
    if (At == llvm::StringRef::npos)
      break;
    const bool Left =
        At == 0 || !isCIdentChar(static_cast<unsigned char>(Line[At - 1]));
    const size_t End = At + Name.size();
    const bool Right =
        End >= Line.size() || !isCIdentChar(static_cast<unsigned char>(Line[End]));
    if (Left && Right) {
      Found = true;
      size_t I = End;
      while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
        ++I;
      // A declaration ends at the semicolon. `for (; cur;) {` is a use.
      if (I >= Line.size() || Line[I] != ';')
        return false;
      ++I;
      while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
        ++I;
      if (I < Line.size())
        return false;
    }
    Pos = End;
  }
  return Found;
}

std::optional<size_t> firstNameUseOutsideDecl(llvm::StringRef Text,
                                              llvm::StringRef Name) {
  size_t Pos = 0;
  while (Pos < Text.size()) {
    const size_t At = Text.find(Name, Pos);
    if (At == llvm::StringRef::npos)
      return std::nullopt;
    const bool Left =
        At == 0 || !isCIdentChar(static_cast<unsigned char>(Text[At - 1]));
    const size_t End = At + Name.size();
    const bool Right = End >= Text.size() ||
                       !isCIdentChar(static_cast<unsigned char>(Text[End]));
    if (Left && Right) {
      size_t LineStart = Text.rfind('\n', At);
      LineStart = LineStart == llvm::StringRef::npos ? 0 : LineStart + 1;
      size_t LineEnd = Text.find('\n', At);
      if (LineEnd == llvm::StringRef::npos)
        LineEnd = Text.size();
      if (!lineDeclaresName(Text.substr(LineStart, LineEnd - LineStart), Name))
        return At;
    }
    Pos = End;
  }
  return std::nullopt;
}

bool nameUsedOutsideDecl(llvm::StringRef Text, llvm::StringRef Name) {
  return firstNameUseOutsideDecl(Text, Name).has_value();
}

std::string dropUnusedCallDecls(std::string Text,
                                llvm::ArrayRef<std::string> Names) {
  for (const std::string &Name : Names) {
    if (Name.empty() || nameUsedOutsideDecl(Text, Name))
      continue;
    std::string Out;
    Out.reserve(Text.size());
    size_t Start = 0;
    while (Start < Text.size()) {
      size_t End = Text.find('\n', Start);
      const bool HaveNl = End != std::string::npos;
      if (!HaveNl)
        End = Text.size();
      const llvm::StringRef Line(Text.data() + Start, End - Start);
      if (!lineDeclaresName(Line, Name))
        Out.append(Text, Start, HaveNl ? End - Start + 1 : End - Start);
      Start = HaveNl ? End + 1 : End;
    }
    Text = std::move(Out);
  }
  return Text;
}

} // namespace

void LLVMCWriter::writeFunction(llvm::Function &Fn) {
  if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Fn) &&
      (functionHasWindowsEHPads(Fn) || functionNeedsAnalysisOnlyEHWrap(Fn))) {
    writeAnalysisOnlyFunction(Fn);
    return;
  }
  writeFunctionProjection(Fn);
}

void LLVMCWriter::writeAnalysisOnlyFunction(llvm::Function &Fn) {
  if (Opts.EmitComments) {
    OS << "/* neverd.analysis-only: recovered Windows SEH/C++ as readable C. "
          "*/\n";
  }
  writeFunctionProjection(Fn);
}

void LLVMCWriter::writeFunctionProjection(llvm::Function &Fn) {
  setupFunction(Fn);
  CallDeclNames.clear();
  std::string Buffered;
  llvm::raw_string_ostream BufOS(Buffered);
  struct Retarget {
    LLVMCOut &Out;
    llvm::raw_ostream *Primary;
    bool Armed = true;
    ~Retarget() {
      if (Armed)
        Out.retarget(Primary);
    }
  } Guard{OS, &OS.stream()};
  OS.retarget(&BufOS);

  std::string FName = functionIdentifier(Fn);
  writeExceptionAnnotation(Fn);

  const bool IndirectReturn =
      DebugFn && isMsvcIndirectReturn(DebugFn->ReturnType);
  const bool MemberIndirectReturn =
      IndirectReturn && !DebugFn->Params.empty() &&
      DebugFn->Params[0].first == "this";
  const int SretParamIdx =
      !IndirectReturn ? -1 : (MemberIndirectReturn ? 1 : 0);
  if (IndirectReturn)
    InferredVoid = false;
  auto IndirectReturnTypeStr = [&]() -> std::string {
    if (!IndirectReturn)
      return {};
    const NdType *Record = msvcIndirectReturnRecord(DebugFn->ReturnType);
    if (!Record)
      return {};
    const std::string Tag = cNamedTypeSpelling(Record->SourceName);
    if (Tag.empty())
      return {};
    return Tag + "*";
  };

  CProjectionIdentifierAllocator ParameterIdentifiers;
  auto BindParam = [&](llvm::Argument &Arg, unsigned ParamIdx) {
    std::string Raw;
    if (SretParamIdx >= 0 && static_cast<int>(ParamIdx) == SretParamIdx)
      Raw = "result";
    else if (DebugFn) {
      size_t DebugIdx = ParamIdx;
      if (SretParamIdx >= 0 && static_cast<int>(ParamIdx) > SretParamIdx)
        DebugIdx = static_cast<size_t>(static_cast<int>(ParamIdx) - 1);
      if (DebugIdx < DebugFn->Params.size() &&
          !DebugFn->Params[DebugIdx].first.empty())
        Raw = DebugFn->Params[DebugIdx].first;
    }
    if (Raw.empty() && Arg.hasName())
      Raw = Arg.getName().str();
    if (Raw.empty())
      Raw = "arg" + std::to_string(ParamIdx);
    std::string ParamName = ParameterIdentifiers.allocate(Raw, "nd_arg");
    ValNames[&Arg] = ParamName;
    UsedNames.insert(ParamName);
    return ParamName;
  };
  auto ParamTypeStr = [&](llvm::Argument &Arg, unsigned ParamIdx) {
    if (SretParamIdx >= 0 && static_cast<int>(ParamIdx) == SretParamIdx) {
      if (std::string Ty = IndirectReturnTypeStr(); !Ty.empty())
        return Ty;
    }
    size_t DebugIdx = ParamIdx;
    if (SretParamIdx >= 0 && static_cast<int>(ParamIdx) > SretParamIdx)
      DebugIdx = static_cast<size_t>(static_cast<int>(ParamIdx) - 1);
    if (DebugFn && DebugIdx < DebugFn->Params.size()) {
      TypeRef Ty = DebugFn->Params[DebugIdx].second;
      if (Ty && Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
          Ty->Pointee->Kind == NdTypeKind::Struct &&
          !Ty->Pointee->SourceName.empty()) {
        const std::string Tag = cNamedTypeSpelling(Ty->Pointee->SourceName);
        if (!Tag.empty())
          return Tag + "*";
      }
      if (Ty && Ty->Kind == NdTypeKind::Struct && Ty->IsEnum &&
          !Ty->SourceName.empty()) {
        const std::string Tag = cNamedTypeSpelling(Ty->SourceName);
        if (!Tag.empty())
          return Tag;
      }
    }
    return typeToCLLVM(Arg.getType());
  };

  if (EmitFunctionWrapper) {
    auto *FuncTy = Fn.getFunctionType();
    std::string RetStr =
        InferredVoid ? "void" : typeToCLLVM(FuncTy->getReturnType());
    if (std::string Ty = IndirectReturnTypeStr(); !Ty.empty())
      RetStr = std::move(Ty);
    OS << RetStr << " " << FName << "(";

    unsigned ParamIdx = 0;
    for (auto &Arg : Fn.args()) {
      if (ParamIdx > 0)
        OS << ", ";
      OS << ParamTypeStr(Arg, ParamIdx) << " " << BindParam(Arg, ParamIdx);
      ++ParamIdx;
    }
    if (Fn.isVarArg() && ParamIdx != 0) {
      if (ParamIdx > 0)
        OS << ", ";
      OS << "...";
    }
    OS << ") {\n";
  } else {
    unsigned ParamIdx = 0;
    for (auto &Arg : Fn.args()) {
      BindParam(Arg, ParamIdx);
      ++ParamIdx;
    }
  }

  BufOS.flush();
  const size_t DeclInsertPos = Buffered.size();
  emitFunctionDecls(Fn);
  {
    llvm::SmallPtrSet<const llvm::AllocaInst *, 8> Seen;
    for (const llvm::BasicBlock &BB : Fn) {
      for (const llvm::Instruction &Inst : BB) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        if (!SI)
          continue;
        const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
        if (!Slot || !Seen.insert(Slot).second)
          continue;
        std::optional<std::string> Imm = joinFieldDefaultText(Slot);
        if (!Imm || !allocaHasPrintedLoad(Slot))
          continue;
        const std::string Name = getName(const_cast<llvm::AllocaInst *>(Slot));
        if (Name.empty())
          continue;
        emitIndent(1);
        OS << Name << " = " << *Imm << ";\n";
      }
    }
  }

  std::vector<EHWrapClause> EHWraps;
  if (functionHasWindowsEHPads(Fn))
    EHWraps = collectWindowsEHWraps(Fn);
  else if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Fn) &&
           functionNeedsAnalysisOnlyEHWrap(Fn)) {
    EHWrapClause Fallback;
    const bool Cxx = functionIsCxxEH(Fn) || [&Fn]() {
      const llvm::MDNode *Payload =
          Fn.getMetadata(windows_eh_md::FunctionAttachment);
      if (!Payload)
        Payload = Fn.getMetadata(windows_eh_md::NativeAttachment);
      if (!Payload ||
          windows_eh_md::PersonalityName >= Payload->getNumOperands())
        return false;
      if (const auto *Name = llvm::dyn_cast<llvm::MDString>(
              Payload->getOperand(windows_eh_md::PersonalityName)))
        return Name->getString().contains("CxxFrame");
      return false;
    }();
    if (Cxx)
      Fallback.Kind = EHWrapClause::Kind::CxxCleanup;
    else {
      Fallback.Kind = EHWrapClause::Kind::SEHExcept;
      Fallback.Clause = "EXCEPTION_EXECUTE_HANDLER";
    }
    EHWraps.push_back(std::move(Fallback));
  }
  EHWrapIsCxx = functionIsCxxEH(Fn);
  EHTryDepth = 0;

  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> SkipInTry;
  for (const EHWrapClause &Clause : EHWraps)
    for (const llvm::BasicBlock *BB : Clause.Body)
      SkipInTry.insert(BB);
  for (const llvm::BasicBlock &BB : Fn) {
    if (isEHDispatchOrPad(BB))
      SkipInTry.insert(&BB);
  }

  struct SehRangeMarker {
    const llvm::BasicBlock *Begin = nullptr;
    const llvm::BasicBlock *End = nullptr;
  };
  std::vector<SehRangeMarker> RangeMarkers(EHWraps.size());
  bool UseRanges = !EHWraps.empty();
  auto PadBlock = [](const EHWrapClause &Clause) -> const llvm::BasicBlock * {
    if (Clause.Switch)
      return Clause.Switch->getParent();
    if (Clause.Cleanup)
      return Clause.Cleanup->getParent();
    return nullptr;
  };
  auto SehRangeInvoke = [](const llvm::BasicBlock &BB,
                           llvm::Intrinsic::ID ID) -> const llvm::InvokeInst * {
    const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(BB.getTerminator());
    if (!Invoke)
      return nullptr;
    const llvm::Function *Callee = Invoke->getCalledFunction();
    if (!Callee || Callee->getIntrinsicID() != ID)
      return nullptr;
    return Invoke;
  };
  if (UseRanges) {
    for (const llvm::BasicBlock &BB : Fn) {
      const llvm::InvokeInst *Begin =
          SehRangeInvoke(BB, llvm::Intrinsic::seh_try_begin);
      const llvm::InvokeInst *End =
          SehRangeInvoke(BB, llvm::Intrinsic::seh_try_end);
      const llvm::InvokeInst *Marker = Begin ? Begin : End;
      if (!Marker)
        continue;
      int Index = -1;
      for (size_t I = 0; I < EHWraps.size(); ++I)
        if (PadBlock(EHWraps[I]) == Marker->getUnwindDest())
          Index = static_cast<int>(I);
      if (Index < 0) {
        UseRanges = false;
        break;
      }
      SehRangeMarker &Slot = RangeMarkers[static_cast<size_t>(Index)];
      const llvm::BasicBlock *&Bound = Begin ? Slot.Begin : Slot.End;
      if (Bound) {
        UseRanges = false;
        break;
      }
      Bound = &BB;
    }
  }
  if (UseRanges) {
    for (const SehRangeMarker &Slot : RangeMarkers)
      if (!Slot.Begin || !Slot.End)
        UseRanges = false;
  }
  if (UseRanges) {
    std::vector<int> Stack;
    for (const llvm::BasicBlock &BB : Fn) {
      int BeginIndex = -1;
      int EndIndex = -1;
      for (size_t I = 0; I < RangeMarkers.size(); ++I) {
        if (RangeMarkers[I].Begin == &BB)
          BeginIndex = static_cast<int>(I);
        if (RangeMarkers[I].End == &BB)
          EndIndex = static_cast<int>(I);
      }
      if (BeginIndex >= 0 && EndIndex >= 0) {
        UseRanges = false;
        break;
      }
      if (BeginIndex >= 0)
        Stack.push_back(BeginIndex);
      if (EndIndex >= 0) {
        if (Stack.empty() || Stack.back() != EndIndex) {
          UseRanges = false;
          break;
        }
        Stack.pop_back();
      }
    }
    if (UseRanges && !Stack.empty())
      UseRanges = false;
  }

  auto WriteRangeResidue = [&](llvm::BasicBlock &BB, int Indent) {
    for (llvm::Instruction &Inst : BB) {
      if (llvm::isa<llvm::InvokeInst, llvm::UncondBrInst>(&Inst))
        continue;
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        if (const llvm::Function *Callee = Call->getCalledFunction()) {
          const llvm::Intrinsic::ID IID = Callee->getIntrinsicID();
          if (IID == llvm::Intrinsic::seh_try_begin ||
              IID == llvm::Intrinsic::seh_try_end ||
              IID == llvm::Intrinsic::sideeffect ||
              IID == llvm::Intrinsic::donothing)
            continue;
        }
      }
      writeInstruction(Inst, Indent);
    }
  };

  // A single __except whose handler resumes at the invoke's normal
  // destination prints that destination after the wrap. Leaving it inside
  // the try drops it from the handler path once the resume goto is omitted.
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> AfterWrap;
  if (!UseRanges && EHWraps.size() == 1 &&
      EHWraps.front().Kind == EHWrapClause::Kind::SEHExcept &&
      EHWraps.front().Switch && EHWraps.front().Body.size() == 1) {
    const llvm::BasicBlock *Dispatch = EHWraps.front().Switch->getParent();
    const llvm::BasicBlock *Handler = EHWraps.front().Body.front();
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Normal;
    collectNormalReachable(Fn, Normal);
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> InvokeBlocks;
    for (const llvm::BasicBlock &BB : Fn) {
      const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(BB.getTerminator());
      if (!Invoke || Invoke->getUnwindDest() != Dispatch)
        continue;
      InvokeBlocks.insert(&BB);
    }
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> TryRegion = InvokeBlocks;
    llvm::SmallVector<const llvm::BasicBlock *, 16> Back(InvokeBlocks.begin(),
                                                         InvokeBlocks.end());
    while (!Back.empty()) {
      const llvm::BasicBlock *Cur = Back.pop_back_val();
      for (const llvm::BasicBlock *Pred : llvm::predecessors(Cur)) {
        if (Pred && Normal.count(Pred) && TryRegion.insert(Pred).second)
          Back.push_back(Pred);
      }
    }
    for (const llvm::BasicBlock &BB : Fn) {
      if (!Normal.count(&BB) || TryRegion.count(&BB) || &BB == Handler ||
          isEHDispatchOrPad(BB) || hasCleanupPad(BB))
        continue;
      AfterWrap.insert(&BB);
    }
    const llvm::BasicBlock *FirstCont = nullptr;
    for (const llvm::BasicBlock &BB : Fn) {
      if (!AfterWrap.count(&BB))
        continue;
      FirstCont = &BB;
      break;
    }
    const llvm::Instruction *HandlerTerm =
        Handler ? Handler->getTerminator() : nullptr;
    const auto *HandlerBr = llvm::dyn_cast_or_null<llvm::UncondBrInst>(HandlerTerm);
    bool Ok = FirstCont && !InvokeBlocks.empty() && HandlerTerm &&
              ((HandlerBr && HandlerBr->getSuccessor(0) == FirstCont &&
                !edgePrintsPhiCopy(Handler, FirstCont)) ||
               llvm::isa<llvm::ReturnInst>(HandlerTerm));
    if (Ok) {
      for (const llvm::BasicBlock *InvokeBB : InvokeBlocks) {
        const auto *Invoke =
            llvm::cast<llvm::InvokeInst>(InvokeBB->getTerminator());
        if (Invoke->getNormalDest() != FirstCont) {
          Ok = false;
          break;
        }
      }
    }
    if (Ok) {
      for (const llvm::BasicBlock *BB : AfterWrap) {
        bool Any = false;
        for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
          Any = true;
          const bool FromInvoke =
              InvokeBlocks.count(Pred) && BB == FirstCont;
          const bool FromHandler = Pred == Handler && BB == FirstCont;
          const bool FromCont = AfterWrap.count(Pred);
          if (!FromInvoke && !FromHandler && !FromCont) {
            Ok = false;
            break;
          }
        }
        if (!Any || !Ok)
          break;
      }
    }
    if (!Ok)
      AfterWrap.clear();
  }

  if (!UseRanges && AfterWrap.empty() && EHWraps.size() == 1 &&
      (EHWraps.front().Kind == EHWrapClause::Kind::SEHFinally ||
       EHWraps.front().Kind == EHWrapClause::Kind::CxxCleanup) &&
      EHWraps.front().Cleanup && EHWraps.front().Body.size() == 1) {
    const llvm::BasicBlock *CleanupBB = EHWraps.front().Cleanup->getParent();
    const auto *CRet = llvm::dyn_cast_or_null<llvm::CleanupReturnInst>(
        CleanupBB ? CleanupBB->getTerminator() : nullptr);
    const llvm::BasicBlock *Dest = CRet ? CRet->getUnwindDest() : nullptr;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Normal;
    collectNormalReachable(Fn, Normal);
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> InvokeBlocks;
    for (const llvm::BasicBlock &BB : Fn) {
      const auto *Invoke =
          llvm::dyn_cast<llvm::InvokeInst>(BB.getTerminator());
      if (!Invoke || Invoke->getUnwindDest() != CleanupBB)
        continue;
      InvokeBlocks.insert(&BB);
    }
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> TryRegion = InvokeBlocks;
    llvm::SmallVector<const llvm::BasicBlock *, 16> Back(InvokeBlocks.begin(),
                                                         InvokeBlocks.end());
    while (!Back.empty()) {
      const llvm::BasicBlock *Cur = Back.pop_back_val();
      for (const llvm::BasicBlock *Pred : llvm::predecessors(Cur)) {
        if (Pred && Normal.count(Pred) && TryRegion.insert(Pred).second)
          Back.push_back(Pred);
      }
    }
    for (const llvm::BasicBlock &BB : Fn) {
      if (!Normal.count(&BB) || TryRegion.count(&BB) || &BB == CleanupBB ||
          isEHDispatchOrPad(BB) || hasCleanupPad(BB))
        continue;
      AfterWrap.insert(&BB);
    }
    const llvm::BasicBlock *FirstCont = nullptr;
    for (const llvm::BasicBlock &BB : Fn) {
      if (!AfterWrap.count(&BB))
        continue;
      FirstCont = &BB;
      break;
    }
    bool Ok = FirstCont && !InvokeBlocks.empty() && Dest == FirstCont &&
              !edgePrintsPhiCopy(CleanupBB, FirstCont);
    if (Ok) {
      for (const llvm::BasicBlock *InvokeBB : InvokeBlocks) {
        const auto *Invoke =
            llvm::cast<llvm::InvokeInst>(InvokeBB->getTerminator());
        if (Invoke->getNormalDest() != FirstCont) {
          Ok = false;
          break;
        }
      }
    }
    if (Ok) {
      for (const llvm::BasicBlock *BB : AfterWrap) {
        bool Any = false;
        for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
          Any = true;
          const bool FromInvoke = InvokeBlocks.count(Pred) && BB == FirstCont;
          const bool FromCleanup = Pred == CleanupBB && BB == FirstCont;
          const bool FromCont = AfterWrap.count(Pred);
          if (!FromInvoke && !FromCleanup && !FromCont) {
            Ok = false;
            break;
          }
        }
        if (!Any || !Ok)
          break;
      }
    }
    if (!Ok)
      AfterWrap.clear();
    else {
      for (const llvm::BasicBlock *BB : AfterWrap) {
        OmitCleanupRetTo.insert(BB);
        ReferencedBlocks.erase(BB);
      }
    }
  }

  for (const llvm::BasicBlock &BB : Fn) {
    const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(BB.getTerminator());
    if (!Br || !Br->getSuccessor(0))
      continue;
    const llvm::BasicBlock *Latch = nullptr;
    const llvm::BasicBlock *Step = nullptr;
    const llvm::AllocaInst *Slot = nullptr;
    if (splitPhiCursorLoop(Br->getSuccessor(0), Latch, Step, Slot) &&
        Latch == &BB)
      InlinedFallthroughBlocks.insert(&BB);
  }

  if (!UseRanges) {
    EHSkippedMainBlocks.insert(SkipInTry.begin(), SkipInTry.end());
    // Multiple try ranges may share one recovered clause.  The fallback
    // prints all normal blocks first and moves handler bodies to __except;
    // only a try-end marker in this main walk can be a fallthrough target.
    for (const llvm::BasicBlock &BB : Fn)
      if (!SkipInTry.contains(&BB) && !AfterWrap.contains(&BB) &&
          SehRangeInvoke(BB, llvm::Intrinsic::seh_try_end))
        EHBoundaryBlocks.insert(&BB);
    for (const EHWrapClause &Clause : EHWraps) {
      writeEHWrapOpen(Clause, 1 + EHTryDepth);
      ++EHTryDepth;
    }
    for (auto &BB : Fn) {
      if (SkipInTry.contains(&BB) || AfterWrap.contains(&BB))
        continue;
      if (&BB != &Fn.getEntryBlock() && llvm::pred_empty(&BB))
        continue;
      if (backEdgeCallBeforeHeader(&BB))
        continue;
      EHMainBlock = &BB;
      writeBasicBlock(BB, 1 + EHTryDepth);
      EHMainBlock = nullptr;
    }
    bool EmittedEHLabel = false;
    for (const llvm::BasicBlock &BB : Fn) {
      if (!SkipInTry.contains(&BB) || !isEHDispatchOrPad(BB))
        continue;
      OS << blockLabel(&BB) << ":\n";
      EmittedEHLabel = true;
    }
    if (EmittedEHLabel) {
      // A label at the end of a compound statement needs a statement in C11.
      emitIndent(1 + EHTryDepth);
      OS << ";\n";
    }
    while (EHTryDepth > 0) {
      --EHTryDepth;
      writeEHWrapClose(EHWraps[static_cast<size_t>(EHTryDepth)],
                       1 + EHTryDepth);
    }
    for (auto &BB : Fn) {
      if (!AfterWrap.contains(&BB))
        continue;
      if (&BB != &Fn.getEntryBlock() && llvm::pred_empty(&BB))
        continue;
      if (backEdgeCallBeforeHeader(&BB))
        continue;
      writeBasicBlock(BB, 1);
    }
  } else {
    EHBoundaryBlocks.clear();
    for (const SehRangeMarker &Slot : RangeMarkers) {
      EHBoundaryBlocks.insert(Slot.Begin);
      EHBoundaryBlocks.insert(Slot.End);
    }
    for (auto &BB : Fn) {
      int BeginIndex = -1;
      int EndIndex = -1;
      for (size_t I = 0; I < RangeMarkers.size(); ++I) {
        if (RangeMarkers[I].Begin == &BB)
          BeginIndex = static_cast<int>(I);
        if (RangeMarkers[I].End == &BB)
          EndIndex = static_cast<int>(I);
      }
      if (BeginIndex >= 0) {
        OS << blockLabel(&BB) << ":\n";
        WriteRangeResidue(BB, 1 + EHTryDepth);
        writeEHWrapOpen(EHWraps[static_cast<size_t>(BeginIndex)],
                        1 + EHTryDepth);
        ++EHTryDepth;
        continue;
      }
      if (EndIndex >= 0) {
        OS << blockLabel(&BB) << ":\n";
        WriteRangeResidue(BB, 1 + EHTryDepth);
        --EHTryDepth;
        writeEHWrapClose(EHWraps[static_cast<size_t>(EndIndex)],
                         1 + EHTryDepth);
        continue;
      }
      if (SkipInTry.contains(&BB))
        continue;
      if (&BB != &Fn.getEntryBlock() && llvm::pred_empty(&BB))
        continue;
      if (backEdgeCallBeforeHeader(&BB))
        continue;
      EHMainBlock = &BB;
      writeBasicBlock(BB, 1 + EHTryDepth);
      EHMainBlock = nullptr;
    }
  }

  if (EmitFunctionWrapper)
    OS << "}\n";
  BufOS.flush();
  Guard.Armed = false;
  OS.retarget(Guard.Primary);
  // Declaration prediction runs before the statement walk. EH wraps and
  // cross-block alloca homes can change which values and slots the statement
  // writer actually prints. Reconcile against the rendered body so
  // every printed local has a C declaration, without naming unprinted values
  // or changing the instruction-order freshVar sequence.
  std::set<std::string> DeclaredNames(CallDeclNames.begin(),
                                      CallDeclNames.end());
  std::vector<std::pair<size_t, std::string>> MissingDecls;
  for (llvm::BasicBlock &BB : Fn) {
    for (llvm::Instruction &Inst : BB) {
      if (Inst.getType()->isVoidTy() || Inst.getType()->isTokenTy())
        continue;
      const auto It = ValNames.find(&Inst);
      if (It == ValNames.end() || It->second.empty() ||
          DeclaredNames.count(It->second))
        continue;
      const std::optional<size_t> FirstUse =
          firstNameUseOutsideDecl(Buffered, It->second);
      if (!FirstUse)
        continue;
      std::string Declaration;
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
        llvm::Type *Allocated = AI->getAllocatedType();
        if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Allocated)) {
          Declaration = "    " + typeToCLLVM(Array->getElementType()) + " " +
                        It->second + "[" +
                        std::to_string(Array->getNumElements()) + "];\n";
        } else {
          std::string Type = typeToCLLVM(Allocated);
          if (auto Found = AllocaTypes.find(AI);
              Found != AllocaTypes.end() && Found->second &&
              Found->second->Kind == NdTypeKind::Ptr &&
              Found->second->Pointee &&
              Found->second->Pointee->Kind == NdTypeKind::Struct) {
            const std::string Tag =
                cNamedTypeSpelling(Found->second->Pointee->SourceName);
            if (!Tag.empty())
              Type = Tag + "*";
          }
          Declaration = "    " + Type + " " + It->second + ";\n";
        }
      } else {
        Declaration = "    " + typeToCLLVM(Inst.getType()) + " " + It->second +
                      ";\n";
      }
      MissingDecls.emplace_back(*FirstUse, std::move(Declaration));
      DeclaredNames.insert(It->second);
    }
  }
  if (!MissingDecls.empty()) {
    std::stable_sort(MissingDecls.begin(), MissingDecls.end(),
                     [](const auto &A, const auto &B) {
                       return A.first < B.first;
                     });
    std::string Declarations;
    for (const auto &[_, Decl] : MissingDecls)
      Declarations += Decl;
    Buffered.insert(DeclInsertPos, Declarations);
  }
  std::vector<std::string> DropNames = CallDeclNames;
  DropNames.insert(DropNames.end(), FoldedLocalNames.begin(),
                   FoldedLocalNames.end());
  OS << dropUnusedCallDecls(Buffered, DropNames);
}

} // namespace neverd
