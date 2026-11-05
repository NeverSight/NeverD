//===- RegistrationStateFilter.cpp - Resumed SEH filter state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

#include <utility>

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferConditionalDispatch(
    size_t I, const Domain &Before, Domain &After, const LowOp &Op,
    const BlockFacts &Facts, const ConditionalCallResult &Invocation,
    std::optional<Domain> &CallException) {
  if (!Conditional || KnownCxx || !Invocation.Dispatch ||
      !Invocation.StackPopBytes || After.Unknown || Facts.Invalid ||
      !After.Parent || After.Callback || !After.Installed ||
      After.Uninstalled || !After.JumpBindingsIntact ||
      !After.Frame.ImportBindingsIntact || Chain.RealignedFrame ||
      Chain.RegistrationOffset != -16 || Chain.TryLevelOffset != -4 ||
      (EH4 && (!CompleteCookies || !cookiesReady(After.Frame))) ||
      !Function.hasCompleteLiftCoverage())
    return std::nullopt;
  // This ordinary helper returns into the current instruction stream. Unlike
  // a finally invocation, its CALL need not terminate the LowIR block.
  const auto &Block = Function.Blocks[I];
  const auto Source = Boundaries.find(Op.Addr);
  if (Source == Boundaries.end() || Source->second.first != Block.Id ||
      Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Seq < 0)
    return std::nullopt;
  const auto &Insn = Source->second.second;
  if (Insn.Size != 5 || Insn.FirstOp != size_t(&Op - Block.Ops.data()) ||
      Insn.OpCount != 1 || Insn.Mode != InstructionMode::Default ||
      Insn.TargetMode != LowInstructionTargetMode::Preserve ||
      Insn.Control != LowInstructionControl::Call ||
      Insn.ControlFlags != LowInstructionControlFlag::Call ||
      Insn.Immediate != Op.Inputs[0].Offset)
    return std::nullopt;
  const auto SP = parentStackOffset(After);
  if (!SP ||
      int64_t(*SP) + *Invocation.StackPopBytes >
          *Chain.RegistrationOffset - 8 ||
      After.Levels.empty() || !charge(After.snapshotWork()))
    return std::nullopt;
  const auto &Dispatch = *Invocation.Dispatch;
  FrameState Effects = After.Frame;
  std::vector<RegistrationObjectExtent> Writes;
  auto Join = [&](const FrameState &Frame) {
    if (!charge(Effects.cellCount() + Frame.cellCount() +
                Effects.OtherRegisterBytes.size() +
                Frame.OtherRegisterBytes.size() + 10))
      return false;
    Effects.merge(Frame);
    return true;
  };
  if (Dispatch.Raise)
    for (int32_t From : After.Levels) {
      if (!charge(After.Frame.cellCount() + 1))
        return std::nullopt;
      FrameState Parent = After.Frame;
      Parent.store(*Chain.TryLevelOffset, 4,
                   FrameValue::constant(uint32_t(From)));
      SEHFrameOutcomes Continued;
      SEHHandlerOutcomes Selected;
      if (!runSEHFilterSearch(Dispatch.Raise, Parent, &Writes, Continued,
                              /*RequireResume=*/true, &Selected, nullptr))
        return std::nullopt;
      for (const auto &[Level, Frame] : Continued)
        if (Level != From || !Join(Frame))
          return std::nullopt;
      for (const auto &[Scope, Frames] : Selected)
        for (const auto &[Level, Frame] : Frames) {
          SEHFrameOutcomes Unwound;
          if (Level != From || !Join(Frame) ||
              !runSEHUnwind(Frame, Level, Scope, Unwound, Writes))
            return std::nullopt;
          for (const auto &[UnwoundLevel, UnwoundFrame] : Unwound)
            if (!Join(UnwoundFrame))
              return std::nullopt;
        }
    }
  // The callback cannot alter any suspended helper input or its private
  // stack. State changes, external searches and nonlocal exits need a richer
  // invocation proof; stack balance alone cannot retain these bindings.
  const int64_t ArgumentEnd = int64_t(*SP) + Dispatch.ArgumentEnd - 8;
  for (const auto &Write : Writes)
    if (!charge(1) || Write.Begin < ArgumentEnd ||
        !frameObjectIsDisjointFromAdministration(Write.Begin, Write.End))
      return std::nullopt;
  if (!Writes.empty() && !CallException) {
    if (!charge(Before.snapshotWork()))
      return std::nullopt;
    CallException = Before;
  }
  for (const auto &Write : Writes) {
    const auto Width = uint16_t(Write.End - Write.Begin);
    if (!charge(Effects.cellCount() + After.Frame.cellCount() +
                CallException->Frame.cellCount() + 1))
      return std::nullopt;
    // These are may-writes. Retain frame provenance from every checked exit
    // while discarding scalar values, including effects before selection.
    const auto Value = join(Effects.load(Write.Begin, Width), {});
    After.Frame.store(Write.Begin, Width, Value);
    CallException->Frame.store(
        Write.Begin, Width,
        join(Value, join(CallException->Frame.load(Write.Begin, Width), {})));
    invalidateJumpCaptures(After, FrameValue::frame(Write.Begin), Width);
    invalidateJumpCaptures(*CallException, FrameValue::frame(Write.Begin),
                           Width);
  }
  return FrameValue::frame(int32_t(int64_t(*SP) + *Invocation.StackPopBytes));
}

bool RegistrationStateSolver::applySEHContinueFilters(size_t I, Domain &State,
                                                      const LowOp &Raise,
                                                      const BlockFacts &Facts) {
  const auto SP = State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                      .CallbackAddress;
  if (!SP || State.Frame.loadCallback(SP->Offset + 4, 4).Constant != 0 ||
      State.Levels.empty())
    return false;

  SEHFrameOutcomes Continued;
  SEHHandlerOutcomes Selected;
  SEHUnwindJump Jump{State.JumpCaptures, {}};
  for (int32_t Level : State.Levels) {
    if (!validLevel(Level) ||
        !charge(State.Frame.cellCount() +
                State.Frame.OtherRegisterBytes.size() + 10))
      return false;
    FrameState Frame = State.Frame;
    Frame.store(*Chain.TryLevelOffset, 4,
                FrameValue::constant(uint32_t(Level)));
    if (!runSEHFilterSearch(Raise, Frame, nullptr, Continued,
                            /*RequireResume=*/false, &Selected, &Jump))
      return false;
  }

  // A selected filter can activate a new cleanup before handler entry. Seed
  // its exceptional roots with that actual frame even though the suspended
  // invocation never continues. Handler entry itself has a separate owner.
  for (const auto &[Scope, Frames] : Selected)
    for (const auto &[Level, Frame] : Frames) {
      if (!charge(State.snapshotWork() + Frame.cellCount() + 10))
        return false;
      Domain Search = State;
      Search.Levels = {Level};
      Search.Frame = Frame;
      dispatchBlock(I, Search, Facts);
    }

  // Selection and nonlocal jumps retire the suspended invocation. Keeping
  // its existing overapproximation cannot supply a returning frame; source
  // callback occurrences and nonlocal unwind independently close those exits.
  if (Continued.empty())
    return true;

  // Publish the checked returning effects only after every local search.
  // A callback can activate a scope which no ordinary parent block entered.
  // Its exceptional roots need the actual returned frame, before joining
  // different filter-written levels for subsequent ordinary instructions.
  std::optional<FrameState> Joined;
  State.Levels.clear();
  for (const auto &[Level, Frame] : Continued) {
    if (!charge(Frame.cellCount() + Frame.OtherRegisterBytes.size() +
                State.catchCellCount() + State.sehCallerCellCount() + 10))
      return false;
    Domain Resumed = State;
    Resumed.Levels = {Level};
    Resumed.Frame = Frame;
    dispatchBlock(I, Resumed, Facts);
    State.Levels.insert(Level);
    if (!Joined)
      Joined = Frame;
    else
      Joined->merge(Frame);
  }
  if (!Joined)
    return false;
  State.Frame = std::move(*Joined);
  return !Exhausted;
}

} // namespace neverd::registration_state
