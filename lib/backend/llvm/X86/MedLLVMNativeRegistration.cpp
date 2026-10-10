//===- MedLLVMNativeRegistration.cpp - Native PE32 SEH --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMRegistrationIncoming.h"
#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

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
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      Func.CalleePopBytes || !Func.RegistrationCallerCleanupABIComplete ||
      !FrameAlloca || FrameEntrySPOffset < 24 || Parent.hasPersonalityFn())
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const auto Source = classifyWindowsEHNativeSource(
      EH, TargetArch, TargetFormat, WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::X86RegistrationSEH)
    return false;
  const RegistrationChainInfo &Chain = *EH.Registration;
  const RegistrationStateAnalysis &States = *Func.RegistrationStates;
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

  auto FrameValues = x86_registration::getPrivateSEHFrameValues(
      Func, States, [&](const MedVar &Address, uint16_t Width) {
        const auto Slot = canonicalFrameSlotKey(Address, true);
        return Slot && Slot->second >= -int64_t(FrameEntrySPOffset) &&
               Width <= FrameBytes->getFixedValue() &&
               Slot->second <= int64_t(FrameBytes->getFixedValue() - Width) -
                                   int64_t(FrameEntrySPOffset);
      });
  if (!FrameValues)
    return false;
  auto IsFrame = [&](const MedVar &V) { return FrameValues->contains(V); };

  std::map<va_t, const MedBlock *> BlocksAt;
  std::map<int, const RegistrationBlockState *> BlockStates;
  for (const RegistrationBlockState &State : States.Blocks)
    if (!BlockStates.emplace(State.BlockId, &State).second)
      return false;
  std::map<llvm::BasicBlock *, int32_t> Active;
  std::set<llvm::BasicBlock *> CallbackBlocks;
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
    if (State->second->CallbackOnly) {
      CallbackBlocks.insert(IR->second);
      continue;
    }
    if (State->second->Levels.size() > 1)
      return false;
    const int32_t Level = State->second->Levels.empty()
                              ? Sentinel
                              : State->second->Levels.front();
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

  auto ChainProof = x86_registration::getCheckedSEHChainInstructions(
      Parent, States, RegistrationChainIR);
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
        if (!SourceBlock)
          return false;
        for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores)
          if (Store.StoreVA >= SourceBlock->StartAddr &&
              Store.StoreVA < SourceBlock->EndAddr)
            return false;
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
  auto &SourceOperations = SourceProof->Operations;
  auto &CookieCheckCalls = SourceProof->CookieCheckCalls;

  // Each scope must actually participate in the proven dispatch graph.
  std::vector<bool> Used(Regions.size());
  for (const auto &[Block, Level] : Active)
    for (int32_t State = Level; State >= 0;
         State = Chain.Scopes[State].EnclosingLevel)
      Used[State] = true;
  if (llvm::any_of(Used, [](bool Used) { return !Used; }))
    return false;

  struct CallPlan {
    llvm::CallInst *Call;
    va_t Address;
    int32_t State;
    std::optional<size_t> Finally;
  };
  std::map<va_t, va_t> DirectCalls;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
          !DirectCalls.emplace(Op.Addr, Op.Inputs[0].ConstVal).second)
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
    if (AllCallbackBlocks.count(Instruction->getParent()))
      return false;
  std::vector<CallPlan> Calls;
  std::vector<llvm::Instruction *> DeadFinallyResults;
  for (const auto &[Call, Address] : CallSiteAddrs) {
    if (CookieCheckCalls.count(const_cast<llvm::CallInst *>(Call)))
      continue;
    auto *CallBlock = const_cast<llvm::BasicBlock *>(Call->getParent());
    if (AllCallbackBlocks.count(CallBlock))
      continue;
    auto State = Active.find(CallBlock);
    if (State == Active.end() || !Call->getNextNode() || Call->isMustTailCall())
      return false;
    std::optional<size_t> Finally;
    if (auto Direct = DirectCalls.find(Address); Direct != DirectCalls.end())
      if (auto Callback = CallbackIndices.find({Direct->second, true});
          Callback != CallbackIndices.end())
        Finally = Callback->second;
    if (Finally && !x86_registration::collectDeadFinallyResult(
                       *const_cast<llvm::CallInst *>(Call), DeadFinallyResults))
      return false;
    if ((State->second >= 0 && mayUnwind(*Call)) || Finally)
      Calls.push_back({const_cast<llvm::CallInst *>(Call), Address,
                       State->second, Finally});
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
  auto Outlined = outlineX86RegistrationCallbacks(Parent, Requests);
  if (!Outlined) {
    const std::string Detail = llvm::toString(Outlined.takeError());
    LLVM_DEBUG(llvm::dbgs() << Detail << '\n');
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
  IncomingProjection.commit();
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

  for (auto *Instruction : llvm::reverse(DeadFinallyResults))
    Instruction->eraseFromParent();

  uint64_t ProtectedCalls = 0;
  for (const CallPlan &Plan : Calls) {
    llvm::CallInst *Call = Plan.Call;
    if (Plan.Finally) {
      llvm::IRBuilder<> B(Call);
      auto *Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress));
      auto *Replacement =
          B.CreateCall((*Outlined)[*Plan.Finally], {B.getInt8(0), Frame});
      Replacement->copyMetadata(*Call);
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
