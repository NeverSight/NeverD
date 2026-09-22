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

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

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
  if (Name == "KeAcquireInterruptSpinLock") {
    auto Old = Interrupts.acquire(A[0], CurrentExecution, CurrentIRQL);
    if (!Old)
      return Old.takeError();
    CurrentIRQL = Interrupts.connection(A[0])->IRQL;
    return *Old;
  }
  if (Name == "KeReleaseInterruptSpinLock") {
    auto Restored =
        Interrupts.release(A[0], CurrentExecution, uint8_t(A[1]), CurrentIRQL);
    if (!Restored)
      return Restored.takeError();
    CurrentIRQL = *Restored;
    return 0;
  }
  if (Name == "KeSynchronizeExecution") {
    if (hasPendingModelGuestCall())
      return apiError("cannot replace a prepared guest callback");
    const auto *Connection = Interrupts.connection(A[0]);
    if (!Connection || CurrentIRQL > Connection->IRQL)
      return apiError(
          "synchronization requires a live interrupt at caller IRQL <= DIRQL");
    auto Call = Interrupts.synchronize(A[0], A[1], A[2]);
    if (!Call)
      return Call.takeError();
    PendingInterruptCall = std::move(*Call);
    return 0;
  }
  if (Name == "IoDisconnectInterrupt" || Name == "IoDisconnectInterruptEx") {
    uint64_t Object = A[0];
    uint32_t Version = 0;
    if (Name == "IoDisconnectInterruptEx") {
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
          Version != interrupts::LineBased)
        return apiError("unsupported Ex disconnect version");
      Object = *Context;
    }
    if (auto E = Interrupts.disconnect(Object, Version))
      return E;
    FreedRanges.emplace(Object, interrupts::TokenSize);
    return 0;
  }
  if (Name != "IoConnectInterrupt" && Name != "IoConnectInterruptEx")
    return apiError("unknown interrupt routine");

  uint64_t Output = 0, Routine = 0, Context = 0, SpinLock = 0, PDO = 0;
  uint64_t Vector = 0, IRQL = 0, Synchronize = 0, Mode = 0;
  uint64_t Share = 0, Affinity = 0, Floating = 0, Group = 0;
  uint32_t Version = 0;
  bool LineBased = false;
  if (Name == "IoConnectInterrupt") {
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
        Version != interrupts::LineBased)
      return apiError("unsupported Ex connect version (message/passive "
                      "interrupts are not modeled)");
    LineBased = Version == interrupts::LineBased;
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
    if (!LineBased) {
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
  if (!Output || !Routine)
    return apiError(
        "registration requires output storage and a service routine");
  if (SpinLock || Share || Floating || Group ||
      (!LineBased && Mode != uint32_t(DriverInterruptMode::Latched)))
    return apiError("only private-lock exclusive latched CPU0/group0 "
                    "interrupts are modeled");
  if (!LineBased && !Affinity)
    return windows::StatusInvalidParameter;
  if (auto E = validateGuestAccess(Output, profile::PointerSize, true))
    return E;
  if (Context)
    if (auto E = validateDispatcherStorage(Context, 1, false))
      return E;
  auto Candidate = Interrupts.match(PDO, uint32_t(Vector), uint8_t(IRQL),
                                    Affinity, LineBased);
  if (!Candidate)
    return Candidate.takeError();
  if (LineBased && !Synchronize)
    Synchronize = Candidate->IRQL;
  if (Synchronize != Candidate->IRQL)
    return apiError(
        "single-vector synchronization IRQL must equal assigned DIRQL");
  const uint64_t Aligned = (NextAllocation + interrupts::TokenSize - 1) &
                           ~(interrupts::TokenSize - 1);
  if (Aligned > AllocationEnd ||
      interrupts::TokenSize > AllocationEnd - Aligned)
    return windows::StatusInsufficientResources;
  auto Object = allocate(interrupts::TokenSize);
  if (!Object)
    return Object.takeError();
  Candidate->Object = *Object;
  Candidate->Routine = Routine;
  Candidate->Context = Context;
  Candidate->Version = Version;
  for (const auto &[Address, Device] : Devices)
    if (Device.OwnerKind == DeviceOwnerKind::Guest && Device.Extension &&
        Output >= Device.Extension && Output < Address + Device.Size &&
        profile::PointerSize <= Address + Device.Size - Output) {
      if (Device.DeletePending)
        return apiError("interrupt output storage belongs to a deleted device");
      Candidate->OutputDeviceBase = Address;
      Candidate->OutputDeviceSize = Device.Size;
      break;
    }
  // The candidate has been checked without publishing a connection. A failed
  // output write cannot leave a live opaque interrupt registration behind.
  if (auto E = Memory.writeInteger(Output, *Object, profile::PointerSize))
    return E;
  if (auto E = Interrupts.connect(*Candidate))
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
  return std::optional<uint64_t>{Return->Value};
}

llvm::Error KernelModel::validateExecutionReturn(uint64_t Identity,
                                                 uint8_t EntryIRQL) const {
  if (Identity != CurrentExecution)
    return apiError("return does not own the active execution identity");
  if (auto E = Interrupts.validateExecutionReturn(Identity))
    return E;
  if (CurrentIRQL != EntryIRQL)
    return apiError("guest return did not restore entry IRQL");
  return llvm::Error::success();
}
} // namespace neverd::emulation
