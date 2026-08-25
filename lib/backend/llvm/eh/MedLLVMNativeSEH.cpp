//===- MedLLVMNativeSEH.cpp - Native SEH lowering -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native LLVM lowering for supported Windows SEH functions.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
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
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

using med_llvm_eh::mdUInt;

namespace {

bool hasSupportedSEHCallbackABI(const llvm::Function &Callback,
                                const llvm::FunctionType *ExpectedType,
                                Arch TargetArch) {
  if (TargetArch != Arch::X64)
    return false;
  if (Callback.getFunctionType() != ExpectedType ||
      Callback.getCallingConv() != llvm::CallingConv::C)
    return false;

  // Lifted image functions use ordinary external linkage.  Source-produced
  // outlined helpers commonly use local definitions.  Discardable, weak, and
  // available-externally bodies cannot represent an address-backed callback
  // with stable identity.
  return Callback.hasExternalLinkage() ||
         (Callback.hasLocalLinkage() && !Callback.isDeclaration());
}

bool canPreserveSEHCallbackAcrossOptimization(const llvm::Function &Callback) {
  if (Callback.hasFnAttribute(llvm::Attribute::AlwaysInline) ||
      Callback.hasFnAttribute(llvm::Attribute::NoReturn) ||
      Callback.hasFnAttribute(llvm::Attribute::ReturnsTwice) ||
      Callback.hasFnAttribute(llvm::Attribute::Speculatable))
    return false;
  if (Callback.doesNotThrow() && Callback.onlyReadsMemory() &&
      (Callback.willReturn() ||
       Callback.hasFnAttribute(llvm::Attribute::MustProgress)))
    return false;
  if (Callback.isDeclaration())
    return true;
  return !Callback.hasFnAttribute(llvm::Attribute::OptimizeForSize) &&
         !Callback.hasFnAttribute(llvm::Attribute::MinSize) &&
         !Callback.hasFnAttribute(llvm::Attribute::OptimizeForDebugging);
}

} // namespace

bool MedLLVMEmitter::emitNativeSEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (!Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(EH, TargetArch, TargetFormat,
                                    WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::SEH)
    return false;

  if (!med_llvm_eh::collectExactSourceCallAddresses(LLVMFunc, CallSiteAddrs))
    return false;
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, "__C_specific_handler", PersonalityTy))
    return false;
  const med_llvm_eh::I32ModuleFlagState AsyncFlagState =
      med_llvm_eh::classifyI32ModuleFlag(*Mod, "eh-asynch",
                                         llvm::Module::Warning, 1);
  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Conflict)
    return false;

  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *FilterCallbackTy =
      llvm::FunctionType::get(I32Ty, {PtrTy, PtrTy}, false);
  auto *FinallyCallbackTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*Ctx), {llvm::Type::getInt8Ty(*Ctx), PtrTy}, false);

  struct Region {
    const SEHScopeRecord *Scope = nullptr;
    ExceptionAddressRange SemanticRange;
    llvm::BasicBlock *Handler = nullptr;
    llvm::Function *Callback = nullptr;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
    std::set<llvm::BasicBlock *> Blocks;
    uint32_t SourceIndex = 0;
    llvm::mc_rewrite::RewriteWinEHSemanticToken SemanticToken;
  };

  auto FunctionAt = [&](va_t Address) -> llvm::Function * {
    return resolveLiftedFunctionEntry(Address);
  };
  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end() || !It->second ||
          It->second->getParent() != &LLVMFunc)
        return nullptr;
      return It->second;
    }
    return nullptr;
  };

  std::vector<Region> Regions;
  Regions.reserve(EH.SEH->Scopes.size());
  if (EH.SEH->Scopes.size() > std::numeric_limits<uint32_t>::max())
    return false;
  for (size_t ScopeIndex = 0; ScopeIndex < EH.SEH->Scopes.size();
       ++ScopeIndex) {
    const SEHScopeRecord &Scope = EH.SEH->Scopes[ScopeIndex];
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH.CodeRange);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete || !SemanticRange)
      return false;

    Region R;
    R.Scope = &Scope;
    R.SemanticRange = *SemanticRange;
    R.SourceIndex = static_cast<uint32_t>(ScopeIndex);
    const auto SemanticToken = windows_eh_semantics::getSEHScopeSemanticToken(
        EH, TargetArch, R.SourceIndex);
    if (!SemanticToken)
      return false;
    R.SemanticToken = *SemanticToken;
    if (Scope.Kind == SEHScopeKind::Finally) {
      R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
      if (!R.Callback || R.Callback == &LLVMFunc ||
          !hasSupportedSEHCallbackABI(*R.Callback, FinallyCallbackTy,
                                      TargetArch) ||
          !canPreserveSEHCallbackAcrossOptimization(*R.Callback))
        return false;
    } else {
      R.Handler = BlockAt(Scope.HandlerVA);
      if (!R.Handler || R.SemanticRange.contains(Scope.HandlerVA))
        return false;
      if (Scope.Kind == SEHScopeKind::Filter) {
        R.Callback = FunctionAt(Scope.FilterOrFinallyVA);
        if (!R.Callback || R.Callback == &LLVMFunc ||
            !hasSupportedSEHCallbackABI(*R.Callback, FilterCallbackTy,
                                        TargetArch) ||
            !canPreserveSEHCallbackAcrossOptimization(*R.Callback))
          return false;
      }
    }

    bool HasBegin = false;
    bool HasEnd = false;
    for (const MedBlock &Block : Func.Blocks) {
      ExceptionAddressRange BlockRange{Block.StartAddr, Block.EndAddr};
      if (!BlockRange.isValid())
        continue;
      if (R.SemanticRange.overlaps(BlockRange) &&
          !R.SemanticRange.contains(BlockRange))
        return false;
      if (!R.SemanticRange.contains(BlockRange))
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end() || !It->second ||
          It->second->getParent() != &LLVMFunc)
        return false;
      R.Blocks.insert(It->second);
      HasBegin |= Block.StartAddr == R.SemanticRange.Begin;
      HasEnd |= Block.EndAddr == R.SemanticRange.End;
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
      const ExceptionAddressRange &A = Regions[I].SemanticRange;
      const ExceptionAddressRange &B = Regions[J].SemanticRange;
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return false;
    }
  }
  std::stable_sort(
      Regions.begin(), Regions.end(), [](const Region &A, const Region &B) {
        if (A.SemanticRange.size() != B.SemanticRange.size())
          return A.SemanticRange.size() > B.SemanticRange.size();
        return A.SemanticRange.Begin < B.SemanticRange.Begin;
      });
  for (size_t I = 0; I < Regions.size(); ++I) {
    uint64_t ParentSize = std::numeric_limits<uint64_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!Regions[J].SemanticRange.contains(Regions[I].SemanticRange))
        continue;
      if (Regions[J].SemanticRange.size() < ParentSize) {
        Regions[I].Parent = static_cast<int>(J);
        ParentSize = Regions[J].SemanticRange.size();
      }
    }
  }

  // Complete every check that can reject the lowering before creating the
  // personality, declarations, funclets, or CFG edges. The commit phase below
  // consumes only this immutable plan and has no failure exits.
  auto IsMayUnwindCall = [](const llvm::CallInst &Call) {
    return !Call.doesNotThrow() && !Call.isMustTailCall() &&
           !llvm::isa<llvm::IntrinsicInst>(Call);
  };
  struct CallPlan {
    llvm::CallInst *Call = nullptr;
    size_t RegionIndex = 0;
    va_t SourceAddress = 0;
  };

  std::set<const llvm::CallInst *> ProtectedCalls;
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks) {
      if (!Block || Block->getParent() != &LLVMFunc)
        return false;
      for (const llvm::Instruction &Inst : *Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
          ProtectedCalls.insert(Call);
    }

  llvm::SmallVector<CallPlan, 16> CallPlans;
  std::set<const llvm::CallInst *> PlannedCalls;
  std::set<va_t> PlannedAddresses;
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::BasicBlock *InitialBB = BBIt->second;
    if (!InitialBB || InitialBB->getParent() != &LLVMFunc)
      return false;

    int Innermost = -1;
    uint64_t InnermostSize = std::numeric_limits<uint64_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].SemanticRange.size() >= InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].SemanticRange.size();
    }
    if (Innermost < 0)
      continue;

    for (llvm::Instruction &Inst : *InitialBB) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!Call || !IsMayUnwindCall(*Call))
        continue;
      auto AddressIt = CallSiteAddrs.find(Call);
      if (Call->getParent() != InitialBB || !Call->getNextNode() ||
          AddressIt == CallSiteAddrs.end() ||
          !EH.CodeRange.contains(AddressIt->second) ||
          !PlannedCalls.insert(Call).second ||
          !PlannedAddresses.insert(AddressIt->second).second)
        return false;
      CallPlans.push_back(
          {Call, static_cast<size_t>(Innermost), AddressIt->second});
    }
  }
  if (PlannedCalls != ProtectedCalls)
    return false;
  const uint64_t ProtectedCallCount = static_cast<uint64_t>(CallPlans.size());

  // The asynchronous marker pass walks every protected terminator and its
  // successors. Unterminated emitter intermediates are rejected atomically.
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks)
      if (!Block->getTerminator())
        return false;

  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction("__C_specific_handler", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  // Filter/finally callbacks are address-backed ABI objects, not ordinary
  // implementation details of the lifted parent.  Native reconstruction
  // authenticates the exact direct callee against the source scope table, so
  // allowing an optimizer to inline, summarize, or erase the body/call would
  // destroy information the output gate must retain.  optnone prevents a
  // trivial definition from acquiring a no-write/nounwind/willreturn summary
  // that makes an outer finally call dead; eh-asynch separately preserves the
  // nested invoke edge.  Add these only after the atomic preflight has
  // completed; the commit path below has no failure exits.
  for (const Region &R : Regions) {
    if (!R.Callback)
      continue;
    if (!R.Callback->hasFnAttribute(llvm::Attribute::NoInline))
      R.Callback->addFnAttr(llvm::Attribute::NoInline);
    if (!R.Callback->isDeclaration())
      R.Callback->addFnAttr(llvm::Attribute::OptimizeNone);
  }

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
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
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.SemanticToken))
        llvm_unreachable("prevalidated SEH semantic token was rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RegionDispatch,
          EH.CodeRange.Begin, /*SourceVA=*/0, R.SourceIndex, /*Clause=*/0, Pad,
          R.Scope->FilterOrFinallyVA, static_cast<uint32_t>(R.Scope->Kind));
      llvm::Function *LocalAddress = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress);
      llvm::Value *Frame = B.CreateCall(LocalAddress);
      llvm::SmallVector<llvm::Value *, 2> Args{
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), 1), Frame};
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Funclet("funclet", BundleInputs);
      if (ParentDest) {
        auto *ContinueBB = llvm::BasicBlock::Create(
            *Ctx, "seh.finally.continue." + Suffix, &LLVMFunc);
        B.CreateInvoke(FinallyCallbackTy, R.Callback, ContinueBB, ParentDest,
                       Args, {Funclet});
        llvm::IRBuilder<> ContinueBuilder(ContinueBB);
        ContinueBuilder.CreateCleanupRet(Pad, ParentDest);
      } else {
        B.CreateCall(FinallyCallbackTy, R.Callback, Args, {Funclet});
        B.CreateCleanupRet(Pad, nullptr);
      }
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
    if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.SemanticToken))
      llvm_unreachable("prevalidated SEH semantic token was rejected");
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        PB, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::RegionDispatch, EH.CodeRange.Begin,
        R.Scope->HandlerVA, R.SourceIndex, /*Clause=*/0, Pad,
        R.Scope->Kind == SEHScopeKind::Filter ? R.Scope->FilterOrFinallyVA : 0,
        static_cast<uint32_t>(R.Scope->Kind));
    PB.CreateCatchRet(Pad, R.Handler);
    llvm::IRBuilder<> HandlerBuilder(&*R.Handler->getFirstInsertionPt());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        HandlerBuilder, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::HandlerTarget, EH.CodeRange.Begin,
        R.Scope->HandlerVA, R.SourceIndex, /*Clause=*/0);
    R.UnwindDest = Dispatch;
  }

  // Replace each planned call with an invoke to its innermost active region.
  // Non-call hardware faults are covered by the asynchronous markers below.
  for (const CallPlan &Plan : CallPlans) {
    llvm::CallInst *Call = Plan.Call;
    llvm::BasicBlock *CallBB = Call->getParent();
    llvm::Instruction *Next = Call->getNextNode();
    assert(CallBB && Next && "preflighted SEH call changed before commit");
    llvm::BasicBlock *Cont =
        CallBB->splitBasicBlock(Next, CallBB->getName() + ".seh.cont");
    auto *OldBranch = CallBB->getTerminator();
    assert(OldBranch && "split block must have a branch");
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Use &Arg : Call->args())
      Args.push_back(Arg.get());
    llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
    Call->getOperandBundlesAsDefs(Bundles);
    auto *Invoke = llvm::InvokeInst::Create(
        Call->getFunctionType(), Call->getCalledOperand(), Cont,
        Regions[Plan.RegionIndex].UnwindDest, Args, Bundles, Call->getName(),
        OldBranch->getIterator());
    llvm::IRBuilder<> ProvenanceBuilder(Invoke);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        ProvenanceBuilder, windows_eh_md::NativeProvenanceModel::SEH,
        windows_eh_md::NativeProvenanceRole::ProtectedInvoke,
        EH.CodeRange.Begin, Plan.SourceAddress,
        Regions[Plan.RegionIndex].SourceIndex, /*Clause=*/0);
    Invoke->setCallingConv(Call->getCallingConv());
    Invoke->setAttributes(Call->getAttributes());
    Invoke->setDebugLoc(Call->getDebugLoc());
    Invoke->copyMetadata(*Call);
    Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
    OldBranch->eraseFromParent();
    for (Region &R : Regions)
      if (R.Blocks.count(CallBB))
        R.Blocks.insert(Cont);
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
  struct RangeTargetPlan {
    llvm::InvokeInst *Marker = nullptr;
    windows_eh_md::NativeProvenanceRole Role =
        windows_eh_md::NativeProvenanceRole::RangeEnterTarget;
    va_t SourceVA = 0;
    uint32_t Region = 0;
    uint32_t Boundary = 0;
  };
  llvm::SmallVector<RangeTargetPlan, 16> RangeTargets;
  auto PlanRangeTarget =
      [&](llvm::InvokeInst *Marker, windows_eh_md::NativeProvenanceRole Role,
          va_t SourceVA, uint32_t Region, uint32_t Boundary) {
        RangeTargets.push_back({Marker, Role, SourceVA, Region, Boundary});
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
      const uint32_t Boundary = Marker++;
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Boundary),
          &LLVMFunc, Edge.Target);
      Edge.Pred->getTerminator()->setSuccessor(Edge.SuccessorIndex, BeginBB);
      llvm::IRBuilder<> B(BeginBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeEnter, EH.CodeRange.Begin,
          R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryBegin, Edge.Target, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeEnterTarget,
                      R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(BeginBB);
    }
    if (EntryWithoutPred) {
      const uint32_t Boundary = Marker++;
      auto *OldEntry = &LLVMFunc.getEntryBlock();
      auto *BeginBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.begin." + std::to_string(RegionIndex) + "." +
              std::to_string(Boundary),
          &LLVMFunc, OldEntry);
      llvm::IRBuilder<> B(BeginBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeEnter, EH.CodeRange.Begin,
          R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryBegin, OldEntry, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeEnterTarget,
                      R.Scope->GuardedRange.Begin, R.SourceIndex, Boundary);
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
      const uint32_t Boundary = Marker++;
      auto *EndBB = llvm::BasicBlock::Create(*Ctx,
                                             "seh.try.end." +
                                                 std::to_string(RegionIndex) +
                                                 "." + std::to_string(Boundary),
                                             &LLVMFunc, Edge.Target);
      Edge.Term->setSuccessor(Edge.SuccessorIndex, EndBB);
      llvm::IRBuilder<> B(EndBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeExit, EH.CodeRange.Begin,
          R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      auto *Marker = B.CreateInvoke(TryEnd, Edge.Target, R.UnwindDest);
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeExitTarget,
                      R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
    for (llvm::ReturnInst *Ret : Returns) {
      const uint32_t Boundary = Marker++;
      llvm::BasicBlock *Source = Ret->getParent();
      auto *EndBB = llvm::BasicBlock::Create(
          *Ctx,
          "seh.try.end." + std::to_string(RegionIndex) + ".ret." +
              std::to_string(Boundary),
          &LLVMFunc);
      auto *RetBB = llvm::BasicBlock::Create(
          *Ctx, "seh.try.ret." + std::to_string(RegionIndex), &LLVMFunc);
      llvm::Value *ReturnValue = Ret->getReturnValue();
      Ret->eraseFromParent();
      llvm::IRBuilder<> SourceBuilder(Source);
      SourceBuilder.CreateBr(EndBB);
      llvm::IRBuilder<> EndBuilder(EndBB);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          EndBuilder, windows_eh_md::NativeProvenanceModel::SEH,
          windows_eh_md::NativeProvenanceRole::RangeExit, EH.CodeRange.Begin,
          R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      auto *Marker = EndBuilder.CreateInvoke(TryEnd, RetBB, R.UnwindDest);
      llvm::IRBuilder<> RetBuilder(RetBB);
      if (ReturnValue)
        RetBuilder.CreateRet(ReturnValue);
      else
        RetBuilder.CreateRetVoid();
      PlanRangeTarget(Marker,
                      windows_eh_md::NativeProvenanceRole::RangeExitTarget,
                      R.Scope->GuardedRange.End, R.SourceIndex, Boundary);
      for (int Parent = R.Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        Regions[static_cast<size_t>(Parent)].Blocks.insert(EndBB);
    }
  }

  // Inner scopes may retarget an outer marker's normal edge while their
  // boundary blocks are inserted. Bind target provenance only after the full
  // nesting graph is stable so every anchor names the marker's final normal
  // destination rather than an intermediate source block.
  for (const RangeTargetPlan &Target : RangeTargets) {
    assert(Target.Marker && Target.Marker->getNormalDest() &&
           "committed SEH range marker must have a normal destination");
    llvm::BasicBlock *Destination = Target.Marker->getNormalDest();
    llvm::IRBuilder<> TargetBuilder(&*Destination->getFirstInsertionPt());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        TargetBuilder, windows_eh_md::NativeProvenanceModel::SEH, Target.Role,
        EH.CodeRange.Begin, Target.SourceVA, Target.Region, Target.Boundary);
  }

  if (AsyncFlagState == med_llvm_eh::I32ModuleFlagState::Absent)
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  const llvm::StringRef NativeKind =
      TargetArch == Arch::X64
          ? "seh-x64-native"
          : (TargetArch == Arch::ARM ? "seh-arm32-native"
                                     : "seh-aarch64-native");
  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {mdUInt(*Ctx, 1, 1), llvm::MDString::get(*Ctx, NativeKind)}));
  exception_rewrite::setContract(
      LLVMFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, ProtectedCallCount,
      ProtectedCallCount, /*SkippedPads=*/0);
  return true;
}

} // namespace neverd
