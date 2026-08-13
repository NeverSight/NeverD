//===- MedLLVMNativeSEH.cpp - Native SEH lowering -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native LLVM lowering for supported x64 Windows SEH functions.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

using med_llvm_eh::mdUInt;

bool MedLLVMEmitter::emitNativeSEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::CSpecificHandler || !EH.SEH ||
      EH.SEH->Scopes.empty() ||
      (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
       EH.Encoding != ExceptionEncoding::X64UnwindV2))
    return false;

  struct Region {
    const SEHScopeRecord *Scope = nullptr;
    llvm::BasicBlock *Handler = nullptr;
    llvm::Function *Callback = nullptr;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
    std::set<llvm::BasicBlock *> Blocks;
  };

  auto FunctionAt = [&](va_t Address) -> llvm::Function * {
    auto NameIt = FuncNames.find(Address);
    return NameIt == FuncNames.end() ? nullptr
                                     : Mod->getFunction(NameIt->second);
  };
  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      return It == OriginalBlockMap.end() ? nullptr : It->second;
    }
    return nullptr;
  };

  std::vector<Region> Regions;
  Regions.reserve(EH.SEH->Scopes.size());
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !Scope.GuardedRange.isValid() ||
        !EH.CodeRange.contains(Scope.GuardedRange))
      return false;

    Region R;
    R.Scope = &Scope;
    if (Scope.Kind == SEHScopeKind::Finally) {
      R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
      if (!R.Callback || R.Callback == &LLVMFunc)
        return false;
    } else {
      R.Handler = BlockAt(Scope.HandlerVA);
      if (!R.Handler || Scope.GuardedRange.contains(Scope.HandlerVA))
        return false;
      if (Scope.Kind == SEHScopeKind::Filter) {
        R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
        if (!R.Callback || R.Callback == &LLVMFunc)
          return false;
      }
    }

    bool HasBegin = false;
    bool HasEnd = false;
    for (const MedBlock &Block : Func.Blocks) {
      ExceptionAddressRange BlockRange{Block.StartAddr, Block.EndAddr};
      if (!BlockRange.isValid())
        continue;
      if (Scope.GuardedRange.overlaps(BlockRange) &&
          !Scope.GuardedRange.contains(BlockRange))
        return false;
      if (!Scope.GuardedRange.contains(BlockRange))
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end())
        return false;
      R.Blocks.insert(It->second);
      HasBegin |= Block.StartAddr == Scope.GuardedRange.Begin;
      HasEnd |= Block.EndAddr == Scope.GuardedRange.End;
    }
    if (R.Blocks.empty() || !HasBegin || !HasEnd)
      return false;
    Regions.push_back(std::move(R));
  }

  // Native WinEH can express nested or disjoint intervals.  Crossing and
  // duplicate intervals have no unambiguous unwind-parent relation, so leave
  // those functions in lossless-metadata-only form.
  for (size_t I = 0; I < Regions.size(); ++I) {
    for (size_t J = I + 1; J < Regions.size(); ++J) {
      const ExceptionAddressRange &A = Regions[I].Scope->GuardedRange;
      const ExceptionAddressRange &B = Regions[J].Scope->GuardedRange;
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return false;
    }
  }
  std::stable_sort(
      Regions.begin(), Regions.end(), [](const Region &A, const Region &B) {
        if (A.Scope->GuardedRange.size() != B.Scope->GuardedRange.size())
          return A.Scope->GuardedRange.size() > B.Scope->GuardedRange.size();
        return A.Scope->GuardedRange.Begin < B.Scope->GuardedRange.Begin;
      });
  for (size_t I = 0; I < Regions.size(); ++I) {
    uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!Regions[J].Scope->GuardedRange.contains(
              Regions[I].Scope->GuardedRange))
        continue;
      if (Regions[J].Scope->GuardedRange.size() < ParentSize) {
        Regions[I].Parent = static_cast<int>(J);
        ParentSize = Regions[J].Scope->GuardedRange.size();
      }
    }
  }

  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction("__C_specific_handler", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
    std::string Suffix = std::to_string(I);
    if (R.Scope->Kind == SEHScopeKind::Finally) {
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "seh.finally.dispatch." + Suffix, &LLVMFunc);
      llvm::IRBuilder<> B(PadBB);
      auto *Pad = B.CreateCleanupPad(TokenNone, {}, "seh.finally.pad");
      llvm::Function *LocalAddress = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress);
      llvm::Value *Frame = B.CreateCall(LocalAddress);
      auto *CallbackTy =
          llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                  {llvm::Type::getInt8Ty(*Ctx), PtrTy}, false);
      llvm::SmallVector<llvm::Value *, 2> Args{
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), 1), Frame};
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Funclet("funclet", BundleInputs);
      B.CreateCall(CallbackTy, R.Callback, Args, {Funclet});
      B.CreateCleanupRet(Pad, ParentDest);
      R.UnwindDest = PadBB;
      continue;
    }

    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "seh.catch.dispatch." + Suffix, &LLVMFunc);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch =
        DB.CreateCatchSwitch(TokenNone, ParentDest, 1, "seh.catch.switch");
    auto *PadBB =
        llvm::BasicBlock::Create(*Ctx, "seh.catch.pad." + Suffix, &LLVMFunc);
    Switch->addHandler(PadBB);
    llvm::IRBuilder<> PB(PadBB);
    llvm::Value *Filter =
        R.Scope->Kind == SEHScopeKind::CatchAll
            ? static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(
                  llvm::cast<llvm::PointerType>(PtrTy)))
            : static_cast<llvm::Value *>(R.Callback);
    auto *Pad = PB.CreateCatchPad(Switch, {Filter}, "seh.catch.pad.token");
    PB.CreateCatchRet(Pad, R.Handler);
    R.UnwindDest = Dispatch;
  }

  // Replace may-unwind calls in each protected machine block with invokes to
  // the innermost active region.  Non-call hardware faults are covered by the
  // asynchronous try markers installed below.
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::BasicBlock *InitialBB = BBIt->second;
    int Innermost = -1;
    uint64_t InnermostSize = std::numeric_limits<uint64_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].Scope->GuardedRange.size() >= InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].Scope->GuardedRange.size();
    }
    if (Innermost < 0)
      continue;

    llvm::SmallVector<llvm::CallInst *, 8> Calls;
    for (llvm::Instruction &Inst : *InitialBB)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
        if (!Call->doesNotThrow() && !Call->isMustTailCall() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          Calls.push_back(Call);

    for (llvm::CallInst *Call : Calls) {
      llvm::BasicBlock *CallBB = Call->getParent();
      llvm::Instruction *Next = Call->getNextNode();
      if (!Next)
        return false;
      llvm::BasicBlock *Cont =
          CallBB->splitBasicBlock(Next, CallBB->getName() + ".seh.cont");
      auto *OldBranch = CallBB->getTerminator();
      llvm::SmallVector<llvm::Value *, 8> Args;
      for (llvm::Use &Arg : Call->args())
        Args.push_back(Arg.get());
      llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
      Call->getOperandBundlesAsDefs(Bundles);
      auto *Invoke = llvm::InvokeInst::Create(
          Call->getFunctionType(), Call->getCalledOperand(), Cont,
          Regions[static_cast<size_t>(Innermost)].UnwindDest, Args, Bundles,
          Call->getName(), OldBranch->getIterator());
      Invoke->setCallingConv(Call->getCallingConv());
      Invoke->setAttributes(Call->getAttributes());
      Invoke->setDebugLoc(Call->getDebugLoc());
      Invoke->copyMetadata(*Call);
      Call->replaceAllUsesWith(Invoke);
      Call->eraseFromParent();
      OldBranch->eraseFromParent();
      for (Region &R : Regions)
        if (R.Blocks.count(CallBB))
          R.Blocks.insert(Cont);
    }
  }

  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_try_end);
  auto IsNormalSuccessor = [](llvm::Instruction *Term, unsigned Index) {
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term))
      return Index == 0 && Invoke->getNormalDest();
    if (llvm::isa<llvm::CatchSwitchInst, llvm::CleanupReturnInst>(Term))
      return false;
    return true;
  };

  // Outer-first placement produces the required nesting order on a shared
  // boundary: outer.begin -> inner.begin -> body -> inner.end -> outer.end.
  for (size_t RegionIndex = 0; RegionIndex < Regions.size(); ++RegionIndex) {
    Region &R = Regions[RegionIndex];
    struct EntryEdge {
      llvm::BasicBlock *Pred = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<EntryEdge, 8> Entries;
    bool EntryWithoutPred = false;
    llvm::BasicBlock *FunctionEntry = &LLVMFunc.getEntryBlock();
    for (llvm::BasicBlock *Target : R.Blocks) {
      bool HasNormalPred = false;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Target)) {
        llvm::Instruction *Term = Pred->getTerminator();
        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
          if (Term->getSuccessor(I) != Target || !IsNormalSuccessor(Term, I))
            continue;
          HasNormalPred = true;
          if (!R.Blocks.count(Pred))
            Entries.push_back({Pred, Target, I});
        }
      }
      EntryWithoutPred |= Target == FunctionEntry && !HasNormalPred;
    }

    unsigned Marker = 0;
    for (const EntryEdge &Edge : Entries) {
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Marker++),
          &LLVMFunc, Edge.Target);
      Edge.Pred->getTerminator()->setSuccessor(Edge.SuccessorIndex, BeginBB);
      llvm::IRBuilder<> B(BeginBB);
      B.CreateInvoke(TryBegin, Edge.Target, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }
    if (EntryWithoutPred) {
      auto *OldEntry = &LLVMFunc.getEntryBlock();
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.begin." + std::to_string(RegionIndex) + ".entry",
          &LLVMFunc, OldEntry);
      llvm::IRBuilder<> B(BeginBB);
      B.CreateInvoke(TryBegin, OldEntry, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }

    struct ExitEdge {
      llvm::Instruction *Term = nullptr;
      llvm::BasicBlock *Target = nullptr;
      unsigned SuccessorIndex = 0;
    };
    llvm::SmallVector<ExitEdge, 8> Exits;
    llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
    for (llvm::BasicBlock *Source : R.Blocks) {
      llvm::Instruction *Term = Source->getTerminator();
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Term)) {
        Returns.push_back(Ret);
        continue;
      }
      for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
        llvm::BasicBlock *Target = Term->getSuccessor(I);
        if (IsNormalSuccessor(Term, I) && !R.Blocks.count(Target))
          Exits.push_back({Term, Target, I});
      }
    }

    Marker = 0;
    for (const ExitEdge &Edge : Exits) {
      auto *EndBB = llvm::BasicBlock::Create(*Ctx,
                                             "seh.try.end." +
                                                 std::to_string(RegionIndex) +
                                                 "." + std::to_string(Marker++),
                                             &LLVMFunc, Edge.Target);
      Edge.Term->setSuccessor(Edge.SuccessorIndex, EndBB);
      llvm::IRBuilder<> B(EndBB);
      B.CreateInvoke(TryEnd, Edge.Target, R.UnwindDest);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
    for (llvm::ReturnInst *Ret : Returns) {
      llvm::BasicBlock *Source = Ret->getParent();
      auto *EndBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.end." + std::to_string(RegionIndex) + ".ret." +
              std::to_string(Marker++),
          &LLVMFunc);
      auto *RetBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.ret." + std::to_string(RegionIndex), &LLVMFunc);
      llvm::Value *ReturnValue = Ret->getReturnValue();
      Ret->eraseFromParent();
      llvm::IRBuilder<> SourceBuilder(Source);
      SourceBuilder.CreateBr(EndBB);
      llvm::IRBuilder<> EndBuilder(EndBB);
      EndBuilder.CreateInvoke(TryEnd, RetBB, R.UnwindDest);
      llvm::IRBuilder<> RetBuilder(RetBB);
      if (ReturnValue)
        RetBuilder.CreateRet(ReturnValue);
      else
        RetBuilder.CreateRetVoid();
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
  }

  if (!Mod->getModuleFlag("eh-asynch"))
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(*Ctx, {mdUInt(*Ctx, 1, 1),
                               llvm::MDString::get(*Ctx, "seh-x64-native")}));
  return true;
}

} // namespace neverd
