//===- X86ModeExecution.cpp - Continuous x86 machine execution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full-width state ownership and terminal routing of non-64-bit events.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

#include "X86ModeState.h"
#include "X86SgxExecution.h"
#include "X86SmmExecution.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <utility>

namespace neverd::analysis {
using namespace translate;
using detail::modeRegisters;

namespace detail {
struct X86ModeContinuation {
  X86ModeState Expected;
  X86PagingState Controls;
  X86PagingFeatures Features;
  X86ModeExecutionOptions Options;
  X86ModeInstruction Instruction;
  X86LegacyExecutionState Low;
  analysis::X64PagedInstructionContinuation WideInstruction;
  std::optional<X64ContinuationEventV1> Event;
  std::optional<X64PagedEventContinuation> WideEvent;
  std::optional<detail::X86RsmProgress> PendingRSM;
  std::optional<detail::X86SgxInstructionProgress> PendingSGX;
  std::shared_ptr<const X86SgxMemory> EnclaveBank;
  uint64_t EnclaveGeneration = 0;
  bool AttemptConsumed = false;
  bool PendingHaltFetch = false;
  bool SoftwareINT = false;
  bool CompletionReported = false;
  bool AcceptanceReported = false;

  bool eventPending() const { return Event || WideEvent || Low.PendingEvent; }
  bool instructionPending() const {
    return PendingHaltFetch || WideInstruction || PendingRSM || PendingSGX ||
           x86LegacyInstructionPending(Low);
  }
};
} // namespace detail

namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

std::optional<bool> systemLockPrefix(llvm::ArrayRef<uint8_t> Bytes,
                                     llvm::ArrayRef<uint8_t> Opcode,
                                     bool Long64) {
  if (Bytes.size() < Opcode.size() || Bytes.size() > 15 ||
      Bytes.take_back(Opcode.size()) != Opcode)
    return {};
  bool Lock = false;
  for (auto B : Bytes.drop_back(Opcode.size())) {
    if (Long64 && (B & 0xf0) == 0x40)
      continue;
    switch (B) {
    case 0xf0:
      Lock = true;
      break;
    case 0xf2:
    case 0xf3:
      Lock = false;
      break;
    case 0x26:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
      break;
    default:
      return {};
    }
  }
  return Lock;
}

void reportReceipts(detail::X86ModeContinuation &P, X86ModeExecutionResult &R) {
  if (R.InstructionCompleted) {
    R.InstructionCompleted = !P.CompletionReported;
    P.CompletionReported = true;
  }
  if (R.EventAccepted) {
    R.EventAccepted = !P.AcceptanceReported;
    P.AcceptanceReported = true;
  }
}

void retain(X86ModeState &S, const X86PagedMemory &M,
            std::shared_ptr<detail::X86ModeContinuation> P) {
  if (!P->eventPending() && !P->instructionPending()) {
    S.Pending.reset();
    return;
  }
  P->Expected = S;
  P->Expected.Pending.reset();
  P->Controls = M.controls();
  P->Features = M.features();
  P->EnclaveBank = M.sgxBank();
  P->EnclaveGeneration = P->EnclaveBank ? P->EnclaveBank->generation() : 0;
  S.Pending = std::move(P);
}

void mergeEvent(X86ModeExecutionResult &R,
                const X86LegacyExecutionResult &Event) {
  const bool Attempted = R.InstructionAttempted;
  const bool Completed = R.InstructionCompleted;
  const bool CR2Written = R.CR2Written, CSWritten = R.CSWritten,
             SSWritten = R.SSWritten;
  const auto DataSegmentsWritten = R.DataSegmentsWritten;
  const bool TaskStateWritten = R.TaskStateWritten;
  const auto SegmentWritten = R.SegmentWritten;
  const auto DebugAddressWritten = R.DebugAddressWritten;
  auto Effects = std::move(R.SelfModifications);
  const auto Exception = R.Exception;
  static_cast<X86LegacyExecutionResult &>(R) = Event;
  R.InstructionAttempted = Attempted;
  R.InstructionCompleted = Completed || Event.SoftwareCompleted;
  R.CR2Written |= CR2Written;
  R.CSWritten |= CSWritten;
  R.SSWritten |= SSWritten;
  R.DataSegmentsWritten |= DataSegmentsWritten;
  R.TaskStateWritten |= TaskStateWritten;
  if (SegmentWritten)
    R.SegmentWritten = SegmentWritten;
  if (DebugAddressWritten)
    R.DebugAddressWritten = DebugAddressWritten;
  if (!R.Exception)
    R.Exception = Exception;
  R.SelfModifications.insert(R.SelfModifications.begin(), Effects.begin(),
                             Effects.end());
}

void mergeEvent(X86ModeExecutionResult &R, const X64PagedEventResult &Event) {
  R.EventAccepted = Event.InputAccepted;
  R.SoftwareCompleted = Event.SoftwareCompleted;
  R.InstructionCompleted |= Event.SoftwareCompleted;
  R.DeliveredVector = Event.DeliveredVector;
  R.DeliveryAttempts = Event.DeliveryAttempts;
  R.BackingFault = Event.BackingFault;
  if (Event.LastException)
    R.Exception = Event.LastException;
  switch (Event.Status) {
  case X64PagedEventStatus::Delivered:
    R.Status = X86LegacyExecutionStatus::EventDelivered;
    break;
  case X64PagedEventStatus::Deferred:
    R.Status = X86LegacyExecutionStatus::EventDeferred;
    break;
  case X64PagedEventStatus::InvalidState:
    R.Status = X86LegacyExecutionStatus::ModeUnavailable;
    break;
  case X64PagedEventStatus::BackingFault:
    R.Status = X86LegacyExecutionStatus::BackingFault;
    break;
  case X64PagedEventStatus::Shutdown:
    R.Status = X86LegacyExecutionStatus::Shutdown;
    break;
  case X64PagedEventStatus::BudgetExhausted:
    R.Status = X86LegacyExecutionStatus::EventBudgetExhausted;
    break;
  }
}

llvm::Error admit(const X86ModeState &S, const X86ModeExecutionOptions &O,
                  const X86PagedMemory &M) {
  const auto &R = modeRegisters(S);
  const auto &V = S.Core.Core;
  const auto &Controls = V.Core.Core.Core;
  if (M.sgxMemory() && M.sgxMemory()->active() &&
      !M.sgxMemory()->hasRunningThread())
    return invalid(
        "mixed-mode enclave execution requires a complete running thread");
  if (V.NMIBlocked > 1 || Controls.CR8 > 15 || Controls.Reserved ||
      Controls.ReservedTail || V.Core.Core.Reserved || V.Core.Reserved ||
      V.GDTR.Reserved || V.GDTR.ReservedTail || V.IDTR.Reserved ||
      V.IDTR.ReservedTail || S.Core.Reserved ||
      S.Core.SysenterCS > UINT32_MAX ||
      std::any_of(std::begin(V.Reserved), std::end(V.Reserved),
                  [](uint8_t Byte) { return Byte != 0; }))
    return invalid("mixed-mode execution requires valid complete CPU metadata");
  if (!O.Undefined || !O.MXCSRMask || O.Long.Debug || O.Legacy.Non64Mode ||
      !O.Legacy.Segments.FullLimitChoice ||
      (O.PagingAddressBits != 48 && O.PagingAddressBits != 57) ||
      (S.Core.Core.Core.Core.Core.MXCSR & ~O.MXCSRMask))
    return invalid(
        "mixed-mode execution requires full CPU and undefined-value bindings");
  const bool VM86 = R.RFlags & 0x20000;
  if (S.RetainedRealCS &&
      (!(M.controls().CR0 & 1) || VM86 || (V.Core.CS.Attributes & 0x2000)))
    return invalid("pre-PE code cache requires protected non-64-bit execution");
  if (S.SMM && O.Legacy.ControlRegisters &&
      (O.Legacy.ControlRegisters->OtherCR4Features &
       ~kX86LegacyOtherCR4FeatureMask))
    return invalid("SMM requires paging feature bits at their paging owner");
  if ((VM86 && ((M.controls().EFER & 0x400) ||
                O.Legacy.Virtual8086 != X86LegacyVM86Profile::IntelVME)) ||
      (R.RFlags & ~uint64_t(0x3f7fd7)) || !(R.RFlags & 2) ||
      bool(S.Debug) != bool(O.Legacy.Debug) ||
      (S.Debug &&
       (*O.Legacy.Debug != O.Long.DebugProfile ||
        !x86InstructionDebugValid(S.Debug->DR6, S.Debug->DR7, S.Debug->DebugCTL,
                                  O.Long.DebugProfile, M.controls().CR4,
                                  S.Debug->PendingData))))
    return invalid(
        "mixed-mode execution requires protected flags and one debug bank");
  if ((S.STIBlocked && !(R.RFlags & 0x200)) ||
      (S.Halted && (VM86 ||
                    ((M.controls().CR0 & 1) && !S.RetainedRealCS &&
                     (V.Core.CS.Selector & 3)) ||
                    S.STIBlocked || S.SSBlocked)))
    return invalid("mixed-mode halt/shadow state is inconsistent");
  if (!x86DescriptorCanonicalityValid(O.Long.EventDescriptorCanonicality))
    return invalid("invalid event descriptor canonicality profile");
  if (!O.Legacy.EventDelivery.MaxAttempts || !O.Long.EventDelivery.MaxAttempts)
    return invalid("mixed-mode event owners require positive work budgets");
  return llvm::Error::success();
}

llvm::Expected<X86ModeExecutionResult>
executeWideHalt(X86ModeState &S, X86PagedMemory &Memory,
                const X86ModeInstruction &I, const X86ModeExecutionOptions &O,
                bool Locked, X86EventKind &FaultKind) {
  X86ModeExecutionResult R;
  auto &Regs = modeRegisters(S);
  const uint8_t CPL = S.Core.Core.Core.CS.Selector & 3;
  const bool TF = Regs.RFlags & 0x100;
  if (TF && !S.Debug)
    return invalid("single-step HLT requires the CPU debug bank");
  R.InstructionAttempted = true;
  if (x86PendingVirtualInterrupt(CPL, Memory.controls().CR4, Regs.RFlags)) {
    R.Exception = X86MemoryException{13, 0, {}};
    return R;
  }
  const bool Enclave = Memory.sgxMemory() && Memory.sgxMemory()->active();
  if (S.Debug && !Enclave) {
    const auto Matches = x86ExecutionBreakpoints(
        S.Debug->DR, S.Debug->DR7, I.Address, Regs.RFlags, S.SSBlocked);
    if (Matches) {
      x86PublishInstructionDebug(*S.Debug, false, Matches);
      R.Exception = X86MemoryException{1, 0, {}};
      FaultKind = X86EventKind::InstructionBreakpoint;
      return R;
    }
  }
  R.InstructionAttempted = false;
  auto Fetch = detail::fetchX86SystemInstruction(
      S, Memory, I.Bytes, O.Legacy.Segments.FullLimitChoice, R);
  if (!Fetch)
    return Fetch.takeError();
  if (!*Fetch)
    return R;
  if (Locked || CPL) {
    R.Exception = X86MemoryException{uint8_t(Locked ? 6 : 13), 0, {}};
    return R;
  }
  Regs.PC += I.Bytes.size();
  Regs.RFlags &= ~uint64_t(0x10000);
  auto Controls = Memory.controls();
  Controls.RFlags = Regs.RFlags;
  llvm::cantFail(Memory.bindControls(Controls));
  S.SSBlocked = S.STIBlocked = false;
  S.Halted = true;
  R.InstructionCompleted = true;
  R.Status = X86LegacyExecutionStatus::Halted;
  if (TF && x86InstructionDebugStep(TF, S.Debug->DebugCTL, false)) {
    x86PublishInstructionDebug(*S.Debug, true);
    R.Exception = X86MemoryException{1, 0, {}};
  }
  return R;
}

// This is a terminal handoff: no low view is published after the wide owner
// has installed a handler or retained a partly completed frame.
void deliver(X86ModeState &S, X86PagedMemory &M, detail::X86ModeContinuation &P,
             X86ModeExecutionResult &R) {
  const auto &O = P.Options;
  if (M.controls().EFER & 0x400) {
    auto Got =
        P.WideEvent
            ? resumeX64PagedEvent(S.Core.Core, M, *P.WideEvent, O.MXCSRMask,
                                  S.Debug ? &*S.Debug : nullptr,
                                  O.Long.DebugProfile, O.Long.EventDelivery)
            : deliverX64PagedEvent(S.Core.Core, M, *P.Event, O.MXCSRMask,
                                   S.Debug ? &*S.Debug : nullptr,
                                   O.Long.DebugProfile, O.Long.EventDelivery,
                                   O.Long.EventDescriptorCanonicality);
    if (!Got) {
      R.OwnerError = llvm::toString(Got.takeError());
      R.Status = X86LegacyExecutionStatus::ModeUnavailable;
      return;
    }
    mergeEvent(R, *Got);
    const bool Invalid = Got->Status == X64PagedEventStatus::InvalidState;
    if (Invalid)
      R.OwnerError = "IA-32e event owner refused the committed state";
    if (Got->InputAccepted || P.WideEvent) {
      S.STIBlocked = S.SSBlocked = S.Halted = false;
    }
    P.WideEvent = std::move(Got->Continuation);
    if (!Invalid && !P.WideEvent)
      P.Event.reset();
  } else {
    const bool Continuing = P.Low.PendingEvent.has_value();
    auto EventOptions = O.Legacy;
    EventOptions.Non64Mode.reset();
    auto Got =
        P.Low.PendingEvent
            ? resumeX86LegacyEvent(P.Low, M, EventOptions)
            : deliverX86LegacyEvent(
                  P.Low, M,
                  {uint32_t(P.Event->SavedPC), uint32_t(P.Event->ErrorCode),
                   uint32_t(P.Event->FaultAddress), P.Event->Vector,
                   P.Event->Kind, P.SoftwareINT},
                  EventOptions, S.Debug ? &S.Debug->DR : nullptr);
    if (!Got) {
      R.OwnerError = llvm::toString(Got.takeError());
      R.Status = X86LegacyExecutionStatus::ModeUnavailable;
      // Legacy entry can retain architectural CR2/NMI and journal effects even
      // when a subsequent owner refuses. Preserve its view and progress.
      if (P.Low.PendingEvent) {
        R.EventAccepted = true;
        R.CR2Written |= P.Low.PendingEvent->CR2Written;
        R.CSWritten |= P.Low.PendingEvent->CSWritten;
        R.SSWritten |= P.Low.PendingEvent->SSWritten;
        R.DataSegmentsWritten |= P.Low.PendingEvent->DataSegmentsWritten;
        R.TaskStateWritten |= P.Low.PendingEvent->TaskStateWritten;
        detail::publishNon64(S, P.Low, R);
      }
      return;
    }
    if (Got->EventAccepted || Continuing)
      detail::publishNon64(S, P.Low, *Got);
    mergeEvent(R, *Got);
    if (!P.Low.PendingEvent)
      P.Event.reset();
  }
  if (R.Status == X86LegacyExecutionStatus::Shutdown)
    S.Shutdown = true;
}
} // namespace

llvm::Expected<X86PagedMemory &>
X86ModeExecution::memoryFor(const X86ModeState &S) const {
  if (S.SMM && S.SMM->Active) {
    if (!SMRAM || SMRAM == &NormalMemory ||
        SMRAM->features() != NormalMemory.features())
      return invalid("active SMM requires its separate matching SMRAM bank");
    return *SMRAM;
  }
  return NormalMemory;
}

llvm::Expected<X86ModeExecutionResult>
X86ModeExecution::systemManagementInterrupt(X86ModeState &S) {
  if (!S.SMM || !SMRAM || SMRAM == &NormalMemory ||
      NormalMemory.features() != SMRAM->features())
    return invalid(
        "software SMI requires a bound SMM state and separate matching SMRAM");
  if (S.Pending || S.Shutdown)
    return invalid("software SMI requires a live instruction boundary");
  if (S.SMM->Active) {
    X86ModeExecutionResult R;
    R.Status = X86LegacyExecutionStatus::EventDeferred;
    return R;
  }
  if (auto Error = admit(S, Options, NormalMemory))
    return std::move(Error);
  return detail::enterX86Smm(S, NormalMemory, *SMRAM, Options);
}

llvm::Expected<X86ExecutionMode>
X86ModeExecution::mode(const X86ModeState &S) const {
  auto Active = memoryFor(S);
  if (!Active)
    return Active.takeError();
  auto &Memory = *Active;
  const auto &C = Memory.controls();
  if (auto Error = validateX86PagingState(Memory.features(), C))
    return std::move(Error);
  const auto &CS = S.Core.Core.Core.CS;
  const bool VM86 = modeRegisters(S).RFlags & 0x20000;
  if (!(C.CR0 & 1) && !VM86 && !(CS.Attributes & 0x2000) &&
      modeRegisters(S).PC <= UINT32_MAX)
    return CS.Attributes & 0x4000 ? X86ExecutionMode::Real32
                                  : X86ExecutionMode::Real16;
  if (!(C.CR0 & 1) || (VM86 && (C.EFER & 0x400)) ||
      ((CS.Attributes & 0x2000) &&
       (!(C.EFER & 0x400) || (CS.Attributes & 0x4000))))
    return invalid("mixed-mode dispatch requires protected or IA-32e state");
  if ((C.EFER & 0x400) && (CS.Attributes & 0x2000))
    return X86ExecutionMode::Long64;
  if (modeRegisters(S).PC > UINT32_MAX)
    return invalid(
        "non-64-bit execution requires a representable instruction offset");
  if (VM86)
    return X86ExecutionMode::Virtual8086;
  if (C.EFER & 0x400)
    return CS.Attributes & 0x4000 ? X86ExecutionMode::Compatibility32
                                  : X86ExecutionMode::Compatibility16;
  return CS.Attributes & 0x4000 ? X86ExecutionMode::Protected32
                                : X86ExecutionMode::Protected16;
}

llvm::Expected<X86ModeExecutionResult>
X86ModeExecution::step(X86ModeState &S, const X86ModeInstruction *I) {
  auto Active = memoryFor(S);
  if (!Active)
    return Active.takeError();
  auto &Memory = *Active;
  std::shared_ptr<detail::X86ModeContinuation> P;
  if (S.Pending) {
    P = std::make_shared<detail::X86ModeContinuation>(*S.Pending);
  } else {
    if (!I)
      return invalid("mixed-mode instruction binding is missing");
    if (auto Error = admit(S, Options, Memory))
      return std::move(Error);
    P = std::make_shared<detail::X86ModeContinuation>();
    P->Options = Options;
    P->Instruction = *I;
    P->Low = detail::projectNon64(S, !(Memory.controls().CR0 & 1));
  }
  X86ModeExecutionResult R;
  if (P->eventPending()) {
    deliver(S, Memory, *P, R);
    reportReceipts(*P, R);
    retain(S, Memory, std::move(P));
    return R;
  }
  const auto &Binding = P->Instruction;
  const auto &O = P->Options;
  auto Mode = P->Low.PendingTask
                  ? llvm::Expected<X86ExecutionMode>(Binding.Mode)
                  : mode(S);
  if (!Mode)
    return Mode.takeError();
  const uint64_t Address =
      *Mode == X86ExecutionMode::Long64
          ? modeRegisters(S).PC
          : uint32_t(S.Core.Core.Core.CS.Base + modeRegisters(S).PC);
  // A published task may suspend while validating its incoming selectors.
  // The journal still belongs to the already authenticated outgoing opcode.
  if (!P->Low.PendingTask &&
      (Binding.Mode != *Mode || Binding.Address != Address))
    return invalid("original instruction binding disagrees with the full "
                   "address and execution mode");
  const auto FinishSystem = [&](X86ModeExecutionResult Result,
                                X86EventKind FaultKind = X86EventKind::Fault) {
    P->AttemptConsumed |= Result.InstructionAttempted;
    if (Result.Exception) {
      const auto &E = *Result.Exception;
      P->Event = X64ContinuationEventV1{
          modeRegisters(S).PC,
          E.ErrorCode,
          E.CR2.value_or(0),
          E.Vector,
          Result.InstructionCompleted ? X86EventKind::Trap : FaultKind,
          uint8_t(S.STIBlocked),
          uint8_t(S.SSBlocked),
          uint8_t(O.Legacy.NMIBlockedBySTI.value_or(false))};
      deliver(S, Memory, *P, Result);
    }
    reportReceipts(*P, Result);
    retain(S, Memory, std::move(P));
    return Result;
  };
  static constexpr uint8_t RsmOpcode[]{0x0f, 0xaa}, HaltOpcode[]{0xf4};
  if (*Mode == X86ExecutionMode::Long64)
    if (auto Halt = systemLockPrefix(Binding.Bytes, HaltOpcode, true)) {
      X86EventKind Kind = X86EventKind::Fault;
      auto Got = executeWideHalt(S, Memory, Binding, O, *Halt, Kind);
      if (!Got)
        return Got.takeError();
      P->PendingHaltFetch =
          Got->Status == X86LegacyExecutionStatus::BackingFault;
      return FinishSystem(std::move(*Got), Kind);
    }
  auto Rsm = systemLockPrefix(Binding.Bytes, RsmOpcode,
                              *Mode == X86ExecutionMode::Long64);
  if (S.SMM && Rsm) {
    if (!P->PendingRSM)
      P->PendingRSM.emplace();
    auto Got =
        detail::executeX86Rsm(S, NormalMemory, Memory, Binding.Bytes, *Rsm,
                              P->AttemptConsumed, O, *P->PendingRSM);
    if (!Got)
      return Got.takeError();
    if (Got->Status != X86LegacyExecutionStatus::BackingFault)
      P->PendingRSM.reset();
    return FinishSystem(std::move(*Got));
  }
  static constexpr uint8_t Enclu[]{0x0f, 0x01, 0xd7};
  bool Illegal = false;
  if (Memory.sgxMemory() && Memory.sgxMemory()->active() &&
      (!Binding.Body || !Binding.Validate)) {
    auto Denied = detail::X86SgxExecution::illegalInstruction(Binding.Bytes,
                                                              Binding.Address);
    if (!Denied)
      return Denied.takeError();
    Illegal = *Denied;
  }
  if (*Mode == X86ExecutionMode::Long64 && Memory.sgxMemory() &&
      (Illegal || llvm::ArrayRef(Binding.Bytes) == llvm::ArrayRef(Enclu))) {
    if (!P->PendingSGX)
      P->PendingSGX.emplace();
    auto Got = detail::X86SgxExecution::instruction(
        S, Memory, P->AttemptConsumed, Binding.Bytes, Illegal, *P->PendingSGX,
        O.MXCSRMask);
    if (!Got)
      return Got.takeError();
    R = std::move(*Got);
    if (R.Status != X86LegacyExecutionStatus::BackingFault)
      P->PendingSGX.reset();
    if (R.InstructionCompleted) {
      S.STIBlocked = S.SSBlocked = false;
      auto Controls = Memory.controls();
      Controls.RFlags = modeRegisters(S).RFlags;
      llvm::cantFail(Memory.bindControls(Controls));
    }
    return FinishSystem(std::move(R));
  }
  const uint64_t OldEFER = Memory.controls().EFER;
  const bool OldPE = Memory.controls().CR0 & 1;
  if (*Mode == X86ExecutionMode::Long64) {
    if (!Binding.Body || !Binding.Validate)
      return invalid(
          "64-bit mode requires matching generated source body and validator");
    auto WideOptions = O.Long;
    X64PagedDebugState EmptyDebug;
    WideOptions.Debug = S.Debug ? &*S.Debug : &EmptyDebug;
    if (!S.Debug)
      WideOptions.DebugProfile = X86InstructionDebugProfile::InstructionStep;
    X64PagedExecution Wide(Memory, WideOptions, P->WideInstruction);
    auto Got =
        Wide.execute(S.Core.Core, Binding.Address, Binding.Bytes, Binding.Body,
                     Binding.Validate, O.MXCSRMask, O.PagingAddressBits,
                     S.SSBlocked, Binding.BranchDebug);
    P->WideInstruction = Wide.continuation();
    const auto &W = Got ? *Got : Wide.lastResult();
    P->Event = W.RequestedEvent;
    R.InstructionAttempted = W.InstructionAttempted;
    R.InstructionCompleted = W.InstructionCompleted;
    R.CR8Written = W.CR8Written;
    R.Exception = W.Exception;
    R.BackingFault = W.BackingFault;
    R.DeviceTransferCompleted = W.DeviceTransferCompleted;
    R.PortInputBytesWritten = W.PortInputBytesWritten;
    P->AttemptConsumed =
        bool(Wide.pendingProgress() & kX64PagedAttemptConsumed);
    if (W.Delivery) {
      mergeEvent(R, *W.Delivery);
      P->WideEvent = W.Delivery->Continuation;
      if (W.Delivery->InputAccepted || P->WideEvent)
        S.STIBlocked = S.SSBlocked = S.Halted = false;
    } else if (W.InstructionCompleted ||
               W.Exit == uint64_t(X64PagedExit::RepeatContinue)) {
      S.STIBlocked = W.StartsSTIShadow;
      S.SSBlocked = W.StartsSSShadow;
    }
    if (!Got) {
      if (!W.InstructionAttempted && !W.InstructionCompleted &&
          !P->instructionPending() && !P->eventPending()) {
        retain(S, Memory, std::move(P));
        return Got.takeError();
      }
      R.OwnerError = llvm::toString(Got.takeError());
      R.Status = X86LegacyExecutionStatus::ModeUnavailable;
    } else if (!W.Delivery) {
      if (W.Exit == uint64_t(X64PagedExit::BackingFault))
        R.Status = X86LegacyExecutionStatus::BackingFault;
      else if (W.Exit == uint64_t(X64PagedExit::CodeChanged))
        R.Status = X86LegacyExecutionStatus::CodeChanged;
      else if (W.Exit == uint64_t(X64PagedExit::RepeatContinue))
        R.Status = X86LegacyExecutionStatus::RepeatContinue;
      else if (W.Exit == uint64_t(X64PagedExit::DeviceUnavailable))
        R.Status = X86LegacyExecutionStatus::DeviceUnavailable;
      else if (W.Exit == uint64_t(X64ContinuationExitV1::InvalidState) ||
               W.Exit == uint64_t(X64ContinuationExitV1::NativeInstruction))
        R.Status = X86LegacyExecutionStatus::ModeUnavailable;
    }
  } else {
    auto LowOptions = O.Legacy;
    const auto &V = S.Core.Core;
    LowOptions.Non64Mode =
        X86Non64ModeBinding{V.GDTR.Base, V.LDTR.Base,
                            S.Debug ? S.Debug->DR : std::array<uint64_t, 4>{},
                            O.EFERSystemCall, V.TR.Base};
    auto Got =
        executeX86Non64Instruction(P->Low, Memory, Binding.Bytes, LowOptions);
    P->AttemptConsumed = P->Low.PendingInstructionAttempted;
    if (!Got) {
      if (P->instructionPending())
        detail::publishNon64(S, P->Low, R);
      retain(S, Memory, std::move(P));
      return Got.takeError();
    }
    static_cast<X86LegacyExecutionResult &>(R) = *Got;
    detail::publishNon64(S, P->Low, R);
    if (OldPE != bool(Memory.controls().CR0 & 1))
      S.RetainedRealCS = P->Low.Core.Segments[1].RetainedRealCS = !OldPE;
    if ((OldEFER ^ Memory.controls().EFER) & 0x400) {
      auto &Registers = modeRegisters(S);
      std::copy(O.Undefined->GPR.begin(), O.Undefined->GPR.end(),
                Registers.GPR + 8);
      for (unsigned N = 0; N != 8; ++N)
        std::copy(O.Undefined->XMM[N].begin(), O.Undefined->XMM[N].end(),
                  Registers.ZMM[N + 8]);
    }
    if (R.RequestedEvent) {
      const auto &E = *R.RequestedEvent;
      P->SoftwareINT = E.SoftwareINT;
      P->Event = X64ContinuationEventV1{
          R.WideFarTransfer && R.InstructionCompleted ? R.WideFarTransfer->PC
                                                      : E.SavedEIP,
          E.ErrorCode,
          R.Exception ? R.Exception->CR2.value_or(E.FaultAddress)
                      : E.FaultAddress,
          E.Vector,
          E.Kind,
          uint8_t(S.STIBlocked),
          uint8_t(S.SSBlocked),
          uint8_t(O.Legacy.NMIBlockedBySTI.value_or(false))};
      deliver(S, Memory, *P, R);
      R.RequestedEvent.reset();
    }
  }
  if (R.Status == X86LegacyExecutionStatus::Shutdown)
    S.Shutdown = true;
  reportReceipts(*P, R);
  retain(S, Memory, std::move(P));
  return R;
}

llvm::Expected<X86ModeExecutionResult>
X86ModeExecution::interrupt(X86ModeState &S, X86EventKind Kind,
                            uint8_t Vector) {
  auto Active = memoryFor(S);
  if (!Active)
    return Active.takeError();
  auto &Memory = *Active;
  if (S.Pending)
    return invalid(
        "resume pending mixed-mode work before supplying an interrupt");
  if (S.Shutdown)
    return invalid("a shutdown CPU requires explicit reset state");
  if ((Kind != X86EventKind::Interrupt && Kind != X86EventKind::NMI) ||
      (Kind == X86EventKind::NMI && Vector != 2))
    return invalid("external input requires an IRQ or vector-2 NMI");
  if (auto Error = admit(S, Options, Memory))
    return std::move(Error);
  auto Current = mode(S);
  if (!Current)
    return Current.takeError();
  if (Kind == X86EventKind::NMI && S.STIBlocked && !S.SSBlocked &&
      !S.Core.Core.NMIBlocked && !Options.Legacy.NMIBlockedBySTI)
    return invalid("NMI under STI requires a bound inhibition policy");

  auto P = std::make_shared<detail::X86ModeContinuation>();
  P->Options = Options;
  P->Low = detail::projectNon64(S, !(Memory.controls().CR0 & 1));
  P->Event = X64ContinuationEventV1{
      modeRegisters(S).PC,
      0,
      0,
      Vector,
      Kind,
      uint8_t(S.STIBlocked),
      uint8_t(S.SSBlocked),
      uint8_t(Options.Legacy.NMIBlockedBySTI.value_or(false))};
  auto Next = S;
  // Compatibility interrupt entry observes complete GPRs, including the old
  // stack pointer. Apply the supplied undefined high halves only if this
  // event is accepted, while preserving every extended bank across LMA=1.
  if (*Current != X86ExecutionMode::Long64)
    detail::publishNon64(Next, P->Low, {});
  const auto OriginalControls = Memory.controls();
  X86ModeExecutionResult R;
  deliver(Next, Memory, *P, R);
  if (!R.EventAccepted) {
    // Both lower owners synchronize their flags cache before admission or
    // inhibition. An unaccepted event performed no RAM accesses; retain the
    // original cache too, without publishing the trial CPU or pending input.
    llvm::cantFail(Memory.bindControls(OriginalControls));
    return R;
  }
  reportReceipts(*P, R);
  retain(Next, Memory, std::move(P));
  S = std::move(Next);
  return R;
}

llvm::Expected<X86ModeExecutionResult>
X86ModeExecution::run(X86ModeState &S,
                      llvm::ArrayRef<X86ModeInstruction> Instructions,
                      uint64_t &Budget) {
  return runBoundaries(S, Instructions, Budget, nullptr);
}

llvm::Expected<X86ModeExecutionResult>
X86ModeExecution::run(X86ModeState &S,
                      llvm::ArrayRef<X86ModeInstruction> Instructions,
                      uint64_t &Budget, X86ModeInterrupts &Interrupts) {
  return runBoundaries(S, Instructions, Budget, &Interrupts);
}

llvm::Expected<X86ModeExecutionResult> X86ModeExecution::runBoundaries(
    X86ModeState &S, llvm::ArrayRef<X86ModeInstruction> Instructions,
    uint64_t &Budget, X86ModeInterrupts *Interrupts) {
  uint64_t Attempts = 0, Completions = 0;
  X86ModeExecutionResult Last;
  const auto Finish = [&]() {
    Last.Attempts = Attempts;
    Last.Completions = Completions;
    return Last;
  };
  for (;;) {
    auto Active = memoryFor(S);
    if (!Active)
      return Active.takeError();
    auto &Memory = *Active;
    if (S.Pending) {
      if (!detail::sameModeState(S, S.Pending->Expected) ||
          Memory.controls() != S.Pending->Controls ||
          Memory.features() != S.Pending->Features ||
          Memory.sgxBank() != S.Pending->EnclaveBank ||
          (Memory.sgxMemory() &&
           Memory.sgxMemory()->generation() != S.Pending->EnclaveGeneration))
        return invalid("resume mixed-mode progress with the retained complete "
                       "state and controls");
    }
    if (S.Shutdown) {
      Last.Status = X86LegacyExecutionStatus::Shutdown;
      return Finish();
    }
    if (Interrupts && !S.Pending) {
      if (Interrupts->APIC.readCr8() != S.Core.Core.Core.Core.Core.CR8)
        return invalid("CPU CR8 and local APIC task priority disagree");
      if (Interrupts->SMIPending) {
        auto Got = systemManagementInterrupt(S);
        if (!Got)
          return Got.takeError();
        if (Got->Status != X86LegacyExecutionStatus::EventDeferred) {
          if (Got->EventAccepted)
            Interrupts->SMIPending = false;
          Last = std::move(*Got);
          return Finish();
        }
      }
      const auto Fixed = Interrupts->APIC.nextFixedInterrupt();
      for (auto Kind : {X86EventKind::NMI, X86EventKind::Interrupt}) {
        const bool NMI = Kind == X86EventKind::NMI;
        if (NMI ? !Interrupts->NMIPending : !Fixed)
          continue;
        const auto Vector = NMI ? uint8_t{2} : *Fixed;
        auto Got = interrupt(S, Kind, Vector);
        if (!Got)
          return Got.takeError();
        if (Got->Status == X86LegacyExecutionStatus::EventDeferred)
          continue;
        if (Got->EventAccepted) {
          if (NMI)
            Interrupts->NMIPending = false;
          else if (!Interrupts->APIC.acceptFixedInterrupt(Vector))
            llvm_unreachable("serialized APIC candidate changed during entry");
        }
        Last = std::move(*Got);
        return Finish();
      }
    }
    const bool Event = S.Pending && S.Pending->eventPending();
    const bool Paid = S.Pending && S.Pending->instructionPending() &&
                      S.Pending->AttemptConsumed;
    if (!Event && S.Halted) {
      Last.Status = X86LegacyExecutionStatus::Halted;
      return Finish();
    }
    if (!Event && !Paid && !Budget) {
      Last.BudgetExhausted = true;
      return Finish();
    }
    const X86ModeInstruction *I = nullptr;
    if (!S.Pending) {
      auto Current = mode(S);
      if (!Current)
        return Current.takeError();
      const auto Address =
          *Current == X86ExecutionMode::Long64
              ? modeRegisters(S).PC
              : uint32_t(S.Core.Core.Core.CS.Base + modeRegisters(S).PC);
      for (const auto &Candidate : Instructions)
        if (Candidate.Mode == *Current && Candidate.Address == Address) {
          if (I)
            return invalid(
                "duplicate instruction binding for address and mode");
          I = &Candidate;
        }
      if (!I) {
        Last.MissingInstruction = true;
        return Finish();
      }
    }
    auto Got = step(S, I);
    if (!Got)
      return Got.takeError();
    Last = std::move(*Got);
    if (Last.CR8Written && Interrupts &&
        !Interrupts->APIC.writeCr8(S.Core.Core.Core.Core.Core.CR8))
      llvm_unreachable("completed CR8 write was out of range");
    if (Last.InstructionAttempted) {
      ++Attempts;
      --Budget;
    }
    Completions += Last.InstructionCompleted;
    if (Interrupts && Last.Status == X86LegacyExecutionStatus::Halted &&
        Last.InstructionCompleted)
      continue;
    if (!Last.OwnerError.empty() ||
        Last.Status != X86LegacyExecutionStatus::Completed)
      return Finish();
    if (!Last.InstructionAttempted && !Last.InstructionCompleted && !S.Pending)
      return invalid("mixed-mode instruction made no architectural progress");
  }
}

} // namespace neverd::analysis
