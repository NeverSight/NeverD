//===- X64PagedEventDelivery.cpp - Shared paged IDT entry -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Evaluate the existing event lowering with exact scalar values and paged
/// memory; do not duplicate the descriptor or stack-frame algorithm.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"

#include "X64ContinuationDelivery.h"
#include "X64ContinuationEvents.h"
#include "X64ContinuationSegmentMoves.h"
#include "X64FarPointerEncoding.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/translate/X86/X86WatchpointObservation.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::analysis {
namespace detail {
struct X64PagedTransitionProgress {
  X64ContinuationStateV6 State;
  translate::X86PagingState Controls;
  translate::X86PagingFeatures Features;
  llvm::SmallVector<X64PagedEventContinuation::Access> CompletedAccesses;
  X64PagedAlignmentPriority AlignmentPriority;
  bool AttemptConsumed = false;
};
struct X64PagedReturnContinuation : X64PagedTransitionProgress {
  uint16_t FieldBytes = 0, Discard = 0;
  bool Interrupt = false, TruncateCompatibilityIP = false;
  X86DescriptorCanonicalityProfile Canonicality;
};
struct X64PagedSegmentContinuation : X64PagedTransitionProgress {
  llvm::SmallVector<uint8_t, 15> Bytes;
  std::optional<X64NullSegmentBasePolicy> NullBasePolicy;
  X86DescriptorCanonicalityProfile Canonicality;
  bool FarPointer = false;
};
} // namespace detail
namespace {
using translate::MemoryAccessKind;
using translate::X86MemoryException;
using translate::X86PagedMemory;
using translate::X86PageWalkStatus;

template <class T> uint64_t nativeValue(const uint8_t *Bytes) {
  T Value;
  std::memcpy(&Value, Bytes, sizeof(Value));
  return Value;
}

uint64_t nativeValue(const uint8_t *Bytes, unsigned Size) {
  switch (Size) {
  case 1:
    return nativeValue<uint8_t>(Bytes);
  case 2:
    return nativeValue<uint16_t>(Bytes);
  case 4:
    return nativeValue<uint32_t>(Bytes);
  case 8:
    return nativeValue<uint64_t>(Bytes);
  default:
    llvm_unreachable("event scalar width");
  }
}

template <class T> void nativeStore(uint8_t *Bytes, uint64_t Value) {
  const T Narrow = Value;
  std::memcpy(Bytes, &Narrow, sizeof(Narrow));
}

void nativeStore(uint8_t *Bytes, unsigned Size, uint64_t Value) {
  switch (Size) {
  case 1:
    return nativeStore<uint8_t>(Bytes, Value);
  case 2:
    return nativeStore<uint16_t>(Bytes, Value);
  case 4:
    return nativeStore<uint32_t>(Bytes, Value);
  case 8:
    return nativeStore<uint64_t>(Bytes, Value);
  default:
    llvm_unreachable("event scalar width");
  }
}

class TransitionWriter {
public:
  static constexpr bool FaultableMemory = true;
  X64ContinuationSourceOptions Options;
  X64PagedEventResult Result;
  std::optional<X86MemoryException> Fault;
  std::optional<llvm::Error> Error;
  std::optional<X64ContinuationExitV1> Exit;
  std::optional<X64PagedAlignmentPriority> AlignmentPriority;
  bool Stopped = false;
  bool InstructionEffects = false;
  bool Resuming = false;
  bool CaptureAccesses = false;
  llvm::SmallVector<X64PagedEventContinuation::Access> CompletedAccesses;
  size_t AccessCursor = 0;

  TransitionWriter(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                   const X64ContinuationEventV1 &Event, uint32_t MXCSRMask)
      : State(State), Memory(Memory), Event(Event) {
    Options.MXCSRMask = MXCSRMask;
  }

  NdVar temporary(uint16_t Bytes = 8) {
    const auto Id = Values.size();
    Values.push_back(0);
    return NdVar::tmp(Id, Bytes);
  }

  uint64_t value(NdVar V) const {
    if (Stopped)
      return 0;
    assert((V.isConst() || (V.isTemp() && V.Offset < Values.size())) &&
           "event lowering reads only constants and initialized temporaries");
    return V.isConst() ? V.Offset : Values[V.Offset];
  }

  void emit(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
    if (Stopped)
      return;
    if (Code == NdOp::LOAD || Code == NdOp::STORE) {
      memory(Code, Output, Inputs);
      return;
    }
    if (Code == NdOp::SELECT) {
      assert(Inputs.size() == 3 && Inputs.begin()[0].Size == 1 &&
             Inputs.begin()[1].Size == Output.Size &&
             Inputs.begin()[2].Size == Output.Size);
      Values[Output.Offset] =
          value(Inputs.begin()[value(*Inputs.begin()) ? 1 : 2]);
      return;
    }
    if (Code == NdOp::SUBBYTES) {
      assert(Inputs.size() == 2 && value(Inputs.begin()[1]) == 0 &&
             Output.Size <= Inputs.begin()[0].Size && Output.Size <= 8);
      Values[Output.Offset] =
          value(*Inputs.begin()) & (UINT64_MAX >> ((8 - Output.Size) * 8));
      return;
    }
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    for (auto Input : Inputs)
      Op.addInput(Input);
    const auto V = lowScalarConstantResult(
        Op,
        [&](NdVar Input) -> std::optional<uint64_t> { return value(Input); });
    if (!V) {
      Error = llvm::createStringError(
          llvm::errc::not_supported,
          "event lowering used unsupported scalar operation %u",
          unsigned(Code));
      Stopped = true;
      return;
    }
    Values[Output.Offset] = *V;
  }

  NdVar load(uint64_t Offset, uint16_t Bytes = 8) {
    auto V = temporary(Bytes);
    if (!Stopped) {
      assert(Offset + Bytes <= sizeof(State));
      Values[V.Offset] = nativeValue(
          reinterpret_cast<const uint8_t *>(&State) + Offset, Bytes);
    }
    return V;
  }

  void store(uint64_t Offset, NdVar V) {
    if (!Stopped) {
      assert(Offset + V.Size <= sizeof(State));
      nativeStore(reinterpret_cast<uint8_t *>(&State) + Offset, V.Size,
                  value(V));
    }
  }

  void write(NdVar Destination, NdVar V) {
    assert(Destination.isReg() && Destination.Offset <= x86reg::R15 &&
           Destination.Offset % 8 == 0 && Destination.Size == V.Size &&
           (V.Size == 2 || V.Size == 4 || V.Size == 8));
    // A dword GPR write clears its upper half; a word write preserves it.
    store(offsetof(X64ContinuationStateV1, GPR) + Destination.Offset,
          getTargetRegInfo(Arch::X64).writeZeroExtends(Destination.Offset,
                                                       Destination.Size)
              ? NdVar::scalar(value(V), 8)
              : V);
  }

  NdVar eventValue(NdVar, uint64_t Offset, uint16_t Bytes) {
    auto V = temporary(Bytes);
    if (!Stopped) {
      assert(Offset + Bytes <= sizeof(Event));
      Values[V.Offset] = nativeValue(
          reinterpret_cast<const uint8_t *>(&Event) + Offset, Bytes);
    }
    return V;
  }

  void storeIf(NdVar Condition, NdVar Address, NdVar Value) {
    if (value(Condition))
      emit(NdOp::STORE, {}, {Address, Value});
  }

  NdVar updateEventDescriptorAccessed(NdVar Address, NdVar Original) {
    if (Stopped || (value(Original) & (uint64_t{1} << 40)))
      return Original;
    const auto Image = temporary();
    memory(NdOp::STORE, Image, {Address, NdVar::scalar(uint64_t{1} << 40, 8)},
           true);
    return Image;
  }

  NdVar loadMemoryIf(NdVar Condition, NdVar Address, NdVar Fallback) {
    if (!value(Condition))
      return Fallback;
    auto Result = temporary(Fallback.Size);
    emit(NdOp::LOAD, Result, {Address});
    return Result;
  }

  void rejectIf(NdVar Bad) {
    if (value(Bad)) {
      Stopped = true;
      Result.Status = X64PagedEventStatus::InvalidState;
    }
  }

  void architecturalFaultIf(NdVar Bad, uint8_t Vector, NdVar ErrorCode) {
    if (value(Bad)) {
      Fault =
          X86MemoryException{Vector, uint32_t(value(ErrorCode)), std::nullopt};
      Stopped = true;
    }
  }

  template <class Emit> void exitIf(NdVar Condition, Emit EmitExit) {
    if (value(Condition))
      EmitExit();
  }

  void beginEvent(NdVar NMI) {
    if (Stopped)
      return;
    Result.InputAccepted = true;
    if (value(NMI))
      State.NMIBlocked = 1;
    if (Event.Kind == X64ContinuationEventKind::Fault && Event.Vector == 14)
      State.Core.Core.CR2 = Event.FaultAddress;
  }

  void eventStack(NdVar CPL) {
    if (!Stopped) {
      Explicit = Stack = true;
      AccessCPL = value(CPL);
    }
  }

  void beginReturn() {
    if (!Stopped) {
      State.NMIBlocked = 0;
      InstructionEffects = true;
    }
  }

  void instructionAccess(bool Enable, bool Stack = false) {
    Explicit = Enable;
    this->Stack = Enable && Stack;
    AccessCPL = State.Core.CS.Selector & 3;
  }

  void invalidateMonitor(std::optional<NdVar> = std::nullopt) {
  } // The V6 state has no monitor bank.

  void compatibilityChoices(
      llvm::ArrayRef<X64ContinuationCompatibilityChoice> Choices) {
    if (Stopped)
      return;
    auto &Core = State.Core.Core.Core.Core;
    for (const auto &Choice : Choices) {
      auto &Register = Core.GPR[Choice.Output.Offset / 8];
      Register = (Register & UINT32_MAX) |
                 (Core.Undefined[Choice.Slot] & ~uint64_t(UINT32_MAX));
    }
  }

  void leave(X64ContinuationExitV1 Exit,
             std::optional<NdVar> PC = std::nullopt) {
    if (Stopped)
      return;
    if (PC)
      State.Core.Core.Core.Core.PC = value(*PC);
    this->Exit = Exit;
    Result.Status = Exit == X64ContinuationExitV1::Exception
                        ? X64PagedEventStatus::Delivered
                    : Exit == X64ContinuationExitV1::EventDeferred
                        ? X64PagedEventStatus::Deferred
                        : X64PagedEventStatus::InvalidState;
    Stopped = true;
  }

private:
  void memory(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs,
              bool DescriptorAccessed = false) {
    const bool Write = Code == NdOp::STORE;
    const unsigned Size = Write ? Inputs.begin()[1].Size : Output.Size;
    assert(Size && Size <= 8);
    std::array<uint8_t, 8> Bytes{};
    const translate::X86LinearAccess Access{
        value(*Inputs.begin()),
        Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        Explicit ? AccessCPL : uint8_t(0), !Explicit, Stack};
    if (AccessCursor < CompletedAccesses.size()) {
      const auto &Completed = CompletedAccesses[AccessCursor++];
      if (Completed.Address != Access.Address || Completed.Bytes != Size ||
          Completed.Write != Write ||
          Completed.DescriptorAccessed != DescriptorAccessed ||
          (Write && !DescriptorAccessed &&
           Completed.Value != value(Inputs.begin()[1]))) {
        Error = llvm::createStringError(
            llvm::errc::invalid_argument,
            "paged transition resume changed a completed access");
        Stopped = true;
        return;
      }
      if (!Write || DescriptorAccessed)
        Values[Output.Offset] = Completed.Value;
      return;
    }
    const bool AlignmentFault =
        AlignmentPriority && Explicit && AccessCPL == 3 &&
        (Memory.controls().CR0 & 0x40000) &&
        (Memory.controls().RFlags & 0x40000) && (Access.Address & (Size - 1));
    if (AlignmentFault &&
        *AlignmentPriority == X64PagedAlignmentPriority::BeforePaging) {
      auto Checked =
          translate::checkX86LinearAccess(Memory.controls(), Access, Size);
      if (!Checked)
        Error = Checked.takeError();
      else
        Fault = Checked->value_or(X86MemoryException{17, 0, std::nullopt});
      Stopped = true;
      return;
    }
    if (Write) {
      const auto Value = value(Inputs.begin()[1]);
      for (unsigned I = 0; I != Size; ++I)
        Bytes[I] = Value >> (I * 8);
    }
    auto AccessMemory =
        [&]() -> llvm::Expected<translate::X86MemoryAccessResult> {
      if (!Write)
        return Memory.load(Access,
                           llvm::MutableArrayRef(Bytes).take_front(Size));
      auto Prepared = Memory.prepareStore(Access, Size);
      if (!Prepared)
        return Prepared.takeError();
      // A post-translation #AC must retain page-walk effects without
      // publishing the explicit selector store.
      if (Prepared->Status == X86PageWalkStatus::Completed && !AlignmentFault) {
        if (DescriptorAccessed) {
          // Use the exact prepared physical fragments. Its page walks can
          // update A/D bits inside this descriptor; a second translation or
          // writing the earlier check image would lose those effects.
          auto Current = Memory.loadPreparedStore(*Prepared, Bytes);
          if (!Current || Current->Status != X86PageWalkStatus::Completed)
            return Current;
          Bytes[5] |= 1;
        }
        Memory.commitStore(*Prepared, llvm::ArrayRef(Bytes).take_front(Size));
      }
      return std::move(
          static_cast<translate::X86MemoryAccessResult &>(*Prepared));
    };
    auto R = AccessMemory();
    if (!R) {
      Error = R.takeError();
      Stopped = true;
    } else if (R->Status == X86PageWalkStatus::Exception) {
      Fault = R->Exception;
      if (Fault->CR2)
        State.Core.Core.CR2 = *Fault->CR2;
      Stopped = true;
    } else if (R->Status == X86PageWalkStatus::BackingFault) {
      Result.Status = X64PagedEventStatus::BackingFault;
      Result.BackingFault = R->BackingFault;
      Stopped = true;
    } else if (AlignmentFault) {
      Fault = X86MemoryException{17, 0, std::nullopt};
      Stopped = true;
    } else {
      uint64_t Value =
          Write && !DescriptorAccessed ? value(Inputs.begin()[1]) : 0;
      if (!Write || DescriptorAccessed) {
        for (unsigned I = 0; I != Size; ++I)
          Value |= uint64_t(Bytes[I]) << (I * 8);
        Values[Output.Offset] = Value;
      } else if (AlignmentPriority) {
        InstructionEffects = true;
      }
      if (CaptureAccesses) {
        CompletedAccesses.push_back(
            {Access.Address, Value, uint8_t(Size), Write, DescriptorAccessed});
        ++AccessCursor;
      }
    }
  }

  X64ContinuationStateV6 &State;
  X86PagedMemory &Memory;
  const X64ContinuationEventV1 &Event;
  llvm::SmallVector<uint64_t> Values;
  bool Explicit = false, Stack = false;
  uint8_t AccessCPL = 0;
};

llvm::Expected<X64PagedEventResult> deliverEventSequence(
    X64ContinuationStateV6 &State, X86PagedMemory &Memory,
    const X64ContinuationEventV1 &Input, uint32_t MXCSRMask, uint64_t FaultPC,
    const translate::X86EventDeliveryOptions &Options,
    X86DescriptorCanonicalityProfile DescriptorCanonicality,
    std::optional<X86MemoryException> LastException = {}, bool Accepted = false,
    llvm::SmallVector<X64PagedEventContinuation::Access> CompletedAccesses = {},
    X64PagedDebugState *Debug = nullptr,
    X86InstructionDebugProfile DebugProfile =
        X86InstructionDebugProfile::InstructionStep,
    uint8_t DataBreakpoints = 0, bool SoftwareCompleted = false,
    bool InputAccepted = false) {
  if (!x86DescriptorCanonicalityValid(DescriptorCanonicality))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "invalid event descriptor canonicality profile");
  if (!Options.MaxAttempts)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "event delivery requires a positive work budget");
  if (!Accepted && (!CompletedAccesses.empty() || DataBreakpoints))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "unaccepted event cannot retain partial accesses");
  for (const auto &Access : CompletedAccesses)
    if (Access.DescriptorAccessed && (!Access.Write || Access.Bytes != 8 ||
                                      !(Access.Value & (uint64_t{1} << 40))))
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "invalid completed event descriptor update");
  if (!(Memory.controls().EFER & 0x400))
    return llvm::createStringError(
        llvm::errc::not_supported,
        "paged V6 event delivery requires IA-32e mode");
  auto Controls = Memory.controls();
  if (DataBreakpoints && (!Debug || !x86DebugWatchpoints(DebugProfile) ||
                          (DataBreakpoints & 0xf0)))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "event continuation requires its watchpoint bank");
  if (Debug &&
      !x86InstructionDebugValid(Debug->DR6, Debug->DR7, Debug->DebugCTL,
                                DebugProfile, Controls.CR4, Debug->PendingData))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid event debug bank or profile");
  if (Controls.CR4 & (uint64_t(1) << 32))
    return llvm::createStringError(llvm::errc::not_supported,
                                   "paged V6 IDT delivery requires CR4.FRED=0");
  Controls.RFlags = State.Core.Core.Core.Core.RFlags;
  if (auto Error = Memory.bindControls(Controls))
    return std::move(Error);
  X64PagedEventResult Result;
  Result.InputAccepted = Accepted || InputAccepted;
  Result.LastException = LastException;
  Result.SoftwareCompleted = SoftwareCompleted;
  translate::X86WatchpointObservation Observation(Memory, Debug, DebugProfile,
                                                  DataBreakpoints);
  auto Event = Input;
  for (;;) {
    if (Result.DeliveryAttempts == Options.MaxAttempts) {
      Result.Status = X64PagedEventStatus::BudgetExhausted;
      Result.Continuation = X64PagedEventContinuation{Event,
                                                      FaultPC,
                                                      Result.LastException,
                                                      {},
                                                      Observation.Matches,
                                                      Result.SoftwareCompleted,
                                                      false,
                                                      DescriptorCanonicality};
      return Result;
    }
    TransitionWriter W(State, Memory, Event, MXCSRMask);
    W.Resuming = Accepted && Result.DeliveryAttempts == 0;
    W.CaptureAccesses = true;
    W.CompletedAccesses = std::move(CompletedAccesses);
    detail::X64ContinuationDelivery<TransitionWriter>(
        W, Controls.CR4 & 0x1000 ? 57 : 48, DescriptorCanonicality)
        .execute(NdVar::scalar(0, 8));
    if (Result.DeliveryAttempts == 0)
      Result.InputAccepted |= W.Result.InputAccepted;
    ++Result.DeliveryAttempts;
    if (W.Error)
      return std::move(*W.Error);
    if (Debug && (W.Result.InputAccepted || W.Resuming)) {
      if (x86DebugException(Event.Kind, Event.Vector))
        x86AcceptDebugException(*Debug);
      if (Event.Kind != X64ContinuationEventKind::Software)
        Debug->PendingData = 0;
    }
    if (!W.Fault) {
      Result.Status = W.Result.Status;
      Result.BackingFault = W.Result.BackingFault;
      if (Result.Status == X64PagedEventStatus::BackingFault &&
          Result.InputAccepted)
        Result.Continuation =
            X64PagedEventContinuation{Event,
                                      FaultPC,
                                      Result.LastException,
                                      std::move(W.CompletedAccesses),
                                      Observation.Matches,
                                      Result.SoftwareCompleted,
                                      true,
                                      DescriptorCanonicality};
      if (Result.Status == X64PagedEventStatus::Delivered) {
        Result.DeliveredVector = Event.Vector;
        const bool Software = Event.Kind == X64ContinuationEventKind::Software;
        Result.SoftwareCompleted |= Software;
        auto &Core = State.Core.Core.Core.Core;
        const auto &CS = State.Core.CS;
        const uint64_t LinearPC =
            CS.Attributes & 0x2000 ? Core.PC : uint32_t(CS.Base + Core.PC);
        if (Debug &&
            x86EventDataBreakpoints(*Debug, Software, Observation.Matches,
                                    LinearPC, Core.RFlags)) {
          Observation.Matches = 0;
          Event = {};
          Event.Kind = X64ContinuationEventKind::Trap;
          Event.Vector = 1;
          Event.SavedPC = FaultPC = Core.PC;
          Controls.RFlags = Core.RFlags;
          if (auto Error = Memory.bindControls(Controls))
            return std::move(Error);
          continue;
        }
      }
      Controls.RFlags = State.Core.Core.Core.Core.RFlags;
      if (auto Error = Memory.bindControls(Controls))
        return std::move(Error);
      return Result;
    }
    Result.LastException = W.Fault;
    Observation.Matches = 0;
    if (Debug)
      Debug->PendingData = 0;
    const bool FirstFault = Event.Kind == X64ContinuationEventKind::Fault;
    const bool FirstDouble =
        Event.Kind == X64ContinuationEventKind::Abort && Event.Vector == 8;
    const bool SecondContributory = x86ContributoryException(W.Fault->Vector);
    const bool SecondPage = x86PageClassException(W.Fault->Vector);
    if (FirstDouble && (SecondContributory || SecondPage)) {
      Result.Status = X64PagedEventStatus::Shutdown;
      return Result;
    }
    const bool Double =
        FirstFault && x86DoubleFaultPair(Event.Vector, W.Fault->Vector);
    Event = {};
    Event.SavedPC = FaultPC;
    Event.Vector = Double ? 8 : W.Fault->Vector;
    Event.Kind = Double ? X64ContinuationEventKind::Abort
                        : X64ContinuationEventKind::Fault;
    Event.ErrorCode = Double ? 0 : W.Fault->ErrorCode;
    Event.FaultAddress = Double ? 0 : W.Fault->CR2.value_or(0);
  }
}

} // namespace

llvm::Expected<X64PagedEventResult>
deliverX64PagedEvent(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                     const X64ContinuationEventV1 &Input, uint32_t MXCSRMask,
                     X64PagedDebugState *Debug,
                     X86InstructionDebugProfile DebugProfile,
                     const translate::X86EventDeliveryOptions &Options,
                     X86DescriptorCanonicalityProfile DescriptorCanonicality) {
  // Software entry saves fallthrough only on success. Its secondary faults
  // retain the original instruction PC, also across a physical backing stop.
  const uint64_t FaultPC = Input.Kind == X64ContinuationEventKind::Software
                               ? State.Core.Core.Core.Core.PC
                               : Input.SavedPC;
  return deliverEventSequence(State, Memory, Input, MXCSRMask, FaultPC, Options,
                              DescriptorCanonicality, {}, false, {}, Debug,
                              DebugProfile);
}

llvm::Expected<X64PagedEventResult>
resumeX64PagedEvent(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                    const X64PagedEventContinuation &Continuation,
                    uint32_t MXCSRMask, X64PagedDebugState *Debug,
                    X86InstructionDebugProfile DebugProfile,
                    const translate::X86EventDeliveryOptions &Options) {
  return deliverEventSequence(
      State, Memory, Continuation.Event, MXCSRMask, Continuation.FaultPC,
      Options, Continuation.DescriptorCanonicality, Continuation.LastException,
      Continuation.EventAccepted, Continuation.CompletedAccesses, Debug,
      DebugProfile, Continuation.DataBreakpoints,
      Continuation.SoftwareCompleted, true);
}

namespace {
void retainProgress(detail::X64PagedTransitionProgress &Pending,
                    const X64ContinuationStateV6 &State,
                    const X86PagedMemory &Memory, TransitionWriter &Writer,
                    bool AttemptConsumed) {
  Pending.State = State;
  Pending.Controls = Memory.controls();
  Pending.Features = Memory.features();
  Pending.CompletedAccesses = std::move(Writer.CompletedAccesses);
  Pending.AlignmentPriority = *Writer.AlignmentPriority;
  Pending.AttemptConsumed = AttemptConsumed;
}

llvm::Error
validateProgress(const X64ContinuationStateV6 &State,
                 const X86PagedMemory &Memory,
                 const detail::X64PagedTransitionProgress *Pending) {
  if (!Pending || State != Pending->State ||
      Memory.controls() != Pending->Controls ||
      Memory.features() != Pending->Features)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "paged transition resume changed CPU, paging controls or features");
  return llvm::Error::success();
}

llvm::Expected<X64PagedReturnResult>
returnFrame(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
            uint16_t FieldBytes, uint16_t Discard, bool Interrupt,
            X64PagedAlignmentPriority AlignmentPriority,
            X86DescriptorCanonicalityProfile Canonicality,
            bool TruncateCompatibilityIP = false,
            const detail::X64PagedReturnContinuation *Continuation = nullptr) {
  assert((FieldBytes == 2 || FieldBytes == 4 || FieldBytes == 8) &&
         "authenticated return operand size");
  auto Next = State;
  const X64ContinuationEventV1 Event{};
  TransitionWriter W(Next, Memory, Event, 0);
  W.AlignmentPriority = AlignmentPriority;
  W.CaptureAccesses = true;
  W.Resuming = Continuation != nullptr;
  if (Continuation)
    W.CompletedAccesses = Continuation->CompletedAccesses;
  detail::X64ContinuationEvents<TransitionWriter> Transfer(
      W, Memory.controls().CR4 & 0x1000 ? 57 : 48, Canonicality);
  if (Interrupt)
    Transfer.iret(FieldBytes);
  else
    Transfer.farReturn(FieldBytes, Discard, TruncateCompatibilityIP);
  // IRET unblocks NMI even on fault. Other register and segment-cache changes
  // are staged; the writer has already committed ordered RAM side effects.
  State.NMIBlocked = Next.NMIBlocked;
  if (W.Error)
    return std::move(*W.Error);
  X64PagedReturnResult Result;
  Result.Exception = W.Fault;
  Result.BackingFault = W.Result.BackingFault;
  Result.Completed = W.Exit == X64ContinuationExitV1::PrivilegeReturn;
  const bool Consumed = Continuation && Continuation->AttemptConsumed;
  Result.InstructionAttempted =
      !Consumed &&
      (Result.Completed || bool(Result.Exception) || W.InstructionEffects);
  if (Result.BackingFault) {
    auto Pending = std::make_shared<detail::X64PagedReturnContinuation>();
    retainProgress(*Pending, State, Memory, W,
                   Consumed || Result.InstructionAttempted);
    Pending->FieldBytes = FieldBytes;
    Pending->Discard = Discard;
    Pending->Interrupt = Interrupt;
    Pending->TruncateCompatibilityIP = TruncateCompatibilityIP;
    Pending->Canonicality = Canonicality;
    Result.Continuation = std::move(Pending);
  }
  if (Result.Completed) {
    State = Next;
    auto Controls = Memory.controls();
    Controls.RFlags = State.Core.Core.Core.Core.RFlags;
    if (auto Error = Memory.bindControls(Controls))
      return std::move(Error);
  }
  return Result;
}
} // namespace

llvm::Error
validateX64PagedReturnResume(const X64ContinuationStateV6 &State,
                             const X86PagedMemory &Memory,
                             const X64PagedReturnContinuation &Continuation) {
  return validateProgress(State, Memory, Continuation.get());
}

llvm::Expected<X64PagedReturnResult>
resumeX64PagedReturn(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                     const X64PagedReturnContinuation &Continuation) {
  if (auto Error = validateX64PagedReturnResume(State, Memory, Continuation))
    return std::move(Error);
  return returnFrame(State, Memory, Continuation->FieldBytes,
                     Continuation->Discard, Continuation->Interrupt,
                     Continuation->AlignmentPriority,
                     Continuation->Canonicality,
                     Continuation->TruncateCompatibilityIP, Continuation.get());
}

llvm::Expected<X64PagedInterruptReturnResult>
returnX64PagedInterrupt(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                        uint16_t FieldBytes,
                        X64PagedAlignmentPriority AlignmentPriority,
                        X64PagedIretAccessOrder AccessOrder) {
  X86DescriptorCanonicalityProfile Canonicality;
  switch (AccessOrder) {
  case X64PagedIretAccessOrder::FlagsFirst:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForRange;
    break;
  case X64PagedIretAccessOrder::FlagsFirstTailGPZero:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForStart;
    break;
  default:
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unknown paged IRET access-order profile");
  }
  return returnFrame(State, Memory, FieldBytes, 0, true, AlignmentPriority,
                     Canonicality);
}

llvm::Expected<X64PagedReturnResult>
returnX64PagedFar(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                  uint16_t FieldBytes, uint16_t Discard,
                  X64PagedAlignmentPriority AlignmentPriority,
                  X64PagedFarReturnProfile Profile) {
  X86DescriptorCanonicalityProfile Canonicality;
  bool TruncateCompatibilityIP = false;
  switch (Profile) {
  case X64PagedFarReturnProfile::SelectorFirst:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForRange;
    break;
  case X64PagedFarReturnProfile::SelectorFirstTruncatedIP:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForRange;
    TruncateCompatibilityIP = true;
    break;
  case X64PagedFarReturnProfile::SelectorFirstTailGPZero:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForStart;
    break;
  default:
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unknown paged far-return profile");
  }
  return returnFrame(State, Memory, FieldBytes, Discard, false,
                     AlignmentPriority, Canonicality, TruncateCompatibilityIP);
}

namespace {
llvm::Expected<X64PagedSegmentResult> segmentTransfer(
    X64ContinuationStateV6 &State, X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Instruction,
    X64PagedAlignmentPriority AlignmentPriority,
    std::optional<X64NullSegmentBasePolicy> NullBasePolicy, bool FarPointer,
    X86DescriptorCanonicalityProfile Canonicality,
    const detail::X64PagedSegmentContinuation *Continuation = nullptr) {
  auto Next = State;
  const X64ContinuationEventV1 Event{};
  TransitionWriter W(Next, Memory, Event, 0);
  W.AlignmentPriority = AlignmentPriority;
  W.CaptureAccesses = true;
  W.Resuming = Continuation != nullptr;
  if (Continuation)
    W.CompletedAccesses = Continuation->CompletedAccesses;
  const auto AddressBits = Memory.controls().CR4 & 0x1000 ? 57 : 48;
  const auto NextPC = State.Core.Core.Core.Core.PC + Instruction.size();
  if (FarPointer) {
    const auto Pointer = detail::x64FarPointerLoad(Instruction);
    if (!Pointer)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "invalid paged far-pointer encoding");
    detail::X64ContinuationEvents<TransitionWriter>(W, AddressBits,
                                                    Canonicality)
        .farPointerLoad(*Pointer, NextPC, NullBasePolicy);
  } else {
    const auto Move = detail::x64SegmentMove(Instruction);
    if (!Move)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "invalid paged selector transfer encoding");
    detail::X64ContinuationSegmentMoves<TransitionWriter>(W, AddressBits,
                                                          Canonicality)
        .execute(*Move, NextPC, NullBasePolicy);
  }
  if (W.Error)
    return std::move(*W.Error);
  X64PagedSegmentResult Result;
  Result.Exception = W.Fault;
  Result.BackingFault = W.Result.BackingFault;
  Result.Completed = W.Exit == X64ContinuationExitV1::Continue;
  const bool Consumed = Continuation && Continuation->AttemptConsumed;
  Result.InstructionAttempted =
      !Consumed &&
      (Result.Completed || bool(Result.Exception) || W.InstructionEffects);
  if (Result.BackingFault) {
    auto Pending = std::make_shared<detail::X64PagedSegmentContinuation>();
    retainProgress(*Pending, State, Memory, W,
                   Consumed || Result.InstructionAttempted);
    Pending->Bytes.assign(Instruction.begin(), Instruction.end());
    Pending->NullBasePolicy = NullBasePolicy;
    Pending->Canonicality = Canonicality;
    Pending->FarPointer = FarPointer;
    Result.Continuation = std::move(Pending);
  }
  if (Result.Completed)
    State = Next;
  return Result;
}
} // namespace

llvm::Expected<X64PagedSegmentResult>
loadX64PagedFarPointer(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                       llvm::ArrayRef<uint8_t> Instruction,
                       X64PagedAlignmentPriority AlignmentPriority,
                       X64PagedFarPointerProfile Profile,
                       std::optional<X64NullSegmentBasePolicy> NullBasePolicy) {
  X86DescriptorCanonicalityProfile Canonicality;
  switch (Profile) {
  case X64PagedFarPointerProfile::OffsetFirst:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForRange;
    break;
  case X64PagedFarPointerProfile::OffsetFirstTailGPZero:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForStart;
    break;
  default:
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unknown paged far-pointer profile");
  }
  return segmentTransfer(State, Memory, Instruction, AlignmentPriority,
                         NullBasePolicy, true, Canonicality);
}

llvm::Expected<X64PagedSegmentResult>
moveX64PagedSegment(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                    llvm::ArrayRef<uint8_t> Instruction,
                    X64PagedAlignmentPriority AlignmentPriority,
                    X64PagedSegmentMoveProfile Profile,
                    std::optional<X64NullSegmentBasePolicy> NullBasePolicy) {
  X86DescriptorCanonicalityProfile Canonicality;
  switch (Profile) {
  case X64PagedSegmentMoveProfile::SelectorFirst:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForRange;
    break;
  case X64PagedSegmentMoveProfile::SelectorFirstTailGPZero:
    Canonicality = X86DescriptorCanonicalityProfile::SelectorForStart;
    break;
  default:
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unknown paged selector-transfer profile");
  }
  return segmentTransfer(State, Memory, Instruction, AlignmentPriority,
                         NullBasePolicy, false, Canonicality);
}

llvm::Error
validateX64PagedSegmentResume(const X64ContinuationStateV6 &State,
                              const X86PagedMemory &Memory,
                              const X64PagedSegmentContinuation &Continuation) {
  return validateProgress(State, Memory, Continuation.get());
}

llvm::Expected<X64PagedSegmentResult>
resumeX64PagedSegment(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                      const X64PagedSegmentContinuation &Continuation) {
  if (auto Error = validateX64PagedSegmentResume(State, Memory, Continuation))
    return std::move(Error);
  return segmentTransfer(State, Memory, Continuation->Bytes,
                         Continuation->AlignmentPriority,
                         Continuation->NullBasePolicy, Continuation->FarPointer,
                         Continuation->Canonicality, Continuation.get());
}

} // namespace neverd::analysis
