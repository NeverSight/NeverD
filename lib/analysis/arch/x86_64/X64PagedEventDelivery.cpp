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

#include "neverd/ir/low/LowScalarConstant.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <utility>

namespace neverd::analysis {
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

class EventWriter {
public:
  static constexpr bool FaultableMemory = true;
  X64ContinuationSourceOptions Options;
  X64PagedEventResult Result;
  std::optional<X86MemoryException> Fault;
  std::optional<llvm::Error> Error;
  bool Stopped = false;

  EventWriter(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
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
    if (value(NMI))
      State.NMIBlocked = 1;
    if (Event.Kind == X64ContinuationEventKind::Fault && Event.Vector == 14)
      State.Core.Core.CR2 = Event.FaultAddress;
  }

  void eventStack(NdVar CPL) {
    if (!Stopped) {
      Frame = true;
      FrameCPL = value(CPL);
    }
  }

  void leave(X64ContinuationExitV1 Exit,
             std::optional<NdVar> PC = std::nullopt) {
    if (Stopped)
      return;
    if (PC)
      State.Core.Core.Core.Core.PC = value(*PC);
    Result.Status = Exit == X64ContinuationExitV1::Exception
                        ? X64PagedEventStatus::Delivered
                    : Exit == X64ContinuationExitV1::EventDeferred
                        ? X64PagedEventStatus::Deferred
                        : X64PagedEventStatus::InvalidState;
    Stopped = true;
  }

private:
  void memory(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
    const bool Write = Code == NdOp::STORE;
    const unsigned Size = Write ? Inputs.begin()[1].Size : Output.Size;
    assert(Size && Size <= 8);
    std::array<uint8_t, 8> Bytes{};
    const translate::X86LinearAccess Access{
        value(*Inputs.begin()),
        Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        Frame ? FrameCPL : uint8_t(0), !Frame, Frame};
    if (Write) {
      const auto Value = value(Inputs.begin()[1]);
      for (unsigned I = 0; I != Size; ++I)
        Bytes[I] = Value >> (I * 8);
    }
    auto R = Write
                 ? Memory.store(Access, llvm::ArrayRef(Bytes).take_front(Size))
                 : Memory.load(Access,
                               llvm::MutableArrayRef(Bytes).take_front(Size));
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
    } else if (!Write) {
      uint64_t Value = 0;
      for (unsigned I = 0; I != Size; ++I)
        Value |= uint64_t(Bytes[I]) << (I * 8);
      Values[Output.Offset] = Value;
    }
  }

  X64ContinuationStateV6 &State;
  X86PagedMemory &Memory;
  const X64ContinuationEventV1 &Event;
  llvm::SmallVector<uint64_t> Values;
  bool Frame = false;
  uint8_t FrameCPL = 0;
};

bool contributory(uint8_t Vector) {
  return Vector == 0 || (Vector >= 10 && Vector <= 13) || Vector == 21;
}

bool pageFault(uint8_t Vector) { return Vector == 14 || Vector == 20; }

} // namespace

llvm::Expected<X64PagedEventResult>
deliverX64PagedEvent(X64ContinuationStateV6 &State, X86PagedMemory &Memory,
                     const X64ContinuationEventV1 &Input, uint32_t MXCSRMask) {
  if (!(Memory.controls().EFER & 0x400))
    return llvm::createStringError(
        llvm::errc::not_supported,
        "paged V6 event delivery requires IA-32e mode");
  auto Controls = Memory.controls();
  if (Controls.CR4 & (uint64_t(1) << 32))
    return llvm::createStringError(llvm::errc::not_supported,
                                   "paged V6 IDT delivery requires CR4.FRED=0");
  Controls.RFlags = State.Core.Core.Core.Core.RFlags;
  if (auto Error = Memory.bindControls(Controls))
    return std::move(Error);
  X64PagedEventResult Result;
  auto Event = Input;
  for (;;) {
    EventWriter W(State, Memory, Event, MXCSRMask);
    detail::X64ContinuationDelivery<EventWriter>(W, Controls.CR4 & 0x1000 ? 57
                                                                          : 48)
        .execute(NdVar::scalar(0, 8));
    ++Result.DeliveryAttempts;
    if (W.Error)
      return std::move(*W.Error);
    if (!W.Fault) {
      Result.Status = W.Result.Status;
      Result.BackingFault = W.Result.BackingFault;
      if (Result.Status == X64PagedEventStatus::Delivered)
        Result.DeliveredVector = Event.Vector;
      Controls.RFlags = State.Core.Core.Core.Core.RFlags;
      if (auto Error = Memory.bindControls(Controls))
        return std::move(Error);
      return Result;
    }
    Result.LastException = W.Fault;
    const bool FirstFault = Event.Kind == X64ContinuationEventKind::Fault;
    const bool FirstDouble =
        Event.Kind == X64ContinuationEventKind::Abort && Event.Vector == 8;
    const bool SecondContributory = contributory(W.Fault->Vector);
    const bool SecondPage = pageFault(W.Fault->Vector);
    if (FirstDouble && (SecondContributory || SecondPage)) {
      Result.Status = X64PagedEventStatus::Shutdown;
      return Result;
    }
    const bool Double =
        FirstFault &&
        ((contributory(Event.Vector) && SecondContributory) ||
         (pageFault(Event.Vector) && (SecondContributory || SecondPage)));
    Event = {};
    Event.SavedPC = Input.SavedPC;
    Event.Vector = Double ? 8 : W.Fault->Vector;
    Event.Kind = Double ? X64ContinuationEventKind::Abort
                        : X64ContinuationEventKind::Fault;
    Event.ErrorCode = Double ? 0 : W.Fault->ErrorCode;
    Event.FaultAddress = Double ? 0 : W.Fault->CR2.value_or(0);
  }
}

} // namespace neverd::analysis
