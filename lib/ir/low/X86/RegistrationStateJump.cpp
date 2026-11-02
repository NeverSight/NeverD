//===- RegistrationStateJump.cpp - Invocation-bound SEH jump transfers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Propagates captured SEH state through checked nonlocal resumptions.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

namespace neverd::registration_state {

void RegistrationStateSolver::invalidateJumpRuntimeBindings(
    Domain &State, const FrameValue &Address, uint64_t Width) {
  if ((!JumpRuntime && LocalUnwindImportSlots.empty()) || Address.Offset ||
      Address.EntryOffset || Address.CallbackAddress)
    return;
  if (!charge((JumpRuntime ? JumpRuntime->ImportSlots.size() : 0) +
              LocalUnwindImportSlots.size() + 1))
    return;
  const auto Overlaps = [&](va_t Slot) {
    return !Address.preferredAddress() ||
           (uint64_t(*Address.preferredAddress()) < Slot + 4 &&
            Slot < uint64_t(*Address.preferredAddress()) + Width);
  };
  // Captures and local unwinds share the same live runtime binding invariant.
  // An immutable veneer does not preserve an import slot overwritten earlier.
  if (!Address.preferredAddress() ||
      (JumpRuntime && llvm::any_of(JumpRuntime->ImportSlots, Overlaps)) ||
      llvm::any_of(LocalUnwindImportSlots, Overlaps)) {
    State.JumpBindingsIntact = false;
    State.JumpCaptures.clear();
  }
}

void RegistrationStateSolver::invalidateJumpCaptures(Domain &State,
                                                     const FrameValue &Address,
                                                     uint16_t Width) {
  if (!charge(State.JumpCaptures.size() + 1))
    return;
  if (!Address.Offset) {
    if (Address.MayBeFrame && !Address.CallbackAddress &&
        !Address.EntryOffset) {
      if (State.Callback)
        for (const auto &[Offset, Capture] : State.JumpCaptures)
          ModifiedCallbackCaptures.insert(Capture.Call);
      State.JumpCaptures.clear();
    }
    return;
  }
  std::erase_if(State.JumpCaptures, [&](const auto &Item) {
    const int64_t Begin = Item.first;
    const bool Overlaps =
        int64_t(*Address.Offset) < Begin + SEHJumpCapture::BufferBytes &&
        Begin < int64_t(*Address.Offset) + Width;
    if (Overlaps && State.Callback)
      ModifiedCallbackCaptures.insert(Item.second.Call);
    return Overlaps;
  });
}

bool RegistrationStateSolver::transferJumpCapture(
    size_t I, Domain &After, const LowOp &Op, const BlockFacts &Facts,
    std::optional<FrameValue> &CallSP) {
  if (!JumpRuntime || !JumpRuntime->Capture || KnownCxx || After.Unknown ||
      Facts.Invalid || !After.Parent || After.Callback ||
      !After.JumpBindingsIntact || !After.Installed || After.Uninstalled ||
      !Function.hasCompleteLiftCoverage() || Op.Opcode != NdOp::CALL ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
    return false;
  const auto Capture = JumpRuntime->Capture(Op, After.Frame, true, WorkUsed);
  if (!Capture)
    return false;
  if (Capture->Protocol != (EH4 ? RegistrationSEHJumpCapture::Kind::EH4
                                : RegistrationSEHJumpCapture::Kind::EH3) ||
      (EH4 && (!CompleteCookies || !cookiesReady(After.Frame) ||
               Capture->SecurityCookieVA != SecurityCookieVA)) ||
      (!EH4 && Capture->SecurityCookieVA) ||
      !frameObjectIsDisjointFromAdministration(Capture->BufferOffset,
                                               int64_t(Capture->BufferOffset) +
                                                   SEHJumpCapture::BufferBytes))
    return false;
  const auto Entry = Entries.find(Capture->ReturnPC);
  const auto &Block = Function.Blocks[I];
  if (Entry == Entries.end() || Block.EndAddr != Capture->ReturnPC ||
      Block.Succs.size() != 1 ||
      Block.Succs.front() != Function.Blocks[Entry->second].Id ||
      !charge(After.Frame.cellCount() + After.JumpCaptures.size() * 14 + 40))
    return false;
  const RegistrationSEHJumpCapture Receipt{Op.Addr,
                                           Capture->ReturnPC,
                                           Op.Seq,
                                           Op.Inputs[0].Offset,
                                           Capture->UnwindTarget,
                                           Capture->BufferOffset,
                                           Capture->StackOffset,
                                           {Capture->Level},
                                           Capture->Protocol,
                                           Capture->SecurityCookieVA};
  const auto [Found, New] =
      JumpCaptureOccurrences.emplace(std::make_pair(Op.Addr, Op.Seq), Receipt);
  if (!New) {
    auto Previous = Found->second;
    Previous.Levels = Receipt.Levels;
    if (Previous != Receipt || !charge(Found->second.Levels.size() + 1)) {
      Result.SEHJumpCapturesComplete = false;
      return false;
    }
    auto &Levels = Found->second.Levels;
    const auto At =
        std::lower_bound(Levels.begin(), Levels.end(), Capture->Level);
    if (At == Levels.end() || *At != Capture->Level)
      Levels.insert(At, Capture->Level);
  }
  invalidateJumpCaptures(After, FrameValue::frame(Capture->BufferOffset),
                         SEHJumpCapture::BufferBytes);
  storeSEHJumpCapture(After.Frame, *Capture);
  After.JumpCaptures.emplace(Capture->BufferOffset, *Capture);
  Result.HasSEHJumpCaptures = true;
  CallSP = FrameValue::frame(Capture->StackOffset);
  return true;
}

bool RegistrationStateSolver::transferJumpResume(size_t I, Domain &After,
                                                 const LowOp &Op,
                                                 BlockFacts &Facts) {
  if (!JumpRuntime || !JumpRuntime->Resume ||
      !JumpRuntime->Resume(Op, WorkUsed))
    return false;
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    Result.SEHJumpResumptionsComplete = false;
    return true;
  };
  if (KnownCxx || After.Unknown || Facts.Invalid ||
      (EH4 && (!CompleteCookies || !cookiesReady(After.Frame))) ||
      !After.JumpBindingsIntact || Function.Entry != EH.CodeRange.Begin ||
      Op.Addr > UINT32_MAX - 5 || !After.Installed || After.Uninstalled ||
      After.Levels.empty() || Chain.RealignedFrame ||
      Chain.RegistrationOffset != -16 || Chain.TryLevelOffset != -4 ||
      !Function.hasCompleteLiftCoverage() ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0)
    return Refuse();
  // Out-of-line callbacks belong to their authenticated scope, not the
  // primary function's contiguous code span. Their private stack and physical
  // invocation are checked below before publishing a resumption.
  if ((!EH.CodeRange.contains(Op.Addr) || Op.Addr + 5 > EH.CodeRange.End) &&
      (!After.Callback || After.Parent || !After.Activation.Entry ||
       After.Activation.Entry != After.Frame.CallbackEntry ||
       !charge(Chain.Scopes.size() + 1) ||
       !llvm::any_of(Chain.Scopes, [&](const auto &Scope) {
         return (Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA) ==
                *After.Activation.Entry;
       })))
    return Refuse();
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  FrameValue Buffer, Value;
  if (SP.Offset && *SP.Offset <= INT32_MAX - 8) {
    Buffer = After.Frame.load(*SP.Offset, 4);
    Value = After.Frame.load(*SP.Offset + 4, 4);
  } else if (SP.CallbackAddress && After.Callback &&
             After.Activation.Entry == After.Frame.CallbackEntry &&
             SP.CallbackAddress->Entry == After.Frame.CallbackEntry &&
             SP.CallbackAddress->Offset <= -8 &&
             After.Frame.callbackMemoryIsPrivate(*SP.CallbackAddress, 8,
                                                 true)) {
    // Offset zero is the runtime's implicit return slot. The callback memory
    // owner rejects all source reads/writes there; it is never an initialized
    // private spill in CallbackCells.
    Buffer = After.Frame.loadCallback(SP.CallbackAddress->Offset, 4);
    Value = After.Frame.loadCallback(SP.CallbackAddress->Offset + 4, 4);
  } else
    return Refuse();
  if (Buffer.JumpCapture)
    return transferBorrowedJump(I, After, Op, Facts, Buffer, Value);
  const auto Capture = Buffer.Offset ? After.JumpCaptures.find(*Buffer.Offset)
                                     : After.JumpCaptures.end();
  const auto ParentSP = After.Frame.load(-24, 4).Offset;
  if (Capture == After.JumpCaptures.end() || Buffer.ReturnPC ||
      Buffer.CallbackAddress || Buffer.EntryOffset || Buffer.SEHException ||
      !Value.isFrameIndependentScalar() || !ParentSP ||
      *ParentSP > Capture->first ||
      !hasSEHJumpCapture(After.Frame, Capture->second) ||
      !Entries.count(Capture->second.ReturnPC) ||
      (After.Callback && (!SEHControls || !validateSEHInvocationBlock(I))) ||
      !charge(After.Frame.cellCount() + After.JumpCaptures.size() * 14 + 10))
    return Refuse();
  const auto Saved = Capture->second;
  return resumeJumpCapture(After, Facts, Op, Saved, Value);
}

bool RegistrationStateSolver::resumeJumpCapture(Domain &After,
                                                BlockFacts &Facts,
                                                const LowOp &Op,
                                                const SEHJumpCapture &Saved,
                                                const FrameValue &Value,
                                                va_t CalleeVA, va_t ResumeVA) {
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    Result.SEHJumpResumptionsComplete = false;
    return true;
  };
  SEHFrameOutcomes Outcomes;
  SEHJumpUnwind Jump{Saved, Value, After.JumpCaptures, {}, {}, {}};
  SEHHandlerOutcomes Selected;
  std::vector<RegistrationObjectExtent> Writes;
  for (int32_t Level : After.Levels)
    if (!runSEHUnwind(After.Frame, Level, uint32_t(Saved.Level), Outcomes,
                      Writes, &Jump, &Selected))
      return Refuse();
  const auto Unmodified = [&](const SEHJumpCapture &Capture) {
    if (!charge(Writes.size() + 1))
      return false;
    return llvm::none_of(Writes, [&](const auto &Write) {
      return Write.Begin <
                 int64_t(Capture.BufferOffset) + SEHJumpCapture::BufferBytes &&
             Capture.BufferOffset < Write.End;
    });
  };
  if (!Unmodified(Saved))
    return Refuse();
  for (const auto &[Buffer, Frames] : Jump.Resumed) {
    const auto Capture = After.JumpCaptures.find(Buffer);
    if (Capture == After.JumpCaptures.end() || !Unmodified(Capture->second))
      return Refuse();
  }
  RegistrationSEHJumpResume Receipt{Op.Addr,
                                    Op.Seq,
                                    Saved.Call,
                                    Saved.ReturnPC,
                                    After.Activation.Entry.value_or(0),
                                    After.Activation.OrdinaryCall,
                                    Saved.BufferOffset,
                                    Saved.StackOffset,
                                    CalleeVA,
                                    ResumeVA};
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!Receipt.EntryVA && !Receipt.OrdinaryCall && !CalleeVA && !ResumeVA &&
      SP.Offset && *SP.Offset <= INT32_MAX - 4)
    Receipt.ValueOffset = *SP.Offset + 4;
  if (Receipt.EntryVA && !CalleeVA && !ResumeVA && After.Callback &&
      SP.CallbackAddress && SP.CallbackAddress->Entry == Receipt.EntryVA &&
      SP.CallbackAddress->Offset <= -8)
    Receipt.ValueOffset = SP.CallbackAddress->Offset + 4;
  for (const auto &[Level, Frame] : Outcomes)
    Receipt.ResumedLevels.push_back(Level);
  for (const auto &[Scope, Frames] : Selected)
    Receipt.SelectedScopes.push_back(Scope);
  Receipt.StateRestores.assign(Jump.StateRestores.begin(),
                               Jump.StateRestores.end());
  Receipt.Replacements.assign(Jump.Replacements.begin(),
                              Jump.Replacements.end());
  const SEHExitKey Identity{Receipt.EntryVA, Receipt.OrdinaryCall, Op.Addr,
                            Op.Seq};
  const auto [It, New] = JumpResumptions.emplace(Identity, Receipt);
  if (!New) {
    auto Previous = It->second;
    Previous.ResumedLevels = Receipt.ResumedLevels;
    Previous.StateRestores = Receipt.StateRestores;
    Previous.SelectedScopes = Receipt.SelectedScopes;
    Previous.Replacements = Receipt.Replacements;
    if (Previous != Receipt)
      return Refuse();
    auto &Restores = It->second.StateRestores;
    auto &Levels = It->second.ResumedLevels;
    auto &Scopes = It->second.SelectedScopes;
    auto &Replacements = It->second.Replacements;
    if (!charge(Levels.size() + Receipt.ResumedLevels.size() + Restores.size() +
                Receipt.StateRestores.size() + Scopes.size() +
                Receipt.SelectedScopes.size() + Replacements.size() +
                Receipt.Replacements.size()))
      return Refuse();
    Restores.insert(Restores.end(), Receipt.StateRestores.begin(),
                    Receipt.StateRestores.end());
    llvm::sort(Restores);
    Restores.erase(std::unique(Restores.begin(), Restores.end()),
                   Restores.end());
    Levels.insert(Levels.end(), Receipt.ResumedLevels.begin(),
                  Receipt.ResumedLevels.end());
    llvm::sort(Levels);
    Levels.erase(std::unique(Levels.begin(), Levels.end()), Levels.end());
    Scopes.insert(Scopes.end(), Receipt.SelectedScopes.begin(),
                  Receipt.SelectedScopes.end());
    llvm::sort(Scopes);
    Scopes.erase(std::unique(Scopes.begin(), Scopes.end()), Scopes.end());
    Replacements.insert(Replacements.end(), Receipt.Replacements.begin(),
                        Receipt.Replacements.end());
    llvm::sort(Replacements);
    Replacements.erase(std::unique(Replacements.begin(), Replacements.end()),
                       Replacements.end());
  }
  for (auto &[Buffer, Resumed] : Jump.Resumed) {
    const auto Capture = After.JumpCaptures.find(Buffer);
    if (Capture == After.JumpCaptures.end())
      return Refuse();
    for (auto &[ResumeValue, Frames] : Resumed)
      if (!mergeSEHJumpContinuation(After, Capture->second, ResumeValue,
                                    std::move(Frames)))
        return Refuse();
  }
  for (auto &[Scope, Frames] : Selected)
    for (auto &[Level, Frame] : Frames) {
      if (!charge(After.snapshotWork() + Frame.cellCount() + 10))
        return Refuse();
      Domain Handler = After;
      Handler.Frame = std::move(Frame);
      dispatch(Chain.Scopes[Scope].HandlerVA, {Level}, Handler, false);
    }
  if (Exhausted)
    return Refuse();
  Facts.InvocationAtExit = true;
  return true;
}

} // namespace neverd::registration_state
