//===- RegistrationStatePrologue.cpp - Physical C++ entry calls ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply a loader-checked shared prologue only to its exact incoming frame.

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <algorithm>

namespace neverd::registration_state {

bool RegistrationStateSolver::validateCxxPrologue() const {
  const auto &Call = *CxxPrologue;
  if (!KnownCxx || !EH.Cxx || !EH.Cxx->TryBlocks.empty() ||
      Chain.RealignedFrame || Chain.HasSecurityCookies ||
      Chain.RegistrationOffset != Call.RegistrationOffset ||
      Chain.TryLevelOffset != Call.TryLevelOffset ||
      Chain.SeededTryLevel != -1 || Chain.HandlerVA != Call.HandlerVA ||
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
      Call.PostCallStackOffset != -28 - int64_t(Call.AllocationBytes) ||
      (Call.Writes.size() != 8 && Call.Writes.size() != 9))
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
      Entry->EndAddr != Call.Caller.End ||
      Entry->InstructionBoundaries.size() != 3 || Entry->Ops.size() > 64)
    return false;
  va_t Address = Call.Caller.Begin;
  size_t NextOp = 0;
  for (size_t I = 0; I != 3; ++I) {
    const auto &Insn = Entry->InstructionBoundaries[I];
    const bool IsCall = I == 2;
    if (Insn.Address != Address || !Insn.Size ||
        (I == 0 ? Insn.Size != 2 && Insn.Size != 5 : Insn.Size != 5) ||
        Insn.Size > Call.Caller.End - Address ||
        Insn.Mode != InstructionMode::Default ||
        Insn.TargetMode != LowInstructionTargetMode::Preserve ||
        Insn.Control != (IsCall ? LowInstructionControl::Call
                                : LowInstructionControl::None) ||
        Insn.ControlFlags != (IsCall ? LowInstructionControlFlag::Call
                                     : LowInstructionControlFlag::None) ||
        (IsCall ? Insn.Immediate != Call.Helper.Begin
                : Insn.Immediate.has_value()) ||
        Insn.FirstOp != NextOp || !Insn.OpCount ||
        Insn.OpCount > Entry->Ops.size() - NextOp)
      return false;
    for (size_t J = NextOp; J != NextOp + Insn.OpCount; ++J)
      if (Entry->Ops[J].Addr != Address || Entry->Ops[J].Seq < 0)
        return false;
    if (IsCall) {
      const auto &Op = Entry->Ops[NextOp];
      if (Insn.Address != Call.CallVA || Insn.OpCount != 1 ||
          !hasX86ScalarCallOutput(Op) || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].Offset != Call.Helper.Begin ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
    }
    NextOp += Insn.OpCount;
    Address += Insn.Size;
  }
  return Address == Call.Caller.End && NextOp == Entry->Ops.size();
}

bool RegistrationStateSolver::transferCxxPrologue(size_t I, Domain &State,
                                                  const LowOp &Op,
                                                  BlockFacts &Facts) {
  const auto &Call = *CxxPrologue;
  const auto &Block = Function.Blocks[I];
  const auto &Registers = State.Frame.Registers;
  if (Facts.Invalid || State.Unknown || !State.Parent || State.Callback ||
      State.Installed || !State.Uninstalled || State.Frame.CallbackEntry ||
      Block.StartAddr != Call.Caller.Begin || Op.Addr != Call.CallVA ||
      Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset != 0 ||
      Registers[x86reg::RAX / x86reg::GeneralRegStride].preferredAddress() !=
          Call.HandlerVA ||
      State.Frame.load(0, 4).Constant != Call.AllocationBytes ||
      !State.Frame.load(4, 4).ReturnPC)
    return false;
  // The exact entry prefix changes only EAX and ESP. The seed owns any bound
  // volatile inputs and the remaining opaque identities, including EBP.
  for (unsigned Index : {1, 2, 3, 5, 6, 7})
    if (Registers[Index] != ParentEntryRegisters[Index])
      return false;
  if (!charge(Call.Writes.size() * 12 + State.Frame.cellCount() +
              State.Frame.OtherRegisterBytes.size() +
              State.RuntimeObject.cellCount() +
              State.RuntimeObject.OtherRegisterBytes.size() + 8))
    return false;
  const RegistrationCxxPrologueEffect Effect{Call.CallVA, Call.Caller.End,
                                             Call.Helper.Begin, Op.Seq,
                                             Call.PostCallStackOffset};
  if (CxxPrologueEffect && *CxxPrologueEffect != Effect)
    return false;

  const auto Incoming = Registers;
  const auto IncomingObjects = State.RuntimeObject.Registers;
  using Source = coff_loader::X86CxxPrologueWrite::Value;
  for (const auto &Write : Call.Writes) {
    if (Write.FrameOffset < Call.PostCallStackOffset ||
        int64_t(Write.FrameOffset) + 4 > 4)
      return false;
    FrameValue Value, Object;
    auto Saved = [&](unsigned Index) {
      Value = Incoming[Index];
      Object = IncomingObjects[Index];
    };
    switch (Write.Source) {
    case Source::PreviousEBP:
      Saved(5);
      break;
    case Source::EBX:
      Saved(3);
      break;
    case Source::ESI:
      Saved(6);
      break;
    case Source::EDI:
      Saved(7);
      break;
    case Source::Handler:
      Value = FrameValue::imageAddress(uint32_t(Call.HandlerVA));
      break;
    case Source::PreviousChain:
      Value = FrameValue::previousChain();
      break;
    case Source::EncodedCookie:
      Value.MayBeFrame = Value.SecurityCookie = true;
      Value.CookieFrameOffset = 0;
      break;
    case Source::StackPointer:
      Value = FrameValue::frame(Call.PostCallStackOffset);
      break;
    case Source::InitialState:
      Value = FrameValue::constant(UINT32_MAX);
      break;
    }
    // Machine order matters for zero and unaligned allocations: the optional
    // word at -16 can replace all or only part of an earlier saved register.
    State.Frame.store(Write.FrameOffset, 4, Value);
    if (CheckRuntimeObjects)
      State.RuntimeObject.store(Write.FrameOffset, 4, Object);
    for (int32_t Byte = 0; Byte != 4; ++Byte)
      State.InitializedFrameBytes.insert(Write.FrameOffset + Byte);
  }
  State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(0);
  State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Call.PostCallStackOffset);
  State.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      FrameValue::frame(Call.RegistrationOffset);
  // XOR defines arithmetic flags from cookie and EBP. Retain unknown frame
  // provenance; never carry the caller's flags across this physical helper.
  State.Frame.OtherRegisterBytes.clear();
  State.Frame.OtherRegistersMayBeFrame = true;
  if (CheckRuntimeObjects) {
    for (unsigned Index : {0, 4, 5})
      State.RuntimeObject.Registers[Index] = {};
    State.RuntimeObject.OtherRegisterBytes.clear();
    State.RuntimeObject.OtherRegistersMayBeFrame = false;
  }
  const auto Cookie = State.Frame.load(Call.PostCallStackOffset, 4);
  if (!State.Frame.load(Call.RegistrationOffset, 4).PreviousChain ||
      State.Frame.load(Call.RegistrationOffset + 4, 4).preferredAddress() !=
          Call.HandlerVA ||
      State.Frame.load(Call.TryLevelOffset, 4).Constant != UINT32_MAX ||
      State.Frame.load(0, 4) != Incoming[5] || !Cookie.SecurityCookie ||
      Cookie.CookieFrameOffset != 0 || Cookie.CookieXOR ||
      Cookie.CookieImageXOR || !registrationInstallationReady(State.Frame))
    return false;
  State.Levels = {-1};
  State.Uninstalled = false;
  State.Installed = true;
  State.JumpCaptures.clear();
  ProvenInstallation = true;
  CxxPrologueEffect = Effect;
  ImageReads.emplace(SecurityCookieVA, SecurityCookieVA + 4);
  if (CxxCookies && !cxxCookiesReady(State))
    CompleteCookies = false;
  recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Install);
  return true;
}

} // namespace neverd::registration_state
