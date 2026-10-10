//===- MedLLVMNativeRegistrationCxx.cpp - Native PE32 C++ EH --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMRegistrationCxxCall.h"
#include "MedLLVMRegistrationCxxCatch.h"
#include "MedLLVMRegistrationCxxContinuation.h"
#include "MedLLVMRegistrationCxxStack.h"
#include "MedLLVMRegistrationIncoming.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/backend/llvm/X86RegistrationCxxUnwind.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/X86RegistrationFrame.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Transforms/Utils/Local.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

bool MedLLVMEmitter::emitNativeX86RegistrationCxx(
    const MedFunc &Func, llvm::Function &Parent,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
#ifndef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  return false;
#else
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF || !Img ||
      !Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      Func.CalleePopBytes || !Func.RegistrationCallerCleanupABIComplete ||
      !FrameAlloca || FrameEntrySPOffset < 20 || Parent.hasPersonalityFn() ||
      Func.IsVariadic || !Parent.getReturnType()->isIntegerTy(32))
    return false;
  const auto &EH = *Func.ExceptionMetadata;
  const auto Source = classifyWindowsEHNativeSource(
      EH, TargetArch, TargetFormat, WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::X86RegistrationCxx)
    return false;
  const auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(*Img, EH);
  if (!Runtime || Mod->getModuleFlag("eh-asynch"))
    return false;
  const auto &States = *Func.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !States.IncomingFrameAccessesComplete ||
      !States.CallFrameEffectsComplete || !States.CleanupFrameEffectsComplete ||
      !States.CxxContinuationsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete || !States.ImageReadsComplete)
    return false;
  const auto &Cxx = *EH.Cxx;
  const auto UnwindGraph = projectX86RegistrationCxxUnwind(Cxx);
  if (!UnwindGraph)
    return false;
  const auto FrameBytes = FrameAlloca->getAllocationSize(Mod->getDataLayout());
  const auto Coordinate = cxxRegistrationFrameCoordinate(EH, &States);
  const auto Layout =
      Coordinate && FrameBytes && !FrameBytes->isScalable()
          ? projectX86RegistrationFrame(
                *Coordinate, FrameBytes->getFixedValue(), FrameEntrySPOffset,
                FrameAlloca->getAlign().value())
          : std::nullopt;
  if (!Layout)
    return false;
  const auto CatchOwners = projectX86RegistrationCatchBlocks(Func);
  if (!CatchOwners)
    return false;
  std::map<X86RegistrationCatchIdentity, RegistrationCxxCatchPlan> Catches;
  for (uint32_t Region = 0; Region < Cxx.TryBlocks.size(); ++Region)
    for (uint32_t Index = 0; Index < Cxx.TryBlocks[Region].Handlers.size();
         ++Index) {
      auto Object =
          projectX86RegistrationCatch(EH, States, *Layout, Region, Index);
      if (!Object)
        return false;
      Catches[{Region, Index}].Object = *Object;
    }

  const auto EntryABI = getX86RegistrationCxxEntryABI(Func, Parent);
  if (!EntryABI)
    return false;

  using RangeKey = std::pair<va_t, va_t>;
  std::map<RangeKey, const RegistrationBlockState *> SourceStates;
  for (const auto &State : States.Blocks)
    if (!State.Range.isValid() || State.Unknown ||
        !SourceStates
             .emplace(RangeKey{State.Range.Begin, State.Range.End}, &State)
             .second)
      return false;
  std::map<int, const RegistrationBlockState *> BlockStates;
  std::map<va_t, llvm::BasicBlock *> BlocksAt;
  std::set<const RegistrationBlockState *> Matched;
  for (const auto &Block : Func.Blocks) {
    auto IR = OriginalBlockMap.find(Block.Id);
    if (IR == OriginalBlockMap.end() || !IR->second ||
        IR->second->getParent() != &Parent || !IR->second->getTerminator() ||
        !BlocksAt.emplace(Block.StartAddr, IR->second).second)
      return false;
    auto State = SourceStates.find({Block.StartAddr, Block.EndAddr});
    if (State != SourceStates.end() && State->second->Reached) {
      if (!Matched.insert(State->second).second)
        return false;
      BlockStates.emplace(Block.Id, State->second);
    } else {
      // MedIR may merge or renumber dead blocks. A complete source solver
      // authorizes pruning only when no reached source interval overlaps it.
      for (const auto &Candidate : States.Blocks)
        if (Candidate.Reached &&
            Candidate.Range.overlaps({Block.StartAddr, Block.EndAddr}))
          return false;
    }
  }
  for (const auto &State : States.Blocks)
    if (State.Reached && !Matched.count(&State))
      return false;
  for (auto &[Identity, Plan] : Catches) {
    const auto &[Region, Index] = Identity;
    const auto Handler =
        BlocksAt.find(Cxx.TryBlocks[Region].Handlers[Index].HandlerVA);
    if (Handler == BlocksAt.end() ||
        !windows_eh_semantics::getCxxCatchSemanticToken(EH, Arch::X86, Region,
                                                        Index))
      return false;
    Plan.Handler = Handler->second;
  }

  struct Operation {
    llvm::Instruction *IR;
    const MedOp *Source;
    int SourceBlock;
    uint8_t Kind;
  };
  struct CallPlan {
    llvm::CallInst *Call;
    const RegistrationCallFrameEffect *Effect;
    const RegistrationBlockState *State;
    bool Throw;
    med_llvm_eh::RegistrationCxxCallABI ABI;
    X86RegistrationCatchIdentity CatchIdentity;
    std::string Name;
  };
  struct CleanupPlan {
    const RegistrationCleanupFrameContract *Contract;
    std::string Name;
    std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken> Token;
  };
  std::vector<Operation> Operations;
  std::vector<CallPlan> Calls;
  std::map<uint32_t, CleanupPlan> Cleanups;
  std::map<llvm::ReturnInst *, const RegistrationCxxContinuation *> Resumes;
  std::set<llvm::Instruction *> ChainInstructions;
  size_t Work = 0;
  auto *I8 = llvm::Type::getInt8Ty(*Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto *Void = llvm::Type::getVoidTy(*Ctx);
  auto *Ptr = llvm::PointerType::get(*Ctx, 0);
  auto *CleanupType = llvm::FunctionType::get(Void, {Ptr}, false);
  auto *PersonalityType = llvm::FunctionType::get(I32, {}, true);
  auto CalleeName = [](va_t Target, llvm::StringRef ABI) {
    return "__nd_registration_" + ABI.str() + "_" + llvm::utohexstr(Target);
  };
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, "__CxxFrameHandler3", PersonalityType))
    return false;
  for (const auto &Try : Cxx.TryBlocks)
    for (const auto &Catch : Try.Handlers)
      if (Catch.TypeDescriptorVA &&
          !med_llvm_eh::canMaterializeExternalDataDeclaration(
              *Mod, makeNdDataSymbol(Catch.TypeDescriptorVA), I8, false))
        return false;
  if (auto *Existing = Mod->getFunction("__CxxFrameHandler3")) {
    auto Address = rewrite_source::getOriginalVA(*Existing);
    if (!Address) {
      llvm::consumeError(Address.takeError());
      return false;
    }
    if (*Address && **Address != Runtime->RuntimeVA)
      return false;
  }
  for (const auto &Block : Func.Blocks) {
    auto S = BlockStates.find(Block.Id);
    if (S == BlockStates.end())
      continue;
    const auto *State = S->second;
    auto *IR = OriginalBlockMap.at(Block.Id);
    if (State->CallbackOnly)
      Catches.at(CatchOwners->at(Block.Id)).Blocks.insert(IR);
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INDIR_BR)
        return false;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        auto It = RegistrationMemoryIR.find({Op.Addr, Op.OriginSeq});
        if (Op.OriginSeq < 0 || It == RegistrationMemoryIR.end() ||
            !It->second || It->second->getParent() != IR)
          return false;
        Operations.push_back({It->second, &Op, State->BlockId,
                              uint8_t(Op.Opcode == NdOp::STORE)});
      }
      if (Op.Opcode == NdOp::RETURN && State->CallbackOnly) {
        auto *Return = llvm::dyn_cast<llvm::ReturnInst>(IR->getTerminator());
        const auto *Resume = States.cxxContinuation(Op.Addr, Op.OriginSeq);
        if (!Return || !Resume ||
            X86RegistrationCatchIdentity{Resume->TryIndex,
                                         Resume->CatchIndex} !=
                CatchOwners->at(Block.Id) ||
            !BlocksAt.count(Resume->TargetVA) ||
            Resume->SavedStackOffset > -16 ||
            !Layout->runtimeOffset(Resume->SavedStackOffset, 0) ||
            FrameEntrySPOffset > INT32_MAX ||
            !Resumes.emplace(Return, Resume).second)
          return false;
      }
      if (Op.Opcode != NdOp::CALL)
        continue;
      // Synchronous calls need one source dispatch state. A common resume
      // block may join catch states before its next state store or return.
      if (State->Levels.size() > 1)
        return false;
      const auto *Effect = States.callFrameEffect(Op.Addr, Op.OriginSeq);
      llvm::CallInst *Call = nullptr;
      for (const auto &[Candidate, Address] : CallSiteAddrs)
        if (Address == Op.Addr) {
          if (Call)
            return false;
          Call = const_cast<llvm::CallInst *>(Candidate);
        }
      if (!Effect || Effect->CalleeIndex >= States.CalleeContracts.size() ||
          !Call || Call->getParent() != IR || Call->isMustTailCall() ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].ConstVal != Effect->Target || Effect->StackPopBytes)
        return false;
      const auto &Contract = States.CalleeContracts[Effect->CalleeIndex];
      const bool Throw = Contract.isThrow();
      const auto ABI = med_llvm_eh::getCheckedRegistrationCxxCallABI(
          *Img, Contract, *Effect, *Call, Work);
      if (!ABI || (ABI->ThrowInfoVA &&
                   !med_llvm_eh::canMaterializeExternalDataDeclaration(
                       *Mod, makeNdDataSymbol(ABI->ThrowInfoVA), I8, true)))
        return false;
      const auto Name =
          CalleeName(Effect->Target, ABI->RuntimeThrow ? "runtime_throw"
                                     : Throw           ? "throw"
                                     : ABI->BorrowsECX ? "ecx"
                                                       : "leaf");
      if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(*Mod, Name,
                                                                  ABI->Type))
        return false;
      Calls.push_back(
          {Call, Effect, State, Throw, *ABI,
           State->CallbackOnly
               ? CatchOwners->at(Block.Id)
               : X86RegistrationCatchIdentity{UINT32_MAX, UINT32_MAX},
           Name});
      Operations.push_back({Call, &Op, State->BlockId, 2});
      // The shared receipt and fresh callee proof agree that this call never
      // returns. Its lifted post-call stack/register operations cannot execute
      // and the ordinary emitter has already terminated this same block.
      if (Effect->DoesNotReturn)
        break;
    }
  }
  if (Resumes.empty() || Resumes.size() != States.CxxContinuations.size() ||
      Calls.size() != States.CallFrameEffects.size())
    return false;
  for (const auto &[Identity, Catch] : Catches) {
    if (!Catch.Blocks.count(Catch.Handler))
      return false;
    for (auto *Block : Catch.Blocks) {
      for (auto *Pred : llvm::predecessors(Block))
        if (!Catch.Blocks.count(Pred))
          return false;
      for (auto *Succ : llvm::successors(Block))
        if (!Catch.Blocks.count(Succ))
          return false;
    }
  }

  for (uint32_t State = 0; State < Cxx.UnwindMap.size(); ++State) {
    const auto &Action = Cxx.UnwindMap[State];
    if (!Action.ActionVA)
      continue;
    const RegistrationCleanupFrameContract *Contract = nullptr;
    for (const auto &Candidate : States.CleanupContracts)
      if (Candidate.ActionState == State) {
        if (Contract)
          return false;
        Contract = &Candidate;
      }
    const auto Relay =
        getCheckedX86RegistrationCleanupRelayABI(*Img, Action.ActionVA, &Work);
    if (!Contract || !Relay || Contract->RelayTarget != Action.ActionVA ||
        Contract->ObjectFrameOffset != Relay->ObjectFrameOffset ||
        !EH.Registration->cxxSourceFrameOffset(Contract->ObjectFrameOffset) ||
        Contract->Leaf.Target != Relay->Leaf.Target ||
        !Relay->Leaf.CallerPCWrites.empty())
      return false;
    const auto Name = CalleeName(Relay->Leaf.Target, "cleanup.ecx");
    if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(*Mod, Name,
                                                                CleanupType))
      return false;
    const auto Token =
        windows_eh_semantics::getCxxCleanupSemanticToken(EH, Arch::X86, State);
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
    if (!Token)
      return false;
#endif
    Cleanups.emplace(State, CleanupPlan{Contract, Name, Token});
  }
  for (const auto &Plan : Calls) {
    if (Plan.State->Levels.empty() || Plan.State->CallbackOnly)
      continue;
    const auto Level = Plan.State->Levels[0];
    int32_t Walk = Level;
    while (Walk >= 0) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          uint32_t(Walk) >= Cxx.UnwindMap.size())
        return false;
      const auto &Action = Cxx.UnwindMap[Walk];
      if (Action.ActionVA &&
          !llvm::any_of(States.CleanupFrameEffects, [&](const auto &Effect) {
            return Effect.BlockId == Plan.State->BlockId &&
                   Effect.Range.Begin == Plan.State->Range.Begin &&
                   Effect.Range.End == Plan.State->Range.End &&
                   Effect.DispatchLevel == Level && Effect.ActionState == Walk;
          }))
        return false;
      Walk = Action.ToState;
    }
  }

  if (States.ChainAccesses.empty() ||
      States.ChainAccesses.size() != RegistrationChainIR.size())
    return false;
  for (const auto &Access : States.ChainAccesses) {
    auto It = RegistrationChainIR.find({Access.Address, Access.OpSeq});
    if (It == RegistrationChainIR.end() || !It->second ||
        !ChainInstructions.insert(It->second).second)
      return false;
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(It->second);
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(It->second);
    const bool Read =
        Access.AccessKind == RegistrationChainAccess::Kind::ReadPreviousHead ||
        Access.AccessKind == RegistrationChainAccess::Kind::ReadInstalledHead;
    auto *Pointer = Load    ? Load->getPointerOperand()
                    : Store ? Store->getPointerOperand()
                            : nullptr;
    if (Read != bool(Load) || !Pointer ||
        !llvm::isa<llvm::ConstantPointerNull>(Pointer) ||
        Pointer->getType()->getPointerAddressSpace() != 257 ||
        (Load && (!Load->getType()->isIntegerTy(32) || Load->isAtomic() ||
                  Load->isVolatile())) ||
        (Store && (!Store->getValueOperand()->getType()->isIntegerTy(32) ||
                   Store->isAtomic() || Store->isVolatile())))
      return false;
  }
  for (auto &Block : Parent)
    for (auto &I : Block) {
      if (I.isEHPad() || llvm::isa<llvm::InvokeInst>(I) ||
          llvm::isa<llvm::CallBrInst>(I) ||
          llvm::isa<llvm::IndirectBrInst>(I) || llvm::isa<llvm::ResumeInst>(I))
        return false;
      for (const auto &Operand : I.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257 &&
            !ChainInstructions.count(&I))
          return false;
    }

  if (EH.Registration->hasCxxCallbackStack())
    for (auto &[Identity, Catch] : Catches) {
      const auto SourceBlock =
          llvm::find_if(Func.Blocks, [&](const auto &Block) {
            return Block.StartAddr == Cxx.TryBlocks[Identity.first]
                                          .Handlers[Identity.second]
                                          .HandlerVA;
          });
      if (SourceBlock == Func.Blocks.end())
        return false;
      std::vector<llvm::StoreInst *> Stores;
      for (const auto &Event : Operations)
        if (Event.Kind == 1 && Catch.Blocks.count(Event.IR->getParent()))
          Stores.push_back(llvm::cast<llvm::StoreInst>(Event.IR));
      const std::vector<llvm::BasicBlock *> Body(Catch.Blocks.begin(),
                                                 Catch.Blocks.end());
      auto Plan = prepareRegistrationCxxStack(*SourceBlock, *Catch.Handler,
                                              Body, VarAllocs, Stores);
      if (!Plan) {
        llvm::consumeError(Plan.takeError());
        return false;
      }
      Catch.Stack = *Plan;
    }

  const auto Incoming = x86_registration::prepareIncomingFrame(
      Func, Parent, RegistrationIncomingIR);
  if (!Incoming)
    return false;

  // Commit only after the source frame, calls, catches and resumption closure
  // are closed. The C++ runtime outlines these catch/cleanup pads; the whole
  // logical source frame remains one escaped alloca across every funclet.
  constexpr auto Model =
      windows_eh_md::NativeProvenanceModel::X86RegistrationCxx;
  using Role = windows_eh_md::NativeProvenanceRole;
  auto *Personality = llvm::cast<llvm::Function>(
      Mod->getOrInsertFunction("__CxxFrameHandler3", PersonalityType)
          .getCallee());
  Parent.setPersonalityFn(Personality);
  rewrite_source::setOriginalVA(*Personality, Runtime->RuntimeVA);
  Parent.setCallingConv(*EntryABI);
  Parent.addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent.addFnAttr("frame-pointer", "all");
  Parent.addFnAttr(llvm::Attribute::NoInline);
  Parent.addFnAttr(llvm::Attribute::OptimizeNone);
  x86_registration::IncomingFrameProjection IncomingProjection(
      Parent, EH.CodeRange.Begin, *Incoming);
  llvm::SmallVector<llvm::Value *, 2> Escaped{FrameAlloca};
  if (auto *Slot = IncomingProjection.slot())
    Escaped.push_back(Slot);
  llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
  Entry.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                       Mod, llvm::Intrinsic::localescape),
                   Escaped);
  IncomingProjection.commit();
  FrameAlloca->setMetadata(
      windows_eh_md::RegistrationFrameAttachment,
      llvm::MDNode::get(*Ctx,
                        {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                         med_llvm_eh::mdUInt(*Ctx, FrameEntrySPOffset, 64)}));
  // Bind retained source occurrences to exact block intervals before calls
  // split the blocks and before the runtime catch entry is installed.
  auto *SideEffect =
      llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::sideeffect);
  std::map<int, llvm::CallInst *> SourceExits;
  for (const auto &Block : Func.Blocks) {
    auto State = BlockStates.find(Block.Id);
    if (State == BlockStates.end())
      continue;
    auto *IR = OriginalBlockMap.at(Block.Id);
    for (bool Enter : {true, false}) {
      llvm::IRBuilder<> B(Enter ? &*IR->getFirstInsertionPt()
                                : IR->getTerminator());
      auto *Anchor = B.CreateCall(SideEffect);
      Anchor->setMetadata(
          windows_eh_md::RegistrationBlockAttachment,
          llvm::MDNode::get(
              *Ctx, {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                     med_llvm_eh::mdUInt(*Ctx, Block.StartAddr, 64),
                     med_llvm_eh::mdUInt(*Ctx, Block.EndAddr, 64),
                     med_llvm_eh::mdUInt(*Ctx, State->second->BlockId, 32),
                     med_llvm_eh::mdUInt(*Ctx, Enter, 1)}));
      if (!Enter)
        SourceExits.emplace(State->second->BlockId, Anchor);
    }
  }
  auto *None = llvm::ConstantTokenNone::get(*Ctx);
  std::vector<llvm::BasicBlock *> Dispatches;
  for (uint32_t Index = 0; Index < Cxx.TryBlocks.size(); ++Index)
    Dispatches.push_back(llvm::BasicBlock::Create(
        *Ctx, "registration.cxx.dispatch." + std::to_string(Index), &Parent));
  std::map<int32_t, llvm::BasicBlock *> Unwinds;
  std::function<llvm::BasicBlock *(int32_t)> UnwindAt = [&](int32_t State) {
    if (State < 0 || uint32_t(State) >= UnwindGraph->size())
      return static_cast<llvm::BasicBlock *>(nullptr);
    const auto &Target = (*UnwindGraph)[State];
    if (Target.TargetKind == X86RegistrationCxxUnwindTarget::Kind::Caller)
      return static_cast<llvm::BasicBlock *>(nullptr);
    if (Target.TargetKind == X86RegistrationCxxUnwindTarget::Kind::Try)
      return Dispatches[Target.Index];
    State = Target.Index;
    auto Found = Unwinds.find(State);
    if (Found != Unwinds.end())
      return Found->second;
    const auto &Action = Cxx.UnwindMap[State];
    auto *Outer = UnwindAt(Action.ToState);
    const auto &Plan = Cleanups.at(State);
    auto *Block = llvm::BasicBlock::Create(
        *Ctx, "registration.cxx.cleanup." + std::to_string(State), &Parent);
    llvm::IRBuilder<> B(Block);
    auto *Cleanup = B.CreateCleanupPad(None);
    if (Plan.Token &&
        !med_llvm_eh::attachRewriteWinEHSemanticToken(*Cleanup, *Plan.Token))
      llvm_unreachable("prevalidated C++ cleanup semantic token rejected");
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationCallback, EH.CodeRange.Begin,
        Action.ActionVA, State, 0, Cleanup, Plan.Contract->Leaf.Target, 1);
    auto *Callee = llvm::cast<llvm::Function>(
        Mod->getOrInsertFunction(Plan.Name, CleanupType).getCallee());
    Callee->setCallingConv(llvm::CallingConv::X86_ThisCall);
    rewrite_source::setOriginalVA(*Callee, Plan.Contract->Leaf.Target);
    auto *Address =
        B.CreateInBoundsGEP(I8, FrameAlloca,
                            B.getInt32(Layout->Establisher +
                                       *EH.Registration->cxxSourceFrameOffset(
                                           Plan.Contract->ObjectFrameOffset)));
    auto *Call = B.CreateCall(Callee, {Address},
                              {llvm::OperandBundleDef("funclet", Cleanup)});
    Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    Call->setDoesNotThrow();
    B.CreateCleanupRet(Cleanup, Outer);
    Unwinds.emplace(State, Block);
    return Block;
  };
  std::vector<llvm::BasicBlock *> OuterUnwinds;
  for (const auto &Try : Cxx.TryBlocks)
    OuterUnwinds.push_back(UnwindAt(Cxx.UnwindMap[Try.TryLow].ToState));
  emitRegistrationCxxCatches(EH, Catches, Dispatches, OuterUnwinds,
                             *FrameAlloca, Parent);
  auto ProtectedCall = [&](const RegistrationBlockState &State) {
    if (State.Levels.empty())
      return false;
    const int32_t Level = State.Levels.front();
    return llvm::any_of(State.CxxSearches, [&](const auto &Search) {
      return Search.Level == Level;
    });
  };
  for (const auto &Plan : Calls) {
    auto *Old = Plan.Call;
    llvm::IRBuilder<> B(Old);
    auto *Callee = llvm::cast<llvm::Function>(
        Mod->getOrInsertFunction(Plan.Name, Plan.ABI.Type).getCallee());
    Callee->setCallingConv(Plan.ABI.Convention);
    rewrite_source::setOriginalVA(*Callee, Plan.Effect->Target);
    if (Plan.Throw)
      Callee->addFnAttr(llvm::Attribute::NoReturn);
    llvm::SmallVector<llvm::Value *, 2> Args;
    if (Plan.ABI.RuntimeThrow) {
      if (Plan.ABI.ThrowInfoVA) {
        auto *Object = Old->getArgOperand(0);
        Args.push_back(Object->getType()->isPointerTy()
                           ? Object
                           : B.CreateIntToPtr(Object, Ptr));
        const auto Name = makeNdDataSymbol(Plan.ABI.ThrowInfoVA);
        auto *Table = Mod->getNamedGlobal(Name);
        if (!Table)
          Table = new llvm::GlobalVariable(*Mod, I8, true,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, Name);
        Args.push_back(Table);
      } else
        Args.assign(2, llvm::ConstantPointerNull::get(Ptr));
    }
    if (Plan.ABI.BorrowsECX)
      Args.push_back(B.CreateInBoundsGEP(
          I8, FrameAlloca,
          B.getInt32(Layout->Establisher + *Plan.Effect->ECXFrameOffset)));
    const int32_t Level =
        Plan.State->Levels.empty() ? -1 : Plan.State->Levels.front();
    auto *Unwind = ProtectedCall(*Plan.State) ? UnwindAt(Level) : nullptr;
    llvm::SmallVector<llvm::OperandBundleDef, 1> Bundles;
    if (Plan.State->CallbackOnly)
      Bundles.emplace_back("funclet", Catches.at(Plan.CatchIdentity).Pad);
    llvm::CallBase *Call = nullptr;
    if (Unwind) {
      auto *Block = Old->getParent();
      auto *Continue = Block->splitBasicBlock(Old->getNextNode(),
                                              "registration.cxx.call.cont");
      Block->getTerminator()->eraseFromParent();
      B.SetInsertPoint(Old);
      Call = B.CreateInvoke(Callee, Continue, Unwind, Args, Bundles);
      B.SetInsertPoint(Call);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::ProtectedInvoke, EH.CodeRange.Begin,
          Plan.Effect->Address, uint32_t(Level), Plan.Effect->OpSeq, nullptr,
          Plan.Effect->Target, Plan.Throw);
    } else {
      Call = B.CreateCall(Callee, Args, Bundles);
    }
    Call->setCallingConv(Callee->getCallingConv());
    if (Plan.Throw)
      Call->setDoesNotReturn();
    Call->copyMetadata(*Old);
    if (Plan.Throw) {
      // A noreturn normal edge cannot execute an anchor after the invoke.
      // Bind the terminal source boundary to the call occurrence itself.
      auto *Exit = SourceExits.at(Plan.State->BlockId);
      Call->setMetadata(
          windows_eh_md::RegistrationBlockAttachment,
          Exit->getMetadata(windows_eh_md::RegistrationBlockAttachment));
      Exit->eraseFromParent();
    }
    for (auto &Operation : Operations)
      if (Operation.IR == Old)
        Operation.IR = Call;
    if (!Plan.Throw) {
      llvm::Value *Result = Call;
      if (Old->getType() != Call->getType()) {
        if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call))
          B.SetInsertPoint(&*Invoke->getNormalDest()->getFirstInsertionPt());
        else
          B.SetInsertPoint(Old);
        Result = B.CreateZExt(Call, Old->getType());
      }
      Old->replaceAllUsesWith(Result);
    }
    CallSiteAddrs.erase(Old);
    Old->eraseFromParent();
  }
  for (const auto &[Return, Resume] : Resumes)
    emitRegistrationCxxContinuation(
        *Return, *FrameAlloca, Layout->Establisher,
        *EH.Registration->RegistrationOffset - 4,
        *Catches.at({Resume->TryIndex, Resume->CatchIndex}).Pad,
        *BlocksAt.at(Resume->TargetVA), EH.CodeRange.Begin, *Resume);
  for (const auto &Access : States.ChainAccesses) {
    auto *I = RegistrationChainIR.at({Access.Address, Access.OpSeq});
    llvm::IRBuilder<> B(I);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationChainAccess, EH.CodeRange.Begin,
        Access.Address, 0, Access.OpSeq, nullptr, Access.EndAddress,
        static_cast<uint32_t>(Access.AccessKind));
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(I)) {
      llvm::Value *Value;
      if (Access.AccessKind ==
          RegistrationChainAccess::Kind::ReadPreviousHead) {
        auto *Head = B.CreateLoad(I32, Load->getPointerOperand());
        Value = B.CreateLoad(I32, B.CreateIntToPtr(Head, Ptr));
        llvm::cast<llvm::LoadInst>(Value)->setAlignment(llvm::Align(1));
      } else {
        Value = B.CreatePtrToInt(
            B.CreateInBoundsGEP(
                I8, FrameAlloca,
                B.getInt32(Layout->Establisher +
                           *EH.Registration->RegistrationOffset)),
            I32);
      }
      Load->replaceAllUsesWith(Value);
    }
    I->eraseFromParent();
  }
  for (const auto &Event : Operations) {
    Event.IR->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(
            *Ctx, {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Source->Addr, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Source->OriginSeq, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.SourceBlock, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.Kind, 8)}));
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Event.IR))
      Load->setVolatile(true);
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Event.IR))
      Store->setVolatile(true);
  }
  RegistrationChainIR.clear();
  RegistrationIncomingIR.clear();
  RegistrationMemoryIR.clear();
  CallSiteAddrs.clear();
  llvm::removeUnreachableBlocks(Parent);
  Parent.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {med_llvm_eh::mdUInt(*Ctx, 1, 1),
                 llvm::MDString::get(*Ctx, "cxx-x86-registration-native")}));
  const auto Protected = llvm::count_if(
      Calls, [&](const CallPlan &Plan) { return ProtectedCall(*Plan.State); });
  exception_rewrite::setContract(
      Parent, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, Protected, Protected, 0);
  return true;
#endif
}

} // namespace neverd
