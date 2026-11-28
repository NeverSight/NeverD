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
#include "X64TaskPriority.h"
#include "X86SgxExecution.h"

#include "neverd/analysis/arch/x86_64/X64PagedSource.h"
#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/ir/X86/X86PopFlagsRules.h"
#include "neverd/lift/X86/X86DebugRegisterEncoding.h"
#include "neverd/translate/X86/X86WatchpointObservation.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::analysis {
namespace detail {
struct X64PagedInstructionContinuation {
  enum class Kind : uint8_t { Return, FarPointer, SegmentMove };
  Kind Operation = Kind::Return;
  analysis::X64PagedReturnContinuation Return;
  analysis::X64PagedSegmentContinuation Segment;
  std::optional<X64NullSegmentBasePolicy> NullBasePolicy;
  llvm::SmallVector<uint8_t, x86::kMaxInstructionLen> Bytes;
  X64PagedBody Body = nullptr, Validate = nullptr;
  uint64_t Address = 0;
  uint32_t MXCSRMask = 0;
  unsigned PagingAddressBits = 0;
  std::optional<bool> DebugSSBlocked;
  bool BranchDebug = false, SingleStep = false, AttemptConsumed = false;
  std::optional<X64PagedDebugState> Debug;
  X86InstructionDebugProfile DebugProfile;
  std::optional<X64PagedAlignmentPriority> AlignmentPriority;
  std::optional<X64PagedIretAccessOrder> IretAccessOrder;
  std::optional<X64PagedFarReturnProfile> FarReturnProfile;
  std::optional<X64PagedFarPointerProfile> FarPointerProfile;
  std::optional<X64PagedSegmentMoveProfile> SegmentMoveProfile;
  X86DescriptorCanonicalityProfile EventDescriptorCanonicality;
  uint8_t DataBreakpoints = 0;
  uint16_t FieldBytes = 0, Discard = 0;
  bool Interrupt = false;
};
} // namespace detail
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
  const X86RepeatDebugBinding *RepeatDebug = nullptr;
  bool Watchpoints = false;
  unsigned SseAlignmentBytes = 0;
  bool Repeated = false, PortIO = false, DebugRegister = false;
  bool TakenBranch = false;
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
  bool SetsInterruptFlag = false;
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
  if (Kind == X64PagedAccess::BranchDecision) {
    T.TakenBranch = Address != 0;
    return 0;
  }
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
    if (T.Watchpoints && (!T.RepeatDebug || !T.RepeatDebug->EndGroup)) {
      T.Error = invalid("paged REP watchpoints require a debug-group binding");
      return 1;
    }
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
    T.SetsInterruptFlag = Address != 0;
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
  if (Kind == X64PagedAccess::DebugRegister) {
    T.DebugRegister = true;
    return 1;
  }
  if (Kind == X64PagedAccess::DebugTrap) {
    T.DebugPC = Size;
    return 1;
  }
  if (Kind == X64PagedAccess::PopFlags) {
    const auto Flags = T.StagedState.Core.Core.Core.Core.RFlags;
    const auto Mask = x86PopFlagsMask(T.CPL, Flags, 8);
    if (T.Memory.sgxMemory() && T.Memory.sgxMemory()->suppressesEnclaveDebug())
      Address &= ~uint64_t(0x100);
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

extern "C" uint64_t neverd_x64_paged_debug_access_v1(void *Exchange,
                                                     uint64_t Address,
                                                     uint64_t Size,
                                                     uint64_t Flags) {
  return neverd_x64_paged_access_v1(Exchange, Address, Size, Flags);
}

llvm::Expected<bool>
X64PagedExecution::validate(X64ContinuationStateV6 &State, uint64_t Address,
                            X64PagedBody Validate, unsigned PagingAddressBits,
                            bool InstructionDebug, bool BranchDebug) {
  Last = {};
  Error.clear();
  if (!Validate || (PagingAddressBits != 48 && PagingAddressBits != 57))
    return invalid("invalid paged entry binding");
  if (!x86DescriptorCanonicalityValid(Options.EventDescriptorCanonicality))
    return invalid("invalid event descriptor canonicality profile");
  if (!Options.EventDelivery.MaxAttempts)
    return invalid("event delivery requires a positive work budget");
  if (PendingInstruction) {
    const auto &P = *PendingInstruction;
    const auto Debug = Options.Debug
                           ? std::optional<X64PagedDebugState>(*Options.Debug)
                           : std::nullopt;
    if (Address != P.Address || Validate != P.Validate ||
        PagingAddressBits != P.PagingAddressBits ||
        InstructionDebug != P.DebugSSBlocked.has_value() ||
        BranchDebug != P.BranchDebug || Debug != P.Debug ||
        Options.DebugProfile != P.DebugProfile ||
        Options.AlignmentPriority != P.AlignmentPriority ||
        Options.IretAccessOrder != P.IretAccessOrder ||
        Options.FarReturnProfile != P.FarReturnProfile ||
        Options.FarPointerProfile != P.FarPointerProfile ||
        Options.SegmentMoveProfile != P.SegmentMoveProfile ||
        Options.EventDescriptorCanonicality != P.EventDescriptorCanonicality)
      return invalid(
          "paged instruction resume changed instruction or debug binding");
    auto Failure =
        P.Return ? validateX64PagedReturnResume(State, Memory, P.Return)
                 : validateX64PagedSegmentResume(State, Memory, P.Segment);
    if (Failure)
      return std::move(Failure);
    return true;
  }
  auto Controls = Memory.controls();
  if (!(Controls.EFER & 0x400) || (Controls.CR4 & (uint64_t(1) << 32)) ||
      PagingAddressBits != (Controls.CR4 & 0x1000 ? 57u : 48u))
    return invalid("paged instruction binding disagrees with IA-32e controls");
  auto &Original = State.Core.Core.Core.Core;
  if ((InstructionDebug && !Options.Debug) ||
      (Options.Debug &&
       !x86InstructionDebugValid(Options.Debug->DR6, Options.Debug->DR7,
                                 Options.Debug->DebugCTL, Options.DebugProfile,
                                 Controls.CR4, Options.Debug->PendingData)))
    return invalid("paged instruction debug requires a valid profile, DR6, "
                   "supported DR7/DEBUGCTL");
  if (Options.Debug && (Options.Debug->DebugCTL & kX86BranchTraceFlag) &&
      (!InstructionDebug || !BranchDebug))
    return invalid("BTF requires the v2 branch-aware instruction-debug bridge");
  if (!InstructionDebug && ((Original.RFlags & 0x100) ||
                            (Options.Debug && ((Options.Debug->DR7 & 0x20ff) ||
                                               Options.Debug->PendingData))))
    return invalid(
        "paged TF and breakpoints require the instruction-debug bridge");
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
  Last.RequestedEvent = Event;
  auto Delivered =
      Continuation
          ? resumeX64PagedEvent(State, Memory, *Continuation, MXCSRMask,
                                Options.Debug, Options.DebugProfile,
                                Options.EventDelivery)
          : deliverX64PagedEvent(State, Memory, Event, MXCSRMask, Options.Debug,
                                 Options.DebugProfile, Options.EventDelivery,
                                 Options.EventDescriptorCanonicality);
  if (!Delivered)
    return Delivered.takeError();
  Last.Delivery = *Delivered;
  if (Delivered->Status != X64PagedEventStatus::InvalidState &&
      !Delivered->Continuation)
    Last.RequestedEvent.reset();
  Last.BackingFault = Delivered->BackingFault;
  switch (Delivered->Status) {
  case X64PagedEventStatus::Delivered:
    Last.Exit = uint64_t(X64ContinuationExitV1::Exception);
    break;
  case X64PagedEventStatus::Deferred:
    Last.Exit = uint64_t(X64ContinuationExitV1::EventDeferred);
    break;
  case X64PagedEventStatus::InvalidState:
    // A post-instruction trap can refuse entry after the original instruction
    // set Exit to Continue, Call or Return. Keep its completion/transfer
    // receipt, and expose the event owner's stop to the driver.
    Last.Exit = uint64_t(X64ContinuationExitV1::InvalidState);
    break;
  case X64PagedEventStatus::BackingFault:
    Last.Exit = uint64_t(X64PagedExit::BackingFault);
    break;
  case X64PagedEventStatus::Shutdown:
    Last.Exit = uint64_t(X64PagedExit::Shutdown);
    break;
  case X64PagedEventStatus::BudgetExhausted:
    Last.Exit = uint64_t(X64PagedExit::EventBudgetExhausted);
    break;
  }
  return llvm::Error::success();
}

llvm::Error X64PagedExecution::deliverDebug(X64ContinuationStateV6 &State,
                                            bool SingleStep, uint32_t MXCSRMask,
                                            uint8_t Breakpoints,
                                            bool InstructionFault, bool Icebp) {
  if (!InstructionFault && !Icebp && (SingleStep || Breakpoints)) {
    const auto &Core = State.Core.Core.Core.Core;
    const auto &CS = State.Core.CS;
    // A completed return may select compatibility code, where PC is an
    // offset and the original linear address includes the loaded CS base.
    const uint64_t LinearPC =
        CS.Attributes & 0x2000 ? Core.PC : uint32_t(CS.Base + Core.PC);
    if (!Memory.sgxMemory() ||
        Memory.sgxMemory()->instructionBreakpointsAt(LinearPC))
      Breakpoints |= x86ExecutionBreakpoints(
          Options.Debug->DR, Options.Debug->DR7, LinearPC, Core.RFlags, false);
  }
  x86PublishInstructionDebug(*Options.Debug, SingleStep, Breakpoints);
  X64ContinuationEventV1 Event;
  Event.SavedPC = State.Core.Core.Core.Core.PC;
  Event.Vector = 1;
  Event.Kind = InstructionFault
                   ? X64ContinuationEventKind::InstructionBreakpoint
                   : X64ContinuationEventKind::Trap;
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
  if (PendingInstruction)
    return invalid("resume the pending instruction before delivering an event");
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
  if (PendingInstruction)
    return invalid("resume the pending instruction before resuming an event");
  if (Options.EventDescriptorCanonicality !=
      Continuation.DescriptorCanonicality)
    return invalid("paged event resume changed descriptor binding");
  if (auto Failure =
          deliverEvent(State, Continuation.Event, MXCSRMask, &Continuation))
    return std::move(Failure);
  Last.Exception = Last.Delivery->LastException;
  return Last;
}

llvm::Expected<X64PagedExecutionResult> X64PagedExecution::execute(
    X64ContinuationStateV6 &State, uint64_t Address,
    llvm::ArrayRef<uint8_t> Bytes, X64PagedBody Body, X64PagedBody Validate,
    uint32_t MXCSRMask, unsigned PagingAddressBits,
    std::optional<bool> DebugSSBlocked, bool BranchDebug) {
  auto Valid = validate(State, Address, Validate, PagingAddressBits,
                        DebugSSBlocked.has_value(), BranchDebug);
  if (!Valid)
    return Valid.takeError();
  if (!Body || Bytes.empty() || Bytes.size() > x86::kMaxInstructionLen)
    return invalid("invalid paged instruction binding");
  if (!*Valid)
    return Last;
  if (PendingInstruction) {
    // Keep the immutable binding alive while executeTransition retires/replaces
    // it.
    const auto P = PendingInstruction;
    if (Body != P->Body || Bytes != llvm::ArrayRef(P->Bytes) ||
        MXCSRMask != P->MXCSRMask || DebugSSBlocked != P->DebugSSBlocked)
      return invalid(
          "paged instruction resume changed body, bytes, mask or shadow");
    return executeTransition(State, *P);
  }
  const auto FetchFault =
      x86PagedCode64FaultEncoding(Bytes.data(), Bytes.size());
  const bool UD0 =
      FetchFault && FetchFault->Operation == X86PagedCode64FaultEncoding::UD0;
  const bool PrefixLengthFault = FetchFault && !UD0;
  if (UD0 && (!Options.UD0Fetch ||
              (*Options.UD0Fetch != X86UD0FetchProfile::OpcodeOnly &&
               *Options.UD0Fetch != X86UD0FetchProfile::CompleteModRM)))
    return invalid("paged UD0 requires a bound instruction-fetch profile");
  if (PrefixLengthFault && !Options.PrefixLengthFaultAfter15Bytes)
    return invalid("overlong prefix stream requires a bound instruction-length "
                   "fault priority");
  const unsigned FetchBytes = FetchFault ? FetchFault->Size : Bytes.size();
  auto Controls = Memory.controls();
  const auto &Original = State.Core.Core.Core.Core;
  const bool SingleStep = Original.RFlags & 0x100;
  const uint64_t DebugCTL = Options.Debug ? Options.Debug->DebugCTL : 0;
  const auto CPL = uint8_t(State.Core.CS.Selector & 3);
  auto Next = State;
  Transaction T{Memory,
                CPL,
                Options.AlignmentPriority.value_or(
                    X64PagedAlignmentPriority::AfterPaging),
                Options.RepeatProfile,
                Next,
                Options.SseAlignment,
                Options.RepeatDebug ? &*Options.RepeatDebug : nullptr,
                x86DebugWatchpoints(Options.DebugProfile)};
  // Generated 128-bit exchange accesses cover only the adjacent value words.
  static_assert(offsetof(X64PagedExchange, ValueHigh) == 8);
  static_assert(offsetof(X64PagedExchange, Transaction) == 16);
  X64PagedExchange Exchange{0, 0, &T};
  auto *NextBytes = reinterpret_cast<uint8_t *>(&Next);
  auto *ExchangeBytes = reinterpret_cast<uint8_t *>(&Exchange);
  Controls.RFlags = Original.RFlags;
  // PVI's pending interrupt condition precedes instruction breakpoints and
  // fetch. External event selection already happened at the scheduler boundary.
  const bool PendingVirtualInterrupt =
      x86PendingVirtualInterrupt(CPL, Controls.CR4, Controls.RFlags);
  uint8_t Matches = 0;
  if (!PendingVirtualInterrupt && DebugSSBlocked &&
      (!Memory.sgxMemory() ||
       Memory.sgxMemory()->instructionBreakpointsAt(Address)))
    Matches =
        x86ExecutionBreakpoints(Options.Debug->DR, Options.Debug->DR7, Address,
                                Original.RFlags, *DebugSSBlocked);
  if (auto Failure = Memory.bindControls(Controls))
    return std::move(Failure);
  if (PendingVirtualInterrupt) {
    T.Access.Status = X86PageWalkStatus::Exception;
    T.Access.Exception = X86MemoryException{13, 0, std::nullopt};
  } else if (Matches) {
    Last.InstructionAttempted = true;
    if (auto Failure = deliverDebug(State, false, MXCSRMask, Matches, true))
      return std::move(Failure);
    return Last;
  }
  // Stop at the first changed byte before fetching a later old instruction
  // byte. The environment must relift changed code; it is never executed as
  // the stale source instruction or skipped as a NOP.
  for (unsigned I = 0;
       T.Access.Status == X86PageWalkStatus::Completed && I != FetchBytes;
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
  if (UD0 && *Options.UD0Fetch == X86UD0FetchProfile::CompleteModRM &&
      T.Access.Status == X86PageWalkStatus::Completed) {
    // The x64 decoder records the UD0 opcode and qualified prefixes without
    // its encoded address. A processor that fetches that address determines the
    // remaining extent from live instruction bytes, even when the image ends
    // after the opcode or later bytes have changed. No data read is implied.
    const auto Fetch = [&](unsigned Offset, uint8_t &Byte) {
      // A complete-fetch processor checks the architectural 15-byte ceiling
      // before fetching the next encoded address byte. This can beat a #PF on
      // the next page, while opcode-only processors already raised #UD.
      if (x86UD0FetchExceedsInstructionLength(Offset)) {
        T.Access.Status = X86PageWalkStatus::Exception;
        T.Access.Exception = X86MemoryException{13, 0, std::nullopt};
        return false;
      }
      return T.accept(
          Memory.load({Address + Offset, MemoryAccessKind::Execute, T.CPL},
                      llvm::MutableArrayRef(&Byte, 1)));
    };
    uint8_t ModRM = 0, SIB = 0, Byte = 0;
    if (Fetch(FetchBytes, ModRM)) {
      const bool HasSIB = x86UD0NeedsSIB32Or64(ModRM);
      if (!HasSIB || Fetch(FetchBytes + 1, SIB)) {
        const unsigned AddressBytes = x86UD0AddressBytes32Or64(ModRM, SIB);
        for (unsigned I = 1 + HasSIB;
             I != AddressBytes &&
             T.Access.Status == X86PageWalkStatus::Completed;
             ++I)
          Fetch(FetchBytes + I, Byte);
      }
    }
  }
  if (!T.Error && T.Access.Status == X86PageWalkStatus::Completed &&
      Memory.sgxMemory() && Memory.sgxMemory()->active()) {
    // UD0's live address tail belongs to the fetch profile, including when a
    // full-mode binding supplied those optional bytes alongside the opcode.
    auto Denied = detail::X86SgxExecution::instructionException(
        Bytes.take_front(FetchBytes), Address, Memory);
    if (!Denied)
      return Denied.takeError();
    if (*Denied) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception = X86MemoryException{**Denied, 0, {}};
    }
  }
  uint64_t Exit = uint64_t(X64PagedExit::AccessFault);
  translate::X86WatchpointObservation Observation(Memory, Options.Debug,
                                                  Options.DebugProfile);
  const auto FinishDebug =
      [&](bool Step, bool NewSSShadow = false, bool Icebp = false,
          bool RepeatGroupContinues = false) -> llvm::Error {
    Observation.stop();
    const uint8_t Data =
        Options.Debug
            ? x86CompleteDataBreakpoints(*Options.Debug, Observation.Matches,
                                         NewSSShadow, RepeatGroupContinues)
            : 0;
    if ((Step && !NewSSShadow) || Data || Icebp)
      return deliverDebug(State, Step && !NewSSShadow, MXCSRMask, Data, false,
                          Icebp);
    return llvm::Error::success();
  };
  const auto CR8 = detail::decodeX64TaskPriority(Bytes);
  const bool WritesCR8 =
      CR8 && CR8->Operation == detail::X64TaskPriorityOperation::Write;
  if (!T.Error && T.Access.Status == X86PageWalkStatus::Completed) {
    if (CR8 &&
        (CR8->Locked || CPL || (WritesCR8 && Original.GPR[CR8->GPR] > 15))) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception =
          X86MemoryException{uint8_t(CR8->Locked ? 6 : 13), 0, {}};
    } else {
      // A valid transfer uses the original instruction's generated source.
      Exit = Body(NextBytes, ExchangeBytes);
    }
  }
  if (T.Error)
    return std::move(*T.Error);
  if (T.DebugPC) {
    if (!DebugSSBlocked)
      return invalid("ICEBP requires the instruction-debug bridge");
    State.Core.Core.Core.Core.PC = *T.DebugPC;
    State.Core.Core.Core.Core.RFlags &= ~uint64_t{0x10000};
    Last.InstructionAttempted = Last.InstructionCompleted = true;
    if (auto Failure = FinishDebug(false, false, true))
      return std::move(Failure);
    return Last;
  }
  if (T.DebugRegister) {
    if (!DebugSSBlocked || !x86DebugRegisters(Options.DebugProfile))
      return invalid("MOV-DR requires the debug-register profile and bridge");
    const auto Move = x86DebugRegisterEncoding(Bytes, true);
    if (!Move || Move->Size != Bytes.size())
      return invalid("invalid paged MOV-DR byte binding");
    auto &Core = Next.Core.Core.Core.Core;
    auto Bank = *Options.Debug;
    const auto Result = x86AccessDebugRegister(
        Bank, Core.GPR[Move->Access.General], Move->Access, Controls.CR4, CPL,
        Options.DebugProfile);
    using Status = X86DebugRegisterResult;
    if (Result == Status::UnsupportedControl)
      return invalid("MOV-DR requests unbound debug controls");
    if (Result == Status::GeneralDetect) {
      if (Options.GeneralDetectFrame != X86GeneralDetectFrame::FaultResume &&
          Options.GeneralDetectFrame != X86GeneralDetectFrame::ClearResume)
        return invalid("general detect requires a saved-RF binding");
      x86PublishInstructionDebug(*Options.Debug, false);
      Options.Debug->DR6 |= 0x2000;
      T.Event.emplace();
      T.Event->Vector = 1;
      T.Event->SavedPC = Address;
      T.Event->Kind =
          Options.GeneralDetectFrame == X86GeneralDetectFrame::FaultResume
              ? X64ContinuationEventKind::Fault
              : X64ContinuationEventKind::DebugRegisterFault;
    } else if (Result != Status::Completed) {
      T.Access.Status = X86PageWalkStatus::Exception;
      T.Access.Exception = X86MemoryException{
          uint8_t(Result == Status::UndefinedOpcode ? 6 : 13), 0, std::nullopt};
    } else {
      *Options.Debug = Bank;
      Core.PC = Address + Bytes.size();
      Exit = uint64_t(X64ContinuationExitV1::Continue);
    }
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
    if (IO->DeviceTransferCompleted)
      Observation.port(IO->Port, IO->TransferBytes);
    T.Access = IO->Access;
    if (IO->DeviceUnavailable) {
      Last.Exit = uint64_t(X64PagedExit::DeviceUnavailable);
      return Last;
    }
    T.Repeated = IO->RepeatContinue;
    Exit = uint64_t(IO->RepeatContinue ? X64PagedExit::RepeatContinue
                                       : X64PagedExit(0));
  }
  if (T.Return || T.FarPointer || T.SegmentMove) {
    if (T.Return && (T.Return->Interrupt ? !Options.IretAccessOrder
                                         : !Options.FarReturnProfile))
      return invalid("paged return requires bound frame-access order");
    if ((T.FarPointer && !Options.FarPointerProfile) ||
        (T.SegmentMove && !Options.SegmentMoveProfile))
      return invalid("paged segment transition requires bound access order");
    detail::X64PagedInstructionContinuation Binding;
    using Kind = detail::X64PagedInstructionContinuation::Kind;
    Binding.Operation = T.Return       ? Kind::Return
                        : T.FarPointer ? Kind::FarPointer
                                       : Kind::SegmentMove;
    Binding.Bytes.assign(Bytes.begin(), Bytes.end());
    Binding.Body = Body;
    Binding.Validate = Validate;
    Binding.Address = Address;
    Binding.MXCSRMask = MXCSRMask;
    Binding.PagingAddressBits = PagingAddressBits;
    Binding.DebugSSBlocked = DebugSSBlocked;
    Binding.BranchDebug = BranchDebug;
    Binding.SingleStep = SingleStep;
    if (Options.Debug)
      Binding.Debug = *Options.Debug;
    Binding.DebugProfile = Options.DebugProfile;
    Binding.AlignmentPriority = Options.AlignmentPriority;
    Binding.IretAccessOrder = Options.IretAccessOrder;
    Binding.FarReturnProfile = Options.FarReturnProfile;
    Binding.FarPointerProfile = Options.FarPointerProfile;
    Binding.SegmentMoveProfile = Options.SegmentMoveProfile;
    Binding.EventDescriptorCanonicality = Options.EventDescriptorCanonicality;
    Binding.DataBreakpoints = Observation.Matches;
    if (T.Return) {
      Binding.FieldBytes = T.Return->Bytes;
      Binding.Discard = T.Return->Discard;
      Binding.Interrupt = T.Return->Interrupt;
    } else {
      Binding.NullBasePolicy = T.FarPointer ? T.FarPointer->NullBasePolicy
                                            : T.SegmentMove->NullBasePolicy;
    }
    Observation.stop();
    return executeTransition(State, Binding);
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
    Observation.stop();
    if (auto Failure = deliverEvent(State, Event, MXCSRMask))
      return std::move(Failure);
    if (T.Event) {
      Last.Exception = Last.Delivery->LastException;
      Last.InstructionCompleted =
          !T.DebugRegister && Last.Delivery->SoftwareCompleted;
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
  bool RepeatGroupContinues = false;
  if (Partial && !T.PortIO && T.Watchpoints) {
    const auto &Before = State.Core.Core.Core.Core.GPR;
    const auto &After = Next.Core.Core.Core.Core.GPR;
    const uint64_t Mask = T.RepeatAddressBytes == 4 ? UINT32_MAX : UINT64_MAX;
    const bool Source = T.RepeatRegisters & kX64PagedRepeatSource;
    const bool Destination = T.RepeatRegisters & kX64PagedRepeatDestination;
    const unsigned Index = Destination ? 7 : 6;
    const auto Bytes =
        uint8_t(EntryFlags & 0x400 ? Before[Index] - After[Index]
                                   : After[Index] - Before[Index]);
    RepeatGroupContinues = x86RepeatDebugDefers(
        *T.RepeatDebug,
        {Address, EntryFlags, After[1] & Mask, After[6] & Mask, After[7] & Mask,
         T.RepeatAddressBytes, Bytes, Source, Destination,
         bool(T.RepeatRegisters & kX64PagedRepeatCompare), DebugCTL});
  }
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
  Last.CR8Written = Last.InstructionCompleted && WritesCR8;
  Last.StartsSTIShadow = !Partial && T.SetsInterruptFlag &&
                         !(EntryFlags & 0x200) &&
                         (State.Core.Core.Core.Core.RFlags & 0x200);
  Last.Exit = Exit;
  if (!Partial && (Exit == uint64_t(X64ContinuationExitV1::Call) ||
                   Exit == uint64_t(X64ContinuationExitV1::Return))) {
    const auto &Core = State.Core.Core.Core.Core;
    Last.Transfer =
        X64PagedTransfer{Exit, Address, Core.PC, Core.GPR[4], Core.GPR[0]};
  }
  const bool Taken = T.TakenBranch ||
                     Exit == uint64_t(X64ContinuationExitV1::Call) ||
                     Exit == uint64_t(X64ContinuationExitV1::Return);
  if (auto Failure =
          FinishDebug(x86InstructionDebugStep(SingleStep, DebugCTL, Taken),
                      false, false, RepeatGroupContinues))
    return std::move(Failure);
  return Last;
}

uint64_t X64PagedExecution::pendingProgress() const {
  return PendingInstruction ? kX64PagedInstructionPending |
                                  (PendingInstruction->AttemptConsumed
                                       ? kX64PagedAttemptConsumed
                                       : 0)
                            : 0;
}

llvm::Expected<X64PagedExecutionResult> X64PagedExecution::executeTransition(
    X64ContinuationStateV6 &State,
    const detail::X64PagedInstructionContinuation &Binding) {
  translate::X86WatchpointObservation Observation(
      Memory, Options.Debug, Options.DebugProfile, Binding.DataBreakpoints);
  // Admission has succeeded. A terminal owner failure or architectural outcome
  // retires this instruction before any subsequent event can begin.
  PendingInstruction.reset();
  const auto Alignment = Binding.AlignmentPriority.value_or(
      X64PagedAlignmentPriority::AfterPaging);
  using Kind = detail::X64PagedInstructionContinuation::Kind;
  const bool IsReturn = Binding.Operation == Kind::Return;
  X64PagedInstructionTransitionResult Result;
  X64PagedReturnContinuation Return;
  X64PagedSegmentContinuation Segment;
  if (IsReturn) {
    auto Got =
        Binding.Return ? resumeX64PagedReturn(State, Memory, Binding.Return)
        : Binding.Interrupt
            ? returnX64PagedInterrupt(State, Memory, Binding.FieldBytes,
                                      Alignment, *Binding.IretAccessOrder)
            : returnX64PagedFar(State, Memory, Binding.FieldBytes,
                                Binding.Discard, Alignment,
                                *Binding.FarReturnProfile);
    if (!Got)
      return Got.takeError();
    Result = *Got;
    Return = std::move(Got->Continuation);
  } else {
    auto Got =
        Binding.Segment ? resumeX64PagedSegment(State, Memory, Binding.Segment)
        : Binding.Operation == Kind::FarPointer
            ? loadX64PagedFarPointer(State, Memory, Binding.Bytes, Alignment,
                                     *Binding.FarPointerProfile,
                                     Binding.NullBasePolicy)
            : moveX64PagedSegment(State, Memory, Binding.Bytes, Alignment,
                                  *Binding.SegmentMoveProfile,
                                  Binding.NullBasePolicy);
    if (!Got)
      return Got.takeError();
    Result = *Got;
    Segment = std::move(Got->Continuation);
  }
  Last.InstructionResumed = bool(Binding.Return) || bool(Binding.Segment);
  Last.InstructionAttempted = Result.InstructionAttempted;
  if (Return || Segment) {
    auto Pending =
        std::make_shared<detail::X64PagedInstructionContinuation>(Binding);
    Pending->Return = std::move(Return);
    Pending->Segment = std::move(Segment);
    Pending->DataBreakpoints = Observation.Matches;
    Pending->AttemptConsumed |= Last.InstructionAttempted;
    PendingInstruction = std::move(Pending);
    Last.Exit = uint64_t(X64PagedExit::BackingFault);
    Last.BackingFault = Result.BackingFault;
    return Last;
  }
  Observation.stop();
  if (Result.Completed) {
    if (!IsReturn) {
      State.Core.Core.Core.Core.RFlags &= ~uint64_t(0x10000);
      auto Controls = Memory.controls();
      Controls.RFlags = State.Core.Core.Core.Core.RFlags;
      if (auto Failure = Memory.bindControls(Controls))
        return std::move(Failure);
      if (Binding.Operation == Kind::SegmentMove && Binding.DebugSSBlocked) {
        const auto Move = detail::x64SegmentMove(Binding.Bytes);
        Last.StartsSSShadow = !Move->Read &&
                              Move->SegmentRegister == X86SegmentRegister::SS &&
                              !*Binding.DebugSSBlocked;
      }
    }
    Last.InstructionCompleted = true;
    Last.Exit = uint64_t(IsReturn ? X64ContinuationExitV1::PrivilegeReturn
                                  : X64ContinuationExitV1::Continue);
    const uint8_t Data =
        Options.Debug
            ? x86CompleteDataBreakpoints(*Options.Debug, Observation.Matches,
                                         Last.StartsSSShadow)
            : 0;
    const bool Step =
        !Last.StartsSSShadow &&
        x86InstructionDebugStep(Binding.SingleStep,
                                Binding.Debug ? Binding.Debug->DebugCTL : 0,
                                IsReturn);
    if (Step || Data)
      if (auto Failure = deliverDebug(State, Step, Binding.MXCSRMask, Data))
        return std::move(Failure);
    return Last;
  }
  if (!Result.Exception) {
    if (IsReturn)
      return invalid("paged return stopped without an architectural outcome");
    // An unbound null-base policy refuses the original load. It is not a #GP
    // or an instruction boundary that consumes the incoming scheduler shadow.
    Last.InstructionResumed = false;
    return Last;
  }
  Last.Exception = Result.Exception;
  X64ContinuationEventV1 Event;
  Event.SavedPC = Binding.Address;
  Event.Vector = Result.Exception->Vector;
  Event.Kind = X64ContinuationEventKind::Fault;
  Event.ErrorCode = Result.Exception->ErrorCode;
  Event.FaultAddress = Result.Exception->CR2.value_or(0);
  if (auto Failure = deliverEvent(State, Event, Binding.MXCSRMask))
    return std::move(Failure);
  return Last;
}

void X64PagedExecution::recordError(llvm::Error Failure) {
  Error = llvm::toString(std::move(Failure));
}

void X64PagedExecution::recordEntryError(llvm::Error Failure) {
  Last = {};
  recordError(std::move(Failure));
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
                                                        : 0) |
         (Last.InstructionCompleted ? kX64PagedInstructionCompleted : 0) |
         (Last.InstructionResumed ? kX64PagedInstructionResumed : 0);
}

extern "C" uint64_t neverd_x64_paged_pending_v1(uint8_t *Context) {
  return reinterpret_cast<X64PagedExecution *>(Context)->pendingProgress();
}

extern "C" uint64_t neverd_x64_paged_transfer_v1(uint8_t *Context,
                                                 uint64_t *Instruction,
                                                 uint64_t *PC, uint64_t *SP,
                                                 uint64_t *RAX) {
  const auto &Last =
      reinterpret_cast<X64PagedExecution *>(Context)->lastResult();
  if (!Last.Transfer)
    return 0;
  const auto Transfer = *Last.Transfer;
  if (Instruction)
    *Instruction = Transfer.Instruction;
  if (PC)
    *PC = Transfer.PC;
  if (SP)
    *SP = Transfer.SP;
  if (RAX)
    *RAX = Transfer.RAX;
  return Transfer.Exit;
}

static uint64_t debugStep(uint8_t *Context, uint8_t *State, uint64_t Address,
                          const uint8_t *Bytes, uint64_t Size,
                          X64PagedBody Body, X64PagedBody Validate,
                          uint64_t MXCSRMask, uint64_t PagingAddressBits,
                          uint64_t SSBlocked, bool BranchDebug) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  if (SSBlocked > 1) {
    Execution.recordEntryError(invalid("invalid instruction-debug SS shadow"));
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  auto Result =
      Execution.execute(*reinterpret_cast<X64ContinuationStateV6 *>(State),
                        Address, {Bytes, Size}, Body, Validate, MXCSRMask,
                        PagingAddressBits, bool(SSBlocked), BranchDebug);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return Result->Exit;
}

static uint64_t debugValidate(uint8_t *Context, uint8_t *State,
                              uint64_t Address, X64PagedBody Validate,
                              uint64_t PagingAddressBits, bool BranchDebug) {
  auto &Execution = *reinterpret_cast<X64PagedExecution *>(Context);
  auto Result = Execution.validate(
      *reinterpret_cast<X64ContinuationStateV6 *>(State), Address, Validate,
      PagingAddressBits, true, BranchDebug);
  if (!Result) {
    Execution.recordError(Result.takeError());
    return uint64_t(X64ContinuationExitV1::InvalidState);
  }
  return uint64_t(*Result ? X64ContinuationExitV1::Continue
                          : X64ContinuationExitV1::InvalidState);
}

extern "C" uint64_t neverd_x64_paged_debug_step_v1(
    uint8_t *Context, uint8_t *State, uint64_t Address, const uint8_t *Bytes,
    uint64_t Size, X64PagedBody Body, X64PagedBody Validate, uint64_t MXCSRMask,
    uint64_t PagingAddressBits, uint64_t SSBlocked) {
  return debugStep(Context, State, Address, Bytes, Size, Body, Validate,
                   MXCSRMask, PagingAddressBits, SSBlocked, false);
}

extern "C" uint64_t neverd_x64_paged_debug_step_v2(
    uint8_t *Context, uint8_t *State, uint64_t Address, const uint8_t *Bytes,
    uint64_t Size, X64PagedBody Body, X64PagedBody Validate, uint64_t MXCSRMask,
    uint64_t PagingAddressBits, uint64_t SSBlocked) {
  return debugStep(Context, State, Address, Bytes, Size, Body, Validate,
                   MXCSRMask, PagingAddressBits, SSBlocked, true);
}

extern "C" uint64_t
neverd_x64_paged_debug_validate_v1(uint8_t *Context, uint8_t *State,
                                   uint64_t Address, X64PagedBody Validate,
                                   uint64_t PagingAddressBits) {
  return debugValidate(Context, State, Address, Validate, PagingAddressBits,
                       false);
}

extern "C" uint64_t
neverd_x64_paged_debug_validate_v2(uint8_t *Context, uint8_t *State,
                                   uint64_t Address, X64PagedBody Validate,
                                   uint64_t PagingAddressBits) {
  return debugValidate(Context, State, Address, Validate, PagingAddressBits,
                       true);
}

} // namespace neverd::analysis
