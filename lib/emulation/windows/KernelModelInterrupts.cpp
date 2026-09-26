//===- KernelModelInterrupts.cpp - WDK interrupt registration and calls ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode genuine WDK argument widths and selected Ex records. Guest callbacks
/// enter their interrupt lock only after the session saves the caller context.
///
//===----------------------------------------------------------------------===//

#include "KernelAPINames.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error apiError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt API: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::callInterruptAPI(llvm::StringRef Name,
                              llvm::ArrayRef<uint64_t> A) {
  if (Name == kernel_api::KeAcquireInterruptSpinLock) {
    auto Old = Interrupts.acquire(A[0], CurrentExecution, CurrentIRQL);
    if (!Old)
      return Old.takeError();
    CurrentIRQL = Interrupts.connection(A[0])->SynchronizeIRQL;
    return *Old;
  }
  if (Name == kernel_api::KeReleaseInterruptSpinLock) {
    auto Restored =
        Interrupts.release(A[0], CurrentExecution, uint8_t(A[1]), CurrentIRQL);
    if (!Restored)
      return Restored.takeError();
    CurrentIRQL = *Restored;
    return 0;
  }
  if (Name == kernel_api::KeSynchronizeExecution) {
    if (hasPendingModelGuestCall() || PendingWait)
      return apiError("cannot replace a prepared guest callback");
    const auto *Connection = Interrupts.connection(A[0]);
    if (!Connection || CurrentIRQL > Connection->SynchronizeIRQL)
      return apiError(
          "synchronization requires a live interrupt at caller IRQL <= DIRQL");
    auto Call = Interrupts.synchronize(A[0], A[1], A[2]);
    if (!Call)
      return Call.takeError();
    if (Connection->Passive) {
      if (PendingWait)
        return apiError(
            "passive synchronization cannot replace a pending wait");
      Wait Waiter;
      Waiter.Type = Wait::Kind::InterruptSynchronization;
      Waiter.Object = Call->Token.ID;
      Waiter.Execution = CurrentExecution;
      Waiter.IRQL = CurrentIRQL;
      PendingWait = Waiter;
    }
    PendingInterruptCall = std::move(*Call);
    return 0;
  }
  if (Name == kernel_api::IoDisconnectInterrupt ||
      Name == kernel_api::IoDisconnectInterruptEx) {
    uint64_t Object = A[0];
    uint32_t Version = 0;
    if (Name == kernel_api::IoDisconnectInterruptEx) {
      if (auto E = validateGuestAccess(A[0], interrupts::DisconnectSize, false))
        return E;
      auto VersionField =
          Memory.readInteger(A[0] + interrupts::VersionOffset, 4);
      auto Context =
          Memory.readInteger(A[0] + interrupts::DisconnectContext, 8);
      if (!VersionField || !Context)
        return llvm::joinErrors(VersionField.takeError(), Context.takeError());
      Version = uint32_t(*VersionField);
      if (Version != interrupts::FullySpecified &&
          Version != interrupts::FullySpecifiedGroup &&
          Version != interrupts::LineBased &&
          Version != interrupts::MessageBased &&
          Version != interrupts::MessageBasedPassive)
        return apiError("unsupported Ex disconnect version");
      Object = *Context;
    }
    if (auto E = Interrupts.disconnect(Object, Version))
      return E;
    if (Version != interrupts::MessageBased &&
        Version != interrupts::MessageBasedPassive)
      FreedRanges.emplace(Object, interrupts::TokenSize);
    return 0;
  }
  if (Name != kernel_api::IoConnectInterrupt &&
      Name != kernel_api::IoConnectInterruptEx)
    return apiError("unknown interrupt routine");

  uint64_t Output = 0, Routine = 0, Context = 0, SpinLock = 0, PDO = 0;
  uint64_t Vector = 0, IRQL = 0, Synchronize = 0, Mode = 0;
  uint64_t Share = 0, Affinity = 0, Floating = 0, Group = 0;
  uint32_t Version = 0;
  bool LineBased = false, MessageBased = false, Passive = false;
  uint64_t Fallback = 0;
  if (Name == kernel_api::IoConnectInterrupt) {
    Output = A[0];
    Routine = A[1];
    Context = A[2];
    SpinLock = A[3];
    Vector = uint32_t(A[4]);
    IRQL = uint8_t(A[5]);
    Synchronize = uint8_t(A[6]);
    Mode = uint32_t(A[7]);
    Share = uint8_t(A[8]);
    Affinity = A[9];
    Floating = uint8_t(A[10]);
  } else {
    if (auto E = validateGuestAccess(A[0], 4, false))
      return E;
    auto VersionField = Memory.readInteger(A[0], 4);
    if (!VersionField)
      return VersionField.takeError();
    Version = uint32_t(*VersionField);
    if (Version != interrupts::FullySpecified &&
        Version != interrupts::FullySpecifiedGroup &&
        Version != interrupts::LineBased &&
        Version != interrupts::MessageBased &&
        Version != interrupts::MessageBasedPassive)
      return apiError("unsupported Ex connect version");
    LineBased = Version == interrupts::LineBased;
    MessageBased = Version == interrupts::MessageBased ||
                   Version == interrupts::MessageBasedPassive;
    Passive = Version == interrupts::MessageBasedPassive;
    struct Field {
      uint64_t Offset;
      unsigned Size;
      uint64_t *Value;
    };
    std::vector<Field> Fields{{interrupts::PDOOffset, 8, &PDO},
                              {interrupts::OutputOffset, 8, &Output},
                              {interrupts::RoutineOffset, 8, &Routine},
                              {interrupts::ContextOffset, 8, &Context},
                              {interrupts::SpinLockOffset, 8, &SpinLock},
                              {interrupts::SynchronizeIRQL, 1, &Synchronize},
                              {interrupts::FloatingSave, 1, &Floating}};
    if (MessageBased)
      Fields.push_back({interrupts::FallbackRoutine, 8, &Fallback});
    if (!LineBased && !MessageBased) {
      Fields.push_back({interrupts::ShareVector, 1, &Share});
      Fields.push_back({interrupts::Vector, 4, &Vector});
      Fields.push_back({interrupts::IRQL, 1, &IRQL});
      Fields.push_back({interrupts::Mode, 4, &Mode});
      Fields.push_back({interrupts::Affinity, 8, &Affinity});
      if (Version == interrupts::FullySpecifiedGroup)
        Fields.push_back({interrupts::Group, 2, &Group});
    }
    for (const Field &Field : Fields) {
      if (Field.Offset > UINT64_MAX - A[0])
        return apiError("Ex parameter field address overflows");
      if (auto E = validateGuestAccess(A[0] + Field.Offset, Field.Size, false))
        return E;
      auto Value = Memory.readInteger(A[0] + Field.Offset, Field.Size);
      if (!Value)
        return Value.takeError();
      *Field.Value = *Value;
    }
    if (!PDO || !isProviderDevice(PDO))
      return apiError("Ex registration requires its configured PDO");
  }
  if (!MessageBased && !LineBased && Version && !IRQL && !Synchronize)
    Passive = true;
  if (Passive && (SpinLock || Synchronize))
    return windows::StatusInvalidParameter;
  if (!Output || !Routine)
    return apiError(
        "registration requires output storage and a service routine");
  if (Floating || Group)
    return apiError("only CPU0/group0 interrupts without floating state saving "
                    "are modeled");
  if (!LineBased && !MessageBased && !Affinity)
    return windows::StatusInvalidParameter;
  if (auto E = validateGuestAccess(Output, profile::PointerSize, true))
    return E;
  if (Context)
    if (auto E = validateDispatcherStorage(Context, 1, false))
      return E;
  std::vector<KernelInterrupts::Connection> Candidates;
  if (MessageBased) {
    auto Messages = Interrupts.matchMessages(PDO);
    if (!Messages)
      return Messages.takeError();
    Candidates = std::move(*Messages);
    if (Candidates.empty()) {
      if (!Fallback)
        return windows::StatusNotFound;
      if (auto E = validateGuestAccess(A[0], 4, true))
        return E;
      Routine = Fallback;
      MessageBased = false;
      LineBased = true;
      Version = interrupts::LineBased;
    }
  }
  if (!MessageBased) {
    auto Candidate = Interrupts.match(PDO, uint32_t(Vector), uint8_t(IRQL),
                                      Affinity, LineBased, Passive);
    if (!Candidate)
      return Candidate.takeError();
    if (LineBased && !Candidate->IRQL && !Synchronize)
      Passive = true;
    if (Passive && SpinLock)
      return windows::StatusInvalidParameter;
    if (!Version && !Candidate->IRQL)
      return apiError("passive interrupts require IoConnectInterruptEx");
    if (!Version && Candidate->ResourceMessage)
      return apiError("message interrupts require IoConnectInterruptEx");
    const auto &Resource =
        Resources.find(Candidate->PDO)->Interrupts[Candidate->ResourceIndex];
    if (!LineBased && Mode != uint32_t(Resource.Mode))
      return apiError("interrupt mode must match the assigned resource");
    if (!LineBased &&
        bool(Share) != (Resource.Share == DriverInterruptShare::Shared))
      return apiError("ShareVector must match the assigned interrupt resource");
    if (LineBased && !Synchronize && !Passive)
      Synchronize = Candidate->IRQL;
    if (!Passive && (Synchronize < Candidate->IRQL ||
                     Synchronize > DriverInterruptMaximumLevel ||
                     (!SpinLock && Synchronize != Candidate->IRQL)))
      return apiError("synchronization IRQL requires the assigned DIRQL or a "
                      "shared caller lock at a higher device DIRQL");
    Candidates.push_back(*Candidate);
  }
  uint8_t UnifiedIRQL = 0;
  if (MessageBased && !Passive) {
    uint8_t MaximumIRQL = 0;
    for (const auto &Candidate : Candidates)
      MaximumIRQL = std::max(MaximumIRQL, Candidate.IRQL);
    if ((Synchronize && Synchronize < MaximumIRQL) ||
        Synchronize > DriverInterruptMaximumLevel)
      return apiError("message synchronization IRQL must cover every message");
    if (SpinLock || Synchronize)
      UnifiedIRQL = uint8_t(Synchronize ? Synchronize : MaximumIRQL);
  }
  if (SpinLock) {
    if (SpinLock < profile::UserProbeLimit ||
        SpinLock > UINT64_MAX - profile::PointerSize ||
        SpinLock % profile::PointerSize ||
        (Output < SpinLock + profile::PointerSize &&
         SpinLock < Output + profile::PointerSize))
      return apiError(
          "interrupt spin lock requires separate aligned kernel storage");
    if (ExecutiveSpinLocks.contains(SpinLock))
      return apiError(
          "interrupt spin lock is already owned by an executive lock");
    // A previously connected lock was already checked and is opaque while
    // connected. Its address intentionally names the same lock authority.
    if (!Interrupts.usesSpinLock(SpinLock)) {
      if (auto E =
              validateDispatcherStorage(SpinLock, profile::PointerSize, true))
        return E;
      auto Writable =
          Memory.canAccess(SpinLock, profile::PointerSize, Read | Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return apiError(
            "interrupt spin lock requires writable nonpaged storage");
      auto Value = Memory.readInteger(SpinLock, profile::PointerSize);
      if (!Value)
        return Value.takeError();
      if (*Value)
        return apiError("interrupt spin lock must be initialized and free");
    }
  }
  const uint64_t Aligned = (NextAllocation + interrupts::TokenSize - 1) &
                           ~(interrupts::TokenSize - 1);
  const uint64_t TableSize =
      MessageBased ? interrupts::MessageTableHeaderSize +
                         Candidates.size() * interrupts::MessageEntrySize
                   : 0;
  const uint64_t TokenOffset =
      (TableSize + interrupts::TokenSize - 1) & ~(interrupts::TokenSize - 1);
  const uint64_t AllocationSize =
      TokenOffset + Candidates.size() * interrupts::TokenSize;
  if (Aligned > AllocationEnd || AllocationSize > AllocationEnd - Aligned)
    return windows::StatusInsufficientResources;
  for (size_t I = 0; I < Candidates.size(); ++I) {
    auto &Candidate = Candidates[I];
    Candidate.Object = Aligned + TokenOffset + I * interrupts::TokenSize;
    Candidate.Routine = Routine;
    Candidate.Context = Context;
    Candidate.Version = Version;
    Candidate.SpinLock = SpinLock;
    Candidate.Passive = Passive;
    Candidate.SynchronizeIRQL =
        Passive        ? 0
        : MessageBased ? (UnifiedIRQL ? UnifiedIRQL : Candidate.IRQL)
                       : uint8_t(Synchronize);
    for (const auto &[Address, Device] : Devices)
      if (Device.OwnerKind == DeviceOwnerKind::Guest && Device.Extension &&
          Output >= Device.Extension && Output < Address + Device.Size &&
          profile::PointerSize <= Address + Device.Size - Output) {
        if (Device.DeletePending)
          return apiError(
              "interrupt output storage belongs to a deleted device");
        Candidate.OutputDeviceBase = Address;
        Candidate.OutputDeviceSize = Device.Size;
        break;
      }
  }
  if (auto E = MessageBased ? Interrupts.canConnectMessages(Aligned, Candidates)
                            : Interrupts.canConnect(Candidates.front()))
    return E;
  auto Storage = allocate(AllocationSize);
  if (!Storage)
    return Storage.takeError();
  if (MessageBased) {
    if (auto E = Memory.writeInteger(*Storage + interrupts::MessageTableIRQL,
                                     UnifiedIRQL, 1))
      return E;
    if (auto E = Memory.writeInteger(*Storage + interrupts::MessageTableCount,
                                     Candidates.size(), 4))
      return E;
    for (size_t I = 0; I < Candidates.size(); ++I) {
      const auto Message = Interrupts.assignment(Candidates[I]);
      const uint64_t Base = *Storage + interrupts::MessageTableHeaderSize +
                            I * interrupts::MessageEntrySize;
      struct Field {
        uint64_t Offset;
        uint64_t Value;
        unsigned Size;
      };
      const Field Fields[] = {
          {interrupts::MessageAddress, Message.MessageAddress, 8},
          {interrupts::MessageAffinity, Message.TranslatedAffinity, 8},
          {interrupts::MessageObject, Candidates[I].Object, 8},
          {interrupts::MessageData, Message.MessageData, 4},
          {interrupts::MessageVector, Message.TranslatedVector, 4},
          {interrupts::MessageIRQL, Message.TranslatedLevel, 1},
          {interrupts::MessageMode, uint32_t(DriverInterruptMode::Latched), 4},
          {interrupts::MessagePolarity, uint32_t(Message.Polarity), 4}};
      for (const auto &Field : Fields)
        if (auto E = Memory.writeInteger(Base + Field.Offset, Field.Value,
                                         Field.Size))
          return E;
    }
  }
  if (Fallback && !MessageBased)
    if (auto E = Memory.writeInteger(A[0], Version, 4))
      return E;
  // All identities and output storage are checked before publishing the group.
  if (auto E = Memory.writeInteger(Output, *Storage, profile::PointerSize))
    return E;
  if (auto E = MessageBased ? Interrupts.connectMessages(*Storage, Candidates)
                            : Interrupts.connect(Candidates.front()))
    return E;
  return windows::StatusSuccess;
}

llvm::Error KernelModel::beginGuestCall(GuestCallToken Token) {
  if (!Token.ID)
    return apiError("guest callback has no continuation identity");
  if (Token.Owner == GuestCallOwner::DMA)
    return beginDMACall(Token.ID);
  if (Token.Owner != GuestCallOwner::Interrupt)
    return llvm::Error::success();
  auto IRQL = Interrupts.beginCall(Token.ID, CurrentIRQL, Scheduler.now100ns());
  if (!IRQL)
    return IRQL.takeError();
  CurrentIRQL = *IRQL;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishInterruptCall(uint64_t Token, uint64_t Value) {
  auto Return =
      Interrupts.finishCall(Token, Value, CurrentIRQL, Scheduler.now100ns());
  if (!Return)
    return Return.takeError();
  CurrentIRQL = Return->RestoredIRQL;
  if (Return->Next) {
    PendingInterruptCall = std::move(*Return->Next);
    return std::optional<uint64_t>{};
  }
  return std::optional<uint64_t>{Return->Value};
}

llvm::Error KernelModel::validateExecutionReturn(uint64_t Identity,
                                                 uint8_t EntryIRQL,
                                                 bool Nested) const {
  if (Identity != CurrentExecution)
    return apiError("return does not own the active execution identity");
  if (!ProcessAttachments.empty() &&
      ProcessAttachments.back().Execution == Identity)
    return apiError("guest return has an unmatched process attachment");
  for (const auto &[Address, Lock] : ExecutiveSpinLocks)
    if (Lock.Execution == Identity)
      return apiError("guest return retains an executive spin lock");
  for (const RaisedIRQL &Raise : RaisedIRQLs)
    if (Raise.Execution == Identity)
      return apiError("guest return retains a raised IRQL");
  if (Dispatcher.ownsMutex(Identity))
    return apiError("guest return retains an owned mutex");
  if (auto It = ApcStates.find(CurrentThreadKey);
      !Nested && It != ApcStates.end()) {
    const bool SystemThread =
        Scheduler.active() &&
        Scheduler.active()->Kind == KernelScheduler::CallbackKind::SystemThread;
    if (It->second.GuardedDepth ||
        It->second.CriticalDepth > (SystemThread ? 1 : 0))
      return apiError("guest return retains an unmatched APC-disable region");
  }
  if (auto E = Interrupts.validateExecutionReturn(Identity))
    return E;
  if (CancelLock.Callback && CancelLock.CallbackExecution == Identity) {
    if (!CancelLock.Callback || CancelLock.Held ||
        CurrentIRQL != CancelLock.OldIRQL)
      return apiError(
          "WDM cancel callback must release the cancel spin lock and "
          "restore its caller IRQL");
    return llvm::Error::success();
  }
  if (CurrentIRQL != EntryIRQL)
    return apiError("guest return did not restore entry IRQL");
  return llvm::Error::success();
}
} // namespace neverd::emulation
