//===- RegistrationStateCxx.cpp - x86 EH catch transfers ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

void RegistrationStateSolver::recordCatchReturn(
    size_t I, const Domain &After, const LowOp &Op,
    const FrameTransfer &Transfer,
    std::optional<RegistrationCxxContinuation> &CatchReturn) {
  if (!KnownCxx || Op.Opcode != NdOp::RETURN || After.CxxCatchStacks.empty())
    return;
  const LowBlock &Block = Function.Blocks[I];
  const auto Identity = std::make_pair(Op.Addr, Op.Seq);
  const FrameValue Target =
      Op.NumInputs == 1 ? Transfer.read(Op.Inputs[0]) : FrameValue{};
  const int64_t SavedSlot = int64_t(*Chain.RegistrationOffset) - 4;
  const auto *Context = After.CxxCatchStacks.size() == 1
                            ? &After.CxxCatchStacks.begin()->back()
                            : nullptr;
  const auto Captured = Context ? Context->savedStack() : FrameValue{};
  const bool SavedCallback =
      Context && Captured.CallbackAddress && Context->SuspendedCallback &&
      Context->SuspendedStackOffset &&
      After.CxxCatchStacks.begin()->size() > 1 &&
      Captured.CallbackAddress->Entry == Context->SuspendedCallback &&
      Captured.CallbackAddress->Offset <= 0 &&
      Captured.CallbackAddress->Offset >= *Context->SuspendedStackOffset &&
      Captured.CallbackAddress->Offset >=
          -int64_t(limits::kMaxRegistrationEHStateWork);
  const bool SavedParent = Captured.Offset && *Captured.Offset <= SavedSlot;
  // The runtime owns this snapshot, independently of subsequent catch writes
  // to SavedESP. The continuation edge restores both the cell and ESP.
  const bool Valid =
      callbackCanReturn(After) && After.CxxCatchStacks.size() == 1 &&
      !After.Parent && !After.OtherCallback && !After.Unknown &&
      !Facts[I].Invalid && After.Installed && !After.Uninstalled &&
      !After.Levels.empty() && callbackReturnInstruction(I, Op) &&
      Op.NumInputs == 1 && Op.Inputs[0].Size == 4 && Target.Constant &&
      !Target.MayBeFrame && EH.ownsCode(*Target.Constant) &&
      *Target.Constant != Function.Entry && (SavedParent || SavedCallback);
  if (!Valid) {
    InvalidCxxContinuations.insert(Identity);
    CxxContinuations.erase(Identity);
    CompleteCxxContinuations = false;
  } else if (!InvalidCxxContinuations.count(Identity)) {
    CatchReturn = RegistrationCxxContinuation{
        Context->TryIndex,
        Context->CatchIndex,
        Op.Addr,
        Block.EndAddr,
        Op.Seq,
        *Target.Constant,
        SavedCallback ? Captured.CallbackAddress->Offset : *Captured.Offset,
        SavedCallback ? Captured.CallbackAddress->Entry : 0};
    const auto [It, Inserted] =
        CxxContinuations.emplace(Identity, *CatchReturn);
    if (!Inserted && It->second != *CatchReturn) {
      InvalidCxxContinuations.insert(Identity);
      CxxContinuations.erase(Identity);
      CatchReturn.reset();
      CompleteCxxContinuations = false;
    }
  }
}

} // namespace neverd::registration_state
