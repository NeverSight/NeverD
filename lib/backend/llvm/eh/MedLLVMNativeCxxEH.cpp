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
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

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
  if (TargetArch != Arch::X64 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::CxxFrameHandler3 || !EH.Cxx ||
      (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
       EH.Encoding != ExceptionEncoding::X64UnwindV2))
    return false;
  const CxxExceptionInfo &Cxx = *EH.Cxx;

  if (!med_llvm_eh::collectExactSourceCallAddresses(LLVMFunc, CallSiteAddrs))
    return false;
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, "__CxxFrameHandler3", PersonalityTy))
    return false;

  // This native closure is intentionally exact and narrow.  Destructors,
  // catch-object frame homes, noexcept, asynchronous /EHa, and out-of-line
  // catch funclets all need parent-frame rewriting; metadata-only IR is safer
  // until that proof is available.  Typed catches without a catch object are
  // representable because the RTTI address remains an external absolute data
  // symbol in the original image.
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
      (Cxx.Flags & ~uint32_t(1)) != 0)
    return false;
  for (const CxxUnwindAction &Action : Cxx.UnwindMap)
    if (Action.ActionVA != 0 ||
        Action.Kind != CxxUnwindAction::ActionKind::None)
      return false;

  struct Handler {
    const CxxCatchHandler *Catch = nullptr;
    llvm::BasicBlock *Target = nullptr;
  };
  struct Region {
    const CxxTryBlock *Try = nullptr;
    std::set<llvm::BasicBlock *> Blocks;
    std::vector<Handler> Handlers;
    llvm::BasicBlock *UnwindDest = nullptr;
    int Parent = -1;
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
  for (const CxxTryBlock &Try : Cxx.TryBlocks) {
    if (Try.Handlers.empty())
      return false;
    Region R;
    R.Try = &Try;
    for (const MedBlock &Block : Func.Blocks) {
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
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      if (Catch.CatchObjectOffset != 0 || Catch.ParentFrameOffset != 0 ||
          Catch.HandlerVA == 0)
        return false;
      llvm::BasicBlock *Target = BlockAt(Catch.HandlerVA);
      if (!Target || Target == &LLVMFunc.getEntryBlock() ||
          R.Blocks.count(Target) || !llvm::pred_empty(Target))
        return false;
      R.Handlers.push_back({&Catch, Target});
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

  // Catch bodies in this closure execute after catchret.  They must therefore
  // be ordinary, call-free continuation blocks and cannot themselves be in a
  // protected region.  This is equivalent for simple catch bodies and avoids
  // pretending an out-of-line native funclet has the regenerated frame ABI.
  for (const Region &R : Regions)
    for (const Handler &H : R.Handlers) {
      for (const Region &Protected : Regions)
        if (Protected.Blocks.count(H.Target))
          return false;
      for (const llvm::Instruction &Inst : *H.Target)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst);
            Call && IsMayUnwindCall(*Call))
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
  };
  llvm::SmallVector<CallPlan, 16> CallPlans;
  std::set<const llvm::CallInst *> PlannedCalls;
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
      if (Call->getParent() != InitialBB || !Call->getNextNode() ||
          CallSiteAddrs.find(Call) == CallSiteAddrs.end() ||
          !PlannedCalls.insert(Call).second)
        return false;
      CallPlans.push_back({Call, static_cast<size_t>(Innermost)});
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
      Mod->getOrInsertFunction("__CxxFrameHandler3", PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

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
  for (size_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    llvm::BasicBlock *ParentDest =
        R.Parent >= 0 ? Regions[static_cast<size_t>(R.Parent)].UnwindDest
                      : nullptr;
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
      PB.CreateCatchRet(Pad, H.Target);
    }
    R.UnwindDest = Dispatch;
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
                               llvm::MDString::get(*Ctx, "cxx-fh3-native")}));
  exception_rewrite::setContract(
      LLVMFunc, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, ProtectedCallCount,
      ProtectedCallCount, /*SkippedPads=*/0);
  return true;
}

} // namespace neverd
