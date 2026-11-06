//===- X64PagedExecution.cpp - Faultable generated steps ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original instruction restart state while retaining page-walk
/// effects, then deliver synchronous faults through the shared IDT owner.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X64PagedExecution.h"

#include "X64MachineFlags.h"
#include "X64PagedSse.h"
#include "X64SegmentMoveEncoding.h"

#include "neverd/analysis/arch/x86_64/X64PagedSource.h"
#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/ir/X86/X86PopFlagsRules.h"

#include "llvm/Support/Errc.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::analysis {
namespace {
using translate::MemoryAccessKind;
using translate::X86LinearAccess;
using translate::X86MemoryAccessResult;
using translate::X86MemoryException;
using translate::X86PagedMemory;
using translate::X86PageWalkStatus;

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

struct Transaction {
  X86PagedMemory &Memory;
  uint8_t CPL;
  X64PagedAlignmentPriority AlignmentPriority;
  std::optional<X64PagedRepeatProfile> RepeatProfile;
  X64ContinuationStateV6 &StagedState;
  std::optional<X64PagedSseAlignment> SseAlignment;
  unsigned SseAlignmentBytes = 0;
  bool Repeated = false, PortIO = false;
  uint8_t RepeatRegisters = 0, RepeatAddressBytes = 0;
  X86MemoryAccessResult Access;
  std::optional<llvm::Error> Error;
  std::optional<X64ContinuationEventV1> Event;
  std::optional<uint64_t> DebugPC;
  struct FrameReturn {
    uint16_t Bytes, Discard;
    bool Interrupt;
  };
  std::optional<FrameReturn> Return;
  struct SegmentLoad {
    std::optional<X64NullSegmentBasePolicy> NullBasePolicy;
  };
  std::optional<SegmentLoad> FarPointer, SegmentMove;
  std::optional<X86PagedMemory::PreparedStore> Store;
  std::vector<uint8_t> Bytes;

  void normalizeRepeatEntry(X64ContinuationStateV6 &State) {
    if (!Repeated || RepeatAddressBytes != 4)
      return;
    auto &GPR = State.Core.Core.Core.Core.GPR;
    if (RepeatRegisters & kX64PagedRepeatCompare) {
      GPR[1] = uint32_t(GPR[1]);
    } else if (RepeatRegisters & kX64PagedRepeatDestination) {
      GPR[1] = uint32_t(GPR[1]);
      GPR[7] = uint32_t(GPR[7]);
      if (RepeatRegisters & kX64PagedRepeatSource)
        GPR[6] = uint32_t(GPR[6]);
    } else if (*RepeatProfile == X64PagedRepeatProfile::ClearLodsCountHigh) {
      GPR[1] = uint32_t(GPR[1]);
    }
  }

  bool alignmentFault(uint64_t Address, unsigned Bytes) {
    if (Bytes == 16)
      Bytes = SseAlignmentBytes;
    const auto &Controls = Memory.controls();
    if (CPL == 3 && (Controls.CR0 & 0x40000) && (Controls.RFlags & 0x40000) &&
        (Address & (Bytes - 1))) {
      Access.Status = X86PageWalkStatus::Exception;
      Access.Exception = X86MemoryException{17, 0, std::nullopt};
      return true;
    }
    return false;
  }

  bool accept(llvm::Expected<X86MemoryAccessResult> Result) {
    if (!Result) {
      Error = Result.takeError();
      return false;
    }
    Access = std::move(*Result);
    return Access.Status == X86PageWalkStatus::Completed;
  }
};

} // namespace

extern "C" uint64_t neverd_x64_paged_access_v1(void *Opaque, uint64_t Address,
                                               uint64_t Size, uint64_t Flags) {
  auto &Exchange = *reinterpret_cast<X64PagedExchange *>(Opaque);
  auto &T = *static_cast<Transaction *>(Exchange.Transaction);
  const bool SseMemory = Flags & kX64PagedSseAccess;
  const auto Kind =
      X64PagedAccess(Flags & ~(kX64PagedStackAccess | kX64PagedSseAccess));
  if (Kind == X64PagedAccess::BeginSse || SseMemory) {
    const auto &Controls = T.Memory.controls();
    if (auto Vector =
            detail::x64PagedSseEntryFault(Controls.CR0, Controls.CR4)) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception = X86MemoryException{Vector, 0, std::nullopt};
      return 1;
    }
    T.SseAlignmentBytes = 1;
    if (SseMemory && T.CPL == 3 && (Controls.CR0 & 0x40000) &&
        (Controls.RFlags & 0x40000)) {
      T.SseAlignmentBytes =
          T.SseAlignment ? detail::x64PagedSseAlignmentBytes(*T.SseAlignment)
                         : 0;
      if (!T.SseAlignmentBytes) {
        T.Error = invalid("paged unaligned SSE requires a target #AC binding");
        return 1;
      }
    }
    if (!SseMemory)
      return 0;
  }
  if (Kind == X64PagedAccess::PortIO) {
    T.PortIO = true;
    return 1;
  }
  if (Kind == X64PagedAccess::BeginRepeat) {
    if (!T.RepeatProfile ||
        (*T.RepeatProfile != X64PagedRepeatProfile::ClearLodsCountHigh &&
         *T.RepeatProfile != X64PagedRepeatProfile::PreserveLodsCountHigh)) {
      T.Error = invalid("paged REP requires a bound element-progress profile");
      return 1;
    }
    if ((Size != 4 && Size != 8) ||
        !(Address & (kX64PagedRepeatSource | kX64PagedRepeatDestination)) ||
        (Address & ~(kX64PagedRepeatSource | kX64PagedRepeatDestination |
                     kX64PagedRepeatCompare))) {
      T.Error = invalid("invalid paged REP entry binding");
      return 1;
    }
    T.Repeated = true;
    T.RepeatRegisters = Address;
    T.RepeatAddressBytes = Size;
    T.normalizeRepeatEntry(T.StagedState);
    return 0;
  }
  if (Kind == X64PagedAccess::FarPointerLoad ||
      Kind == X64PagedAccess::SegmentMove) {
    auto &Transfer =
        Kind == X64PagedAccess::FarPointerLoad ? T.FarPointer : T.SegmentMove;
    Transfer.emplace();
    if (Address)
      Transfer->NullBasePolicy = Address == 1
                                     ? X64NullSegmentBasePolicy::Clear
                                     : X64NullSegmentBasePolicy::Preserve;
    return 1;
  }
  if (Kind == X64PagedAccess::InterruptReturn ||
      Kind == X64PagedAccess::FarReturn) {
    T.Return =
        Transaction::FrameReturn{uint16_t(Address), uint16_t(Size),
                                 Kind == X64PagedAccess::InterruptReturn};
    return 1;
  }
  if (Kind == X64PagedAccess::InterruptFlagMask) {
    const auto &Controls = T.Memory.controls();
    const auto IOPL = (Controls.RFlags >> 12) & 3;
    const bool Virtual = T.CPL > IOPL && T.CPL == 3 && (Controls.CR4 & 2);
    if ((T.CPL > IOPL && !Virtual) ||
        (Virtual && Address && (Controls.RFlags & 0x100000))) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception = X86MemoryException{13, 0, std::nullopt};
      return 1;
    }
    Exchange.Value = Virtual ? 0x80000 : 0x200;
    return 0;
  }
  if (Kind == X64PagedAccess::SoftwareInterrupt) {
    T.Event.emplace();
    T.Event->SavedPC = Size;
    T.Event->Vector = uint8_t(Address);
    T.Event->Kind = X64ContinuationEventKind::Software;
    return 1;
  }
  if (Kind == X64PagedAccess::DebugTrap) {
    T.DebugPC = Size;
    return 1;
  }
  if (Kind == X64PagedAccess::PopFlags) {
    const auto Flags = T.StagedState.Core.Core.Core.Core.RFlags;
    const auto Mask = x86PopFlagsMask(T.CPL, Flags, 8);
    Exchange.Value =
        ((Flags & ~(Mask | uint64_t{0x10000})) | (Address & Mask)) &
        ~detail::X64MachineFlags::SplitMask;
    return 0;
  }
  if (Kind == X64PagedAccess::Fault) {
    T.Access.Status = X86PageWalkStatus::Exception;
    T.Access.Exception = X86MemoryException{uint8_t(Address), 0, std::nullopt};
    return 1;
  }
  assert((Size == 1 || Size == 2 || Size == 4 || Size == 8 ||
          (Size == 16 && T.SseAlignmentBytes)) &&
         (Flags & ~kX64PagedSseAccess) <= (kX64PagedStackAccess | 2) &&
         !T.Store &&
         "source admission owns operand widths and the final store boundary");
  X86LinearAccess A{Address, MemoryAccessKind::Read, T.CPL, false,
                    bool(Flags & kX64PagedStackAccess)};
  if (T.AlignmentPriority == X64PagedAlignmentPriority::BeforePaging &&
      T.alignmentFault(Address, Size)) {
    auto Check = translate::checkX86LinearAccess(T.Memory.controls(), A, Size);
    if (!Check)
      T.Error = Check.takeError();
    else if (*Check)
      T.Access.Exception = *Check;
    return 1;
  }
  if (Kind != X64PagedAccess::Load) {
    A.Kind = MemoryAccessKind::Write;
    auto Prepared = T.Memory.prepareStore(A, Size);
    if (!Prepared) {
      T.Error = Prepared.takeError();
      return 1;
    }
    T.Access = *Prepared;
    if (T.Access.Status != X86PageWalkStatus::Completed)
      return 1;
    if (Kind == X64PagedAccess::Store) {
      if (T.alignmentFault(Address, Size))
        return 1;
      T.Store = std::move(*Prepared);
      T.Bytes.resize(Size);
      for (unsigned I = 0; I != Size; ++I)
        T.Bytes[I] =
            (I < 8 ? Exchange.Value : Exchange.ValueHigh) >> ((I % 8) * 8);
      return 0;
    }
  }
  std::array<uint8_t, 16> Bytes{};
  A.Kind = MemoryAccessKind::Read;
  if (!T.accept(
          T.Memory.load(A, llvm::MutableArrayRef(Bytes).take_front(Size))) ||
      T.alignmentFault(Address, Size))
    return 1;
  Exchange.Value = 0;
  Exchange.ValueHigh = 0;
  for (unsigned I = 0; I != Size; ++I)
    (I < 8 ? Exchange.Value : Exchange.ValueHigh) |= uint64_t(Bytes[I])
                                                     << ((I % 8) * 8);
  return 0;
}

llvm::Expected<bool> X64PagedExecution::validate(X64ContinuationStateV6 &State,
                                                 uint64_t Address,
                                                 X64PagedBody Validate,
                                                 unsigned PagingAddressBits,
                                                 bool InstructionDebug) {
  Last = {};
  Error.clear();
  if (!Validate || (PagingAddressBits != 48 && PagingAddressBits != 57))
    return invalid("invalid paged entry binding");
  auto Controls = Memory.controls();
  if (!(Controls.EFER & 0x400) || (Controls.CR4 & (uint64_t(1) << 32)) ||
      PagingAddressBits != (Controls.CR4 & 0x1000 ? 57u : 48u))
    return invalid("paged instruction binding disagrees with IA-32e controls");
  auto &Original = State.Core.Core.Core.Core;
  if ((InstructionDebug && !Options.Debug) ||
      (Options.Debug &&
       !x86InstructionDebugValid(Options.Debug->DR6, Options.Debug->DR7,
                                 Options.Debug->DebugCTL)))
    return invalid("paged instruction debug requires valid DR6, inactive DR7 "
                   "and zero DEBUGCTL");
  if ((Original.RFlags & 0x100) && !InstructionDebug)
    return invalid("paged TF requires the instruction-debug bridge");
  if (Original.PC != Address)
    return false;
  const auto CPL = uint8_t(State.Core.CS.Selector & 3);
  if (CPL == 3 && (Controls.CR0 & 0x40000) && (Original.RFlags & 0x40000) &&
      !Options.AlignmentPriority)
    return invalid(
        "active scalar alignment checks require bound fault priority");
  return Validate(&State, nullptr) == 0;
}

llvm::Error X64PagedExecution::deliverEvent(
    X64ContinuationStateV6 &State, const X64ContinuationEventV1 &Event,
    uint32_t MXCSRMask, const X64PagedEventContinuation *Continuation) {
  auto Delivered =
      Continuation
          ? resumeX64PagedEvent(State, Memory, *Continuation, MXCSRMask)
          : deliverX64PagedEvent(State, Memory, Event, MXCSRMask);
  if (!Delivered)
    return Delivered.takeError();
  Last.Delivery = *Delivered;
  Last.BackingFault = Delivered->BackingFault;
  switch (Delivered->Status) {
  case X64PagedEventStatus::Delivered:
    Last.Exit = uint64_t(X64ContinuationExitV1::Exception);
    break;
  case X64PagedEventStatus::Deferred:
    Last.Exit = uint64_t(X64ContinuationExitV1::EventDeferred);
    break;
  case X64PagedEventStatus::InvalidState:
    break;
  case X64PagedEventStatus::BackingFault:
    Last.Exit = uint64_t(X64PagedExit::BackingFault);
    break;
  case X64PagedEventStatus::Shutdown:
    Last.Exit = uint64_t(X64PagedExit::Shutdown);
    break;
  }
  return llvm::Error::success();
}

llvm::Error X64PagedExecution::deliverDebug(X64ContinuationStateV6 &State,
                                            bool SingleStep,
                                            uint32_t MXCSRMask) {
  Options.Debug->DR6 =
      x86InstructionDebugStatus(Options.Debug->DR6, SingleStep);
  X64ContinuationEventV1 Event;
  Event.SavedPC = State.Core.Core.Core.Core.PC;
  Event.Vector = 1;
  Event.Kind = X64ContinuationEventKind::Trap;
  if (auto Failure = deliverEvent(State, Event, MXCSRMask))
    return Failure;
  Last.Exception = Last.Delivery->LastException;
  return llvm::Error::success();
}

llvm::Expected<X64PagedExecutionResult>
X64PagedExecution::deliver(X64ContinuationStateV6 &State,
                           const X64ContinuationEventV1 &Event,
                           uint32_t MXCSRMask) {
  Last = {};
  Error.clear();
  if (auto Failure = deliverEvent(State, Event, MXCSRMask))
    return std::move(Failure);
  Last.Exception = Last.Delivery->LastException;
  return Last;
}

llvm::Expected<X64PagedExecutionResult>
X64PagedExecution::resume(X64ContinuationStateV6 &State,
                          X64PagedEventContinuation Continuation,
                          uint32_t MXCSRMask) {
  Last = {};
  Error.clear();
  if (auto Failure =
          deliverEvent(State, Continuation.Event, MXCSRMask, &Continuation))
    return std::move(Failure);
  Last.Exception = Last.Delivery->LastException;
  return Last;
}

llvm::Expected<X64PagedExecutionResult>
X64PagedExecution::execute(X64ContinuationStateV6 &State, uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes, X64PagedBody Body,
                           X64PagedBody Validate, uint32_t MXCSRMask,
                           unsigned PagingAddressBits,
                           std::optional<bool> DebugSSBlocked) {
  auto Valid = validate(State, Address, Validate, PagingAddressBits,
                        DebugSSBlocked.has_value());
  if (!Valid)
    return Valid.takeError();
  if (!Body || Bytes.empty() || Bytes.size() > 15)
    return invalid("invalid paged instruction binding");
  if (!*Valid)
    return Last;
  auto Controls = Memory.controls();
  const auto &Original = State.Core.Core.Core.Core;
  const bool SingleStep = Original.RFlags & 0x100;
  const auto CPL = uint8_t(State.Core.CS.Selector & 3);
  auto Next = State;
  Transaction T{Memory,
                CPL,
                Options.AlignmentPriority.value_or(
                    X64PagedAlignmentPriority::AfterPaging),
                Options.RepeatProfile,
                Next,
                Options.SseAlignment};
  // Generated 128-bit exchange accesses cover only the adjacent value words.
  static_assert(offsetof(X64PagedExchange, ValueHigh) == 8);
  static_assert(offsetof(X64PagedExchange, Transaction) == 16);
  X64PagedExchange Exchange{0, 0, &T};
  auto *NextBytes = reinterpret_cast<uint8_t *>(&Next);
  auto *ExchangeBytes = reinterpret_cast<uint8_t *>(&Exchange);
  Controls.RFlags = Original.RFlags;
  if (auto Failure = Memory.bindControls(Controls))
    return std::move(Failure);
  // Stop at the first changed byte before fetching a later old instruction
  // byte. The environment must relift changed code; it is never executed as
  // the stale source instruction or skipped as a NOP.
  // Protected-mode virtual interrupts raise #GP at instruction entry when
  // VIF and VIP are both set, independent of IOPL and of the opcode. External
  // event selection has already happened at the scheduler boundary.
  if (x86PendingVirtualInterrupt(CPL, Controls.CR4, Controls.RFlags)) {
    T.Access.Status = X86PageWalkStatus::Exception;
    T.Access.Exception = X86MemoryException{13, 0, std::nullopt};
  }
  for (unsigned I = 0;
       T.Access.Status == X86PageWalkStatus::Completed && I != Bytes.size();
       ++I) {
    uint8_t Byte;
    if (!T.accept(Memory.load({Address + I, MemoryAccessKind::Execute, T.CPL},
                              llvm::MutableArrayRef(&Byte, 1))))
      break;
    if (Byte != Bytes[I]) {
      Last.Exit = uint64_t(X64PagedExit::CodeChanged);
      return Last;
    }
  }
  uint64_t Exit = uint64_t(X64PagedExit::AccessFault);
  if (!T.Error && T.Access.Status == X86PageWalkStatus::Completed)
    Exit = Body(NextBytes, ExchangeBytes);
  if (T.Error)
    return std::move(*T.Error);
  if (T.DebugPC) {
    if (!DebugSSBlocked)
      return invalid("ICEBP requires the instruction-debug bridge");
    State.Core.Core.Core.Core.PC = *T.DebugPC;
    State.Core.Core.Core.Core.RFlags &= ~uint64_t{0x10000};
    Last.InstructionAttempted = Last.InstructionCompleted = true;
    if (auto Failure = deliverDebug(State, false, MXCSRMask))
      return std::move(Failure);
    return Last;
  }
  if (T.PortIO) {
    if (!Options.PortIO)
      return invalid(
          "paged port I/O requires a bound device and element profile");
    auto IO = executeX64PagedPortIO(Next, Memory, Bytes, T.AlignmentPriority,
                                    *Options.PortIO);
    if (!IO)
      return IO.takeError();
    Last.DeviceTransferCompleted = IO->DeviceTransferCompleted;
    Last.PortInputBytesWritten = IO->InputBytesWritten;
    T.Access = IO->Access;
    if (IO->DeviceUnavailable) {
      Last.Exit = uint64_t(X64PagedExit::DeviceUnavailable);
      return Last;
    }
    T.Repeated = IO->RepeatContinue;
    Exit = uint64_t(IO->RepeatContinue ? X64PagedExit::RepeatContinue
                                       : X64PagedExit(0));
  }
  if (T.Return) {
    if (T.Return->Interrupt ? !Options.IretAccessOrder
                            : !Options.FarReturnProfile)
      return invalid("paged return requires bound frame-access order");
    auto Returned =
        T.Return->Interrupt
            ? returnX64PagedInterrupt(State, Memory, T.Return->Bytes,
                                      T.AlignmentPriority,
                                      *Options.IretAccessOrder)
            : returnX64PagedFar(State, Memory, T.Return->Bytes,
                                T.Return->Discard, T.AlignmentPriority,
                                *Options.FarReturnProfile);
    if (!Returned)
      return Returned.takeError();
    Last.InstructionAttempted = Returned->InstructionAttempted;
    if (Returned->Completed) {
      Last.InstructionCompleted = true;
      Last.Exit = uint64_t(X64ContinuationExitV1::PrivilegeReturn);
      if (SingleStep)
        if (auto Failure = deliverDebug(State, true, MXCSRMask))
          return std::move(Failure);
      return Last;
    }
    if (Returned->BackingFault) {
      Last.Exit = uint64_t(X64PagedExit::BackingFault);
      Last.BackingFault = Returned->BackingFault;
      return Last;
    }
    if (!Returned->Exception)
      return invalid("paged return stopped without an architectural outcome");
    T.Access.Status = X86PageWalkStatus::Exception;
    T.Access.Exception = Returned->Exception;
  }
  if (T.FarPointer || T.SegmentMove) {
    if (T.FarPointer ? !Options.FarPointerProfile : !Options.SegmentMoveProfile)
      return invalid("paged segment transition requires bound access order");
    auto Loaded =
        T.FarPointer
            ? loadX64PagedFarPointer(Next, Memory, Bytes, T.AlignmentPriority,
                                     *Options.FarPointerProfile,
                                     T.FarPointer->NullBasePolicy)
            : moveX64PagedSegment(Next, Memory, Bytes, T.AlignmentPriority,
                                  *Options.SegmentMoveProfile,
                                  T.SegmentMove->NullBasePolicy);
    if (!Loaded)
      return Loaded.takeError();
    Last.InstructionAttempted = Loaded->InstructionAttempted;
    if (Loaded->BackingFault) {
      Last.Exit = uint64_t(X64PagedExit::BackingFault);
      Last.BackingFault = Loaded->BackingFault;
      return Last;
    }
    if (Loaded->Exception) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception = Loaded->Exception;
    } else if (Loaded->Completed) {
      Exit = uint64_t(X64ContinuationExitV1::Continue);
    } else {
      // An unbound null-base policy is an explicit refusal, with neither
      // destination published. It must not become a synthesized guest #GP.
      Last.Exit = uint64_t(X64ContinuationExitV1::InvalidState);
      return Last;
    }
  }
  if (T.Access.Status == X86PageWalkStatus::BackingFault) {
    Last.Exit = uint64_t(X64PagedExit::BackingFault);
    Last.BackingFault = T.Access.BackingFault;
    return Last;
  }
  if (T.Event || T.Access.Status == X86PageWalkStatus::Exception) {
    T.normalizeRepeatEntry(State);
    Last.Exception = T.Access.Exception;
    X64ContinuationEventV1 Event = T.Event.value_or(X64ContinuationEventV1{});
    if (!T.Event) {
      Event.SavedPC = Address;
      Event.Vector = T.Access.Exception->Vector;
      Event.Kind = X64ContinuationEventKind::Fault;
      Event.ErrorCode = T.Access.Exception->ErrorCode;
      Event.FaultAddress = T.Access.Exception->CR2.value_or(0);
    }
    Last.InstructionAttempted = true;
    if (auto Failure = deliverEvent(State, Event, MXCSRMask))
      return std::move(Failure);
    if (T.Event) {
      Last.Exception = Last.Delivery->LastException;
      Last.InstructionCompleted =
          Last.Delivery->Status == X64PagedEventStatus::Delivered &&
          !Last.Exception;
    }
    return Last;
  }
  const bool Partial =
      T.Repeated && Exit == uint64_t(X64PagedExit::RepeatContinue);
  if (Exit > uint64_t(X64ContinuationExitV1::Return) && !Partial) {
    if (Exit != uint64_t(X64ContinuationExitV1::NativeInstruction) &&
        Exit != uint64_t(X64ContinuationExitV1::InvalidState))
      return invalid("paged body returned an unowned execution status");
    Last.Exit = Exit;
    return Last;
  }
  if (T.Store)
    Memory.commitStore(*T.Store, T.Bytes);
  const auto EntryFlags = State.Core.Core.Core.Core.RFlags;
  State = Next;
  // The bound processor retains the pre-instruction flags during unfinished
  // CMPS/SCAS, including interrupt entry. Only a terminal comparison publishes
  // its arithmetic flags; fault delivery already uses the prior state.
  if (Partial && (T.RepeatRegisters & kX64PagedRepeatCompare))
    State.Core.Core.Core.Core.RFlags = EntryFlags;
  if (Partial)
    State.Core.Core.Core.Core.RFlags |=
        0x10000; // Restart skips the execute BP.
  else
    State.Core.Core.Core.Core.RFlags &= ~uint64_t(0x10000); // Completed RF.
  Controls.RFlags = State.Core.Core.Core.Core.RFlags;
  if (auto Failure = Memory.bindControls(Controls))
    return std::move(Failure);
  Last.InstructionAttempted = true;
  Last.InstructionCompleted = !Partial;
  Last.Exit = Exit;
  bool NewSSShadow = false;
  if (DebugSSBlocked && T.SegmentMove) {
    const auto Move = detail::x64SegmentMove(Bytes);
    NewSSShadow = Move && !Move->Read &&
                  Move->SegmentRegister == X86SegmentRegister::SS &&
                  !*DebugSSBlocked;
  }
  if (SingleStep && !NewSSShadow)
    if (auto Failure = deliverDebug(State, true, MXCSRMask))
      return std::move(Failure);
  return Last;
}

void X64PagedExecution::recordError(llvm::Error Failure) {
  Error = llvm::toString(std::move(Failure));
}

extern "C" uint64_t
neverd_x64_paged_step_v1(uint8_t *Context, uint8_t *State, uint64_t Address,
                         const uint8_t *Bytes, uint64_t Size, X64PagedBody Body,
                         X64PagedBody Validate, uint64_t MXCSRMask,
                         uint64_t PagingAddressBits) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  auto Result = Execution.execute(
      *reinterpret_cast<X64ContinuationStateV6 *>(State), Address,
      {Bytes, Size}, Body, Validate, MXCSRMask, PagingAddressBits);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return Result->Exit;
}

extern "C" uint64_t neverd_x64_paged_validate_v1(uint8_t *Context,
                                                 uint8_t *State,
                                                 uint64_t Address,
                                                 X64PagedBody Validate,
                                                 uint64_t PagingAddressBits) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  auto Result =
      Execution.validate(*reinterpret_cast<X64ContinuationStateV6 *>(State),
                         Address, Validate, PagingAddressBits);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return uint64_t(*Result ? X64ContinuationExitV1::Continue
                          : X64ContinuationExitV1::InvalidState);
}

extern "C" uint64_t neverd_x64_paged_event_v1(uint8_t *Context, uint8_t *State,
                                              const uint8_t *Event,
                                              uint64_t MXCSRMask) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  auto Result = Execution.deliver(
      *reinterpret_cast<X64ContinuationStateV6 *>(State),
      *reinterpret_cast<const X64ContinuationEventV1 *>(Event), MXCSRMask);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return Result->Exit;
}

extern "C" uint64_t neverd_x64_paged_progress_v1(uint8_t *Context) {
  const auto &Last =
      reinterpret_cast<X64PagedExecution *>(Context)->lastResult();
  return (Last.InstructionAttempted ? kX64PagedInstructionAttempted : 0) |
         (Last.Delivery && Last.Delivery->InputAccepted ? kX64PagedEventAccepted
                                                        : 0);
}

extern "C" uint64_t neverd_x64_paged_debug_step_v1(
    uint8_t *Context, uint8_t *State, uint64_t Address, const uint8_t *Bytes,
    uint64_t Size, X64PagedBody Body, X64PagedBody Validate, uint64_t MXCSRMask,
    uint64_t PagingAddressBits, uint64_t SSBlocked) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  if (SSBlocked > 1) {
    Execution.recordError(invalid("invalid instruction-debug SS shadow"));
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  auto Result =
      Execution.execute(*reinterpret_cast<X64ContinuationStateV6 *>(State),
                        Address, {Bytes, Size}, Body, Validate, MXCSRMask,
                        PagingAddressBits, bool(SSBlocked));
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return Result->Exit;
}

extern "C" uint64_t
neverd_x64_paged_debug_validate_v1(uint8_t *Context, uint8_t *State,
                                   uint64_t Address, X64PagedBody Validate,
                                   uint64_t PagingAddressBits) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  auto Result =
      Execution.validate(*reinterpret_cast<X64ContinuationStateV6 *>(State),
                         Address, Validate, PagingAddressBits, true);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return uint64_t(*Result ? X64ContinuationExitV1::Continue
                          : X64ContinuationExitV1::InvalidState);
}

} // namespace neverd::analysis
