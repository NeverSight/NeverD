//===- RegistrationStateLocalUnwind.cpp - checked SEH local unwinds ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferLocalUnwind(
    size_t I, Domain &After, const LowOp &Op, BlockFacts &Facts) {
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
    return std::nullopt;
  const auto Runtime = LocalUnwindTargets.find(
      {Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL});
  if (Runtime == LocalUnwindTargets.end())
    return std::nullopt;
  Result.HasSEHLocalUnwinds = true;
  auto Refuse = [&]() -> std::optional<FrameValue> {
    FailedLocalUnwind = Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    return std::nullopt;
  };
  const auto &Block = Function.Blocks[I];
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const bool HasCookie =
      Runtime->second.Protocol == RegistrationLocalUnwindContract::Kind::EH4;
  const int32_t FrameArgument = HasCookie ? 4 : 0;
  if (!Function.hasCompleteLiftCoverage() || KnownCxx || HasCookie != EH4 ||
      EH.Encoding != (EH4 ? ExceptionEncoding::X86ScopeTableEH4
                          : ExceptionEncoding::X86ScopeTableEH3) ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != (EH4 ? -2 : -1) ||
      !After.Installed || After.Uninstalled || After.Unknown || Facts.Invalid ||
      !After.JumpBindingsIntact || !SP.Offset ||
      *SP.Offset > INT32_MAX - FrameArgument - 4 || Op.Seq < 0 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Boundary == Boundaries.end() || Boundary->second.first != Block.Id ||
      Boundary->second.second.Control != LowInstructionControl::Call ||
      hasLowInstructionControlFlag(Boundary->second.second.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      hasLowInstructionControlFlag(
          Boundary->second.second.ControlFlags,
          LowInstructionControlFlag::InstructionGuard) ||
      Boundary->second.second.Address + Boundary->second.second.Size !=
          Block.EndAddr ||
      After.Frame.load(*SP.Offset + FrameArgument, 4).Offset !=
          Chain.RegistrationOffset)
    return Refuse();
  if (HasCookie &&
      (!CompleteCookies || !cookiesReady(After.Frame) ||
       After.Frame.load(*SP.Offset, 4).preferredAddress() != SecurityCookieVA))
    return Refuse();
  const auto &Insn = Boundary->second.second;
  const size_t OpIndex = &Op - Block.Ops.data();
  if (Insn.FirstOp > OpIndex || OpIndex - Insn.FirstOp >= Insn.OpCount ||
      Insn.FirstOp > Block.Ops.size() ||
      Insn.OpCount > Block.Ops.size() - Insn.FirstOp)
    return Refuse();
  const auto Bound =
      After.Frame.load(*SP.Offset + FrameArgument + 4, 4).Constant;
  // Only the current level indexes the scope table. The requested bound may
  // be any constant uint32_t; rejecting an out-of-table bound changes no-op
  // unwinds that the runtime accepts. Dynamic bounds remain unproved.
  if (!Bound || After.Levels.empty())
    return Refuse();
  SEHFrameOutcomes Outcomes;
  std::vector<RegistrationObjectExtent> Writes;
  std::set<std::pair<int32_t, int32_t>> Transitions;
  for (int32_t From : After.Levels)
    if (!runSEHUnwind(After.Frame, From, *Bound, Outcomes, Writes, nullptr,
                      nullptr, &Transitions))
      return Refuse();
  RegistrationSEHLocalUnwind Call;
  Call.Address = Op.Addr;
  Call.EndAddress = Insn.Address + Insn.Size;
  Call.OpSeq = Op.Seq;
  Call.Runtime = Runtime->second;
  Call.StackOffset = *SP.Offset;
  Call.Bound = *Bound;
  Call.Transitions.assign(Transitions.begin(), Transitions.end());
  recordLocalUnwind(After, std::move(Call));
  for (const auto &Write : Writes)
    invalidateJumpCaptures(After, FrameValue::frame(Write.Begin),
                           uint16_t(Write.End - Write.Begin));
  std::optional<FrameState> Joined;
  std::set<int32_t> StoppedLevels;
  for (auto &[Level, Frame] : Outcomes) {
    StoppedLevels.insert(Level);
    if (!Joined)
      Joined = std::move(Frame);
    else if (!charge(Joined->cellCount() + Frame.cellCount() + 10))
      return Refuse();
    else
      Joined->merge(Frame);
  }
  After.Frame.Cells = std::move(Joined->Cells);
  After.Levels = std::move(StoppedLevels);
  if (After.Levels.size() == 1)
    After.Frame.store(*Chain.TryLevelOffset, 4,
                      FrameValue::constant(uint32_t(*After.Levels.begin())));
  // Both protocols use caller cleanup. The proved returning path retains the
  // argument-area ESP, independently of every memory/native-output contract.
  return SP;
}

} // namespace neverd::registration_state
