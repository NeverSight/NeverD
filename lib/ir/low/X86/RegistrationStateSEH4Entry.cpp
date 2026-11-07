//===- RegistrationStateSEH4Entry.cpp - Shared SEH4 frame transfer --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply authenticated shared entry/exit calls to the actual parent frame.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include <algorithm>

namespace neverd::registration_state {

bool RegistrationStateSolver::validateSEH4Prologue() const {
  const auto &Call = *SEH4Prologue;
  if (!EH4 || KnownCxx || CxxPrologue || Chain.RealignedFrame ||
      Chain.RegistrationOffset != Call.RegistrationOffset ||
      Chain.TryLevelOffset != Call.TryLevelOffset ||
      Chain.SeededTryLevel != -2 || Chain.HandlerVA != Call.HandlerVA ||
      Chain.ScopeTableVA != Call.ScopeTableVA ||
      Chain.ChainInstallVA != Call.CallVA ||
      Function.Entry != EH.CodeRange.Begin ||
      Call.Caller.Begin != Function.Entry || !Call.Caller.isValid() ||
      Call.CallVA > UINT32_MAX - 5 || Call.Caller.End != Call.CallVA + 5 ||
      Call.Caller.End > EH.CodeRange.End || !Call.Helper.isValid() ||
      Call.Helper.End > uint64_t(UINT32_MAX) + 1 ||
      EH.CodeRange.overlaps(Call.Helper) || !SecurityCookieVA ||
      SecurityCookieVA > UINT32_MAX - 3 ||
      Call.SecurityCookieVA != SecurityCookieVA ||
      Call.AllocationBytes > limits::kMaxSourceCallBorrowedBytes ||
      Call.PostCallStackOffset != -32 - int64_t(Call.AllocationBytes) ||
      Call.Writes.size() != 13)
    return false;
  const auto Root =
      Function.OrdinaryModuleAnalysisRoots.upper_bound(Function.Entry);
  if (Root != Function.OrdinaryModuleAnalysisRoots.end() &&
      *Root < Call.Caller.End)
    return false;
  const auto Entry = std::find_if(
      Function.Blocks.begin(), Function.Blocks.end(),
      [&](const auto &Block) { return Block.StartAddr == Function.Entry; });
  if (Entry == Function.Blocks.end() || !Entry->Preds.empty() ||
      !Entry->ExceptionalPreds.empty() || Entry->EndAddr != Call.Caller.End ||
      Entry->InstructionBoundaries.size() != 3 || Entry->Ops.empty())
    return false;
  const auto &Insn = Entry->InstructionBoundaries.back();
  const auto &Op = Entry->Ops.back();
  return Insn.Address == Call.CallVA && Insn.Size == 5 &&
         Insn.Mode == InstructionMode::Default &&
         Insn.TargetMode == LowInstructionTargetMode::Preserve &&
         Insn.Control == LowInstructionControl::Call &&
         Insn.ControlFlags == LowInstructionControlFlag::Call &&
         Insn.Immediate == Call.Helper.Begin && Insn.OpCount == 1 &&
         Insn.FirstOp == Entry->Ops.size() - 1 && Op.Addr == Call.CallVA &&
         Op.Seq >= 0 && hasX86ScalarCallOutput(Op) && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
         Op.Inputs[0].Offset == Call.Helper.Begin &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool RegistrationStateSolver::transferSEH4Prologue(size_t I, Domain &State,
                                                   const LowOp &Op,
                                                   BlockFacts &Facts) {
  const auto &Call = *SEH4Prologue;
  const auto &Block = Function.Blocks[I];
  const auto &Registers = State.Frame.Registers;
  if (Facts.Invalid || State.Unknown || !State.Parent || State.Callback ||
      State.Installed || !State.Uninstalled || State.Frame.CallbackEntry ||
      Block.StartAddr != Call.Caller.Begin || Op.Addr != Call.CallVA ||
      Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset != -4 ||
      State.Frame.load(-4, 4).preferredAddress() != Call.ScopeTableVA ||
      State.Frame.load(0, 4).Constant != Call.AllocationBytes ||
      !State.Frame.load(4, 4).ReturnPC)
    return false;
  for (unsigned Index : {0, 1, 2, 3, 5, 6, 7})
    if (Registers[Index] != ParentEntryRegisters[Index])
      return false;
  if (!charge(Call.Writes.size() * 12 + State.Frame.cellCount() +
              State.Frame.OtherRegisterBytes.size() + 8))
    return false;
  const RegistrationSEH4PrologueEffect Effect{Call.CallVA, Call.Caller.End,
                                              Call.Helper.Begin, Op.Seq,
                                              Call.PostCallStackOffset};
  if (SEH4PrologueEffect && *SEH4PrologueEffect != Effect)
    return false;
  const auto Incoming = Registers;
  using Source = coff_loader::X86SEH4PrologueWrite::Value;
  for (const auto &Write : Call.Writes) {
    if (Write.FrameOffset < Call.PostCallStackOffset - 4 ||
        int64_t(Write.FrameOffset) + 4 > 4)
      return false;
    FrameValue Value;
    switch (Write.Source) {
    case Source::ReturnPC:
      Value = FrameValue::imageAddress(uint32_t(Call.Caller.End));
      break;
    case Source::Handler:
      Value = FrameValue::imageAddress(uint32_t(Call.HandlerVA));
      break;
    case Source::PreviousChain:
      Value = FrameValue::previousChain();
      break;
    case Source::PreviousEBP:
      Value = Incoming[5];
      break;
    case Source::EBX:
      Value = Incoming[3];
      break;
    case Source::ESI:
      Value = Incoming[6];
      break;
    case Source::EDI:
      Value = Incoming[7];
      break;
    case Source::EncodedTable:
      Value.MayBeFrame = Value.SecurityCookie = true;
      Value.CookieImageXOR = uint32_t(Call.ScopeTableVA);
      break;
    case Source::EncodedCookie:
      Value.MayBeFrame = Value.SecurityCookie = true;
      Value.CookieFrameOffset = 0;
      break;
    case Source::StackPointer:
      Value = FrameValue::frame(Call.PostCallStackOffset);
      break;
    case Source::InitialState:
      Value = FrameValue::constant(uint32_t(-2));
      break;
    }
    State.Frame.store(Write.FrameOffset, 4, Value);
    for (int32_t Byte = 0; Byte != 4; ++Byte)
      State.InitializedFrameBytes.insert(Write.FrameOffset + Byte);
  }
  State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(0);
  State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Call.PostCallStackOffset);
  State.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      FrameValue::frame(Call.RegistrationOffset);
  State.Frame.OtherRegisterBytes.clear();
  State.Frame.OtherRegistersMayBeFrame = true;
  if (!State.Frame.load(-16, 4).PreviousChain ||
      State.Frame.load(-12, 4).preferredAddress() != Call.HandlerVA ||
      State.Frame.load(-4, 4).Constant != uint32_t(-2) ||
      State.Frame.load(-24, 4).Offset != Call.PostCallStackOffset ||
      State.Frame.load(0, 4) != Incoming[5] || !cookiesReady(State.Frame))
    return false;
  State.Levels = {-2};
  State.Uninstalled = false;
  State.Installed = true;
  State.JumpCaptures.clear();
  ProvenInstallation = true;
  SEH4PrologueEffect = Effect;
  ImageReads.emplace(SecurityCookieVA, SecurityCookieVA + 4);
  recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Install);
  return true;
}

bool RegistrationStateSolver::transferSEH4Epilogue(
    size_t I, Domain &State, const LowOp &Op, BlockFacts &Facts,
    const coff_loader::X86SEH4EpilogueCall &Call) {
  const auto &Block = Function.Blocks[I];
  const auto Found = Boundaries.find(Op.Addr);
  const auto SP = parentStackOffset(State);
  if (!EH4 || !SEH4Prologue || !SEH4PrologueEffect || Facts.Invalid ||
      State.Unknown || !State.Parent || State.Callback ||
      State.Frame.CallbackEntry || !State.Installed || State.Uninstalled ||
      State.Levels != std::set<int32_t>{-2} ||
      State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      SP != SEH4Prologue->PostCallStackOffset ||
      !State.Frame.load(-16, 4).PreviousChain ||
      !State.Frame.load(4, 4).ReturnPC || !cookiesReady(State.Frame) ||
      Op.Seq < 0 || !hasX86ScalarCallOutput(Op) || Op.NumInputs != 1 ||
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
  if (!charge(24 + State.Frame.cellCount() +
              State.InitializedFrameBytes.size()))
    return false;
  const unsigned Registers[] = {7, 6, 3, 5};
  const int32_t Slots[] = {*SP + 4, *SP + 8, *SP + 12, 0};
  for (unsigned J = 0; J != 4; ++J)
    if (State.Frame.load(Slots[J], 4) != ParentEntryRegisters[Registers[J]])
      return false;
  for (unsigned J = 0; J != 4; ++J)
    State.Frame.Registers[Registers[J]] = State.Frame.load(Slots[J], 4);
  const auto PC = FrameValue::imageAddress(uint32_t(Call.Caller.End));
  State.Frame.store(*SP - 4, 4, PC);
  State.Frame.store(0, 4, PC);
  State.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = PC;
  State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
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
