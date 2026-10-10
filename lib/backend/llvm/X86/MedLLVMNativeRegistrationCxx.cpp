//===- MedLLVMNativeRegistrationCxx.cpp - Native PE32 C++ EH --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMRegistrationCxxContinuation.h"
#include "MedLLVMRegistrationCxxStack.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/TargetRegInfo.h"
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
      !States.IncomingFrameAccesses.empty() ||
      !States.CallFrameEffectsComplete || !States.CleanupFrameEffectsComplete ||
      !States.CxxContinuationsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete || !States.ImageReadsComplete)
    return false;
  const auto &Cxx = *EH.Cxx;
  if (States.CxxCatchObjects.size() != 1)
    return false;
  const auto &Try = Cxx.TryBlocks[0];
  const auto &Catch = Try.Handlers[0];
  const auto &Object = States.CxxCatchObjects[0];
  const auto FrameBytes = FrameAlloca->getAllocationSize(Mod->getDataLayout());
  const auto Coordinate =
      EH.Registration->RealignedFrame
          ? realignedRegistrationFrameCoordinate(EH, &States)
          : std::optional(RegistrationFrameCoordinate{-4, 1, 0});
  const auto Layout =
      Coordinate && FrameBytes && !FrameBytes->isScalable()
          ? projectX86RegistrationFrame(
                *Coordinate, FrameBytes->getFixedValue(), FrameEntrySPOffset,
                FrameAlloca->getAlign().value())
          : std::nullopt;
  if (!Layout)
    return false;
  const int64_t ObjectOffset =
      int64_t(Layout->Establisher) + Object.FrameOffset;
  if (!FrameBytes || FrameBytes->isScalable() ||
      FrameBytes->getFixedValue() > UINT32_MAX || Object.TryIndex ||
      Object.CatchIndex || Object.TypeDescriptorVA != Catch.TypeDescriptorVA ||
      Object.FrameOffset != Catch.CatchObjectOffset || !Object.ObjectSize ||
      Object.Reference != (Catch.Adjectives == 8) || ObjectOffset < 0 ||
      uint64_t(ObjectOffset) + (Object.Reference ? 4 : Object.ObjectSize) >
          FrameBytes->getFixedValue())
    return false;

  // Preserve an actual incoming ECX as ECX. A guessed cdecl formal would read
  // an extra caller-stack word even when the source only spills this register.
  // No additional register/stack parameters inherit that physical ABI.
  const auto &TRI = getTargetRegInfo(Arch::X86);
  const bool EntryECX = Func.Params.size() == 1;
  if (Func.Params.size() != Parent.arg_size() || Func.Params.size() > 1 ||
      (EntryECX && (Func.Params[0].RegOff != TRI.IntParamRegs[0] ||
                    Func.Params[0].Size != 4 ||
                    !Parent.getArg(0)->getType()->isIntegerTy(32))))
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
      if (State->second->Levels.size() > 1 ||
          !Matched.insert(State->second).second)
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
  const auto HandlerIt = BlocksAt.find(Catch.HandlerVA);
  if (HandlerIt == BlocksAt.end())
    return false;
  auto *Handler = HandlerIt->second;
  auto CatchToken =
      windows_eh_semantics::getCxxCatchSemanticToken(EH, Arch::X86, 0, 0);
  if (!CatchToken)
    return false;

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
    bool Borrow;
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
  std::set<llvm::BasicBlock *> CatchBlocks;
  std::set<llvm::Instruction *> ChainInstructions;
  size_t Work = 0;
  auto *I8 = llvm::Type::getInt8Ty(*Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto *Void = llvm::Type::getVoidTy(*Ctx);
  auto *Ptr = llvm::PointerType::get(*Ctx, 0);
  auto *ThrowType = llvm::FunctionType::get(Void, false);
  auto *BorrowType = llvm::FunctionType::get(I32, {Ptr}, false);
  auto *LeafType = llvm::FunctionType::get(I32, false);
  auto *CleanupType = llvm::FunctionType::get(Void, {Ptr}, false);
  auto *PersonalityType = llvm::FunctionType::get(I32, {}, true);
  auto CalleeName = [](va_t Target, llvm::StringRef ABI) {
    return "__nd_registration_" + ABI.str() + "_" + llvm::utohexstr(Target);
  };
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, "__CxxFrameHandler3", PersonalityType) ||
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
    if (State->CallbackOnly) {
      if (State->CxxMinimumTryLevel != Try.TryHigh + 1)
        return false;
      CatchBlocks.insert(IR);
    }
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
        if (!Return || !Resume || Resume->TryIndex || Resume->CatchIndex ||
            !BlocksAt.count(Resume->TargetVA) ||
            Resume->SavedStackOffset > -16 ||
            !Layout->runtimeOffset(Resume->SavedStackOffset, 0) ||
            FrameEntrySPOffset > INT32_MAX ||
            !Resumes.emplace(Return, Resume).second)
          return false;
      }
      if (Op.Opcode != NdOp::CALL)
        continue;
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
      const bool Throw = Contract.CalleeKind ==
                         RegistrationCalleeFrameContract::Kind::PrivateThrow;
      bool Borrow = false;
      if (Contract.Target != Effect->Target ||
          Contract.DoesNotReturn != Effect->DoesNotReturn)
        return false;
      if (Throw) {
        const auto Proof = getCheckedX86RegistrationThrowCalleeABI(
            *Img, Effect->Target, &Work);
        if (!Proof || !Effect->DoesNotReturn || Effect->ECXFrameOffset ||
            !Call->use_empty())
          return false;
      } else {
        const auto Proof =
            getCheckedX86RegistrationLeafCalleeABI(*Img, Effect->Target, &Work);
        if (!Proof || !Proof->HasIndependentScalarReturn ||
            Proof->StackPopBytes || Effect->DoesNotReturn ||
            (!Call->getType()->isIntegerTy(32) &&
             !Call->getType()->isIntegerTy(64)))
          return false;
        Borrow = !Proof->ECXReads.empty() || !Proof->ECXWrites.empty();
        if (Borrow != Effect->ECXFrameOffset.has_value())
          return false;
      }
      const auto Name = CalleeName(Effect->Target, Throw    ? "throw"
                                                   : Borrow ? "ecx"
                                                            : "leaf");
      if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
              *Mod, Name,
              Throw    ? ThrowType
              : Borrow ? BorrowType
                       : LeafType))
        return false;
      Calls.push_back({Call, Effect, State, Throw, Borrow, Name});
      Operations.push_back({Call, &Op, State->BlockId, 2});
      // The shared receipt and fresh callee proof agree that this call never
      // returns. Its lifted post-call stack/register operations cannot execute
      // and the ordinary emitter has already terminated this same block.
      if (Effect->DoesNotReturn)
        break;
    }
  }
  if (!CatchBlocks.count(Handler) || Resumes.empty() ||
      Resumes.size() != States.CxxContinuations.size() ||
      Calls.size() != States.CallFrameEffects.size())
    return false;
  for (auto *Block : CatchBlocks)
    for (auto *Pred : llvm::predecessors(Block))
      if (!CatchBlocks.count(Pred))
        return false;
  for (auto *Block : CatchBlocks)
    for (auto *Succ : llvm::successors(Block))
      if (!CatchBlocks.count(Succ))
        return false;

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
    if (Plan.State->Levels.empty() ||
        Plan.State->CxxMinimumTryLevel > Try.TryLow)
      continue;
    const auto Level = Plan.State->Levels[0];
    if (Level < Try.TryLow || Level > Try.TryHigh)
      continue;
    int32_t Walk = Level;
    while (Walk != Try.TryLow) {
      if (++Work > limits::kMaxRegistrationEHStateWork || Walk < Try.TryLow)
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

  std::optional<RegistrationCxxStackPlan> Stack;
  if (EH.Registration->RealignedFrame) {
    const auto SourceBlock = llvm::find_if(Func.Blocks, [&](const auto &Block) {
      return Block.StartAddr == Catch.HandlerVA;
    });
    if (SourceBlock == Func.Blocks.end())
      return false;
    std::vector<llvm::StoreInst *> Stores;
    for (const auto &Event : Operations)
      if (Event.Kind == 1 && CatchBlocks.count(Event.IR->getParent()))
        Stores.push_back(llvm::cast<llvm::StoreInst>(Event.IR));
    const std::vector<llvm::BasicBlock *> Body(CatchBlocks.begin(),
                                               CatchBlocks.end());
    auto Plan = prepareRegistrationCxxStack(*SourceBlock, *Handler, Body,
                                            VarAllocs, Stores);
    if (!Plan) {
      llvm::consumeError(Plan.takeError());
      return false;
    }
    Stack = *Plan;
  }

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
  Parent.setCallingConv(EntryECX ? llvm::CallingConv::X86_ThisCall
                                 : llvm::CallingConv::C);
  Parent.addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent.addFnAttr("frame-pointer", "all");
  Parent.addFnAttr(llvm::Attribute::NoInline);
  Parent.addFnAttr(llvm::Attribute::OptimizeNone);
  llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
  Entry.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                       Mod, llvm::Intrinsic::localescape),
                   {FrameAlloca});
  FrameAlloca->setMetadata(
      windows_eh_md::RegistrationFrameAttachment,
      llvm::MDNode::get(*Ctx,
                        {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                         med_llvm_eh::mdUInt(*Ctx, FrameEntrySPOffset, 64)}));
  auto *Type = Mod->getNamedGlobal(makeNdDataSymbol(Catch.TypeDescriptorVA));
  if (!Type)
    Type = new llvm::GlobalVariable(*Mod, I8, false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    makeNdDataSymbol(Catch.TypeDescriptorVA));
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
  auto *Dispatch =
      llvm::BasicBlock::Create(*Ctx, "registration.cxx.dispatch", &Parent);
  llvm::IRBuilder<> D(Dispatch);
  auto *Switch = D.CreateCatchSwitch(None, nullptr, 1);
  Switch->addHandler(Handler);
  llvm::IRBuilder<> H(&*Handler->getFirstInsertionPt());
  auto *Pad = H.CreateCatchPad(
      Switch, {Type, H.getInt32(Catch.Adjectives), FrameAlloca});
  Pad->setMetadata(
      llvm::RewriteWinX86CxxCatchObjectAttachment,
      llvm::MDNode::get(
          *Ctx, {med_llvm_eh::mdUInt(*Ctx, 1, 32),
                 med_llvm_eh::mdUInt(*Ctx, ObjectOffset, 32),
                 med_llvm_eh::mdUInt(
                     *Ctx, Object.Reference ? 4 : Object.ObjectSize, 32)}));
  if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, *CatchToken))
    llvm_unreachable("prevalidated C++ catch semantic token rejected");
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      H, Model, Role::RegionDispatch, EH.CodeRange.Begin, Catch.HandlerVA, 0, 0,
      Pad, Catch.TypeDescriptorVA, Catch.Adjectives);

  if (Stack)
    emitRegistrationCxxStack(*Stack, EH.CodeRange.Begin, Catch.HandlerVA);

  std::map<int32_t, llvm::BasicBlock *> Unwinds;
  Unwinds.emplace(Try.TryLow, Dispatch);
  std::function<llvm::BasicBlock *(int32_t)> UnwindAt = [&](int32_t State) {
    if (State < Try.TryLow || State > Try.TryHigh)
      return static_cast<llvm::BasicBlock *>(nullptr);
    auto Found = Unwinds.find(State);
    if (Found != Unwinds.end())
      return Found->second;
    const auto &Action = Cxx.UnwindMap[State];
    auto *Outer = UnwindAt(Action.ToState);
    if (!Action.ActionVA) {
      Unwinds.emplace(State, Outer);
      return Outer;
    }
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
    auto *Address = B.CreateInBoundsGEP(
        I8, FrameAlloca,
        B.getInt32(Layout->Establisher + Plan.Contract->ObjectFrameOffset));
    auto *Call = B.CreateCall(Callee, {Address},
                              {llvm::OperandBundleDef("funclet", Cleanup)});
    Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    Call->setDoesNotThrow();
    B.CreateCleanupRet(Cleanup, Outer);
    Unwinds.emplace(State, Block);
    return Block;
  };
  for (const auto &Plan : Calls) {
    auto *Old = Plan.Call;
    llvm::IRBuilder<> B(Old);
    auto *Callee = llvm::cast<llvm::Function>(
        Mod->getOrInsertFunction(Plan.Name, Plan.Throw    ? ThrowType
                                            : Plan.Borrow ? BorrowType
                                                          : LeafType)
            .getCallee());
    Callee->setCallingConv(Plan.Borrow ? llvm::CallingConv::X86_ThisCall
                                       : llvm::CallingConv::C);
    rewrite_source::setOriginalVA(*Callee, Plan.Effect->Target);
    if (Plan.Throw)
      Callee->addFnAttr(llvm::Attribute::NoReturn);
    llvm::SmallVector<llvm::Value *, 1> Args;
    if (Plan.Borrow)
      Args.push_back(B.CreateInBoundsGEP(
          I8, FrameAlloca,
          B.getInt32(Layout->Establisher + *Plan.Effect->ECXFrameOffset)));
    const int32_t Level =
        Plan.State->Levels.empty() ? -1 : Plan.State->Levels.front();
    auto *Unwind = Plan.State->CxxMinimumTryLevel <= Try.TryLow
                       ? UnwindAt(Level)
                       : nullptr;
    llvm::CallBase *Call = nullptr;
    if (Unwind) {
      auto *Block = Old->getParent();
      auto *Continue = Block->splitBasicBlock(Old->getNextNode(),
                                              "registration.cxx.call.cont");
      Block->getTerminator()->eraseFromParent();
      B.SetInsertPoint(Old);
      Call = B.CreateInvoke(Callee, Continue, Unwind, Args);
      B.SetInsertPoint(Call);
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::ProtectedInvoke, EH.CodeRange.Begin,
          Plan.Effect->Address, uint32_t(Level), Plan.Effect->OpSeq, nullptr,
          Plan.Effect->Target, Plan.Throw);
    } else {
      llvm::SmallVector<llvm::OperandBundleDef, 1> Bundles;
      if (Plan.State->CallbackOnly)
        Bundles.emplace_back("funclet", Pad);
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
    emitRegistrationCxxContinuation(*Return, *FrameAlloca, Layout->Establisher,
                                    *Pad, *BlocksAt.at(Resume->TargetVA),
                                    EH.CodeRange.Begin, *Resume);
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
            B.CreateInBoundsGEP(I8, FrameAlloca,
                                B.getInt32(Layout->Establisher - 12)),
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
  RegistrationMemoryIR.clear();
  CallSiteAddrs.clear();
  llvm::removeUnreachableBlocks(Parent);
  Parent.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {med_llvm_eh::mdUInt(*Ctx, 1, 1),
                 llvm::MDString::get(*Ctx, "cxx-x86-registration-native")}));
  const auto Protected = llvm::count_if(Calls, [&](const CallPlan &Plan) {
    const auto Level = Plan.State->Levels.empty() ? -1 : Plan.State->Levels[0];
    return Plan.State->CxxMinimumTryLevel <= Try.TryLow &&
           Level >= Try.TryLow && Level <= Try.TryHigh;
  });
  exception_rewrite::setContract(
      Parent, exception_rewrite::SourceState::Complete,
      exception_rewrite::LoweringState::Complete, Protected, Protected, 0);
  return true;
#endif
}

} // namespace neverd
