//===- RegistrationStateDispatch.cpp - x86 EH exceptional roots -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
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
  if (Exhausted ||
      !charge(Source.Levels.size() + Source.Frame.cellCount() +
              Source.Frame.OtherRegisterBytes.size() +
              Incoming[Target].Frame.cellCount() +
              Incoming[Target].Frame.OtherRegisterBytes.size() +
              Source.catchCellCount() + Incoming[Target].catchCellCount() +
              Source.RuntimeObject.cellCount() +
              Source.RuntimeObject.OtherRegisterBytes.size() +
              Incoming[Target].RuntimeObject.cellCount() +
              Incoming[Target].RuntimeObject.OtherRegisterBytes.size() +
              Source.InitializedFrameBytes.size() +
              Incoming[Target].InitializedFrameBytes.size() + 10))
    return;
  Domain &Dest = Incoming[Target];
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
    MergeFlag(Dest.CanDispatch, Source.CanDispatch);
  }
  if (Changed)
    Work.emplace(Order[Target], Target);
}

void RegistrationStateSolver::dispatch(
    va_t Address, int32_t Level, const Domain &Source, bool Unknown,
    bool Callback, bool SearchFilter,
    std::optional<std::pair<uint32_t, uint32_t>> CxxCatch) {
  auto It = Entries.find(Address);
  if (It == Entries.end()) {
    if (CxxCatch)
      CompleteCxxContinuations = false;
    return;
  }
  Domain Root;
  Root.Levels = {Level};
  for (auto &Register : Root.Frame.Registers)
    Register.MayBeFrame = true;
  Root.Frame.OtherRegistersMayBeFrame = true;
  Root.Frame.Cells = Source.Frame.Cells;
  Root.Frame.EntryCells = Source.Frame.EntryCells;
  Root.InitializedFrameBytes = Source.InitializedFrameBytes;
  Root.RuntimeObject.Cells = Source.RuntimeObject.Cells;
  for (auto &[Offset, Value] : Root.RuntimeObject.Cells)
    Value = registration_state::join(Value, {});
  Root.Frame.Cells[*Chain.TryLevelOffset] =
      FrameValue::constant(uint32_t(Level));
  Root.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(KnownCxx ? *Chain.cxxRuntimeFrameOffset() : 0);
  Root.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].MayBeFrame =
      true;
  if (Chain.hasCxxCallbackStack() && CxxCatch) {
    if (!charge(Root.Frame.cellCount() + 8))
      return;
    Root.Frame.enterCallback(Address);
  }
  Root.Unknown = Unknown || Source.Unknown;
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
    for (auto &[Offset, Value] : Root.Frame.Cells)
      if (Offset != *Chain.RegistrationOffset &&
          int64_t(Offset) != int64_t(*Chain.RegistrationOffset) - 4 &&
          Offset != *Chain.TryLevelOffset &&
          (!Chain.RealignedFrame ||
           Offset != Chain.RealignedFrame->SavedParentFrameOffset))
        Value = registration_state::join(Value, {});
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
            *Offset < -int64_t(limits::kMaxRegistrationEHStateWork) ||
            End > *Chain.cxxRuntimeFrameOffset() ||
            (*Offset < int64_t(*Chain.TryLevelOffset) + 4 &&
             int64_t(*Chain.RegistrationOffset) - 4 < End) ||
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
  Initial.Frame.OtherRegistersMayBeFrame = true;
  // After push EBP, the canonical direct prologue establishes EBP at this
  // incoming SP minus four bytes. Do not assume later reads still name it.
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      Chain.RealignedFrame ? FrameValue::entryFrame(4) : FrameValue::frame(4);
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
