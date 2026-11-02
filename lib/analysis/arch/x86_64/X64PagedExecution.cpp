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

#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/Support/Errc.h"

#include <array>
#include <cassert>
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
  X86MemoryAccessResult Access;
  std::optional<llvm::Error> Error;
  std::optional<X64ContinuationEventV1> Event;
  std::optional<X86PagedMemory::PreparedStore> Store;
  std::vector<uint8_t> Bytes;

  bool alignmentFault(uint64_t Address, unsigned Bytes) {
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
  const auto Kind = X64PagedAccess(Flags & 7);
  if (Kind == X64PagedAccess::SoftwareInterrupt) {
    T.Event.emplace();
    T.Event->SavedPC = Size;
    T.Event->Vector = uint8_t(Address);
    T.Event->Kind = X64ContinuationEventKind::Software;
    return 1;
  }
  if (Kind == X64PagedAccess::Fault) {
    T.Access.Status = X86PageWalkStatus::Exception;
    T.Access.Exception = X86MemoryException{uint8_t(Address), 0, std::nullopt};
    return 1;
  }
  assert((Size == 1 || Size == 2 || Size == 4 || Size == 8) &&
         Flags <= (kX64PagedStackAccess | 2) && !T.Store &&
         "source admission owns scalar widths and the final store boundary");
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
        T.Bytes[I] = Exchange.Value >> (I * 8);
      return 0;
    }
  }
  std::array<uint8_t, 8> Bytes{};
  A.Kind = MemoryAccessKind::Read;
  if (!T.accept(
          T.Memory.load(A, llvm::MutableArrayRef(Bytes).take_front(Size))) ||
      T.alignmentFault(Address, Size))
    return 1;
  Exchange.Value = 0;
  for (unsigned I = 0; I != Size; ++I)
    Exchange.Value |= uint64_t(Bytes[I]) << (I * 8);
  return 0;
}

llvm::Expected<X64PagedExecutionResult>
X64PagedExecution::execute(X64ContinuationStateV6 &State, uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes, X64PagedBody Body,
                           X64PagedBody Validate, uint32_t MXCSRMask,
                           unsigned PagingAddressBits) {
  Last = {};
  Error.clear();
  if (!Body || !Validate || Bytes.empty() || Bytes.size() > 15 ||
      (PagingAddressBits != 48 && PagingAddressBits != 57))
    return invalid("invalid paged instruction binding");
  auto Controls = Memory.controls();
  if (!(Controls.EFER & 0x400) || (Controls.CR4 & (uint64_t(1) << 32)) ||
      PagingAddressBits != (Controls.CR4 & 0x1000 ? 57u : 48u))
    return invalid("paged instruction binding disagrees with IA-32e controls");
  auto &Original = State.Core.Core.Core.Core;
  if (Original.PC != Address)
    return Last;
  const auto CPL = uint8_t(State.Core.CS.Selector & 3);
  if (CPL == 3 && (Controls.CR0 & 0x40000) && (Original.RFlags & 0x40000) &&
      !Options.AlignmentPriority)
    return invalid(
        "active scalar alignment checks require bound fault priority");
  auto Next = State;
  Transaction T{Memory, CPL,
                Options.AlignmentPriority.value_or(
                    X64PagedAlignmentPriority::AfterPaging)};
  X64PagedExchange Exchange{0, &T};
  auto *NextBytes = reinterpret_cast<uint8_t *>(&Next);
  auto *ExchangeBytes = reinterpret_cast<uint8_t *>(&Exchange);
  if (Validate(NextBytes, ExchangeBytes) != 0)
    return Last;
  Controls.RFlags = Original.RFlags;
  if (auto Failure = Memory.bindControls(Controls))
    return std::move(Failure);
  // Stop at the first changed byte before fetching a later old instruction
  // byte. The environment must relift changed code; it is never executed as
  // the stale source instruction or skipped as a NOP.
  for (unsigned I = 0; I != Bytes.size(); ++I) {
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
  if (T.Access.Status == X86PageWalkStatus::BackingFault) {
    Last.Exit = uint64_t(X64PagedExit::BackingFault);
    Last.BackingFault = T.Access.BackingFault;
    return Last;
  }
  if (T.Event || T.Access.Status == X86PageWalkStatus::Exception) {
    Last.Exception = T.Access.Exception;
    X64ContinuationEventV1 Event = T.Event.value_or(X64ContinuationEventV1{});
    if (!T.Event) {
      Event.SavedPC = Address;
      Event.Vector = T.Access.Exception->Vector;
      Event.Kind = X64ContinuationEventKind::Fault;
      Event.ErrorCode = T.Access.Exception->ErrorCode;
      Event.FaultAddress = T.Access.Exception->CR2.value_or(0);
    }
    auto Delivered = deliverX64PagedEvent(State, Memory, Event, MXCSRMask);
    if (!Delivered)
      return Delivered.takeError();
    Last.Delivery = *Delivered;
    Last.BackingFault = Delivered->BackingFault;
    if (T.Event)
      Last.Exception = Delivered->LastException;
    switch (Delivered->Status) {
    case X64PagedEventStatus::Delivered:
      Last.Exit = uint64_t(X64ContinuationExitV1::Exception);
      Last.InstructionCompleted = T.Event && !Delivered->LastException;
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
    return Last;
  }
  if (Exit > uint64_t(X64ContinuationExitV1::Return)) {
    if (Exit != uint64_t(X64ContinuationExitV1::NativeInstruction) &&
        Exit != uint64_t(X64ContinuationExitV1::InvalidState))
      return invalid("paged body returned an unowned execution status");
    Last.Exit = Exit;
    return Last;
  }
  if (T.Store)
    Memory.commitStore(*T.Store, T.Bytes);
  State = Next;
  State.Core.Core.Core.Core.RFlags &= ~uint64_t(0x10000); // Completed RF.
  Controls.RFlags = State.Core.Core.Core.Core.RFlags;
  if (auto Failure = Memory.bindControls(Controls))
    return std::move(Failure);
  Last.InstructionCompleted = true;
  Last.Exit = Exit;
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

} // namespace neverd::analysis
