//===- MedLLVMNativeCxxEH.cpp - Native MSVC C++ EH lowering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native LLVM lowering for the supported MSVC C++ exception subset.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/Cloning.h"

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

bool MedLLVMEmitter::emitNativeCxxEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (!Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const WindowsEHNativeSourceClassification Source =
      classifyWindowsEHNativeSource(EH, TargetArch, TargetFormat,
                                    WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      (Source.Model != WindowsEHNativeSourceModel::CxxFH3 &&
       Source.Model != WindowsEHNativeSourceModel::CxxFH4))
    return false;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  const bool IsFH4 = Source.Model == WindowsEHNativeSourceModel::CxxFH4;
  const bool IsGSWrapped =
      EH.Personality == ExceptionPersonality::GSHandlerCheckEH4;
  const llvm::StringRef PersonalityName =
      IsFH4 ? "__CxxFrameHandler4" : "__CxxFrameHandler3";
  constexpr llvm::StringLiteral GSWrapperName("__GSHandlerCheck_EH4");
  const windows_eh_md::NativeProvenanceModel ProvenanceModel =
      IsFH4 ? windows_eh_md::NativeProvenanceModel::CxxFH4
            : windows_eh_md::NativeProvenanceModel::CxxFH3;

  if (!med_llvm_eh::collectExactSourceCallAddresses(LLVMFunc, CallSiteAddrs))
    return false;
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, PersonalityName, PersonalityTy))
    return false;
  if (IsGSWrapped && !med_llvm_eh::canMaterializeExternalFunctionDeclaration(
                         *Mod, GSWrapperName, PersonalityTy))
    return false;

  // This native closure is intentionally exact and narrow.  Catch-object frame
  // homes, noexcept, and asynchronous /EHa stay metadata-only.  A standalone
  // out-of-line catch calls that funclet with the establisher frame.  One
  // in-function continuation is that call's block.  A continuation strictly
  // inside one block splits that block at the call whose address is the
  // continuation.  An earlier unaddressed instruction stays in the prefix.
  // An unaddressed instruction before a later call stays out.  An empty continuation
  // list on a void function gets a synthesized block that calls the funclet
  // and returns.  Two or more in-function continuations switch on the
  // funclet's pointer result; an address outside that list is unreachable.
  // A chain of Direct funclets and
  // DestructorWithObject actions is innermost call first.  A Direct action is
  // passed the establisher frame.  A DestructorWithObject action is passed the
  // synthetic frame plus the table offset.  Actions inside the try run before
  // the catch; actions below every try run only when the exception propagates.
  // A nested try keeps one cleanup per try, so an inner catchswitch unwinds
  // through the outer try's funclets before the outer catch.  Along the chain,
  // each next action stays in the same try or moves to an outer one.  The
  // highest action outside a try while a lower one is inside stays out.
  // Typed catches without a catch object are representable
  // because the RTTI address remains an external absolute data symbol in the
  // original image.
  //
  // A dynamic exception specification is excluded for a different reason: it
  // is not dispatch at all.  Escaping a `throw(A)` calls `unexpected` rather
  // than selecting a handler, and nothing in the LLVM WinEH model spells that,
  // so regenerating from this IR would silently drop the contract.  Only a
  // record whose magic declares `EHFlags` can be trusted about /EHs either, and
  // an older one leaves `IsSynchronous` unset, which the check above rejects.
  if (!Cxx.hasValidStateGraph() || Cxx.TryBlocks.empty() || Cxx.IPMap.empty() ||
      Cxx.IsCatchFunclet || Cxx.IsSeparated || !Cxx.IsSynchronous ||
      Cxx.IsNoExcept || Cxx.hasExceptionSpecification() ||
      (!IsFH4 && (Cxx.Flags & ~uint32_t(1)) != 0))
    return false;
  const std::vector<const CxxUnwindAction *> Chain =
      cxxLowerableDestructorChain(EH);
  const CxxUnwindAction *Dtor = Chain.empty() ? nullptr : Chain.front();
  if (!Dtor) {
    for (const CxxUnwindAction &Action : Cxx.UnwindMap)
      if (Action.ActionVA != 0 ||
          Action.Kind != CxxUnwindAction::ActionKind::None)
        return false;
  }
  struct UnwindCall {
    std::string Name;
    int32_t Offset = 0;
    int32_t State = -1;
    int Region = -1;
    bool Frame = false;
    llvm::BasicBlock *LocalBody = nullptr;
  };
  std::vector<UnwindCall> UnwindCalls;
  llvm::FunctionType *DtorTy = nullptr;
  if (Dtor) {
    DtorTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {PtrTy},
                                     false);
    auto LocalBodyAt = [&](va_t Address) -> llvm::BasicBlock * {
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
    auto BodyUsesOnlyItsOwnInstructions = [](const llvm::BasicBlock &BB) {
      for (const llvm::Instruction &I : BB) {
        if (I.isTerminator() || llvm::isa<llvm::PHINode>(&I))
          continue;
        for (const llvm::Value *Op : I.operands()) {
          if (llvm::isa<llvm::Constant>(Op) || llvm::isa<llvm::Argument>(Op))
            continue;
          const auto *Def = llvm::dyn_cast<llvm::Instruction>(Op);
          if (!Def || Def->getParent() != &BB)
            return false;
        }
      }
      return true;
    };
    auto Push = [&](const CxxUnwindAction *Action, bool Frame) {
      UnwindCall Call;
      Call.Offset = Action->ObjectOffset;
      Call.State = static_cast<int32_t>(Action - Cxx.UnwindMap.data());
      Call.Frame = Frame;
      if (EH.CodeRange.contains(Action->ActionVA)) {
        if (!Frame)
          return false;
        llvm::BasicBlock *Body = LocalBodyAt(Action->ActionVA);
        if (!Body || Body == &LLVMFunc.getEntryBlock() ||
            !BodyUsesOnlyItsOwnInstructions(*Body))
          return false;
        Call.LocalBody = Body;
        UnwindCalls.push_back(std::move(Call));
        return true;
      }
      auto NameIt = FuncNames.find(Action->ActionVA);
      Call.Name = NameIt != FuncNames.end() && !NameIt->second.empty()
                      ? NameIt->second
                      : "sub_" + llvm::utohexstr(Action->ActionVA);
      if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
              *Mod, Call.Name, DtorTy))
        return false;
      UnwindCalls.push_back(std::move(Call));
      return true;
    };
    for (const CxxUnwindAction *Action : Chain) {
      const bool Frame = Action->Kind == CxxUnwindAction::ActionKind::Direct;
      if (!Push(Action, Frame))
        return false;
    }
  }

  struct Handler {
    const CxxCatchHandler *Catch = nullptr;
    llvm::BasicBlock *Target = nullptr;
    uint32_t SourceIndex = 0;
    std::string FuncletName;
    llvm::mc_rewrite::RewriteWinEHSemanticToken SemanticToken;
    bool SyntheticContinuation = false;
    std::vector<llvm::BasicBlock *> Continuations;
    std::vector<va_t> ContinuationVAs;
    llvm::BasicBlock *InteriorOwner = nullptr;
    llvm::Instruction *InteriorAt = nullptr;
  };
  struct Region {
    const CxxTryBlock *Try = nullptr;
    std::set<llvm::BasicBlock *> Blocks;
    std::vector<Handler> Handlers;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
    uint32_t SourceIndex = 0;
  };

  auto StateAt = [&](va_t Address) {
    int32_t State = -1;
    for (const CxxIPState &Entry : Cxx.IPMap) {
      if (Entry.IP > Address)
        break;
      State = Entry.State;
    }
    return State;
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
  // A continuation strictly inside one block is that block's suffix. The
  // split is the first call whose source address is the continuation, so
  // every earlier instruction is before that address. An unaddressed
  // instruction before a later call, two owning blocks, or the entry block
  // stays out.
  auto PlanInteriorSplit =
      [&](va_t ContVA,
          const std::set<llvm::BasicBlock *> &Protected)
      -> std::pair<llvm::BasicBlock *, llvm::Instruction *> {
    const MedBlock *Owner = nullptr;
    for (const MedBlock &Block : Func.Blocks) {
      if (ContVA <= Block.StartAddr || ContVA >= Block.EndAddr)
        continue;
      if (Owner)
        return {nullptr, nullptr};
      Owner = &Block;
    }
    if (!Owner)
      return {nullptr, nullptr};
    auto It = OriginalBlockMap.find(Owner->Id);
    if (It == OriginalBlockMap.end() || !It->second ||
        It->second->getParent() != &LLVMFunc ||
        It->second == &LLVMFunc.getEntryBlock() ||
        Protected.count(It->second))
      return {nullptr, nullptr};
    llvm::BasicBlock *BB = It->second;
    llvm::Instruction *SplitAt = nullptr;
    bool UnanchoredBeforeSplit = false;
    for (llvm::Instruction &Inst : *BB) {
      if (llvm::isa<llvm::PHINode>(Inst))
        continue;
      if (Inst.isTerminator())
        break;
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
      auto AddrIt = Call ? CallSiteAddrs.find(Call) : CallSiteAddrs.end();
      if (!Call || AddrIt == CallSiteAddrs.end()) {
        if (!SplitAt)
          UnanchoredBeforeSplit = true;
        continue;
      }
      if (SplitAt || AddrIt->second < ContVA)
        continue;
      if (UnanchoredBeforeSplit && AddrIt->second != ContVA)
        return {nullptr, nullptr};
      SplitAt = &Inst;
    }
    if (!SplitAt)
      return {nullptr, nullptr};
    return {BB, SplitAt};
  };
  auto IsMayUnwindCall = [](const llvm::CallInst &Call) {
    return !Call.doesNotThrow() && !Call.isMustTailCall() &&
           !llvm::isa<llvm::IntrinsicInst>(Call);
  };

  // Every IP-state boundary must already be a machine-block boundary.  This
  // avoids assigning one generated call site to two native states.
  for (const MedBlock &Block : Func.Blocks) {
    if (Block.StartAddr >= Block.EndAddr)
      return false;
    for (const CxxIPState &Entry : Cxx.IPMap)
      if (Entry.IP > Block.StartAddr && Entry.IP < Block.EndAddr)
        return false;
  }

  std::vector<Region> Regions;
  Regions.reserve(Cxx.TryBlocks.size());
  if (Cxx.TryBlocks.size() > std::numeric_limits<uint32_t>::max())
    return false;
  std::set<va_t> InParentFunclets;
  for (const CxxUnwindAction *Action : Chain)
    if (Action && EH.CodeRange.contains(Action->ActionVA))
      InParentFunclets.insert(Action->ActionVA);
  for (size_t TryIndex = 0; TryIndex < Cxx.TryBlocks.size(); ++TryIndex) {
    const CxxTryBlock &Try = Cxx.TryBlocks[TryIndex];
    if (Try.Handlers.empty() ||
        Try.Handlers.size() > std::numeric_limits<uint32_t>::max())
      return false;
    Region R;
    R.Try = &Try;
    R.SourceIndex = static_cast<uint32_t>(TryIndex);
    for (const MedBlock &Block : Func.Blocks) {
      if (InParentFunclets.count(Block.StartAddr))
        continue;
      int32_t State = StateAt(Block.StartAddr);
      if (State < Try.TryLow || State > Try.TryHigh)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      if (It == OriginalBlockMap.end() || !It->second ||
          It->second->getParent() != &LLVMFunc)
        return false;
      R.Blocks.insert(It->second);
    }
    if (R.Blocks.empty())
      return false;
    for (size_t HandlerIndex = 0; HandlerIndex < Try.Handlers.size();
         ++HandlerIndex) {
      const CxxCatchHandler &Catch = Try.Handlers[HandlerIndex];
      if (Catch.CatchObjectOffset != 0 || Catch.ParentFrameOffset != 0 ||
          Catch.HandlerVA == 0)
        return false;
      va_t TargetVA = Catch.HandlerVA;
      std::string FuncletName;
      bool SyntheticContinuation = false;
      std::vector<llvm::BasicBlock *> ContBlocks;
      std::vector<va_t> ContVAs;
      if (!EH.CodeRange.contains(Catch.HandlerVA)) {
        auto NameIt = FuncNames.find(Catch.HandlerVA);
        FuncletName = NameIt != FuncNames.end() && !NameIt->second.empty()
                          ? NameIt->second
                          : "sub_" + llvm::utohexstr(Catch.HandlerVA);
        const bool SelectsContinuation = Catch.ContinuationVAs.size() > 1;
        auto *FuncletTy = llvm::FunctionType::get(
            SelectsContinuation ? PtrTy : llvm::Type::getVoidTy(*Ctx), {PtrTy},
            false);
        if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
                *Mod, FuncletName, FuncletTy))
          return false;
        if (Catch.ContinuationVAs.empty()) {
          // A synthesized continuation has nowhere to put a returned value.
          if (!LLVMFunc.getReturnType()->isVoidTy())
            return false;
          SyntheticContinuation = true;
        } else if (!SelectsContinuation) {
          TargetVA = Catch.ContinuationVAs.front();
          if (TargetVA == 0 || TargetVA == EH.CodeRange.Begin ||
              !EH.CodeRange.contains(TargetVA))
            return false;
        } else {
          for (va_t ContVA : Catch.ContinuationVAs) {
            if (ContVA == 0 || ContVA == EH.CodeRange.Begin ||
                !EH.CodeRange.contains(ContVA))
              return false;
            for (va_t Prev : ContVAs)
              if (Prev == ContVA)
                return false;
            llvm::BasicBlock *Cont = BlockAt(ContVA);
            if (!Cont || Cont == &LLVMFunc.getEntryBlock() ||
                R.Blocks.count(Cont) || !llvm::pred_empty(Cont))
              return false;
            for (llvm::BasicBlock *Prev : ContBlocks)
              if (Prev == Cont)
                return false;
            ContBlocks.push_back(Cont);
            ContVAs.push_back(ContVA);
          }
        }
      }
      llvm::BasicBlock *Target = nullptr;
      llvm::BasicBlock *InteriorOwner = nullptr;
      llvm::Instruction *InteriorAt = nullptr;
      if (!SyntheticContinuation && ContBlocks.empty()) {
        Target = BlockAt(TargetVA);
        if (!Target && !EH.CodeRange.contains(Catch.HandlerVA) &&
            Catch.ContinuationVAs.size() == 1) {
          std::tie(InteriorOwner, InteriorAt) =
              PlanInteriorSplit(TargetVA, R.Blocks);
        }
        if (InteriorAt) {
          if (!InteriorOwner || R.Blocks.count(InteriorOwner))
            return false;
        } else if (!Target || Target == &LLVMFunc.getEntryBlock() ||
                   R.Blocks.count(Target) || !llvm::pred_empty(Target)) {
          return false;
        }
      }
      const uint32_t SourceHandlerIndex = static_cast<uint32_t>(HandlerIndex);
      const auto SemanticToken = windows_eh_semantics::getCxxCatchSemanticToken(
          EH, TargetArch, R.SourceIndex, SourceHandlerIndex);
      if (!SemanticToken)
        return false;
      R.Handlers.push_back({&Catch, Target, SourceHandlerIndex,
                            std::move(FuncletName), *SemanticToken,
                            SyntheticContinuation, std::move(ContBlocks),
                            std::move(ContVAs), InteriorOwner, InteriorAt});
    }
    Regions.push_back(std::move(R));
  }

  auto IsSubset = [](const std::set<llvm::BasicBlock *> &A,
                     const std::set<llvm::BasicBlock *> &B) {
    return std::includes(B.begin(), B.end(), A.begin(), A.end());
  };
  auto Overlaps = [](const std::set<llvm::BasicBlock *> &A,
                     const std::set<llvm::BasicBlock *> &B) {
    for (llvm::BasicBlock *Block : A)
      if (B.count(Block))
        return true;
    return false;
  };
  for (size_t I = 0; I < Regions.size(); ++I) {
    for (size_t J = I + 1; J < Regions.size(); ++J) {
      if (!Overlaps(Regions[I].Blocks, Regions[J].Blocks))
        continue;
      if (Regions[I].Blocks == Regions[J].Blocks ||
          (!IsSubset(Regions[I].Blocks, Regions[J].Blocks) &&
           !IsSubset(Regions[J].Blocks, Regions[I].Blocks)))
        return false;
    }
  }
  std::stable_sort(Regions.begin(), Regions.end(),
                   [](const Region &A, const Region &B) {
                     return A.Blocks.size() > B.Blocks.size();
                   });
  for (size_t I = 0; I < Regions.size(); ++I) {
    size_t ParentSize = std::numeric_limits<size_t>::max();
    for (size_t J = 0; J < I; ++J) {
      if (!IsSubset(Regions[I].Blocks, Regions[J].Blocks) ||
          Regions[J].Blocks.size() >= ParentSize)
        continue;
      Regions[I].Parent = static_cast<int>(J);
      ParentSize = Regions[J].Blocks.size();
    }
  }

  std::vector<std::vector<UnwindCall>> LocalCalls(Regions.size());
  std::vector<UnwindCall> RootCalls;
  if (Dtor) {
    auto SameOrOuter = [&](int Cur, int Prev) {
      if (Cur == Prev || Cur < 0)
        return true;
      if (Prev < 0)
        return false;
      for (int Parent = Regions[static_cast<size_t>(Prev)].Parent; Parent >= 0;
           Parent = Regions[static_cast<size_t>(Parent)].Parent)
        if (Parent == Cur)
          return true;
      return false;
    };
    for (UnwindCall &Call : UnwindCalls) {
      size_t Best = std::numeric_limits<size_t>::max();
      int AtBest = 0;
      int Found = -1;
      for (size_t I = 0; I < Regions.size(); ++I) {
        const CxxTryBlock &Try = *Regions[I].Try;
        if (Call.State < Try.TryLow || Call.State > Try.TryHigh)
          continue;
        if (Regions[I].Blocks.size() < Best) {
          Best = Regions[I].Blocks.size();
          Found = static_cast<int>(I);
          AtBest = 1;
        } else if (Regions[I].Blocks.size() == Best) {
          ++AtBest;
        }
      }
      if (AtBest > 1)
        return false;
      Call.Region = Found;
    }
    if (!UnwindCalls.empty() && UnwindCalls.front().Region < 0) {
      int RootRegions = 0;
      for (const Region &R : Regions)
        if (R.Parent < 0)
          ++RootRegions;
      if (RootRegions != 1)
        return false;
    }
    int PrevRegion = UnwindCalls.empty() ? -1 : UnwindCalls.front().Region;
    for (size_t I = 1; I < UnwindCalls.size(); ++I) {
      if (!SameOrOuter(UnwindCalls[I].Region, PrevRegion))
        return false;
      PrevRegion = UnwindCalls[I].Region;
    }
    for (const UnwindCall &Call : UnwindCalls) {
      if (Call.LocalBody)
        for (const Region &R : Regions)
          if (R.Blocks.count(Call.LocalBody))
            return false;
      if (Call.Region < 0)
        RootCalls.push_back(Call);
      else
        LocalCalls[static_cast<size_t>(Call.Region)].push_back(Call);
    }
  }

  // Catch continuations execute after catchret, so they stay outside every
  // protected region.  A call there is the in-function catch funclet body and
  // remains an ordinary call.
  for (const Region &R : Regions)
    for (const Handler &H : R.Handlers) {
      auto InProtected = [&](llvm::BasicBlock *Block) {
        if (!Block)
          return false;
        for (const Region &Protected : Regions)
          if (Protected.Blocks.count(Block))
            return true;
        return false;
      };
      if (InProtected(H.Target) || InProtected(H.InteriorOwner))
        return false;
      for (llvm::BasicBlock *Cont : H.Continuations)
        if (InProtected(Cont))
          return false;
    }

  std::set<const llvm::CallInst *> MayUnwindCallSet;
  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks)
      for (const llvm::Instruction &Inst : *Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
          MayUnwindCallSet.insert(Call);
  size_t MayUnwindCalls = MayUnwindCallSet.size();
  if (MayUnwindCalls == 0)
    return false;

  // Finish the complete rejection plan before creating type globals,
  // personality declarations, funclets, or CFG edges. The commit phase below
  // has no failure exits and consumes only these preflighted call pointers.
  struct CallPlan {
    llvm::CallInst *Call = nullptr;
    size_t RegionIndex = 0;
    va_t SourceAddress = 0;
  };
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
    size_t InnermostSize = std::numeric_limits<size_t>::max();
    for (size_t I = 0; I < Regions.size(); ++I) {
      if (!Regions[I].Blocks.count(InitialBB) ||
          Regions[I].Blocks.size() >= InnermostSize)
        continue;
      Innermost = static_cast<int>(I);
      InnermostSize = Regions[I].Blocks.size();
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
  if (PlannedCalls != MayUnwindCallSet)
    return false;
  const uint64_t ProtectedCallCount = static_cast<uint64_t>(CallPlans.size());

  for (const Region &R : Regions)
    for (llvm::BasicBlock *Block : R.Blocks)
      if (!Block || Block->getParent() != &LLVMFunc || !Block->getTerminator())
        return false;

  std::set<va_t> TypeDescriptors;
  for (const Region &R : Regions)
    for (const Handler &H : R.Handlers)
      if (H.Catch->TypeDescriptorVA != 0)
        TypeDescriptors.insert(H.Catch->TypeDescriptorVA);
  for (va_t Address : TypeDescriptors)
    if (!med_llvm_eh::canMaterializeExternalDataDeclaration(
            *Mod, makeNdDataSymbol(Address), I8Ty, /*IsConstant=*/true))
      return false;

  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction(PersonalityName, PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));
  if (IsFH4) {
    LLVMFunc.addFnAttr(llvm::mc_rewrite::RewriteWinCxxFH4Attribute);
    if (IsGSWrapped) {
      // Keep LLVM's semantic personality as C++ EH. The authenticated writer
      // attribute makes code generation allocate and check a fresh cookie in
      // the final machine frame, then installs the runtime GS wrapper around
      // the newly emitted FH4 payload.
      Mod->getOrInsertFunction(GSWrapperName, PersonalityTy);
      LLVMFunc.addFnAttr(llvm::mc_rewrite::RewriteWinGSHandlerAttribute,
                         llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4);
      LLVMFunc.addFnAttr(llvm::Attribute::StackProtectReq);
    }
  }

  auto TypeDescriptor = [&](va_t Address) -> llvm::Constant * {
    if (Address == 0)
      return llvm::ConstantPointerNull::get(PtrTy);
    std::string Name = makeNdDataSymbol(Address);
    llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
    if (!GV)
      GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
    return GV;
  };

  auto *TokenNone = llvm::ConstantTokenNone::get(*Ctx);
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  llvm::Value *Base = FrameBaseInt;
  auto EnsureBase = [&]() {
    if (Base)
      return;
    llvm::BasicBlock &EntryBB = LLVMFunc.getEntryBlock();
    llvm::IRBuilder<> Entry(&EntryBB, EntryBB.getFirstNonPHIIt());
    auto *Slot = Entry.CreateAlloca(I8Ty, nullptr, "eh.object.base");
    Base = Entry.CreatePtrToInt(Slot, I64Ty, "eh.object.base.int");
  };
  auto EmitCalls = [&](llvm::IRBuilder<> &CB,
                       const std::vector<UnwindCall> &Calls) {
    EnsureBase();
    for (const UnwindCall &Call : Calls) {
      if (Call.LocalBody) {
        llvm::ValueToValueMapTy VMap;
        for (llvm::Instruction &I : *Call.LocalBody) {
          if (I.isTerminator() || llvm::isa<llvm::PHINode>(&I))
            continue;
          auto *Cloned = I.clone();
          VMap[&I] = Cloned;
          llvm::RemapInstruction(Cloned, VMap,
                                 llvm::RF_NoModuleLevelChanges |
                                     llvm::RF_IgnoreMissingLocals);
          if (auto *ClonedCall = llvm::dyn_cast<llvm::CallInst>(Cloned))
            ClonedCall->setDoesNotThrow();
          CB.Insert(Cloned);
        }
        continue;
      }
      llvm::Value *Obj = nullptr;
      if (Call.Frame) {
        Obj = CB.CreateIntToPtr(Base, PtrTy, "eh.frame");
      } else {
        llvm::Value *Addr = CB.CreateAdd(
            Base, llvm::ConstantInt::getSigned(I64Ty, Call.Offset),
            "eh.object.addr");
        Obj = CB.CreateIntToPtr(Addr, PtrTy, "eh.object");
      }
      llvm::CallInst *DtorCall =
          CB.CreateCall(Mod->getOrInsertFunction(Call.Name, DtorTy), {Obj});
      DtorCall->setDoesNotThrow();
    }
  };
  llvm::BasicBlock *RootCleanup = nullptr;
  if (!RootCalls.empty()) {
    RootCleanup = llvm::BasicBlock::Create(*Ctx, "cxx.unwind.cleanup.outer",
                                           &LLVMFunc);
    llvm::IRBuilder<> CB(RootCleanup);
    auto *Pad = CB.CreateCleanupPad(TokenNone);
    EmitCalls(CB, RootCalls);
    CB.CreateCleanupRet(Pad, nullptr);
  }
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : RootCleanup;
    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "cxx.catch.dispatch." + std::to_string(I), &LLVMFunc);
    llvm::IRBuilder<> DB(Dispatch);
    auto *Switch = DB.CreateCatchSwitch(TokenNone, ParentDest,
                                        R.Handlers.size(), "cxx.catch.switch");
    for (size_t J = 0; J < R.Handlers.size(); ++J) {
      const Handler &H = R.Handlers[J];
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "cxx.catch.pad." + std::to_string(I) + "." + std::to_string(J),
          &LLVMFunc);
      Switch->addHandler(PadBB);
      llvm::IRBuilder<> PB(PadBB);
      llvm::SmallVector<llvm::Value *, 3> Args{
          TypeDescriptor(H.Catch->TypeDescriptorVA),
          llvm::ConstantInt::get(I32Ty, H.Catch->Adjectives),
          llvm::ConstantPointerNull::get(PtrTy)};
      auto *Pad = PB.CreateCatchPad(Switch, Args, "cxx.catch.pad.token");
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, H.SemanticToken))
        llvm_unreachable("prevalidated C++ EH semantic token was rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          PB, ProvenanceModel,
          windows_eh_md::NativeProvenanceRole::RegionDispatch,
          EH.CodeRange.Begin, H.Catch->HandlerVA, R.SourceIndex, H.SourceIndex,
          Pad, H.Catch->TypeDescriptorVA, H.Catch->Adjectives);
      llvm::BasicBlock *Target = H.Target;
      if (H.InteriorAt)
        Target = H.InteriorOwner->splitBasicBlock(
            H.InteriorAt, "cxx.cont.split." + std::to_string(I) + "." +
                             std::to_string(J));
      if (H.SyntheticContinuation) {
        Target = llvm::BasicBlock::Create(
            *Ctx,
            "cxx.catch.cont." + std::to_string(I) + "." + std::to_string(J),
            &LLVMFunc);
        llvm::IRBuilder<>(Target).CreateRetVoid();
      }
      const bool SelectsContinuation = !H.Continuations.empty();
      if (SelectsContinuation) {
        Target = llvm::BasicBlock::Create(
            *Ctx,
            "cxx.catch.cont." + std::to_string(I) + "." + std::to_string(J),
            &LLVMFunc);
      }
      PB.CreateCatchRet(Pad, Target);
      if (SelectsContinuation) {
        llvm::IRBuilder<> HB(Target);
        EnsureBase();
        med_llvm_eh::emitWindowsEHProvenanceAnchor(
            HB, ProvenanceModel,
            windows_eh_md::NativeProvenanceRole::HandlerTarget,
            EH.CodeRange.Begin, H.Catch->HandlerVA, R.SourceIndex,
            H.SourceIndex);
        auto *FuncletTy = llvm::FunctionType::get(PtrTy, {PtrTy}, false);
        llvm::Value *Frame = HB.CreateIntToPtr(Base, PtrTy, "eh.frame");
        llvm::CallInst *FuncletCall = HB.CreateCall(
            Mod->getOrInsertFunction(H.FuncletName, FuncletTy), {Frame},
            "eh.cont");
        FuncletCall->setDoesNotThrow();
        llvm::Value *ContInt =
            HB.CreatePtrToInt(FuncletCall, I64Ty, "eh.cont.int");
        auto *Miss = llvm::BasicBlock::Create(
            *Ctx,
            "cxx.catch.cont.miss." + std::to_string(I) + "." +
                std::to_string(J),
            &LLVMFunc);
        llvm::IRBuilder<>(Miss).CreateUnreachable();
        auto *SW = HB.CreateSwitch(ContInt, Miss, H.Continuations.size());
        for (size_t K = 0; K < H.Continuations.size(); ++K)
          SW->addCase(llvm::ConstantInt::get(
                          I64Ty, static_cast<uint64_t>(H.ContinuationVAs[K])),
                      H.Continuations[K]);
        continue;
      }
      llvm::IRBuilder<> HandlerBuilder(&*Target->getFirstInsertionPt());
      if (!H.FuncletName.empty()) {
        EnsureBase();
        auto *FuncletTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx),
                                                  {PtrTy}, false);
        llvm::Value *Frame =
            HandlerBuilder.CreateIntToPtr(Base, PtrTy, "eh.frame");
        llvm::CallInst *FuncletCall = HandlerBuilder.CreateCall(
            Mod->getOrInsertFunction(H.FuncletName, FuncletTy), {Frame});
        FuncletCall->setDoesNotThrow();
      }
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          HandlerBuilder, ProvenanceModel,
          windows_eh_md::NativeProvenanceRole::HandlerTarget,
          EH.CodeRange.Begin, H.Catch->HandlerVA, R.SourceIndex, H.SourceIndex);
    }
    R.UnwindDest = Dispatch;
    if (!LocalCalls[I].empty()) {
      auto *CleanupBB = llvm::BasicBlock::Create(
          *Ctx, "cxx.unwind.cleanup." + std::to_string(I), &LLVMFunc);
      llvm::IRBuilder<> CB(CleanupBB);
      auto *Pad = CB.CreateCleanupPad(TokenNone);
      EmitCalls(CB, LocalCalls[I]);
      CB.CreateCleanupRet(Pad, Dispatch);
      R.UnwindDest = CleanupBB;
    }
  }

  for (const CallPlan &Plan : CallPlans) {
    llvm::CallInst *Call = Plan.Call;
    llvm::BasicBlock *CallBB = Call->getParent();
    llvm::Instruction *Next = Call->getNextNode();
    assert(CallBB && Next && "preflighted C++ EH call changed before commit");
    llvm::BasicBlock *Cont =
        CallBB->splitBasicBlock(Next, CallBB->getName() + ".cxx.cont");
    llvm::Instruction *OldBranch = CallBB->getTerminator();
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
        ProvenanceBuilder, ProvenanceModel,
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
    for (Region &Protected : Regions)
      if (Protected.Blocks.count(CallBB))
        Protected.Blocks.insert(Cont);
  }

  LLVMFunc.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(*Ctx, {mdUInt(*Ctx, 1, 1),
                               llvm::MDString::get(
                                   *Ctx, IsFH4 ? "cxx-fh4-native"
                                               : "cxx-fh3-native")}));
  exception_rewrite::setContract(
      LLVMFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, ProtectedCallCount,
      ProtectedCallCount, /*SkippedPads=*/0);
  return true;
}

} // namespace neverd
