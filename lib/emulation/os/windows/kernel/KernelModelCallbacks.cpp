//===- KernelModelCallbacks.cpp - Named executive callbacks --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// WDK Ex*Callback contracts; Windows 19041 ExCreateCallback and
// ExRegisterCallback confirm failure output preservation and the independent
// registration reference.
///
/// \file
/// Named executive callbacks.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error callbackError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "executive callback: " + Text);
}
bool overlap(uint64_t A, uint64_t Size, uint64_t B, uint64_t Count = 1) {
  return Size && Count && A < B + Count && B < A + Size;
}
constexpr size_t MaxObjects = 128, MaxRegistrations = 256,
                 MaxNotifications = 128;
bool systemName(llvm::StringRef Name) {
  return Name.equals_insensitive("\\Callback\\PowerState") ||
         Name.equals_insensitive("\\Callback\\SetSystemTime");
}
std::string systemSpelling(llvm::StringRef Name) {
  return Name.equals_insensitive("\\Callback\\PowerState")
             ? "\\Callback\\PowerState"
             : "\\Callback\\SetSystemTime";
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::createExecutiveCallback(llvm::ArrayRef<uint64_t> A) {
  using namespace windows;
  if (auto E = objectBuffer(A[1], ObjectAttributesSize, false))
    return E;
  auto Size = Memory.readInteger(A[1], 4),
       Root = Memory.readInteger(A[1] + ObjectRootOffset, 8),
       Name = Memory.readInteger(A[1] + ObjectNameOffset, 8),
       Flags = Memory.readInteger(A[1] + ObjectFlagsOffset, 4),
       Security = Memory.readInteger(A[1] + ObjectSecurityOffset, 8),
       QoS = Memory.readInteger(A[1] + ObjectQualityOffset, 8);
  if (!Size)
    return Size.takeError();
  if (!Root)
    return Root.takeError();
  if (!Name)
    return Name.takeError();
  if (!Flags)
    return Flags.takeError();
  if (!Security)
    return Security.takeError();
  if (!QoS)
    return QoS.takeError();
  if (*Size != ObjectAttributesSize)
    return StatusInvalidParameter;
  if (*Root || *Security || *QoS || (*Flags & ~uint64_t(0x250)))
    return callbackError(
        "unsupported directory handle, attributes or security policy");
  if (!*Name)
    return uint64_t(0xc0000001);
  auto Text = readObjectName(*Name);
  if (!Text)
    return Text.takeError();
  if (Text->empty())
    return uint64_t(0xc0000001);
  auto Expanded = resolveObjectPath(*Text, bool(*Flags & 0x40));
  if (!Expanded)
    return Expanded.takeError();
  llvm::StringRef Path(*Expanded);
  if (!Path.starts_with_insensitive("\\Callback\\") || Path.size() <= 10 ||
      Path.drop_front(10).contains('\\'))
    return callbackError(
        "requires one name in the bounded \\Callback directory");
  if (auto E = objectBuffer(A[0], 8, true))
    return E;
  const std::string Key = Path.lower();
  auto Binding = ExecutiveCallbackNames.find(Key);
  const bool System = systemName(Path);
  const bool Sensitive = !(*Flags & 0x40);
  const std::string Spelling = System ? systemSpelling(Path) : Path.str();
  if (Sensitive &&
      (!Path.starts_with("\\Callback\\") || (System && Path != Spelling) ||
       (Binding != ExecutiveCallbackNames.end() &&
        ExecutiveCallbacks.at(Binding->second).Name != Path))) {
    if (uint8_t(A[2]))
      return callbackError(
          "case-distinct callback bindings are outside this namespace");
    return StatusObjectNameNotFound;
  }
  if (Binding != ExecutiveCallbackNames.end()) {
    auto &Object = ExecutiveCallbacks.at(Binding->second);
    if (Object.PointerReferences == SIZE_MAX)
      return callbackError("object reference overflow");
    if (auto E = Memory.writeInteger(A[0], Binding->second, 8))
      return E;
    ++Object.PointerReferences;
    return StatusSuccess;
  }
  if (!System && !uint8_t(A[2]))
    return StatusObjectNameNotFound;
  if (ExecutiveCallbacks.size() >= MaxObjects)
    return StatusInsufficientResources;
  auto Address = allocate(8);
  if (!Address)
    return Address.takeError();
  if (auto E = Memory.writeInteger(A[0], *Address, 8))
    return E;
  ExecutiveCallbacks.emplace(
      *Address,
      ExecutiveCallbackObject{Spelling, 1, 0, 0, System || bool(uint8_t(A[3])),
                              System || bool(*Flags & 0x10), System});
  // ExCreateCallback closes its internal handle before returning the pointer.
  // With no remaining handles, a temporary object's name is already unlinked;
  // pointer and registration references preserve only its body, not the name.
  if (ExecutiveCallbacks.at(*Address).Permanent)
    ExecutiveCallbackNames.emplace(Key, *Address);
  return StatusSuccess;
}

void KernelModel::retireExecutiveCallback(uint64_t Object) {
  const auto &O = ExecutiveCallbacks.at(Object);
  if (O.PointerReferences || O.Registrations || O.Notifications || O.Permanent)
    return;
  auto Name = ExecutiveCallbackNames.find(llvm::StringRef(O.Name).lower());
  if (Name != ExecutiveCallbackNames.end() && Name->second == Object)
    ExecutiveCallbackNames.erase(Name);
  FreedRanges.emplace(Object, 8);
  ExecutiveCallbacks.erase(Object);
}

llvm::Expected<uint64_t>
KernelModel::dereferenceExecutiveCallback(uint64_t Object) {
  auto &O = ExecutiveCallbacks.at(Object);
  if (!O.PointerReferences)
    return callbackError(
        "dereference requires an owned callback pointer reference");
  --O.PointerReferences;
  retireExecutiveCallback(Object);
  return 0;
}

llvm::Expected<uint64_t> KernelModel::makeCallbackTemporary(uint64_t Object) {
  auto It = ExecutiveCallbacks.find(Object);
  if (It == ExecutiveCallbacks.end())
    return callbackError(
        "temporary-object conversion requires a live callback object");
  if (It->second.System)
    return callbackError("system callback objects are provider-owned");
  if (!It->second.PointerReferences)
    return callbackError(
        "temporary-object conversion requires an owned reference");
  It->second.Permanent = false;
  auto Name =
      ExecutiveCallbackNames.find(llvm::StringRef(It->second.Name).lower());
  if (Name != ExecutiveCallbackNames.end() && Name->second == Object)
    ExecutiveCallbackNames.erase(Name);
  retireExecutiveCallback(Object);
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::registerExecutiveCallback(llvm::ArrayRef<uint64_t> A) {
  auto It = ExecutiveCallbacks.find(A[0]);
  if (It == ExecutiveCallbacks.end())
    return callbackError("registration requires a live callback object");
  auto Executable = Memory.canAccess(A[1], 1, Execute);
  if (!Executable)
    return Executable.takeError();
  if (!A[1] || !*Executable)
    return callbackError("callback routine must be executable guest code");
  auto &O = It->second;
  if ((!O.Multiple && O.Registrations) ||
      ExecutiveRegistrations.size() >= MaxRegistrations)
    return 0;
  auto Token = allocate(8);
  if (!Token)
    return Token.takeError();
  ExecutiveRegistrations.emplace(
      *Token, ExecutiveCallbackRegistration{A[0], A[1], A[2]});
  ++O.Registrations;
  return *Token;
}

void KernelModel::eraseExecutiveRegistration(uint64_t Registration) {
  const auto Object = ExecutiveRegistrations.at(Registration).Object;
  ExecutiveRegistrations.erase(Registration);
  FreedRanges.emplace(Registration, 8);
  --ExecutiveCallbacks.at(Object).Registrations;
  retireExecutiveCallback(Object);
}

llvm::Expected<uint64_t>
KernelModel::unregisterExecutiveCallback(uint64_t Registration) {
  auto It = ExecutiveRegistrations.find(Registration);
  if (It == ExecutiveRegistrations.end() || It->second.Removing)
    return callbackError(
        "unregister requires a live, non-removing registration");
  if (!It->second.Busy) {
    eraseExecutiveRegistration(Registration);
    return 0;
  }
  if (PendingWait)
    return callbackError("unregister cannot replace a pending wait");
  for (const auto &[ID, N] : ExecutiveNotifications)
    if (N.Registration == Registration && N.Thread == CurrentThreadKey)
      return callbackError(
          "unregister would wait for its own callback to return");
  It->second.Removing = true;
  It->second.UnregisterThread = CurrentThreadKey;
  Wait W;
  W.Type = Wait::Kind::ExecutiveCallbackUnregister;
  W.Object = Registration;
  W.Execution = CurrentExecution;
  W.Thread = CurrentThreadKey;
  W.IRQL = CurrentIRQL;
  PendingWait = W;
  return 0;
}

llvm::Expected<std::optional<uint32_t>>
KernelModel::pollExecutiveUnregister(const Wait &W) {
  auto It = ExecutiveRegistrations.find(W.Object);
  if (It == ExecutiveRegistrations.end() || !It->second.Removing ||
      It->second.UnregisterThread != W.Thread)
    return callbackError("unregister wait lost its registration or thread");
  if (It->second.Busy)
    return std::optional<uint32_t>{};
  eraseExecutiveRegistration(W.Object);
  return std::optional<uint32_t>{0};
}

llvm::Expected<bool> KernelModel::prepareExecutiveCallback(uint64_t ID) {
  auto &N = ExecutiveNotifications.at(ID);
  for (auto It = ExecutiveRegistrations.upper_bound(N.Cursor);
       It != ExecutiveRegistrations.end(); ++It) {
    auto &R = It->second;
    if (R.Object != N.Object || R.Removing)
      continue;
    if (R.Busy == SIZE_MAX)
      return callbackError("notification busy count overflow");
    N.Cursor = N.Registration = It->first;
    ++R.Busy;
    PendingExecutiveCallback =
        KernelGuestCall{{GuestCallOwner::ExecutiveCallback, ID},
                        R.Routine,
                        {R.Context, N.Argument1, N.Argument2},
                        0};
    return true;
  }
  return false;
}

llvm::Expected<uint64_t>
KernelModel::notifyExecutiveCallback(llvm::ArrayRef<uint64_t> A) {
  // ExNotifyCallback's native common body explicitly accepts NULL as no work.
  if (!A[0])
    return 0;
  auto It = ExecutiveCallbacks.find(A[0]);
  if (It == ExecutiveCallbacks.end())
    return callbackError("notification requires a live callback object");
  if (It->second.System)
    return callbackError(
        "driver notification of system-defined callbacks is prohibited");
  if (hasPendingModelGuestCall() || PendingWait)
    return callbackError(
        "notification cannot replace a prepared callback or wait");
  if (ExecutiveNotifications.size() >= MaxNotifications ||
      NextExecutiveNotification == UINT64_MAX)
    return callbackError("notification continuation limit exceeded");
  const uint64_t ID = NextExecutiveNotification++;
  ExecutiveNotifications.emplace(ID, ExecutiveNotification{A[0], A[1], A[2], 0,
                                                           0, CurrentThreadKey,
                                                           CurrentIRQL});
  ++It->second.Notifications;
  auto Ready = prepareExecutiveCallback(ID);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready) {
    --It->second.Notifications;
    ExecutiveNotifications.erase(ID);
    retireExecutiveCallback(A[0]);
  }
  return 0;
}

llvm::Error KernelModel::beginExecutiveCallback(uint64_t ID) {
  auto It = ExecutiveNotifications.find(ID);
  if (It == ExecutiveNotifications.end() || It->second.Active ||
      It->second.Thread != CurrentThreadKey || It->second.IRQL != CurrentIRQL ||
      !It->second.Registration)
    return callbackError(
        "callback entry lost its notification, caller thread or IRQL");
  It->second.Active = true;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishExecutiveCallback(uint64_t ID) {
  auto It = ExecutiveNotifications.find(ID);
  if (It == ExecutiveNotifications.end() || !It->second.Active ||
      It->second.Thread != CurrentThreadKey || It->second.IRQL != CurrentIRQL)
    return callbackError(
        "callback return lost its notification, caller thread or IRQL");
  auto &N = It->second;
  --ExecutiveRegistrations.at(N.Registration).Busy;
  N.Registration = 0;
  N.Active = false;
  auto Ready = prepareExecutiveCallback(ID);
  if (!Ready)
    return Ready.takeError();
  if (*Ready)
    return std::optional<uint64_t>{};
  const auto Object = N.Object;
  --ExecutiveCallbacks.at(Object).Notifications;
  ExecutiveNotifications.erase(It);
  retireExecutiveCallback(Object);
  return std::optional<uint64_t>{0};
}

llvm::Error KernelModel::validateExecutiveCallbackAccess(uint64_t Address,
                                                         uint32_t Size) const {
  for (const auto &[O, State] : ExecutiveCallbacks)
    if (overlap(Address, Size, O, 8))
      return callbackError("guest access to an opaque callback object");
  for (const auto &[R, State] : ExecutiveRegistrations)
    if (overlap(Address, Size, R, 8))
      return callbackError("guest access to an opaque registration token");
  return llvm::Error::success();
}

llvm::Error
KernelModel::canReleaseExecutiveCallbackStorage(uint64_t Base,
                                                uint64_t Size) const {
  for (const auto &[Token, R] : ExecutiveRegistrations)
    if (overlap(Base, Size, R.Routine) ||
        (R.Context && overlap(Base, Size, R.Context)))
      return callbackError(
          "cannot release registered callback code or context storage");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateExecutiveCallbackUnload() const {
  if (!ExecutiveRegistrations.empty() || !ExecutiveNotifications.empty() ||
      PendingExecutiveCallback)
    return callbackError(
        "unload returned with registered or executing callbacks");
  for (const auto &[Address, O] : ExecutiveCallbacks)
    if (O.PointerReferences || (!O.System && O.Permanent))
      return callbackError("unload returned with callback references or "
                           "permanent guest objects");
  return llvm::Error::success();
}
} // namespace neverd::emulation
