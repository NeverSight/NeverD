//===- RegistrationCxxState.cpp - Current-image C++ call evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind ordinary and invocation-specific C++ state proofs to current code.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationCxxState.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationConditionalFrame.h"
#include "RegistrationFrame.h"
#include "RegistrationFrameEffects.h"
#include "RegistrationMemoryPermissions.h"
#include "RegistrationOrdinaryGraph.h"
#include "RegistrationSourceCall.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace neverd {

namespace {
using CxxCalleeQuery = std::pair<const BinaryImage *, va_t>;
thread_local std::set<CxxCalleeQuery> ActiveCxxCallees;

bool hasCurrentCxxDispatchOperations(const BinaryImage &Image,
                                     const LowFunc &Function, size_t &Work) {
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  Dec.setStrict(true);
  for (const auto &Block : Function.Blocks) {
    size_t First = 0;
    va_t Address = Block.StartAddr;
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Work, Boundary.OpCount + 1) ||
          Boundary.FirstOp != First || Boundary.Address != Address ||
          !Boundary.Size || Address > Block.EndAddr ||
          Boundary.Size > Block.EndAddr - Address || First > Block.Ops.size() ||
          Boundary.OpCount > Block.Ops.size() - First ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Boundary,
                                                           Dec))
        return false;
      const auto *Bytes = Image.readVA(Address, Boundary.Size);
      DecodedInsn Insn;
      if (!Bytes || !Dec.decodeOne(Bytes, Boundary.Size, Address, Insn) ||
          Insn.Size != Boundary.Size)
        return false;
      const auto Relocations = instructionRelocations(Image, Insn);
      std::vector<LowOp> Expected;
      try {
        Dec.liftToLow(Insn, Expected, Relocations.Addresses,
                      Relocations.Scalars);
      } catch (const UnliftedInstruction &) {
        return false;
      }
      if (!registration_abi::chargeCalleeWork(Work, Expected.size() + 1) ||
          Expected.size() != Boundary.OpCount ||
          !std::equal(Expected.begin(), Expected.end(),
                      Block.Ops.begin() + First))
        return false;
      First += Expected.size();
      Address += Boundary.Size;
    }
    if (First != Block.Ops.size() || Address != Block.EndAddr)
      return false;
  }
  return true;
}

bool matchesCxxDispatchLeaf(const RegistrationLeafCalleeABI &Leaf,
                            const RegistrationCalleeFrameContract &Contract,
                            size_t &Work) {
  if (!registration_abi::chargeCalleeWork(
          Work, Contract.ECXReads.size() + Contract.ECXWrites.size() +
                    Contract.ImageReads.size() + Contract.ImageWrites.size() +
                    Contract.CallerPCWrites.size() +
                    Contract.CodeRanges.size() + 1))
    return false;
  const auto SameRanges = [](const auto &A, const auto &B) {
    return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin(),
                                              [](const auto &X, const auto &Y) {
                                                return X.Begin == Y.Begin &&
                                                       X.End == Y.End;
                                              });
  };
  return Leaf.PreservesBoundDispatchBindings &&
         Contract.CalleeKind == RegistrationCalleeFrameContract::Kind::Leaf &&
         Contract.Target == Leaf.Target && !Contract.DoesNotReturn &&
         !Leaf.StackPopBytes && !Contract.StackPopBytes &&
         Contract.RequiredImports.empty() && Contract.ECXInitialized.empty() &&
         Contract.ECXReads == Leaf.ECXReads &&
         Contract.ECXWrites == Leaf.ECXWrites &&
         SameRanges(Contract.ImageReads, Leaf.ImageReads) &&
         SameRanges(Contract.ImageWrites, Leaf.ImageWrites) &&
         SameRanges(Contract.CallerPCWrites, Leaf.CallerPCWrites) &&
         SameRanges(Contract.CodeRanges, Leaf.CodeRanges);
}

void proveScalarCxxDispatch(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    registration_state::CxxCallerProof &Calls, size_t &Work) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Cxx ||
      !Function.ExceptionMetadata->Registration || !Callees)
    return;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Cxx = *EH.Cxx;
  if (EH.Registration->RealignedFrame || !Cxx.IsSynchronous || Cxx.IsNoExcept ||
      Cxx.hasExceptionSpecification() ||
      !registration_abi::chargeCalleeWork(Work, Cxx.UnwindMap.size() + 1) ||
      !coff_loader::getCheckedX86CxxHandlerABI(Image, EH) ||
      !coff_loader::getCheckedX86CxxMetadataRanges(Image, EH) ||
      !hasCurrentCxxDispatchOperations(Image, Function, Work))
    return;
  std::set<uint32_t> ClosedCleanups;
  for (uint32_t State = 0; State < Cxx.UnwindMap.size(); ++State) {
    const auto &Action = Cxx.UnwindMap[State];
    if (!Action.ActionVA)
      continue;
    if (!Cleanups ||
        !registration_abi::chargeCalleeWork(Work, Cleanups->size() + 1) ||
        Action.Kind != CxxUnwindAction::ActionKind::Direct ||
        Action.ObjectOffset)
      return;
    const auto Contract = llvm::find_if(
        *Cleanups, [&](const auto &C) { return C.ActionState == State; });
    const auto Relay =
        getCheckedX86RegistrationCleanupRelayABI(Image, Action.ActionVA, &Work);
    if (Contract == Cleanups->end() || !Relay || Relay->RealignedParent ||
        Contract->RelayTarget != Relay->Target ||
        Contract->Calls.size() != Relay->Calls.size() ||
        !Relay->matchesParentFrame(*EH.Registration))
      return;
    for (size_t Index = 0; Index < Relay->Calls.size(); ++Index) {
      const auto &Current = Relay->Calls[Index];
      const auto &Borrow = Contract->Calls[Index];
      if (!registration_abi::chargeCalleeWork(Work, 1) ||
          Borrow.ObjectFrameOffset != Current.ObjectFrameOffset ||
          !matchesCxxDispatchLeaf(Current.Leaf, Borrow.Leaf, Work))
        return;
    }
    ClosedCleanups.insert(State);
  }
  // Constructors and nested EH parents retain their separate closure. A
  // reached ordinary object leaf must prove both its current closed effects
  // and its own frame borrow, even when it is not an unwind-map action.
  for (const auto &Callee : *Callees)
    if (!registration_abi::chargeCalleeWork(Work, 1) ||
        Callee.isInitializer() || Callee.isCxxParent())
      return;
  Calls.ClosedDispatchCleanups = std::move(ClosedCleanups);
  // Each throw still needs a current scalar object and closed preparation.
  // Each unwind origin separately proves its cleanup object's lifetime.
  Calls.ProveDispatchMemory =
      [&Image](const LowOp &Op,
               const registration_state::FrameTransfer &Transfer,
               size_t &Used) {
        return registration_abi::hasClosedOrdinaryRegistrationMemoryAccess(
            Image, Op, Transfer, sizeof(uint32_t), Used);
      };
  Calls.ProveDispatchCall =
      [&Image, &Function,
       Throws = std::map<va_t, std::optional<RegistrationThrowCalleeABI>>{},
       Objects = std::map<va_t, std::optional<RegistrationThrowObjectABI>>{},
       Leaves = std::map<va_t, std::optional<RegistrationLeafCalleeABI>>{}](
          const LowOp &Op, const RegistrationCalleeFrameContract &Contract,
          const RegistrationCallFrameEffect &Effect, size_t &Used) mutable {
        if (Effect.StackPopBytes ||
            !registration_state::checkedRegistrationDirectCall(Image, Function,
                                                               Op, Used))
          return false;
        if (Contract.CalleeKind ==
                RegistrationCalleeFrameContract::Kind::Leaf &&
            !Effect.DoesNotReturn) {
          if (!registration_abi::chargeCalleeWork(Used, Leaves.size() + 1))
            return false;
          const auto [It, New] = Leaves.try_emplace(Effect.Target);
          if (New)
            It->second = getCheckedX86RegistrationLeafCalleeABI(
                Image, Effect.Target, &Used, RegistrationLeafResult::Discarded);
          const auto &Leaf = It->second;
          return Leaf && matchesCxxDispatchLeaf(*Leaf, Contract, Used) &&
                 (Leaf->PreservesDispatchBindings || Effect.ECXFrameOffset);
        }
        if (!Contract.isThrow() || !Effect.DoesNotReturn)
          return false;
        if (Contract.isRuntimeThrow()) {
          if (!Effect.RuntimeThrow || !getCheckedX86RegistrationThrowImportABI(
                                          Image, Effect.Target, &Used))
            return false;
          const auto &Throw = *Effect.RuntimeThrow;
          if (Throw.isRethrow())
            return true; // The state solver separately binds the live catch.
          if (!registration_abi::chargeCalleeWork(Used, Objects.size() + 1))
            return false;
          const auto [It, New] = Objects.try_emplace(Throw.ThrowInfoVA);
          if (New)
            It->second = getCheckedX86RegistrationThrowObjectABI(
                Image, Throw.ThrowInfoVA, &Used);
          const auto &Object = It->second;
          return Object && Object->PreservesDispatchBindings &&
                 Object->Info.ObjectSize == Throw.ObjectSize;
        }
        if (!registration_abi::chargeCalleeWork(Used, Throws.size() + 1))
          return false;
        const auto [It, New] = Throws.try_emplace(Effect.Target);
        if (New)
          It->second = getCheckedX86RegistrationThrowCalleeABI(
              Image, Effect.Target, &Used);
        const auto &Throw = It->second;
        return Throw && Throw->PreservesDispatchBindings &&
               Throw->IsRethrow == Contract.isRethrow() &&
               (Throw->IsRethrow ||
                (Throw->ThrowInfo.TypeDescriptorVA == Contract.ThrownTypeVA &&
                 Throw->ThrowInfo.ObjectSize == Contract.ThrownObjectSize));
      };
}

RegistrationStateAnalysis solveCheckedCxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls,
    registration_state::ConditionalFrame *Inputs = nullptr,
    size_t *StateWork = nullptr,
    const registration_state::CxxCallerProof *CxxCalls = nullptr) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return {};
  const auto &EH = *Function.ExceptionMetadata;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  const auto Prologue = coff_loader::getCheckedX86CxxPrologueCall(Image, EH);
  std::vector<coff_loader::X86CxxEpilogueCall> Epilogues;
  if (Prologue)
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.Addr != Prologue->CallVA)
          if (const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
                  Image, EH, Op.Addr)) {
            if (Epilogues.size() == limits::kMaxRegistrationEHRecords)
              return {};
            Epilogues.push_back(*Exit);
          }
  const va_t Cookie =
      Image.Base <= UINT32_MAX && Image.DynInfo.SecurityCookieRVA &&
              Image.DynInfo.SecurityCookieRVA <= UINT32_MAX - Image.Base
          ? Image.Base + Image.DynInfo.SecurityCookieRVA
          : 0;
  registration_state::RegistrationStateSolver Solver(
      Function, Cookie, 0, Callees, Cleanups, Stacks, LocalUnwinds, SEHControls,
      Inputs, nullptr, nullptr, Handler ? &*Handler : nullptr,
      Prologue ? &*Prologue : nullptr, &Epilogues, nullptr, nullptr, CxxCalls);
  auto Result = Solver.run(StateWork ? *StateWork : 0);
  if (StateWork)
    *StateWork = Solver.workUsed();
  return Result;
}
} // namespace

// Reject only a current shared-entry prefix that already needs unsupported
// unbound inputs or call effects before any ordinary call or branch. A later
// incoming access or RET pop cannot reject a parent: an earlier call might
// never return.
bool registration_abi::hasUnboundCxxEntryBarrier(const BinaryImage &Image,
                                                 const LowFunc &Function,
                                                 size_t &Work) {
  using namespace registration_state;
  const auto Prefix =
      getCheckedX86RegistrationCxxProloguePrefix(Image, Function);
  if (!Prefix)
    return false;
  Decoder Dec;
  if (!Dec.init(Image) ||
      !registration_abi::chargeCalleeWork(Work, Function.Blocks.size()))
    return false;
  FrameState State;
  State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  const LowBlock *Block = nullptr;
  for (const auto &Candidate : Function.Blocks)
    if (Candidate.StartAddr == Function.Entry)
      Block = &Candidate;
  std::set<int> Seen;
  bool Installed = false;
  while (Block && Seen.insert(Block->Id).second) {
    for (const auto &Insn : Block->InstructionBoundaries)
      if (!registration_abi::chargeCalleeWork(Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Insn, Dec))
        return false;
    FrameTransfer Transfer(State, 0);
    for (const auto &Op : Block->Ops) {
      if (!registration_abi::chargeCalleeWork(Work, 1 + Op.Output.Size +
                                                        Op.NumInputs))
        return false;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::CALL && Op.Addr == Prefix->CallVA && !Installed) {
        if (State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset != 0)
          return false;
        State.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
            FrameValue::frame(0);
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(Prefix->PostCallStackOffset);
        State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
            FrameValue::frame(Prefix->RegistrationOffset);
        Installed = true;
        continue;
      }
      // The unbound native query has direct-callee contracts only. Its first
      // indirect call cannot acquire an invocation effect from later callees.
      // Rejecting here grants no stack cleanup or import-binding semantics.
      if (Installed && Op.Opcode == NdOp::INDIR_CALL)
        return true;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN ||
          (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)))
        return false;
      if (Installed && (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.MemoryOrdering == NdMemoryOrdering::None) {
        const auto Address = Transfer.read(Op.Inputs[0]);
        if (Address.Offset && *Address.Offset >= 8)
          return true;
      }
      Transfer.write(Op, Transfer.evaluate(Op, Installed));
    }
    if (Block->Succs.size() != 1)
      return false;
    if (!registration_abi::chargeCalleeWork(Work, Function.Blocks.size()))
      return false;
    const auto Next = llvm::find_if(Function.Blocks, [&](const LowBlock &B) {
      return B.Id == Block->Succs.front() && B.StartAddr == Block->EndAddr;
    });
    Block = Next == Function.Blocks.end() ? nullptr : &*Next;
  }
  return false;
}

bool registration_abi::hasActiveCxxCalleeProof() {
  return !ActiveCxxCallees.empty();
}

RegistrationStateAnalysis analyzeCheckedX86CxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls,
    RegistrationCallCalleeIndex *CalleeIndex) {
  size_t Work = 0;
  registration_state::CxxCallerProof Calls;
  proveScalarCxxDispatch(Image, Function, Callees, Cleanups, Calls, Work);
  for (const auto &Import : Image.Imports) {
    if (!registration_abi::chargeCalleeWork(Work, 1)) {
      if (CalleeIndex)
        CalleeIndex->chargeStateWork(Work);
      return {};
    }
    Calls.ImportSlots.insert(Import.IATAddr);
  }
  Calls.ProveCall =
      [&](const LowFunc &Caller, const LowOp &Call,
          const registration_state::FrameState &Frame,
          const std::set<int32_t> &Initialized,
          const registration_state::BoundCxxBorrowStorage &Storage,
          size_t &Used)
      -> std::optional<registration_state::BoundCxxCalleeEffect> {
    if (!Frame.ImportBindingsIntact ||
        !registration_state::checkedRegistrationDirectCall(Image, Caller, Call,
                                                           Used))
      return std::nullopt;
    return registration_state::proveBoundCxxCallee(
        Image, Call, Frame, 0, Used, {}, &Initialized, CalleeIndex, &Storage);
  };
  auto States =
      solveCheckedCxxStates(Image, Function, Callees, Cleanups, Stacks,
                            LocalUnwinds, SEHControls, nullptr, &Work, &Calls);
  if (CalleeIndex && !CalleeIndex->chargeStateWork(Work)) {
    States.Complete = States.CallbackStatesComplete =
        States.RegistrationLifetimeComplete = States.ChainOperationsComplete =
            States.SourceCallFrameEffectsComplete =
                States.CallFrameEffectsComplete = false;
    States.CallFrameEffects.clear();
    States.CxxPrologue.reset();
    States.Diagnostics.push_back("C++ callee state proof budget exhausted");
  }
  return States;
}

std::optional<RegistrationCalleeFrameContract>
RegistrationCallCalleeIndex::cxxCallee(va_t Target) {
  using namespace registration_abi;
  const auto *EH = Image.ExceptionMetadata.findFunction(Target);
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.IsRelocatable || !EH ||
      !EH->Cxx || !EH->Registration || EH->CodeRange.Begin != Target ||
      EH->Registration->RealignedFrame || !EH->Cxx->TryBlocks.empty() ||
      !EH->Registration->Scopes.empty() ||
      CxxDepth >= limits::ir::kRegistrationCxxMaxCalleeDepth ||
      !chargeCalleeWork(Work, 1))
    return std::nullopt;
  // A plain object/initializer query can enter through a separate public
  // stack query before returning to this shared index. Never let that route
  // restart a currently active C++ proof with a fresh depth or budget.
  const CxxCalleeQuery Identity{&Image, Target};
  if (ActiveCxxCallees.size() >= limits::ir::kRegistrationCxxMaxCalleeDepth ||
      !ActiveCxxCallees.insert(Identity).second)
    return std::nullopt;
  ++CxxDepth;
  const llvm::scope_exit Leave([&] {
    --CxxDepth;
    ActiveCxxCallees.erase(Identity);
  });
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, *EH);
  auto Metadata = coff_loader::getCheckedX86CxxMetadataRanges(Image, *EH);
  if (!Handler || !Metadata)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(this);
  if (coff_loader::getCheckedX86CxxPrologueCall(Image, *EH)) {
    const LowFunc Ordinary =
        buildOrdinaryCxxFrameGraph(Image, Dec, Target, *this);
    if (!chargeDecodedCallee(Work, Ordinary) ||
        hasUnboundCxxEntryBarrier(Image, Ordinary, Work) ||
        Work == limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
  }
  auto Function =
      Builder.buildIntegerFrameProof(Image, Dec, Target, "closed-cxx-callee");
  if (!Function.hasCompleteLiftCoverage() ||
      !chargeDecodedCallee(Work, Function))
    return std::nullopt;
  RegistrationCalleeFrameContract Result;
  Result.CalleeKind = RegistrationCalleeFrameContract::Kind::CxxParent;
  Result.Target = Target;
  if (!collectCalleeCodeRanges(Function, Image, Work, Result.CodeRanges))
    return std::nullopt;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries)
      if (!chargeCalleeWork(Work, 1) ||
          !hasImmutableCalleeInstruction(Image, Insn, Dec))
        return std::nullopt;
  const auto Callees = contracts(Function);
  const auto Cleanups = cleanupContracts(Function);
  const auto Stacks = stackContracts(Function);
  if (!Callees || !Cleanups || !Stacks)
    return std::nullopt;
  ImageFrameEffects Effects;
  std::set<std::pair<va_t, va_t>> Code;
  std::set<va_t> Imports{Handler->Dispatch.IATVA};
  std::map<va_t, RegistrationRuntimeThrowInfo> Throws;
  auto AddRanges = [&](const auto &From, auto &To) {
    if (!chargeCalleeWork(Work, From.size()))
      return false;
    for (const auto &Range : From)
      To.emplace(Range.Begin, Range.End);
    return true;
  };
  auto AddCallee = [&](const RegistrationCalleeFrameContract &Callee) {
    if (!Callee.CallerPCWrites.empty() || Callee.isRethrow() ||
        !chargeCalleeWork(Work, Callee.RequiredImports.size() +
                                    Callee.RuntimeThrowInfos.size() + 1) ||
        !AddRanges(Callee.ImageReads, Effects.Reads) ||
        !AddRanges(Callee.ImageWrites, Effects.Writes) ||
        !AddRanges(Callee.CodeRanges, Code))
      return false;
    Imports.insert(Callee.RequiredImports.begin(),
                   Callee.RequiredImports.end());
    for (const auto &Info : Callee.RuntimeThrowInfos) {
      const auto [It, New] = Throws.emplace(Info.Address, Info);
      if (!New && It->second != Info)
        return false;
    }
    if (Callee.CalleeKind ==
        RegistrationCalleeFrameContract::Kind::PrivateThrow) {
      const auto Throw =
          getCheckedX86RegistrationThrowCalleeABI(Image, Callee.Target, &Work);
      if (!Throw || Throw->IsRethrow)
        return false;
      const auto &Info = Throw->ThrowInfo;
      const RegistrationRuntimeThrowInfo Value{
          Info.Address, Info.TypeDescriptorVA, Info.ObjectSize};
      const auto [It, New] = Throws.emplace(Info.Address, Value);
      if (!New && It->second != Value)
        return false;
    }
    return true;
  };
  if (!AddRanges(Result.CodeRanges, Code) ||
      !AddRanges(Handler->Dispatch.CodeRanges, Code))
    return std::nullopt;
  for (const auto &Callee : *Callees)
    if (!AddCallee(Callee))
      return std::nullopt;
  for (const auto &Cleanup : *Cleanups) {
    const auto Relay = getCheckedX86RegistrationCleanupRelayABI(
        Image, Cleanup.RelayTarget, &Work);
    if (!Relay || !Relay->matchesParentFrame(*EH->Registration))
      return std::nullopt;
    Code.emplace(Relay->Target, Relay->EndAddress);
    for (const auto &Call : Cleanup.Calls)
      if (!AddCallee(Call.Leaf))
        return std::nullopt;
  }
  registration_state::ConditionalFrame Inputs;
  Inputs.ProveImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                                size_t &Used) {
    if (!hasSEHCallbackImageMemory(Image, Address, Width, Write, Used))
      return false;
    (Write ? Effects.Writes : Effects.Reads).emplace(Address, Address + Width);
    return true;
  };
  auto States =
      solveCheckedCxxStates(Image, Function, &*Callees, &*Cleanups, &*Stacks,
                            nullptr, nullptr, &Inputs, &Work);
  if (!Inputs.Complete || !Inputs.NativeFramePrivate || !States.Complete ||
      !States.CallbackStatesComplete || !States.RegistrationLifetimeComplete ||
      !States.ChainOperationsComplete || !States.CallFrameEffectsComplete ||
      !States.CleanupFrameEffectsComplete || !States.CxxContinuationsComplete ||
      !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete ||
      !States.IncomingFrameAccessesComplete || !States.ImageReadsComplete ||
      (!Handler->CookieGuards.empty() && !States.SecurityCookiesComplete) ||
      !AddRanges(States.ImageReads, Effects.Reads))
    return std::nullopt;
  Result.HasIndependentScalarReturn = !Inputs.ReturnValues.empty();
  for (const auto &[Occurrence, Value] : Inputs.ReturnValues) {
    if (!chargeCalleeWork(Work, 1))
      return std::nullopt;
    Result.HasIndependentScalarReturn &= Value.isFrameIndependentScalar();
  }
  if (const auto Entry = coff_loader::getCheckedX86CxxPrologueCall(Image, *EH))
    Code.emplace(Entry->Helper.Begin, Entry->Helper.End);
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      if (Op.Opcode != NdOp::CALL)
        continue;
      if (const auto Exit =
              coff_loader::getCheckedX86CxxEpilogueCall(Image, *EH, Op.Addr)) {
        Code.emplace(Exit->Helper.Begin, Exit->Helper.End);
        Code.emplace(Exit->Restore.Begin, Exit->Restore.End);
        if (Exit->CookieCheck)
          Code.emplace(Exit->CookieCheck->Checker.Begin,
                       Exit->CookieCheck->Checker.End);
      }
      if (States.cookieCheck(Op.Addr, Op.Seq)) {
        const auto Check = coff_loader::getCheckedX86CookieCheckSuccessPath(
            Image, Op.Inputs[0].Offset);
        if (!Check)
          return std::nullopt;
        Code.emplace(Check->CodeRange.Begin, Check->CodeRange.End);
      }
    }
  for (const auto &[Address, Info] : Throws) {
    const auto Object =
        getCheckedX86RegistrationThrowObjectABI(Image, Address, &Work);
    if (!Object ||
        !chargeCalleeWork(Work, Object->Info.ReadOnlyRanges.size() + 1))
      return std::nullopt;
    Metadata->insert(Metadata->end(), Object->Info.ReadOnlyRanges.begin(),
                     Object->Info.ReadOnlyRanges.end());
    Metadata->push_back(Object->Info.TypeDescriptorRange);
  }
  for (const auto &[Begin, End] : Effects.Writes) {
    if (!chargeCalleeWork(Work, Metadata->size() + Imports.size() + 1))
      return std::nullopt;
    if (llvm::any_of(
            *Metadata,
            [&](const auto &Range) { return Range.overlaps({Begin, End}); }) ||
        llvm::any_of(Imports,
                     [&](va_t Slot) { return Begin < Slot + 4 && Slot < End; }))
      return std::nullopt;
  }
  // Dispatch reads these immutable tables and eager bindings outside the
  // parent's LowIR. Transitive callers must see the same read footprint.
  if (!AddRanges(*Metadata, Effects.Reads) ||
      !chargeCalleeWork(Work, Imports.size()))
    return std::nullopt;
  for (va_t Slot : Imports)
    Effects.Reads.emplace(Slot, Slot + 4);
  Function.RegistrationStates = std::move(States);
  const auto Entry = parentEntryABI(Function, Image, &Work);
  if (!Entry || Entry->PopBytes ||
      Entry->HasNormalReturn != !Inputs.ReturnValues.empty() ||
      !chargeCalleeWork(Work, Effects.Reads.size() + Effects.Writes.size() +
                                  Code.size() + Imports.size() + Throws.size()))
    return std::nullopt;
  Result.DoesNotReturn = !Entry->HasNormalReturn;
  Result.CodeRanges.clear();
  copyExtents(Code, Result.CodeRanges);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  Result.RequiredImports.assign(Imports.begin(), Imports.end());
  for (const auto &[Address, Info] : Throws)
    Result.RuntimeThrowInfos.push_back(Info);
  return Result;
}

std::optional<RegistrationCalleeFrameContract>
getCheckedX86RegistrationCxxCalleeABI(const BinaryImage &Image, va_t Target,
                                      size_t *CumulativeWork) {
  RegistrationCallCalleeIndex Index(Image, CumulativeWork);
  return Index.cxxCallee(Target);
}

namespace registration_state {
std::optional<BoundCxxCalleeEffect>
proveBoundCxxCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, uint32_t StackArgumentBytes,
                    size_t &Work,
                    llvm::ArrayRef<RegistrationObjectExtent> CallerObjects,
                    const std::set<int32_t> *Initialized,
                    RegistrationCallCalleeIndex *OwnerIndex,
                    const BoundCxxBorrowStorage *BorrowStorage) {
  if ((OwnerIndex && !OwnerIndex->ownsImage(Image)) ||
      Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Op.Opcode != NdOp::CALL ||
      Op.Seq < 0 || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Addr > UINT32_MAX - 5 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      StackArgumentBytes > UINT16_MAX || StackArgumentBytes % 4 ||
      !registration_abi::chargeCalleeWork(Work, 5))
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  // Invocation inputs are never cached by target. Cycles and deep acyclic
  // chains refuse before another decoder/solver can consume host stack.
  using Query = std::pair<const BinaryImage *, va_t>;
  static thread_local std::set<Query> Active;
  const Query Identity{&Image, Target};
  if (Active.size() >= limits::ir::kRegistrationCxxMaxBoundCallDepth ||
      !Active.insert(Identity).second)
    return std::nullopt;
  const auto Leave = llvm::make_scope_exit([&] { Active.erase(Identity); });
  const auto Call = readImmutableCodeBytes(Image, Op.Addr, 5);
  const auto *EH = Image.ExceptionMetadata.findFunction(Target);
  if (!Call || (*Call)[0] != 0xe8 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Call->data() + 1))) !=
          Target ||
      !EH || !EH->Registration || !EH->Cxx || EH->CodeRange.Begin != Target ||
      EH->Registration->RealignedFrame || !EH->Cxx->TryBlocks.empty() ||
      !coff_loader::getCheckedX86CxxHandlerPointerSource(Image, *EH))
    return std::nullopt;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, *EH);
  if (!Handler)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  RegistrationCallCalleeIndex LocalIndex(Image, &Work);
  // A root already owns the immutable-image callee closure. Reuse only its
  // context-free contracts; the conditional inputs and solver remain fresh.
  // The owner bills these queries directly and adds the caller's state work
  // once at publication, including this invocation's decoded/transfer work.
  auto &Index = OwnerIndex ? *OwnerIndex : LocalIndex;
  // Invocation inputs belong to the conditional solver below. Building an
  // unbound native state closure here repeats unrelated callee work before
  // those inputs can even be checked. The physical graph grants no states;
  // every admitted invocation still replays its full cleanup/effect proof.
  const auto Function =
      registration_abi::buildOrdinaryCxxFrameGraph(Image, Dec, Target, Index);
  if (!Function.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Work, Function))
    return std::nullopt;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Insn, Dec))
        return std::nullopt;
      if (Insn.Control == LowInstructionControl::Return) {
        const auto Bytes = readImmutableCodeBytes(Image, Insn.Address, 3);
        if (Insn.Size != 3 || !Bytes || (*Bytes)[0] != 0xc2)
          return std::nullopt;
        const uint32_t Pop = readLE<uint16_t>(Bytes->data() + 1);
        if (!Pop || Pop % 4 ||
            (StackArgumentBytes && Pop != StackArgumentBytes))
          return std::nullopt;
        StackArgumentBytes = Pop;
      }
    }
  if (!StackArgumentBytes)
    return std::nullopt;
  const auto SP =
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 8 ||
      int64_t(*SP) + StackArgumentBytes > INT32_MAX)
    return std::nullopt;
  const int64_t Base = int64_t(*SP) - 8;
  ConditionalFrame Inputs;
  Inputs.StopOnRefusal = true;
  std::map<std::pair<va_t, uint16_t>, bool> ImmutableReads;
  Inputs.ProveImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                                size_t &Used) {
    if (!Inputs.Complete || Write ||
        !registration_abi::chargeCalleeWork(Used, ImmutableReads.size() + 1))
      return false;
    const auto [It, New] = ImmutableReads.try_emplace({Address, Width}, false);
    if (New)
      It->second = registration_abi::readImmutableRegistrationImageBytes(
                       Image, Address, Width, Used)
                       .has_value();
    // This invocation retains the source LOAD and its exact read footprint.
    // It gains no copied value, pointer identity, image writes or native ABI.
    return It->second;
  };
  auto ReadWord = [&](int32_t Offset) -> std::optional<FrameValue> {
    return registration_abi::readCallerFrameWord(Parent, Offset, Initialized,
                                                 Work);
  };
  for (unsigned Index = 0; Index != Parent.Registers.size(); ++Index) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    const auto &Value = Parent.Registers[Index];
    if (isX86VolatileFrameRegister(Index) && !Value.MayBeFrame)
      Inputs.Registers.emplace(Index, Value);
  }
  for (uint32_t Arg = 0; Arg < StackArgumentBytes; Arg += 4) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    const auto Cell = ReadWord(int32_t(*SP + int64_t(Arg)));
    if (!Cell)
      continue;
    const auto &Value = *Cell;
    if (!Value.MayBeFrame) {
      Inputs.Arguments.emplace(8 + Arg, Value);
      continue;
    }
    if (!Value.Offset || Value != FrameValue::frame(*Value.Offset))
      continue;
    const int64_t Offset = int64_t(*Value.Offset) - Base;
    if (!registration_abi::chargeCalleeWork(Work, CallerObjects.size()))
      return std::nullopt;
    const bool Borrowed = llvm::any_of(CallerObjects, [&](const auto &Range) {
      return Range.Begin >= 8 && Range.Begin <= *Value.Offset &&
             int64_t(*Value.Offset) + 4 <= Range.End;
    });
    // The exact incoming area ends before the supplied caller word. Saved
    // EBP/PC are outside the caller allocation. Its other administration and
    // object ownership remain preconditions of the caller's frame proof.
    if (*Value.Offset < int64_t(*SP) + StackArgumentBytes ||
        (!Borrowed && int64_t(*Value.Offset) + 4 > 0) || Offset > INT32_MAX - 4)
      continue;
    int64_t StorageEnd = int64_t(*Value.Offset) + 4;
    if (BorrowStorage) {
      if (!*BorrowStorage || !registration_abi::chargeCalleeWork(Work, 1))
        return std::nullopt;
      const auto Storage = (*BorrowStorage)(*Value.Offset);
      if (!Storage || Storage->Begin != *Value.Offset ||
          Storage->End < StorageEnd ||
          int64_t(Storage->End) - Storage->Begin >
              limits::kMaxSourceCallBorrowedBytes ||
          int64_t(Storage->End) - Base > INT32_MAX)
        continue;
      if (!registration_abi::chargeCalleeWork(Work, CallerObjects.size()))
        return std::nullopt;
      if (Storage->End > 0 &&
          !llvm::any_of(CallerObjects, [&](const auto &Range) {
            return Range.Begin >= 8 && Range.Begin <= Storage->Begin &&
                   Storage->End <= Range.End;
          }))
        continue;
      StorageEnd = int64_t(Storage->Begin) +
                   (int64_t(Storage->End) - Storage->Begin) / 4 * 4;
    }
    int64_t BorrowEnd = *Value.Offset;
    for (int64_t Word = *Value.Offset; Word < StorageEnd; Word += 4) {
      const auto Object = ReadWord(int32_t(Word));
      if (!Object && !Initialized)
        break;
      // Storage ownership does not require an old value. A possibly
      // overlapping pointer, including partial bytes, ends this scalar loan
      // even if the callee would overwrite it before its first read.
      if (!registration_abi::chargeCalleeWork(
              Work,
              Parent.memoryAccessWork(FrameValue::frame(int32_t(Word)), 4)))
        return std::nullopt;
      if (Parent.load(int32_t(Word), 4).MayBeFrame)
        break;
      if (Object)
        Inputs.ObjectCells.emplace(int32_t(Word - Base), *Object);
      BorrowEnd = Word + 4;
    }
    if (BorrowEnd == *Value.Offset)
      continue;
    Inputs.Arguments.emplace(8 + Arg, FrameValue::frame(int32_t(Offset)));
    Inputs.Objects.push_back({int32_t(Offset), int32_t(BorrowEnd - Base)});
  }
  if (Inputs.Objects.empty())
    return std::nullopt;
  Inputs.ProveCxxCall =
      [&](const LowFunc &Caller, const LowOp &Call, const FrameState &State,
          const std::set<int32_t> &InitializedBytes,
          const BoundCxxBorrowStorage &Storage,
          size_t &Used) -> std::optional<BoundCxxCalleeEffect> {
    if (!checkedRegistrationDirectCall(Image, Caller, Call, Used))
      return std::nullopt;
    return proveBoundCxxCallee(Image, Call, State, 0, Used, Inputs.Objects,
                               &InitializedBytes, OwnerIndex, &Storage);
  };
  Inputs.ProvePlainCall =
      [&](const LowFunc &Caller, const LowOp &Call, const FrameState &State,
          const std::set<int32_t> &InitializedBytes,
          size_t &Used) -> std::optional<BoundPlainCalleeEffect> {
    BoundPlainCalleeEffect Effect;
    if (!checkedRegistrationDirectCall(Image, Caller, Call, Used) ||
        !registration_abi::hasConditionalCallFrame(
            Image, Call, State, Used, &InitializedBytes, nullptr, &Effect))
      return std::nullopt;
    return Effect;
  };
  const auto Callees = Index.contracts(Function);
  const auto Cleanups = Index.cleanupContracts(Function);
  const auto Stacks = Index.stackContracts(Function);
  if (!Callees || !Cleanups || !Stacks)
    return std::nullopt;
  // This source-only domain admits private/borrowed writes. An image write
  // could change the personality IAT, a nested import or another proof input
  // while FS0 publishes the incoming pointer. A stack/leaf ABI alone cannot
  // preserve that evidence; image-writing children need a separate closure.
  // Direct CRT throws also need the enclosing runtime-object/callback proof;
  // checked private throw helpers already carry their closed object effects.
  auto HasPrivateEffects = [&](const auto &C) {
    return registration_abi::chargeCalleeWork(Work, 1) && !C.isRuntimeThrow() &&
           C.ImageWrites.empty() && C.CallerPCWrites.empty();
  };
  for (const auto &Callee : *Callees)
    if (!HasPrivateEffects(Callee))
      return std::nullopt;
  for (const auto &Cleanup : *Cleanups) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    for (const auto &Call : Cleanup.Calls)
      if (!HasPrivateEffects(Call.Leaf))
        return std::nullopt;
  }
  size_t StateWork = Work;
  const auto States =
      solveCheckedCxxStates(Image, Function, &*Callees, &*Cleanups, &*Stacks,
                            nullptr, nullptr, &Inputs, &StateWork);
  Work = StateWork;
  if (!Inputs.Complete || Inputs.ReturnValues.empty() || !States.Complete ||
      !States.CallbackStatesComplete || !States.RegistrationLifetimeComplete ||
      !States.ChainOperationsComplete ||
      !States.SourceCallFrameEffectsComplete ||
      !States.CleanupFrameEffectsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete ||
      !States.IncomingFrameAccessesComplete || !States.ImageReadsComplete ||
      (!Handler->CookieGuards.empty() && !States.SecurityCookiesComplete))
    return std::nullopt;
  if (!registration_abi::chargeCalleeWork(
          Work, Inputs.Objects.size() + Inputs.Reads.size() +
                    Inputs.Writes.size() + States.ImageReads.size() + 1))
    return std::nullopt;
  BoundCxxCalleeEffect Effect;
  Effect.StackPopBytes = StackArgumentBytes;
  for (const auto &Object : Inputs.Objects)
    Effect.Objects.push_back(
        {int32_t(Base + Object.Begin), int32_t(Base + Object.End)});
  if (!registration_abi::chargeCalleeWork(Work, Inputs.ObjectCells.size()))
    return std::nullopt;
  for (const auto &[Offset, Value] : Inputs.ObjectCells)
    Effect.RequiredInitializedInputs.push_back(
        {int32_t(Base + Offset), int32_t(Base + Offset + 4)});
  Effect.ImageReads = States.ImageReads;
  for (const auto &[Begin, End] : Inputs.Reads)
    Effect.Reads.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  for (const auto &[Begin, End] : Inputs.Writes)
    Effect.Writes.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  std::optional<std::set<int32_t>> InitializedOnReturn;
  for (const auto &[Identity, Value] : Inputs.ReturnValues) {
    if (!registration_abi::chargeCalleeWork(
            Work,
            Inputs.Objects.size() + Inputs.ReturnInitializedBytes.size() + 1))
      return std::nullopt;
    const auto InitializedReturn = Inputs.ReturnInitializedBytes.find(Identity);
    if (InitializedReturn == Inputs.ReturnInitializedBytes.end() ||
        !registration_abi::chargeCalleeWork(
            Work, InitializedReturn->second.size() +
                      (InitializedOnReturn ? InitializedOnReturn->size() : 0)))
      return std::nullopt;
    if (!InitializedOnReturn)
      InitializedOnReturn = InitializedReturn->second;
    else
      for (auto It = InitializedOnReturn->begin();
           It != InitializedOnReturn->end();)
        if (!InitializedReturn->second.count(*It))
          It = InitializedOnReturn->erase(It);
        else
          ++It;
    BoundCxxCalleeEffect::Return Return{Identity.first, Identity.second,
                                        projectX86CallReturnValue(Value)};
    if (Value.Offset && Value == FrameValue::frame(*Value.Offset) &&
        llvm::any_of(Inputs.Objects, [&](const auto &Object) {
          return Object.Begin <= *Value.Offset && *Value.Offset <= Object.End;
        }))
      Return.Value = FrameValue::frame(int32_t(Base + *Value.Offset));
    Effect.Returns.push_back(Return);
  }
  if (!InitializedOnReturn ||
      !registration_abi::chargeCalleeWork(Work, InitializedOnReturn->size()))
    return std::nullopt;
  for (int32_t Byte : *InitializedOnReturn) {
    const int32_t CallerByte = int32_t(Base + Byte);
    if (!Effect.InitializedOnReturn.empty() &&
        Effect.InitializedOnReturn.back().End == CallerByte)
      ++Effect.InitializedOnReturn.back().End;
    else
      Effect.InitializedOnReturn.push_back({CallerByte, CallerByte + 1});
  }
  return Effect;
}
} // namespace registration_state

} // namespace neverd
