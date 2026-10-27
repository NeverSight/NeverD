//===- MedLLVMNativeRegistration.cpp - Native PE32 SEH --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMRegistrationCopyBlocks.h"
#include "MedLLVMRegistrationIncoming.h"
#include "MedLLVMRegistrationSEHDynamicScope.h"
#include "MedLLVMRegistrationSEHFinally.h"
#include "MedLLVMRegistrationSEHJump.h"
#include "MedLLVMRegistrationSEHLocalUnwind.h"
#include "MedLLVMRegistrationSEHParentExit.h"
#include "MedLLVMRegistrationSEHProof.h"
#include "MedLLVMRegistrationSEHRegisters.h"
#include "MedLLVMRegistrationSEHState.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/backend/llvm/X86RegistrationSEHScope.h"
#include "neverd/backend/llvm/X86RegistrationSEHStateStore.h"
#include "neverd/backend/llvm/X86RegistrationSEHThread.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationEntry.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"
#include "neverd/ir/med/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86RegistrationSEHStateStore.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

#define DEBUG_TYPE "neverd-x86-registration"

namespace neverd {
namespace {

constexpr auto Model = windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
using Role = windows_eh_md::NativeProvenanceRole;

bool mayUnwind(const llvm::CallInst &Call) {
  // A nounwind callee can still fault while this registration is active.
  return !Call.isMustTailCall() && !llvm::isa<llvm::IntrinsicInst>(Call);
}

} // namespace

bool MedLLVMEmitter::emitNativeX86RegistrationSEH(
    const MedFunc &Func, llvm::Function &Parent,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &PHICopyEdges) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      !hasX86RegistrationSEHEntryContract(Func) || !FrameAlloca ||
      FrameEntrySPOffset < 24 || Parent.hasPersonalityFn())
    return false;
  const auto EntryABI =
      Func.RegistrationSEHEntryPopBytes
          ? getX86RegistrationEntryABI(Func, Parent)
          : std::optional<llvm::CallingConv::ID>(llvm::CallingConv::C);
  if (!EntryABI || (Parent.getCallingConv() != llvm::CallingConv::C &&
                    Parent.getCallingConv() != *EntryABI))
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const auto Source = classifyWindowsEHNativeSource(
      EH, TargetArch, TargetFormat, WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::X86RegistrationSEH)
    return false;
  const RegistrationChainInfo &Chain = *EH.Registration;
  const auto *PhysicalStates = getX86RegistrationSEHPhysicalStates(Func);
  if (!PhysicalStates ||
      (Img ? !hasCheckedX86RegistrationSEHPhysicalFrame(*Img, Func)
           : bool(Func.RegistrationSEH4PhysicalFrame)))
    return false;
  const RegistrationStateAnalysis &States = *PhysicalStates;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  if (Img && !EH4 &&
      !coff_loader::isCheckedX86SEH3Personality(*Img, EH.PersonalityVA))
    return false;
  if (EH4) {
    if (!States.SecurityCookiesComplete || !States.SecurityCookieVA ||
        States.SecurityCookieVA > UINT32_MAX - 3 ||
        (Img &&
         (States.SecurityCookieVA !=
              Img->Base + Img->DynInfo.SecurityCookieRVA ||
          !coff_loader::getCheckedX86EH4CookieCheck(*Img, EH.PersonalityVA))))
      return false;
    if (Img && Chain.GSCookieOffset != -2 &&
        !coff_loader::hasCheckedX86CookieCheckSuccessPath(
            *Img,
            *coff_loader::getCheckedX86EH4CookieCheck(*Img, EH.PersonalityVA)))
      return false;
    if (auto *Value = Mod->getNamedValue("__security_cookie")) {
      auto *Cookie = llvm::dyn_cast<llvm::GlobalVariable>(Value);
      if (!Cookie || !Cookie->getValueType()->isIntegerTy(32) ||
          !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
          Cookie->isConstant() || Cookie->isThreadLocal() ||
          Cookie->getAddressSpace() || Cookie->hasDLLImportStorageClass())
        return false;
    }
    if (Chain.GSCookieOffset != -2)
      if (auto *Check = Mod->getNamedValue("__security_check_cookie")) {
        auto *Function = llvm::dyn_cast<llvm::Function>(Check);
        if (!Function || !hasX86RegistrationSecurityCheckABI(*Function))
          return false;
        auto VA = rewrite_source::getOriginalVA(*Function);
        if (!VA) {
          llvm::consumeError(VA.takeError());
          return false;
        }
        if (Img && *VA &&
            **VA != *coff_loader::getCheckedX86EH4CookieCheck(*Img,
                                                              EH.PersonalityVA))
          return false;
      }
  }
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.IncomingFrameAccessesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      States.ChainAccesses.empty() ||
      States.ChainAccesses.size() != RegistrationChainIR.size() ||
      !med_llvm_eh::collectExactSourceCallAddresses(Parent, CallSiteAddrs))
    return false;
  const auto AsyncFlag = med_llvm_eh::classifyI32ModuleFlag(
      *Mod, "eh-asynch", llvm::Module::Warning, 1);
  if (AsyncFlag == med_llvm_eh::I32ModuleFlagState::Conflict)
    return false;
  const auto FrameBytes = FrameAlloca->getAllocationSize(Mod->getDataLayout());
  if (!FrameBytes || FrameBytes->isScalable() ||
      FrameEntrySPOffset > FrameBytes->getFixedValue())
    return false;

  std::map<va_t, const MedBlock *> BlocksAt;
  std::map<int, const RegistrationBlockState *> BlockStates;
  for (const RegistrationBlockState &State : States.Blocks)
    if (!BlockStates.emplace(State.BlockId, &State).second)
      return false;
  std::map<llvm::BasicBlock *, int32_t> Active;
  std::map<llvm::BasicBlock *, std::vector<int32_t>> Dynamic;
  std::set<llvm::BasicBlock *> CallbackBlocks;
  const auto CallbackOwners = registrationSEHCallbackOwners(Func);
  const auto ParentExits = x86_registration::prepareSEHParentExits(
      Func, OriginalBlockMap, PHICopyEdges, &RegistrationChainIR);
  if (!CallbackOwners || !ParentExits)
    return false;
  std::map<int, int> ParentEdges;
  std::set<llvm::BasicBlock *> ParentExitBlocks;
  for (const auto &[Id, Exit] : ParentExits->Sources) {
    ParentEdges.emplace(Id, Exit.TargetBlock);
    ParentExitBlocks.insert(ParentExits->Branches.at(Id)->getParent());
  }
  if (!ParentEdges.empty() &&
      (!llvm::isa<llvm::UncondBrInst>(Parent.getEntryBlock().getTerminator()) ||
       !x86_registration::canPreserveSEHCallbackABI(Parent) ||
       llvm::any_of(OriginalBlockMap, [&](const auto &Block) {
         return Block.second == &Parent.getEntryBlock();
       })))
    return false;
  const auto CopyOwners = registrationPHICopyOwners(
      *CallbackOwners, OriginalBlockMap, PHICopyEdges, ParentEdges);
  if (!CopyOwners)
    return false;
  for (const auto &[Block, Owner] : *CopyOwners)
    CallbackBlocks.insert(Block);
  const int32_t Sentinel = *Chain.SeededTryLevel;
  for (const MedBlock &Block : Func.Blocks) {
    auto IR = OriginalBlockMap.find(Block.Id);
    auto State = BlockStates.find(Block.Id);
    if (!BlocksAt.emplace(Block.StartAddr, &Block).second ||
        IR == OriginalBlockMap.end() || !IR->second ||
        IR->second->getParent() != &Parent || !IR->second->getTerminator() ||
        State == BlockStates.end() ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr || State->second->Unknown)
      return false;
    if (CallbackOwners->count(Block.Id)) {
      CallbackBlocks.insert(IR->second);
      continue;
    }
    if (State->second->levels().size() > 1) {
      for (int32_t Level : State->second->levels())
        if (Level != Sentinel &&
            (Level < 0 || uint32_t(Level) >= Chain.Scopes.size()))
          return false;
      if (!x86_registration::canEmitDynamicSEHScope(*IR->second))
        return false;
      Dynamic.emplace(IR->second, State->second->levels());
      Active.emplace(IR->second, x86_registration::DynamicScopeLevel);
      continue;
    }
    const int32_t Level = State->second->levels().empty()
                              ? Sentinel
                              : State->second->levels().front();
    if (Level != Sentinel &&
        (Level < 0 || uint32_t(Level) >= Chain.Scopes.size()))
      return false;
    Active.emplace(IR->second, Level);
  }
  if (BlockStates.size() != Func.Blocks.size())
    return false;
  auto BlockAt = [&](va_t VA) -> llvm::BasicBlock * {
    auto It = BlocksAt.find(VA);
    return It == BlocksAt.end() ? nullptr : OriginalBlockMap.at(It->second->Id);
  };

  const std::map<std::pair<va_t, int>, const llvm::Instruction *>
      ThreadOperations(RegistrationMemoryIR.begin(),
                       RegistrationMemoryIR.end());
  const auto ThreadReads =
      getCheckedX86SEHThreadReads(Func, Parent, ThreadOperations, false);
  if (!ThreadReads)
    return false;
  auto ChainProof = x86_registration::getCheckedSEHChainInstructions(
      Parent, States, RegistrationChainIR, *ThreadReads);
  if (!ChainProof)
    return false;
  auto &ChainInstructions = *ChainProof;

  struct Region {
    llvm::BasicBlock *Handler = nullptr;
    size_t Callback = 0;
    llvm::BasicBlock *Unwind = nullptr;
    llvm::mc_rewrite::RewriteWinEHSemanticToken Token;
  };
  std::vector<Region> Regions;
  std::vector<X86RegistrationCallbackRequest> Requests;
  std::vector<std::vector<X86RegistrationRootSeed>> Seeds;
  std::map<std::pair<va_t, bool>, size_t> CallbackIndices;
  Regions.reserve(Chain.Scopes.size());
  Requests.reserve(Chain.Scopes.size());
  Seeds.reserve(Chain.Scopes.size());
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (size_t Index = 0; Index < Chain.Scopes.size(); ++Index) {
    const RegistrationScopeRecord &Scope = Chain.Scopes[Index];
    auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
        EH, Arch::X86, static_cast<uint32_t>(Index));
    if (!Token)
      return false;
    Region R;
    R.Token = *Token;
    R.Handler = Scope.IsFinally ? nullptr : BlockAt(Scope.HandlerVA);
    if (!Scope.IsFinally && (!R.Handler || CallbackBlocks.count(R.Handler) ||
                             !Active.count(R.Handler) ||
                             Active.at(R.Handler) != Scope.EnclosingLevel))
      return false;
    const va_t CallbackVA = Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA;
    auto [Callback, New] = CallbackIndices.emplace(
        std::make_pair(CallbackVA, Scope.IsFinally), Requests.size());
    R.Callback = Callback->second;
    if (New) {
      X86RegistrationCallbackRequest Request;
      Request.Entry = BlockAt(CallbackVA);
      Request.Kind = Scope.IsFinally ? X86RegistrationCallbackKind::Finally
                                     : X86RegistrationCallbackKind::Filter;
      Request.Name = Parent.getName().str() + ".registration.callback." +
                     std::to_string(Requests.size());
      if (!Request.Entry || !CallbackBlocks.count(Request.Entry) ||
          Mod->getNamedValue(Request.Name))
        return false;
      std::set<llvm::BasicBlock *> Reached;
      std::vector<llvm::BasicBlock *> Pending{Request.Entry};
      while (!Pending.empty()) {
        llvm::BasicBlock *Block = Pending.back();
        Pending.pop_back();
        if (!Reached.insert(Block).second)
          continue;
        if (!Block->getTerminator() ||
            Reached.size() > limits::kMaxRegistrationEHRecords)
          return false;
        Request.Blocks.push_back(Block);
        const MedBlock *SourceBlock = nullptr;
        for (const auto &[VA, Candidate] : BlocksAt)
          if (OriginalBlockMap.at(Candidate->Id) == Block)
            SourceBlock = Candidate;
        if (!SourceBlock) {
          const auto Copy = CopyOwners->find(Block);
          if (Copy == CopyOwners->end() ||
              Copy->second != std::make_pair(CallbackVA, Scope.IsFinally))
            return false;
        }
        if (!ParentExitBlocks.count(Block))
          for (llvm::BasicBlock *Successor : llvm::successors(Block))
            Pending.push_back(Successor);
      }
      Seeds.emplace_back();
      for (const MedOp &Op : BlocksAt.at(CallbackVA)->Ops) {
        if (Op.RegistrationRoot == MedOp::RegistrationRootKind::None)
          continue;
        if (Op.Opcode != NdOp::COPY || Op.Addr != CallbackVA ||
            Op.Output.Kind != MedVar::Reg || Op.Output.Size != 4 ||
            Op.Output.SSAVer <= 0)
          return false;
        const bool FP = Op.RegistrationRoot ==
                        MedOp::RegistrationRootKind::EstablishedFramePointer;
        if ((!FP && Op.RegistrationRoot !=
                        MedOp::RegistrationRootKind::CallbackStackPointer) ||
            Op.RegistrationStackOffset != 0 ||
            Op.Output.RegOff != (FP ? TRI.FramePointer : TRI.StackPointer))
          return false;
        auto Slot = VarAllocs.find({Op.Output.Id, Op.Output.SSAVer});
        if (Slot == VarAllocs.end())
          return false;
        llvm::StoreInst *Definition = nullptr;
        for (llvm::User *User : Slot->second->users())
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
              Store && Store->getPointerOperand() == Slot->second &&
              Store->getParent() == Request.Entry) {
            if (Definition)
              return false;
            Definition = Store;
          }
        if (!Definition)
          return false;
        Seeds.back().push_back(
            {Definition, FP ? X86RegistrationRootKind::FramePointer
                            : X86RegistrationRootKind::StackPointer});
        if (FP)
          Request.Frame.EstablishedFramePointerOffset = FrameEntrySPOffset - 4;
        else
          Request.Frame.InferStackBounds = true;
      }
      Request.Frame.RootSeeds = Seeds.back();
      if (!Scope.IsFinally)
        Request.Frame.ExceptionPointersOffset = FrameEntrySPOffset - 24;
      if (Request.Frame.EstablishedFramePointerOffset || !Scope.IsFinally)
        Request.Frame.SyntheticFrame = FrameAlloca;
      Requests.push_back(std::move(Request));
    }
    Regions.push_back(R);
  }

  const auto CallbackStores =
      x86_registration::getCheckedSEHCallbackStores(Requests);
  const auto FrameStores = x86_registration::getSEHFrameStores(
      Func, FrameEntrySPOffset, FrameBytes->getFixedValue());
  if (!CallbackStores || !FrameStores)
    return false;
  auto FrameValues = x86_registration::getPrivateSEHFrameValues(
      Func, States, [&](const MedOp &Op) {
        if (FrameStores->count(&Op))
          return true;
        const auto Emitted = RegistrationMemoryIR.find({Op.Addr, Op.OriginSeq});
        if (Emitted != RegistrationMemoryIR.end())
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Emitted->second);
              Store && CallbackStores->count(Store))
            return true;
        const auto Slot = canonicalFrameSlotKey(Op.Inputs[0], true);
        const auto Width = Op.Inputs[1].Size;
        return Slot && Slot->second >= -int64_t(FrameEntrySPOffset) &&
               Width <= FrameBytes->getFixedValue() &&
               Slot->second <= int64_t(FrameBytes->getFixedValue() - Width) -
                                   int64_t(FrameEntrySPOffset);
      });
  if (!FrameValues)
    return false;
  auto IsFrame = [&](const MedVar &V) { return FrameValues->contains(V); };

  const auto Incoming = x86_registration::prepareIncomingFrame(
      Func, Parent, RegistrationIncomingIR);
  if (!Incoming)
    return false;
  for (const auto &Bound : *Incoming)
    if (Bound.Access->Write && IsFrame(Bound.Source->Inputs[1]))
      return false;

  auto SourceProof = x86_registration::getCheckedSEHSourceOperations(
      Func, OriginalBlockMap, RegistrationMemoryIR, CallSiteAddrs);
  if (!SourceProof)
    return false;
  if (!hasCheckedX86RegistrationSEHComputedStores(
          Func, Parent, *FrameAlloca, FrameEntrySPOffset,
          [&](va_t Address, int Seq) -> const llvm::Instruction * {
            const auto Found = RegistrationMemoryIR.find({Address, Seq});
            return Found == RegistrationMemoryIR.end() ? nullptr
                                                       : Found->second;
          }))
    return false;
  auto JumpPlan = x86_registration::prepareSEHJumps(
      Func, Parent, *FrameAlloca, FrameEntrySPOffset, CallSiteAddrs);
  if (!JumpPlan)
    return false;
  const auto LocalUnwinds = x86_registration::prepareSEHLocalUnwinds(
      Func, Parent, *FrameAlloca, FrameEntrySPOffset, *ParentExits,
      CallbackIndices, Requests, CallSiteAddrs, RegistrationMemoryIR);
  if (!LocalUnwinds)
    return false;
  for (const auto &[Call, Source] : JumpPlan->Calls)
    if (Source.Resume && Source.Resume->EntryVA) {
      const auto Callback =
          CallbackIndices.lower_bound({Source.Resume->EntryVA, false});
      if (Callback == CallbackIndices.end() ||
          Callback->first.first != Source.Resume->EntryVA ||
          !llvm::is_contained(Requests[Callback->second].Blocks,
                              Call->getParent()))
        return false;
      Requests[Callback->second].NonReturningCalls.push_back(Call);
    }
  for (const auto &[Call, Bound] : JumpPlan->BoundCalls)
    if (Bound.Source->EntryVA) {
      const auto Callback = CallbackIndices.find({Bound.Source->EntryVA, true});
      if (Callback == CallbackIndices.end() ||
          !llvm::is_contained(Requests[Callback->second].Blocks,
                              Call->getParent()))
        return false;
    }
  auto &SourceOperations = SourceProof->Operations;
  auto &CookieCheckCalls = SourceProof->CookieCheckCalls;

  auto StateWrites = x86_registration::prepareSEHCallbackStateWrites(
      Func, Parent, Requests, RegistrationMemoryIR);
  const auto RetainedScopes = getX86RegistrationSEHRetainedScopes(Func);
  if (!StateWrites || !RetainedScopes ||
      (!StateWrites->empty() &&
       !x86_registration::canPreserveSEHCallbackABI(Parent)) ||
      (!RetainedScopes->empty() &&
       (!x86_registration::canEmitSEHRetainedScopes(Parent) ||
        !llvm::isa<llvm::UncondBrInst>(
            Parent.getEntryBlock().getTerminator()) ||
        llvm::any_of(OriginalBlockMap, [&](const auto &Block) {
          return Block.second == &Parent.getEntryBlock();
        }))))
    return false;

  struct CallPlan {
    llvm::CallInst *Call;
    va_t Address;
    int32_t State;
    std::optional<size_t> Finally;
    bool NoReturn;
  };
  std::map<va_t, va_t> DirectCalls;
  std::map<va_t, std::tuple<int, va_t, int>> CallKeys;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst()) {
        if (!DirectCalls.emplace(Op.Addr, Op.Inputs[0].ConstVal).second)
          return false;
        CallKeys.emplace(Op.Addr,
                         std::make_tuple(Block.Id, Op.Addr, Op.OriginSeq));
      }
  const auto FinallyReturns = getRegistrationSEHReturns(Func);
  const auto FinallyJumps = getRegistrationSEHNonlocalCalls(Func);
  if (!FinallyReturns || !FinallyJumps)
    return false;
  std::set<llvm::BasicBlock *> AllCallbackBlocks;
  for (const auto &Request : Requests)
    for (llvm::BasicBlock *Block : Request.Blocks)
      if (!AllCallbackBlocks.insert(Block).second)
        return false;
  for (auto *Call : CookieCheckCalls)
    if (AllCallbackBlocks.count(Call->getParent()))
      return false;
  // Callback frame recovery changes EBP while the compiler owns FS:[0]. A
  // source callback which observes that chain needs a separate remap proof.
  for (llvm::Instruction *Instruction : ChainInstructions)
    if (AllCallbackBlocks.count(Instruction->getParent()) &&
        llvm::none_of(ParentExits->ChainRemovals, [&](const auto &Removal) {
          return Removal.second == Instruction;
        }))
      return false;
  std::vector<CallPlan> Calls;
  std::vector<x86_registration::SEHNestedFinallyCall> NestedFinallyCalls;
  std::vector<llvm::Instruction *> DeadCallResults = JumpPlan->DiscardedResults;
  // Lowering names what it creates for the calls in this order.
  for (const auto &[Call, Address] :
       med_llvm_eh::sourceCallsInFunctionOrder(Parent, CallSiteAddrs)) {
    if (CookieCheckCalls.count(const_cast<llvm::CallInst *>(Call)))
      continue;
    auto *CallBlock = const_cast<llvm::BasicBlock *>(Call->getParent());
    std::optional<size_t> Finally;
    if (auto Direct = DirectCalls.find(Address); Direct != DirectCalls.end())
      if (auto Callback = CallbackIndices.find({Direct->second, true});
          Callback != CallbackIndices.end())
        Finally = Callback->second;
    if (AllCallbackBlocks.count(CallBlock)) {
      if (Finally) {
        const bool NoReturn = llvm::none_of(
            Requests[*Finally].Blocks, [](const llvm::BasicBlock *Block) {
              return llvm::isa<llvm::ReturnInst>(Block->getTerminator());
            });
        std::vector<llvm::Instruction *> DiscardedResults;
        if (!NoReturn &&
            !x86_registration::collectDiscardedRegistrationResult(
                *const_cast<llvm::CallInst *>(Call), DiscardedResults))
          return false;
        DeadCallResults.insert(DeadCallResults.end(), DiscardedResults.begin(),
                               DiscardedResults.end());
        for (size_t Index = 0; Index != Requests.size(); ++Index)
          if (llvm::is_contained(Requests[Index].Blocks, CallBlock)) {
            const auto &Key = CallKeys.at(Address);
            const auto Returned = FinallyReturns->ReturningCalls.find(Key);
            const auto Jumped = FinallyJumps->find(Key);

            if (Requests[Index].Kind != X86RegistrationCallbackKind::Finally ||
                (NoReturn ? Returned != FinallyReturns->ReturningCalls.end() ||
                                Jumped == FinallyJumps->end() ||
                                Jumped->second != DirectCalls.at(Address)
                          : Returned == FinallyReturns->ReturningCalls.end() ||
                                Returned->second != DirectCalls.at(Address)))
              return false;
            NestedFinallyCalls.push_back(
                {Call, Address, DirectCalls.at(Address), Index, *Finally,
                 NoReturn, std::move(DiscardedResults)});
          }
      }
      continue;
    }
    auto State = Active.find(CallBlock);
    if (State == Active.end() || !Call->getNextNode() || Call->isMustTailCall())
      return false;
    const bool NoReturn =
        Finally &&
        llvm::none_of(
            Requests[*Finally].Blocks, [](const llvm::BasicBlock *Block) {
              return llvm::isa<llvm::ReturnInst>(Block->getTerminator());
            });
    if (Finally && !NoReturn &&
        !x86_registration::collectDiscardedRegistrationResult(
            *const_cast<llvm::CallInst *>(Call), DeadCallResults))
      return false;
    if ((State->second >= 0 && mayUnwind(*Call)) || Finally)
      Calls.push_back({const_cast<llvm::CallInst *>(Call), Address,
                       State->second, Finally, NoReturn});
  }
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
          !CallSiteAddrs.count(Call))
        return false;

  auto *PersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, true);
  const llvm::StringRef PersonalityName =
      EH.Personality == ExceptionPersonality::ExceptHandler4
          ? "_except_handler4"
          : "_except_handler3";
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, PersonalityName, PersonalityTy))
    return false;
  llvm::Function *Previous = Mod->getFunction(PersonalityName);
  auto *Personality = llvm::cast<llvm::Function>(
      Mod->getOrInsertFunction(PersonalityName, PersonalityTy).getCallee());
  Parent.setPersonalityFn(Personality);
  struct SourceOriginal {
    llvm::Instruction *Instruction;
    llvm::MDNode *Marker;
    bool Volatile;
  };
  std::vector<SourceOriginal> SourceOriginals;
  for (const auto &Event : SourceOperations) {
    auto *I = Event.Instruction;
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
    SourceOriginals.push_back(
        {I, I->getMetadata(windows_eh_md::RegistrationOperationAttachment),
         Load ? Load->isVolatile() : Store && Store->isVolatile()});
    I->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(
            *Ctx, {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Operation->Addr, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Operation->OriginSeq, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.Block, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.Kind, 8)}));
  }
  llvm::Function *PreviousSideEffect = Mod->getFunction("llvm.sideeffect");
  std::vector<llvm::Instruction *> SourceAnchors;
  auto *SideEffect =
      llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::sideeffect);
  for (const MedBlock &Block : Func.Blocks) {
    auto *IR = OriginalBlockMap.at(Block.Id);
    for (bool Enter : {true, false}) {
      llvm::IRBuilder<> B(Enter ? &*IR->getFirstInsertionPt()
                                : IR->getTerminator());
      auto *Anchor = B.CreateCall(SideEffect);
      Anchor->setMetadata(
          windows_eh_md::RegistrationBlockAttachment,
          llvm::MDNode::get(*Ctx,
                            {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.StartAddr, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.EndAddr, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.Id, 32),
                             med_llvm_eh::mdUInt(*Ctx, Enter, 1)}));
      SourceAnchors.push_back(Anchor);
    }
  }
  x86_registration::IncomingFrameProjection IncomingProjection(
      Parent, EH.CodeRange.Begin, *Incoming);
  x86_registration::SEHParentExitProjection ParentProjection(
      Parent, EH.CodeRange.Begin, *ParentExits);
  for (const auto &[Id, Call] : ParentProjection.calls()) {
    auto &Request = Requests[CallbackIndices.at(
        {ParentExits->Sources.at(Id).EntryVA, true})];
    Request.NonReturningCalls.push_back(Call);
    Request.Blocks.push_back(Call->getParent());
    AllCallbackBlocks.insert(Call->getParent());
  }
  std::vector<X86RegistrationCallbackInstructionMap> CallbackInstructions;
  auto Outlined =
      outlineX86RegistrationCallbacks(Parent, Requests, &CallbackInstructions);
  if (!Outlined) {
    const std::string Detail = llvm::toString(Outlined.takeError());
    LLVM_DEBUG(llvm::dbgs() << Detail << '\n');
    ParentProjection.rollback();
    IncomingProjection.rollback();
    for (const auto &Original : SourceOriginals) {
      Original.Instruction->setMetadata(
          windows_eh_md::RegistrationOperationAttachment, Original.Marker);
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Original.Instruction))
        Load->setVolatile(Original.Volatile);
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Original.Instruction))
        Store->setVolatile(Original.Volatile);
    }
    for (auto *Anchor : SourceAnchors)
      Anchor->eraseFromParent();
    if (!PreviousSideEffect && SideEffect->use_empty())
      SideEffect->eraseFromParent();
    Parent.setPersonalityFn(nullptr);
    if (!Previous)
      Personality->eraseFromParent();
    return false;
  }

  // Commit. All source occurrences, callbacks and control edges are closed.
  Parent.setCallingConv(*EntryABI);
  ParentProjection.commit();
  IncomingProjection.commit();
  if (!StateWrites->empty() || !ParentExits->Sources.empty()) {
    llvm::SmallVector<llvm::GlobalValue *> CallbackABI(Outlined->begin(),
                                                       Outlined->end());
    llvm::appendToCompilerUsed(*Mod, CallbackABI);
  }
  if (EH4 && !Mod->getNamedValue("__security_cookie"))
    new llvm::GlobalVariable(*Mod, llvm::Type::getInt32Ty(*Ctx), false,
                             llvm::GlobalValue::ExternalLinkage, nullptr,
                             "__security_cookie");
  if (EH4 && Chain.GSCookieOffset != -2) {
    auto *Check = Mod->getFunction("__security_check_cookie");
    if (!Check)
      Check = createX86RegistrationSecurityCheck(*Mod);
    if (Img)
      rewrite_source::setOriginalVA(
          *Check,
          *coff_loader::getCheckedX86EH4CookieCheck(*Img, EH.PersonalityVA));
  }
  RegistrationIncomingIR.clear();
  RegistrationMemoryIR.clear();
  std::vector<llvm::Function *> SourceFunctions{&Parent};
  SourceFunctions.insert(SourceFunctions.end(), Outlined->begin(),
                         Outlined->end());
  for (auto *Function : SourceFunctions)
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (I.getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            Load->setVolatile(true);
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
            Store->setVolatile(true);
        }
  FrameAlloca->setMetadata(
      windows_eh_md::RegistrationFrameAttachment,
      llvm::MDNode::get(*Ctx,
                        {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                         med_llvm_eh::mdUInt(*Ctx, FrameEntrySPOffset, 64)}));
  ParentProjection.emitCapture(OriginalBlockMap);
  if (!LocalUnwinds->Calls.empty())
    x86_registration::emitSEHLocalUnwinds(
        *LocalUnwinds, Parent, *FrameAlloca, FrameEntrySPOffset,
        *ParentProjection.chain(), EH.CodeRange.Begin, *Outlined,
        CallbackInstructions);
  auto *None = llvm::ConstantTokenNone::get(*Ctx);
  auto *I8 = llvm::Type::getInt8Ty(*Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto SetSourceRuntimeState = [&](llvm::IRBuilder<> &B, int32_t State) {
    auto *Slot = B.CreateInBoundsGEP(I8, FrameAlloca,
                                     B.getInt32(FrameEntrySPOffset - 8));
    auto *Store = B.CreateStore(B.getInt32(State), Slot);
    Store->setAlignment(llvm::Align(1));
    Store->setVolatile(true);
  };
  for (size_t I = 0; I < Regions.size(); ++I) {
    const auto &Scope = Chain.Scopes[I];
    Region &R = Regions[I];
    llvm::BasicBlock *Outer = Scope.EnclosingLevel >= 0
                                  ? Regions[Scope.EnclosingLevel].Unwind
                                  : nullptr;
    llvm::Function *Callback = (*Outlined)[R.Callback];
    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "registration.dispatch." + std::to_string(I), &Parent);
    llvm::IRBuilder<> B(Dispatch);
    if (Scope.IsFinally) {
      auto *Pad = B.CreateCleanupPad(None);
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.Token))
        llvm_unreachable("prevalidated registration token rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::RegionDispatch, EH.CodeRange.Begin, Scope.HandlerVA,
          I, 0, Pad, Scope.HandlerVA, 1);
      SetSourceRuntimeState(B, Scope.EnclosingLevel);
      auto *Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress));
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Bundle("funclet", BundleInputs);
      B.CreateCall(Callback, {B.getInt8(1), Frame}, {Bundle});
      B.CreateCleanupRet(Pad, Outer);
    } else {
      auto *Switch = B.CreateCatchSwitch(None, Outer, 1);
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "registration.catch." + std::to_string(I), &Parent);
      Switch->addHandler(PadBB);
      llvm::IRBuilder<> PB(PadBB);
      auto *Pad = PB.CreateCatchPad(Switch, {Callback});
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.Token))
        llvm_unreachable("prevalidated registration token rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          PB, Model, Role::RegionDispatch, EH.CodeRange.Begin, Scope.HandlerVA,
          I, 0, Pad, Scope.FilterVA, 0);
      SetSourceRuntimeState(PB, Scope.EnclosingLevel);
      PB.CreateCatchRet(Pad, R.Handler);
      llvm::IRBuilder<> HB(&*R.Handler->getFirstInsertionPt());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(HB, Model, Role::HandlerTarget,
                                                 EH.CodeRange.Begin,
                                                 Scope.HandlerVA, I, 0);
    }
    R.Unwind = Dispatch;
  }
  for (const auto &[Identity, Index] : CallbackIndices) {
    llvm::Function *Callback = (*Outlined)[Index];
    llvm::IRBuilder<> B(Callback->getEntryBlock().getTerminator());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationCallback, EH.CodeRange.Begin,
        Identity.first, Index, 0, nullptr, 0, Identity.second);
  }

  for (const RegistrationChainAccess &Access : States.ChainAccesses) {
    if (ParentExits->ChainRemovals.count({Access.Address, Access.OpSeq}))
      continue;
    llvm::Instruction *Instruction =
        RegistrationChainIR.at({Access.Address, Access.OpSeq});
    llvm::IRBuilder<> B(Instruction);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationChainAccess, EH.CodeRange.Begin,
        Access.Address, 0, Access.OpSeq, nullptr, Access.EndAddress,
        static_cast<uint32_t>(Access.AccessKind));
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction)) {
      llvm::Value *Value = nullptr;
      if (Access.AccessKind ==
          RegistrationChainAccess::Kind::ReadPreviousHead) {
        auto *Head = B.CreateLoad(I32, Load->getPointerOperand());
        Value = B.CreateLoad(I32, B.CreateIntToPtr(Head, B.getPtrTy()));
        llvm::cast<llvm::LoadInst>(Value)->setAlignment(llvm::Align(1));
      } else {
        auto *Record = B.CreateInBoundsGEP(I8, FrameAlloca,
                                           B.getInt32(FrameEntrySPOffset - 20));
        Value = B.CreatePtrToInt(Record, I32);
      }
      Load->replaceAllUsesWith(Value);
    }
    Instruction->eraseFromParent();
  }
  RegistrationChainIR.clear();

  for (auto *Call : CookieCheckCalls) {
    llvm::IRBuilder<> B(Call);
    auto *Event = B.CreateCall(SideEffect);
    Event->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        Call->getMetadata(windows_eh_md::RegistrationOperationAttachment));
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
  }

  x86_registration::emitSEHNestedFinallyCalls(NestedFinallyCalls, *Outlined,
                                              CallbackInstructions, Parent,
                                              EH.CodeRange.Begin);
  x86_registration::bindSEHRegisterCallbackInputs(*Outlined);
  x86_registration::emitSEHCallbackStateWrites(*StateWrites, *Outlined,
                                               CallbackInstructions, Parent);

  std::set<llvm::Instruction *> DeadResults(DeadCallResults.begin(),
                                            DeadCallResults.end());
  for (auto *Instruction : DeadResults)
    Instruction->dropAllReferences();
  for (auto *Instruction : DeadResults)
    Instruction->eraseFromParent();

  std::map<va_t, llvm::Function *> JumpCallbacks;
  for (const auto &[Identity, Index] : CallbackIndices)
    JumpCallbacks.emplace(Identity.first, (*Outlined)[Index]);
  x86_registration::bindSEHRuntimeCallbackArguments(*JumpPlan, JumpCallbacks);
  auto JumpCalls = x86_registration::emitSEHJumps(
      *JumpPlan, Parent, *FrameAlloca, FrameEntrySPOffset, EH.CodeRange.Begin,
      JumpCallbacks);
  for (const auto &[Original, Replacement] : JumpCalls) {
    if (const auto Site = CallSiteAddrs.find(Original);
        Site != CallSiteAddrs.end()) {
      CallSiteAddrs.emplace(Replacement, Site->second);
      CallSiteAddrs.erase(Site);
    }
    Original->replaceAllUsesWith(Replacement);
    Original->eraseFromParent();
  }

  uint64_t ProtectedCalls = 0;
  for (const CallPlan &Plan : Calls) {
    llvm::CallInst *Call = Plan.Call;
    if (const auto Jump = JumpCalls.find(Call); Jump != JumpCalls.end())
      Call = Jump->second;
    if (Plan.Finally) {
      llvm::IRBuilder<> B(Call);
      auto *Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress));
      auto *Replacement =
          B.CreateCall((*Outlined)[*Plan.Finally], {B.getInt8(0), Frame});
      Replacement->copyMetadata(*Call);
      // The batch outliner has now proved every callback exit. A nonlocal
      // finally has no ordinary result, including the lifted register copies
      // on its unreachable fallthrough. Keep source boundaries for replay.
      // Keep this attribute on the ordinary invocation: the cleanup dispatch
      // still needs its cleanupret edge to preserve the source scope nesting.
      if (Plan.NoReturn) {
        Replacement->addFnAttr(llvm::Attribute::NoReturn);
        if (!Call->getType()->isVoidTy())
          Call->replaceAllUsesWith(llvm::PoisonValue::get(Call->getType()));
      }
      va_t FinallyVA = 0;
      for (const auto &[Identity, Index] : CallbackIndices)
        if (Identity.second && Index == *Plan.Finally)
          FinallyVA = Identity.first;
      Replacement->setMetadata(
          windows_eh_md::RegistrationFinallyCallAttachment,
          llvm::MDNode::get(*Ctx,
                            {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                             med_llvm_eh::mdUInt(*Ctx, Plan.Address, 64),
                             med_llvm_eh::mdUInt(*Ctx, FinallyVA, 64)}));
      CallSiteAddrs.erase(Call);
      Call->eraseFromParent();
      Call = Replacement;
      CallSiteAddrs.emplace(Call, Plan.Address);
    }
    if (Plan.State < 0)
      continue;
    ++ProtectedCalls;
    llvm::BasicBlock *Block = Call->getParent();
    auto *Continue =
        Block->splitBasicBlock(Call->getNextNode(), "registration.call.cont");
    auto *Branch = Block->getTerminator();
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Value *Arg : Call->args())
      Args.push_back(Arg);
    auto *Invoke = llvm::InvokeInst::Create(
        Call->getFunctionType(), Call->getCalledOperand(), Continue,
        Regions[Plan.State].Unwind, Args, Call->getName(),
        Branch->getIterator());
    Invoke->setCallingConv(Call->getCallingConv());
    Invoke->setAttributes(Call->getAttributes());
    Invoke->copyMetadata(*Call);
    llvm::IRBuilder<> B(Invoke);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(B, Model, Role::ProtectedInvoke,
                                               EH.CodeRange.Begin, Plan.Address,
                                               Plan.State, 0);
    Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
    Branch->eraseFromParent();
  }

  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_scope_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_scope_end);
  for (const MedBlock &SourceBlock : Func.Blocks) {
    llvm::BasicBlock *Block = OriginalBlockMap.at(SourceBlock.Id);
    auto State = Active.find(Block);
    if (State == Active.end())
      continue;
    // Preserve asynchronous fault ordering across compiler-owned state stores.
    // The source state is constant within each checked Med block. Every next
    // block explicitly establishes its own state, including nested exits.
    auto Volatilize = [](llvm::BasicBlock &Part) {
      for (llvm::Instruction &Instruction : Part) {
        if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
          Load->setVolatile(true);
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          Store->setVolatile(true);
      }
    };
    Volatilize(*Block);
    if (const auto Choice = Dynamic.find(Block); Choice != Dynamic.end()) {
      std::map<int32_t, llvm::BasicBlock *> Unwinds;
      for (int32_t Level : Choice->second)
        if (Level >= 0)
          Unwinds.emplace(Level, Regions[Level].Unwind);
      ProtectedCalls += x86_registration::emitDynamicSEHScope(
          *Block, *FrameAlloca, FrameEntrySPOffset, EH.CodeRange.Begin,
          SourceBlock, Choice->second, Unwinds, *TryBegin, *TryEnd);
      continue;
    }
    if (State->second < 0) {
      llvm::IRBuilder<> B(&*Block->getFirstInsertionPt());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::RangeEnterTarget, EH.CodeRange.Begin,
          SourceBlock.StartAddr, SourceBlock.Id, 0, nullptr,
          SourceBlock.EndAddr, static_cast<uint32_t>(State->second));
      B.CreateCall(TryEnd);
      llvm::IRBuilder<> Exit(Block->getTerminator());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          Exit, Model, Role::RangeExitTarget, EH.CodeRange.Begin,
          SourceBlock.EndAddr, SourceBlock.Id, 0, nullptr,
          SourceBlock.StartAddr, static_cast<uint32_t>(State->second));
      continue;
    }
    // The final original terminator may now live in a call continuation.
    // Follow the newly created normal edges to the source block's exit.
    llvm::BasicBlock *Exit = Block;
    while (auto *Invoke =
               llvm::dyn_cast<llvm::InvokeInst>(Exit->getTerminator())) {
      Exit = Invoke->getNormalDest();
      Volatilize(*Exit);
    }
    auto *Term = Exit->getTerminator();
    auto *After = Exit->splitBasicBlock(Term, "registration.range.exit");
    Exit->getTerminator()->eraseFromParent();
    llvm::IRBuilder<> EB(Exit);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        EB, Model, Role::RangeExit, EH.CodeRange.Begin, SourceBlock.EndAddr,
        State->second, SourceBlock.Id);
    EB.CreateInvoke(TryEnd, After, Regions[State->second].Unwind);
    llvm::IRBuilder<> AB(After->getTerminator());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        AB, Model, Role::RangeExitTarget, EH.CodeRange.Begin,
        SourceBlock.EndAddr, SourceBlock.Id, 0, nullptr, SourceBlock.StartAddr,
        static_cast<uint32_t>(State->second));
    auto *Begin = llvm::BasicBlock::Create(*Ctx, "registration.range.enter",
                                           &Parent, Block);
    Block->replaceAllUsesWith(Begin);
    llvm::IRBuilder<> BB(Begin);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        BB, Model, Role::RangeEnter, EH.CodeRange.Begin, SourceBlock.StartAddr,
        State->second, SourceBlock.Id);
    BB.CreateInvoke(TryBegin, Block, Regions[State->second].Unwind);
  }
  std::vector<llvm::BasicBlock *> Unwinds;
  for (const auto &R : Regions)
    Unwinds.push_back(R.Unwind);
  x86_registration::emitSEHRetainedScopes(Parent, EH.CodeRange.Begin,
                                          *RetainedScopes, Unwinds);
  for (llvm::BasicBlock *Block : AllCallbackBlocks) {
    for (llvm::Instruction &Instruction : *Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        CallSiteAddrs.erase(Call);
    Block->dropAllReferences();
  }
  for (llvm::BasicBlock *Block : AllCallbackBlocks)
    Block->eraseFromParent();
  if (AsyncFlag == med_llvm_eh::I32ModuleFlagState::Absent)
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  Parent.addFnAttr(llvm::Attribute::NoInline);
  Parent.addFnAttr(llvm::Attribute::OptimizeNone);
  // This protocol also keeps compiler intrinsics from resetting the current
  // asynchronous state, and makes the backend's state writes volatile.
  Parent.addFnAttr("llvm.rewrite.win-x86-registration-state");
  if (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
      Chain.GSCookieOffset != -2)
    Parent.addFnAttr(llvm::Attribute::StackProtectReq);
  Parent.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {med_llvm_eh::mdUInt(*Ctx, 1, 1),
                 llvm::MDString::get(*Ctx, "seh-x86-registration-native")}));
  exception_rewrite::setContract(Parent,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::Complete,
                                 ProtectedCalls, ProtectedCalls, 0);
  return true;
}

} // namespace neverd
