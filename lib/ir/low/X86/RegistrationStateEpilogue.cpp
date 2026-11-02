//===- RegistrationStateEpilogue.cpp - Physical C++ exit calls ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/X86ArithmeticFlags.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"

#include <algorithm>

namespace neverd::registration_state {

bool RegistrationStateSolver::transferCxxEpilogue(
    size_t I, Domain &State, const LowOp &Op, BlockFacts &Facts,
    const coff_loader::X86CxxEpilogueCall &Call) {
  const auto &Block = Function.Blocks[I];
  const auto Found = Boundaries.find(Op.Addr);
  const auto SP = parentStackOffset(State);
  if (!KnownCxx || !CxxPrologue || !CxxPrologueEffect || Facts.Invalid ||
      State.Unknown || !State.Parent || State.Callback ||
      State.Frame.CallbackEntry || !State.Installed || State.Uninstalled ||
      State.Levels != std::set<int32_t>{-1} ||
      State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      !SP || *SP > -16 ||
      *SP < -int64_t(limits::kMaxSourceCallBorrowedBytes) - 28 ||
      !State.Frame.load(-12, 4).PreviousChain ||
      !State.Frame.load(4, 4).ReturnPC || Op.Seq < 0 ||
      !hasX86ScalarCallOutput(Op) || Op.NumInputs != 1 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.Inputs[0].Offset != Call.Helper.Begin ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Found == Boundaries.end() || Found->second.first != Block.Id ||
      Call.Caller.Begin != Op.Addr || Call.Caller.End != Block.EndAddr ||
      &Op != &Block.Ops.back())
    return false;
  const auto &Insn = Found->second.second;
  if (Insn.Size != 5 || Insn.Address != Call.Caller.Begin ||
      Insn.Address + Insn.Size != Call.Caller.End ||
      Insn.Control != LowInstructionControl::Call ||
      Insn.ControlFlags != LowInstructionControlFlag::Call ||
      Insn.Mode != InstructionMode::Default ||
      Insn.TargetMode != LowInstructionTargetMode::Preserve ||
      Insn.Immediate != Call.Helper.Begin || Insn.OpCount != 1 ||
      Insn.FirstOp != Block.Ops.size() - 1)
    return false;
  const auto Root =
      Function.OrdinaryModuleAnalysisRoots.upper_bound(Call.Caller.Begin);
  if (Root != Function.OrdinaryModuleAnalysisRoots.end() &&
      *Root < Call.Caller.End)
    return false;
  if (!charge(24 + State.Frame.cellCount() + State.RuntimeObject.cellCount() +
              State.InitializedFrameBytes.size()))
    return false;
  for (int32_t Byte = 0; Byte != 16; ++Byte)
    if (!State.InitializedFrameBytes.count(*SP + Byte))
      return false;
  const unsigned Registers[] = {7, 6, 3, 5};
  const int32_t Slots[] = {*SP + 4, *SP + 8, *SP + 12, 0};
  for (unsigned J = 0; J != 4; ++J) {
    auto Expected = FrameValue::taint(true);
    Expected.SavedRegister = uint8_t(Registers[J] + 1);
    if (State.Frame.load(Slots[J], 4) != Expected)
      return false;
  }
  if (CxxCookies && !cxxCookiesReady(State))
    return false;
  if (Call.CookieCheck) {
    const auto &Check = *Call.CookieCheck;
    // Current wrapper bytes describe a check, not its successful outcome.
    // The exceptional cookie owner already proves initialization, lifetime
    // and preservation of this same word and global across every call.
    if (!CxxCookies || !CompleteCookies ||
        Check.SecurityCookieVA != SecurityCookieVA ||
        Check.Checker.Begin != CookieCheckVA ||
        !std::any_of(CxxCookieConditions.begin(), CxxCookieConditions.end(),
                     [](const auto &Condition) {
                       return Condition.FrameOffset == -16 &&
                              Condition.XORFrameOffset == 0;
                     }))
      return false;
  }
  for (unsigned J = 0; J != 4; ++J) {
    State.Frame.Registers[Registers[J]] = State.Frame.load(Slots[J], 4);
    if (CheckRuntimeObjects)
      State.RuntimeObject.Registers[Registers[J]] =
          State.RuntimeObject.load(Slots[J], 4);
  }
  // Model the helper's moved PC separately from the incoming parent's PC.
  // The final push overwrites the old EBP slot, below the returned ESP.
  const auto PC = FrameValue::imageAddress(uint32_t(Call.Caller.End));
  State.Frame.store(*SP - 4, 4, PC);
  if (Call.CookieCheck) {
    State.Frame.store(
        *SP - 8, 4, FrameValue::imageAddress(uint32_t(Call.Helper.Begin + 10)));
    ImageReads.emplace(SecurityCookieVA, SecurityCookieVA + 4);
    for (const auto &[Flag, Bit] : kX86EqualComparisonFlags) {
      // Equal four-byte operands make CMP produce 0 without borrow,
      // carry or overflow. Other flag/register provenance is unchanged.
      State.Frame.OtherRegisterBytes[Flag] = FrameValue::constant(Bit);
      if (CheckRuntimeObjects)
        State.RuntimeObject.OtherRegisterBytes[Flag] = {};
    }
  }
  State.Frame.store(0, 4, PC);
  State.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = PC;
  State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  if (CheckRuntimeObjects) {
    State.RuntimeObject.store(*SP - 4, 4, {});
    if (Call.CookieCheck)
      State.RuntimeObject.store(*SP - 8, 4, {});
    State.RuntimeObject.store(0, 4, {});
    State.RuntimeObject.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    State.RuntimeObject.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
  }
  State.InitializedFrameBytes.erase(State.InitializedFrameBytes.begin(),
                                    State.InitializedFrameBytes.lower_bound(4));
  State.Levels.clear();
  State.Installed = false;
  State.Uninstalled = true;
  State.JumpCaptures.clear();
  recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Remove);
  return true;
}

} // namespace neverd::registration_state
