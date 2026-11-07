//===- RegistrationStateDispatch.cpp - x86 EH exceptional roots -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationMemoryPermissions.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <utility>

namespace neverd::registration_state {

int32_t cxxMinimumTryLevel(const Domain &State, const CxxExceptionInfo &Cxx) {
  if (State.Parent || State.OtherCallback || State.CxxCatchStacks.empty())
    return 0;
  int32_t Minimum = INT32_MAX;
  for (const auto &Stack : State.CxxCatchStacks)
    Minimum =
        std::min(Minimum, Cxx.TryBlocks[Stack.back().TryIndex].TryHigh + 1);
  return Minimum;
}

void RegistrationStateSolver::merge(size_t Target, const Domain &Source) {
  if (Exhausted || !charge(1 + Source.Activation.JumpCaptureCalls.size() +
                           Source.Activation.Callers.size() +
                           Source.Levels.size() + Source.JumpCaptures.size() +
                           2 * std::bit_width(States[Target].size() + 1)))
    return;
  SEHFlowKey Key{Source.Activation};
  // The live C++ state owns its initialized cleanup objects. Joining it
  // with a path that never constructed them would lose that correlation.
  // Paths at the same state still intersect their initialization and facts.
  if ((KnownCxx && !Source.Unknown) || (JumpRuntime && !KnownCxx))
    Key.Levels.assign(Source.Levels.begin(), Source.Levels.end());
  if (JumpRuntime && !KnownCxx) {
    for (const auto &[Offset, Capture] : Source.JumpCaptures)
      Key.Captures.emplace_back(Offset, Capture.Call, Capture.Level);
  }
  auto [It, New] = States[Target].try_emplace(Key);
  auto &Node = It->second;
  if (New)
    Node.Facts.Invalid = InvalidBlocks[Target];
  Domain &Dest = Node.Incoming;
  if (!charge(Source.Levels.size() + Source.Frame.cellCount() +
              Source.Frame.OtherRegisterBytes.size() + Dest.Frame.cellCount() +
              Dest.Frame.OtherRegisterBytes.size() + Source.catchCellCount() +
              Dest.catchCellCount() + Source.RuntimeObject.cellCount() +
              Source.RuntimeObject.OtherRegisterBytes.size() +
              Dest.RuntimeObject.cellCount() +
              Dest.RuntimeObject.OtherRegisterBytes.size() +
              Source.InitializedFrameBytes.size() +
              Dest.InitializedFrameBytes.size() +
              Source.JumpCaptures.size() * 14 + Dest.JumpCaptures.size() * 14 +
              Source.sehCallerCellCount() + Dest.sehCallerCellCount() + 10))
    return;
  bool Changed = !Dest.Reached;
  if (!Dest.Reached) {
    Dest = Source;
    Dest.Reached = true;
  } else {
    const size_t Before = Dest.Levels.size();
    Dest.Levels.insert(Source.Levels.begin(), Source.Levels.end());
    Changed |= Before != Dest.Levels.size();
    Changed |= Dest.Frame.merge(Source.Frame);
    Changed |= Dest.RuntimeObject.merge(Source.RuntimeObject);
    if (Dest.CxxDispatchBindingsIntact && !Source.CxxDispatchBindingsIntact) {
      Dest.CxxDispatchBindingsIntact = false;
      Changed = true;
    }
    if (Dest.SEHCallers.size() != Source.SEHCallers.size()) {
      Changed |= !Dest.Unknown;
      Dest.Unknown = true;
    } else {
      for (size_t C = 0; C != Dest.SEHCallers.size(); ++C) {
        auto &Caller = Dest.SEHCallers[C];
        const auto &Other = Source.SEHCallers[C];
        if (Caller.Activation != Other.Activation ||
            Caller.OtherCallback != Other.OtherCallback ||
            Caller.OrdinaryCallStack != Other.OrdinaryCallStack ||
            Caller.StackRestore != Other.StackRestore) {
          Changed |= !Dest.Unknown;
          Dest.Unknown = true;
        }
        Changed |= Caller.Stack.merge(Other.Stack);
      }
    }
    if (Dest.JumpBindingsIntact && !Source.JumpBindingsIntact) {
      Dest.JumpBindingsIntact = false;
      Changed = true;
    }
    for (auto Capture = Dest.JumpCaptures.begin();
         Capture != Dest.JumpCaptures.end();) {
      const auto Other = Source.JumpCaptures.find(Capture->first);
      if (Other == Source.JumpCaptures.end() ||
          Other->second != Capture->second) {
        Capture = Dest.JumpCaptures.erase(Capture);
        Changed = true;
      } else
        ++Capture;
    }
    if (Dest.OrdinaryCallStack &&
        Dest.OrdinaryCallStack != Source.OrdinaryCallStack) {
      Dest.OrdinaryCallStack.reset();
      Changed = true;
    }
    if (Dest.StackRestore && Dest.StackRestore != Source.StackRestore) {
      Dest.StackRestore.reset();
      Changed = true;
    }
    for (auto It = Dest.InitializedFrameBytes.begin();
         It != Dest.InitializedFrameBytes.end();)
      if (!Source.InitializedFrameBytes.count(*It)) {
        It = Dest.InitializedFrameBytes.erase(It);
        Changed = true;
      } else
        ++It;
    auto MergeFlag = [&](bool &Value, bool Other) {
      Changed |= Other && !Value;
      Value |= Other;
    };
    MergeFlag(Dest.Unknown, Source.Unknown);
    MergeFlag(Dest.Parent, Source.Parent);
    MergeFlag(Dest.Callback, Source.Callback);
    MergeFlag(Dest.OtherCallback, Source.OtherCallback);
    for (const auto &Stack : Source.CxxCatchStacks) {
      if (!charge(Stack.size() + 1))
        return;
      Changed |= Dest.CxxCatchStacks.insert(Stack).second;
    }
    MergeFlag(Dest.Uninstalled, Source.Uninstalled);
    MergeFlag(Dest.Installed, Source.Installed);
    // This bit only proves native CRT-guard lifetime. Once that proof fails,
    // another guard-only change cannot recover it or refine language state.
    // Do not revisit an otherwise stable logical frame for that change.
    if (Result.SEHParentChainComplete)
      MergeFlag(Dest.AbandonedSEHGuard, Source.AbandonedSEHGuard);
    MergeFlag(Dest.CanDispatch, Source.CanDispatch);
  }
  if (Changed && charge(1 + 2 * std::bit_width(Work.size() + 1)))
    Work.emplace(Order[Target], Target, std::move(Key));
}

Domain dispatchSource(const Domain &Source) {
  Domain Root;
  // Runtime dispatch supplies no proof that eager IAT bindings stayed intact.
  Root.Frame.ImportBindingsIntact = false;
  Root.CxxDispatchBindingsIntact = false;
  Root.Frame.Cells = Source.Frame.Cells;
  Root.Frame.EntryCells = Source.Frame.EntryCells;
  Root.JumpCaptures = Source.JumpCaptures;
  Root.JumpBindingsIntact = Source.JumpBindingsIntact;
  Root.InitializedFrameBytes = Source.InitializedFrameBytes;
  Root.RuntimeObject.Cells = Source.RuntimeObject.Cells;
  Root.RuntimeObject.forgetCellValues();
  Root.Unknown = Source.Unknown;
  return Root;
}

bool RegistrationStateSolver::preservesCxxDispatchBindings(
    const Domain &State, const LowOp &Op, const FrameTransfer &Transfer,
    bool RuntimeMemory) {
  if (!CxxCalls || !CxxCalls->ProveDispatchMemory || State.Unknown ||
      !charge(1) || Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  if (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE)
    return registration_abi::hasClosedRegistrationIntegerEffect(Op);
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Complete || !Memory.Address ||
      Memory.AccessSize > sizeof(uint32_t))
    return false;
  if (RuntimeMemory)
    return true; // The exact live exception allocation owns this access.
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    const auto Address = Transfer.read(*Memory.Address);
    if (Address.CallbackAddress &&
        Address == FrameValue::callbackFrame(Address.CallbackAddress->Entry,
                                             Address.CallbackAddress->Offset))
      return State.Frame.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                                 Memory.AccessSize,
                                                 Op.Opcode != NdOp::STORE);
    if (Address.Offset && Address == FrameValue::frame(*Address.Offset)) {
      const auto SP = parentStackOffset(State);
      // Source EBP = 0; [4, 8) is the caller PC. Callback code can also
      // access the still-allocated parent frame, never arbitrary arguments.
      return SP && *Address.Offset >= *SP &&
             int64_t(*Address.Offset) + Memory.AccessSize <=
                 (Op.Opcode == NdOp::STORE ? int64_t(sizeof(uint32_t))
                                           : 2 * int64_t(sizeof(uint32_t)));
    }
  }
  return CxxCalls->ProveDispatchMemory(Op, Transfer, WorkUsed);
}

void RegistrationStateSolver::dispatch(
    va_t Address, std::set<int32_t> Levels, const Domain &Source, bool Unknown,
    bool Callback, bool SearchFilter,
    std::optional<std::pair<uint32_t, uint32_t>> CxxCatch,
    bool PreserveBindings) {
  auto It = Entries.find(Address);
  if (It == Entries.end()) {
    if (CxxCatch)
      CompleteCxxContinuations = false;
    return;
  }
  assert(!Levels.empty());
  if (!charge(Levels.size() + 1))
    return;
  Domain Root = dispatchSource(Source);
  if (KnownCxx && CxxCatch && PreserveBindings && !Unknown && !Source.Unknown &&
      CxxCalls && CxxCalls->ProveDispatchCall) {
    Root.Frame.ImportBindingsIntact = Source.Frame.ImportBindingsIntact;
    Root.CxxDispatchBindingsIntact = Source.CxxDispatchBindingsIntact;
  }
  Root.Levels = std::move(Levels);
  if (!KnownCxx && Callback)
    Root.Activation = {Address, SearchFilter};
  if (!KnownCxx && Callback) {
    if (!charge(Source.JumpCaptures.size() + 1))
      return;
    for (const auto &[Offset, Capture] : Source.JumpCaptures)
      Root.Activation.JumpCaptureCalls.emplace_back(Offset, Capture.Call);
  }
  for (auto &Register : Root.Frame.Registers)
    Register.MayBeFrame = true;
  Root.Frame.OtherRegistersMayBeFrame = true;
  // This is the join of roots from one source snapshot, differing only in
  // their logical level. Distinct constants have no exact cell value; the
  // complete finite domain remains in Levels for subsequent state transfers.
  if (Root.Levels.size() == 1)
    Root.Frame.Cells.set(*Chain.TryLevelOffset,
                         FrameValue::constant(uint32_t(*Root.Levels.begin())));
  else
    Root.Frame.Cells.erase(*Chain.TryLevelOffset);
  Root.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(KnownCxx ? *Chain.cxxRuntimeFrameOffset() : 0);
  Root.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].MayBeFrame =
      true;
  // SEH filters/finally handlers are called by the runtime with an invocation
  // stack distinct from the established parent EBP. Their push/pop spills
  // have the same allocation and initialization rules as C++ catch stacks.
  if ((Chain.hasCxxCallbackStack() && CxxCatch) || (!KnownCxx && Callback)) {
    if (!charge(Root.Frame.cellCount() + 8))
      return;
    if (!KnownCxx && SearchFilter && !Chain.RealignedFrame &&
        Chain.RegistrationOffset == -16 && Chain.TryLevelOffset == -4)
      Root.Frame.enterSEHFilter(Address, *Chain.RegistrationOffset - 4);
    else
      Root.Frame.enterCallback(Address);
  }
  Root.Unknown |= Unknown;
  Root.Parent = !Callback;
  Root.Callback = Callback;
  Root.OtherCallback = Callback && !CxxCatch;
  if (CxxCatch) {
    if (!charge(Source.Frame.cellCount() + 1))
      return;
    if (!enterCxxCatch(Root, Source, CxxCatch->first, CxxCatch->second))
      return;
    // Unwinding and catch-object construction may change local objects.
    // Keep only frame taint until their write footprints establish that a
    // particular alias survives. Runtime administration has separate owners.
    Root.Frame.Cells.edit([&](FrameCells::Map &Cells) {
      for (auto It = Cells.begin(); It != Cells.end();) {
        const auto Offset = It->first;
        if (Offset != *Chain.RegistrationOffset &&
            int64_t(Offset) != int64_t(*Chain.RegistrationOffset) - 4 &&
            Offset != *Chain.TryLevelOffset &&
            (!Chain.RealignedFrame ||
             !Chain.RealignedFrame->hasSavedParentFrame() ||
             Offset != Chain.RealignedFrame->SavedParentFrameOffset))
          It->second = registration_state::join(It->second, {});
        if (It->second == FrameValue{})
          It = Cells.erase(It);
        else
          ++It;
      }
    });
    preserveCxxFrameCells(Root, Source);
    if (CheckRuntimeObjects) {
      const auto Object = CatchObjects.find(*CxxCatch);
      const auto &Catch =
          EH.Cxx->TryBlocks[CxxCatch->first].Handlers[CxxCatch->second];
      if (Catch.CatchObjectOffset && Object == CatchObjects.end())
        CompleteCatchObjects = false;
      if (Object != CatchObjects.end()) {
        const auto &C = Object->second;
        const uint16_t SlotBytes = C.Reference ? 4 : uint16_t(C.ObjectSize);
        const auto SP =
            Root.CxxCatchStacks.size() == 1
                ? Root.CxxCatchStacks.begin()->front().SavedStackOffset
                : std::nullopt;
        const auto Offset = Chain.cxxSourceFrameOffset(C.FrameOffset);
        const int64_t End = int64_t(Offset.value_or(0)) + SlotBytes;
        if (!SP || !Offset || Source.Unknown || Unknown || *Offset < *SP ||
            *Offset < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
            End > *Chain.cxxRuntimeFrameOffset() ||
            !frameObjectIsDisjointFromAdministration(*Offset, End) ||
            !charge(SlotBytes + Root.Frame.cellCount() +
                    Root.RuntimeObject.cellCount())) {
          CompleteCatchObjects = false;
        } else {
          Root.Frame.store(*Offset, SlotBytes, {});
          for (int64_t Byte = *Offset; Byte < End; ++Byte)
            Root.InitializedFrameBytes.insert(int32_t(Byte));
          Root.RuntimeObject.store(*Offset, SlotBytes,
                                   C.Reference ? FrameValue::exceptionObject(
                                                     C.TryIndex, C.CatchIndex)
                                               : FrameValue{});
        }
      }
    }
  }
  Root.Installed = true;
  Root.CanDispatch = !SearchFilter;
  merge(It->second, Root);
}

void RegistrationStateSolver::seedEntries() {
  Domain Initial;
  // No register-parameter convention has been proved for this PE32 entry.
  // Retain opaque incoming values (including saved caller EBP) until an
  // explicit source definition replaces them; native emission may not seed
  // them with zero and then expose the invented value.
  for (auto &Register : Initial.Frame.Registers)
    Register.MayBeFrame = true;
  const bool ParentEpilogue =
      SEHControls && !KnownCxx && !Chain.RealignedFrame &&
      std::any_of(SEHControls->begin(), SEHControls->end(), [](const auto &C) {
        return C.Control ==
               RegistrationSEHControlContract::Kind::ParentEpilogue;
      });
  if (Conditional || ParentEpilogue || CxxPrologue || SEH4Prologue) {
    for (unsigned Index = 0; Index != Initial.Frame.Registers.size(); ++Index)
      Initial.Frame.Registers[Index].SavedRegister = uint8_t(Index + 1);
    FrameValue PC;
    PC.MayBeFrame = PC.ReturnPC = true;
    Initial.Frame.Cells.set(4, PC);
  }
  if (Conditional) {
    for (const auto &[Index, Value] : Conditional->Registers) {
      // The query may refine only complete volatile input carriers. Saved
      // nonvolatile identities and both physical frame registers stay owned
      // by the entry protocol above.
      if (!isX86VolatileFrameRegister(Index)) {
        Conditional->Complete = false;
        continue;
      }
      Initial.Frame.Registers[Index] = Value;
    }
    for (const auto &[Offset, Value] : Conditional->Arguments)
      Initial.Frame.store(Offset, 4, Value);
    for (const auto &[Offset, Value] : Conditional->ObjectCells) {
      Initial.Frame.store(Offset, 4, Value);
      if (KnownCxx && Offset <= INT32_MAX - 4)
        for (int32_t Byte = Offset; Byte != Offset + 4; ++Byte)
          Initial.InitializedFrameBytes.insert(Byte);
    }
  }
  Initial.Frame.OtherRegistersMayBeFrame = true;
  // After push EBP, the canonical direct prologue establishes EBP at this
  // incoming SP minus four bytes. Do not assume later reads still name it.
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      Chain.RealignedFrame ? FrameValue::entryFrame(4) : FrameValue::frame(4);
  ParentEntryRegisters = Initial.Frame.Registers;
  Initial.Parent = Initial.Uninstalled = Initial.CanDispatch = true;
  merge(Entries.at(Function.Entry), Initial);
  for (va_t RootVA : Function.OrdinaryModuleAnalysisRoots) {
    if (RootVA == Function.Entry)
      continue;
    auto It = Entries.find(RootVA);
    if (It == Entries.end())
      continue;
    Domain Root;
    Root.Parent = Root.CanDispatch = Root.Unknown = true;
    Root.Installed = Root.Uninstalled = true;
    for (FrameValue &Register : Root.Frame.Registers)
      Register.MayBeFrame = true;
    merge(It->second, Root);
  }
}

} // namespace neverd::registration_state
