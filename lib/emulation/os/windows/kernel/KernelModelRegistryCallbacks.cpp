//===- KernelModelRegistryCallbacks.cpp - Synchronous registry filters ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Synchronous registry filters.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t StorageSize = 256, PostOffset = 160;
// ntstatus.h STATUS_CALLBACK_BYPASS. 0x106 is STATUS_NOT_ALL_ASSIGNED.
constexpr uint32_t CallbackBypass = 0xc0000503;
llvm::Error registryError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "registry callback: " + Message);
}
bool overlap(uint64_t A, uint64_t Size, uint64_t B, uint64_t Length = 1) {
  return Size && Length && A < B + Length && B < A + Size;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::registerRegistryCallback(llvm::ArrayRef<uint64_t> A) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return registryError(
        "registration requires the explicit Windows 10.0.19041 profile");
  if (RegistryCallback)
    return registryError("multiple legacy filter ordering is not modeled");
  auto Executable = Memory.canAccess(A[0], 1, Execute);
  if (!Executable)
    return Executable.takeError();
  if (!A[0] || !*Executable)
    return registryError("routine must be executable guest code");
  if (auto E = objectBuffer(A[2], 8, true))
    return E;
  if (NextRegistryCookie == UINT64_MAX)
    return registryError("cookie generation exhausted");
  const uint64_t Cookie = NextRegistryCookie;
  if (auto E = Memory.writeInteger(A[2], Cookie, 8))
    return E;
  RegistryCallback = RegistryCallbackRegistration{Cookie, A[0], A[1]};
  ++NextRegistryCookie;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::unregisterRegistryCallback(uint64_t Cookie) {
  for (const auto &[ID, N] : RegistryNotifications)
    if (N.Active && N.Thread == CurrentThreadKey)
      return registryError("unregister from a registry callback is prohibited");
  if (!RegistryCallback || RegistryCallback->Cookie != Cookie ||
      RegistryCallback->Removing)
    return windows::StatusInvalidParameter;
  if (!RegistryCallback->Busy) {
    RegistryCallback.reset();
    return windows::StatusSuccess;
  }
  if (PendingWait)
    return registryError("unregister cannot replace an existing wait");
  RegistryCallback->Removing = true;
  RegistryCallback->RemovalThread = CurrentThreadKey;
  Wait W;
  W.Type = Wait::Kind::RegistryCallbackRemove;
  W.Object = Cookie;
  W.Execution = CurrentExecution;
  W.Thread = CurrentThreadKey;
  W.IRQL = CurrentIRQL;
  PendingWait = W;
  return windows::StatusSuccess;
}
llvm::Expected<std::optional<uint32_t>>
KernelModel::pollRegistryCallbackRemove(const Wait &W) {
  if (!RegistryCallback || RegistryCallback->Cookie != W.Object ||
      !RegistryCallback->Removing ||
      RegistryCallback->RemovalThread != W.Thread)
    return registryError("removal lost its cookie or caller thread");
  if (RegistryCallback->Busy)
    return std::optional<uint32_t>{};
  RegistryCallback.reset();
  return std::optional<uint32_t>{0};
}
llvm::Expected<uint64_t> KernelModel::registryKeyObject(uint64_t Identity) {
  auto Found = RegistryKeyObjects.find(Identity);
  if (Found != RegistryKeyObjects.end())
    return Found->second;
  auto Object = allocate(8);
  if (!Object)
    return Object.takeError();
  RegistryKeyObjects.emplace(Identity, *Object);
  return *Object;
}

llvm::Expected<uint64_t>
KernelModel::registryOperation(llvm::StringRef Name,
                               llvm::ArrayRef<uint64_t> A) {
  // Even a filter being removed retains the key references of operations
  // already in flight. Do not let a second close invalidate that captured key.
  for (const auto &[ID, N] : RegistryNotifications) {
    if (N.Thread == CurrentThreadKey)
      return registryError("recursive registry operations are not modeled");
    if (Name != "ZwClose")
      continue;
    const bool Open = N.API == "ZwOpenKey" || N.API == "ZwCreateKey";
    uint64_t Handle = N.Arguments[0];
    if (Open) {
      auto Root = Memory.readInteger(N.Arguments[2] + 8, 8);
      if (!Root)
        return Root.takeError();
      Handle = *Root;
    }
    if (Handle && Handle == A[0])
      return registryError(
          "closing a key borrowed by an in-flight operation is not modeled");
  }
  if (!RegistryCallback || RegistryCallback->Removing ||
      !Registry.isConfigured())
    return Registry.call(Name, A);
  if (CurrentIRQL)
    return registryError("notifications require PASSIVE_LEVEL");
  if (hasPendingModelGuestCall() || PendingWait)
    return registryError(
        "operation cannot replace a prepared callback or wait");
  if (RegistryNotifications.size() >= profile::MaxConcurrentCallbacks ||
      NextRegistryNotification == UINT64_MAX)
    return registryError("notification continuation capacity exhausted");
  RegistryNotification N;
  N.API = Name.str();
  N.Arguments.assign(A.begin(), A.end());
  N.Thread = CurrentThreadKey;
  N.Caller = captureExecutionContext();
  N.Cookie = RegistryCallback->Cookie;
#define NEVERD_REGISTRY_NOTIFY(API, Pre, Post, Size, Context)                  \
  if (Name == #API) {                                                          \
    N.PreClass = Pre;                                                          \
    N.PostClass = Post;                                                        \
    N.PreSize = Size;                                                          \
    N.ContextOffset = Context;                                                 \
  }
#include "KernelRegistryNotify.def"
#undef NEVERD_REGISTRY_NOTIFY
  if (!N.PreSize)
    return registryError("operation has no notification schema");
  const bool Open = Name == "ZwOpenKey" || Name == "ZwCreateKey";
  if (!Open) {
    auto Key = Registry.keyIdentity(A[0]);
    if (!Key)
      return Registry.call(Name, A); // No key object was resolved.
    auto Object = registryKeyObject(*Key);
    if (!Object)
      return Object.takeError();
    N.Object = *Object;
  }
  std::vector<uint8_t> Bytes(StorageSize);
  auto Put = [&](uint32_t Offset, uint64_t Value, unsigned Width = 8) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.at(Offset + I) = uint8_t(Value >> (I * 8));
  };
  auto Read = [&](uint64_t Address,
                  unsigned Width) -> llvm::Expected<uint64_t> {
    if (auto E = objectBuffer(Address, Width, false))
      return E;
    return Memory.readInteger(Address, Width);
  };
  if (Open) {
    if (auto E = objectBuffer(A[0], 8, true))
      return E;
    if (Name == "ZwCreateKey" && A[6])
      if (auto E = objectBuffer(A[6], 4, true))
        return E;
    if (auto E = objectBuffer(A[2], 48, false))
      return E;
    std::vector<uint8_t> OA(48);
    if (auto E = Memory.read(A[2], OA))
      return E;
    auto Field = [&](unsigned Offset) {
      return llvm::support::endian::read64le(OA.data() + Offset);
    };
    const auto RootHandle = Field(8), NamePointer = Field(16);
    auto Length = Read(NamePointer, 2);
    if (!Length)
      return Length.takeError();
    auto Buffer = Read(NamePointer + 8, 8);
    if (!Buffer)
      return Buffer.takeError();
    uint64_t RootID = 0, Skip = 0;
    if (RootHandle) {
      auto Key = Registry.keyIdentity(RootHandle);
      if (!Key)
        return Registry.call(Name, A);
      RootID = *Key;
    } else {
      // Absolute names use the modeled \Registry root object. RemainingName
      // excludes that prefix, following REG_*_KEY_INFORMATION_V1.
      constexpr llvm::StringLiteral Prefix = "\\Registry\\";
      if (*Length < Prefix.size() * 2)
        return registryError("absolute open name lacks the registry root");
      for (unsigned I = 0; I < Prefix.size(); ++I) {
        auto Character = Read(*Buffer + I * 2, 2);
        if (!Character)
          return Character.takeError();
        const auto Lower = [](uint64_t C) {
          return C >= 'A' && C <= 'Z' ? C + 32 : C;
        };
        if (Lower(*Character) != Lower(Prefix[I]))
          return registryError(
              "absolute open name is outside the registry root");
      }
      Skip = Prefix.size() * 2;
    }
    auto Root = registryKeyObject(RootID);
    if (!Root)
      return Root.takeError();
    Put(0, NamePointer);
    Put(8, *Root);
    Put(24, Name == "ZwCreateKey" ? uint32_t(A[5]) : 0, 4);
    Put(32, Name == "ZwCreateKey" ? A[4] : 0);
    Put(40, Field(32));
    Put(48, Field(40));
    Put(56, uint32_t(A[1]), 4);
    Put(96, 0);  // No transactional registry APIs are admitted.
    Put(104, 1); // WDK V1.
    Put(120, uint32_t(A[1]) & 0x300, 4);
    Put(124, llvm::support::endian::read32le(OA.data() + 24), 4);
    Put(128, windows::KernelMode, 1); // A driver called Zw*, not Nt*.
    Put(232, *Length - Skip, 2);
    Put(234, *Length - Skip, 2);
    Put(240, *Buffer + Skip);
  } else {
    Put(0, N.Object);
    if (Name == "ZwSetValueKey") {
      Put(8, A[1]);
      Put(16, uint32_t(A[2]), 4);
      Put(20, uint32_t(A[3]), 4);
      Put(24, A[4]);
      Put(32, uint32_t(A[5]), 4);
    } else if (Name == "ZwEnumerateKey") {
      Put(8, uint32_t(A[1]), 4);
      Put(12, uint32_t(A[2]), 4);
      Put(16, A[3]);
      Put(24, uint32_t(A[4]), 4);
      Put(32, A[5]);
    } else if (Name == "ZwDeleteValueKey")
      Put(8, A[1]);
    else if (Name == "ZwQueryValueKey") {
      Put(8, A[1]);
      Put(16, uint32_t(A[2]), 4);
      Put(24, A[3]);
      Put(32, uint32_t(A[4]), 4);
      Put(40, A[5]);
    }
  }
  auto Storage = allocate(StorageSize);
  if (!Storage)
    return Storage.takeError();
  N.Storage = *Storage;
  if (Open) {
    Put(64, Name == "ZwCreateKey" ? A[6] : 0);
    Put(72, N.Storage + 224);
    Put(112, N.Storage + 232);
  }
  if (auto E = Memory.write(N.Storage, Bytes))
    return E;
  const uint64_t ID = NextRegistryNotification++;
  RegistryNotifications.emplace(ID, std::move(N));
  ++RegistryCallback->Busy;
  const auto &Published = RegistryNotifications.at(ID);
  PendingRegistryCallback = KernelGuestCall{
      {GuestCallOwner::RegistryCallback, ID},
      RegistryCallback->Routine,
      {RegistryCallback->Context, Published.PreClass, Published.Storage},
      0};
  return 0; // DriverSession replaces this provisional result on continuation.
}

llvm::Error KernelModel::beginRegistryCallback(uint64_t ID) {
  auto It = RegistryNotifications.find(ID);
  if (It == RegistryNotifications.end() || It->second.Active ||
      It->second.Thread != CurrentThreadKey || CurrentIRQL ||
      !RegistryCallback || RegistryCallback->Cookie != It->second.Cookie)
    return registryError("entry lost its notification, cookie, thread or IRQL");
  auto &APC = ApcStates[CurrentThreadKey];
  if (APC.CriticalDepth >= profile::MaxAPCRegionNesting)
    return registryError("callback entry exceeds the APC region bound");
  It->second.Baseline = APC;
  // The selected NT registry paths keep normal kernel APCs disabled across
  // filter invocation. Represent that public condition as one owned region;
  // private NT implementation lock nesting is not exposed as a thread field.
  ++APC.CriticalDepth;
  It->second.Active = true;
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishRegistryCallback(uint64_t ID, uint64_t Value) {
  auto It = RegistryNotifications.find(ID);
  if (It == RegistryNotifications.end() || !It->second.Active ||
      It->second.Thread != CurrentThreadKey || CurrentIRQL ||
      !RegistryCallback || RegistryCallback->Cookie != It->second.Cookie)
    return registryError(
        "return lost its notification, cookie, thread or IRQL");
  auto &N = It->second;
  auto &APC = ApcStates[CurrentThreadKey];
  if (APC.CriticalDepth != N.Baseline.CriticalDepth + 1 ||
      APC.GuardedDepth != N.Baseline.GuardedDepth)
    return registryError(
        "callback returned with an unbalanced APC-disable region");
  --APC.CriticalDepth;
  N.Active = false;
  // DriverSession has retired the callback's private stack. Resume the
  // original API in its still-live caller scope; that caller owns any user
  // buffers the registry operation must fill before post notification.
  if (auto E = restoreExecutionContext(N.Caller))
    return E;
  auto Finish =
      [&](uint32_t Status) -> llvm::Expected<std::optional<uint64_t>> {
    --RegistryCallback->Busy;
    FreedRanges.emplace(N.Storage, StorageSize);
    RegistryNotifications.erase(It);
    return std::optional<uint64_t>{Status};
  };
  const bool Open = N.API == "ZwOpenKey" || N.API == "ZwCreateKey";
  const uint32_t Status = uint32_t(Value);
  if (N.Post) {
    if (Status == CallbackBypass) {
      auto Override = Memory.readInteger(N.Storage + PostOffset + 24, 4);
      if (!Override)
        return Override.takeError();
      if ((Open || N.API == "ZwClose") && *Override != N.Status)
        return registryError(
            "open/close post override requires unmodeled object cleanup");
      N.Status = uint32_t(*Override);
    } else if (Status)
      return registryError(
          "post callback must return SUCCESS or CALLBACK_BYPASS");
    return Finish(N.Status);
  }
  if (N.API == "ZwClose" && Status)
    return registryError(
        "non-success close notification return is not modeled");
  if (Status == CallbackBypass) {
    if (Open)
      return registryError(
          "open/create bypass requires a modeled replacement key object");
    return Finish(0); // The callback supplied any required caller outputs.
  }
  if (Status & profile::NTStatusFailureMask)
    return Finish(Status); // No registry effects or post-notification.
  auto Result = Registry.call(N.API, N.Arguments);
  if (!Result)
    return Result.takeError();
  N.Status = uint32_t(*Result);
  if (Open && !N.Status) {
    auto Handle = Memory.readInteger(N.Arguments[0], 8);
    if (!Handle)
      return Handle.takeError();
    auto Key = Registry.keyIdentity(*Handle);
    if (!Key)
      return registryError("successful open lost its key identity");
    auto Object = registryKeyObject(*Key);
    if (!Object)
      return Object.takeError();
    N.Object = *Object;
    if (auto E = Memory.writeInteger(N.Storage + 224, *Object, 8))
      return E;
  }
  auto Context = Memory.readInteger(N.Storage + N.ContextOffset, 8);
  if (!Context)
    return Context.takeError();
  std::vector<uint8_t> Post(56);
  llvm::support::endian::write64le(Post.data(), N.Status ? 0 : N.Object);
  llvm::support::endian::write32le(Post.data() + 8, N.Status);
  llvm::support::endian::write64le(Post.data() + 16, N.Storage);
  llvm::support::endian::write32le(Post.data() + 24, N.Status);
  llvm::support::endian::write64le(Post.data() + 32, *Context);
  if (auto E = Memory.write(N.Storage + PostOffset, Post))
    return E;
  N.Post = true;
  PendingRegistryCallback = KernelGuestCall{
      {GuestCallOwner::RegistryCallback, ID},
      RegistryCallback->Routine,
      {RegistryCallback->Context, N.PostClass, N.Storage + PostOffset},
      0};
  return std::optional<uint64_t>{};
}

llvm::Error KernelModel::validateRegistryCallbackAccess(uint64_t Address,
                                                        uint32_t Size,
                                                        bool IsWrite) const {
  for (const auto &[Identity, Object] : RegistryKeyObjects)
    if (overlap(Address, Size, Object, 8))
      return registryError("guest access to an opaque registry key object");
  for (const auto &[ID, N] : RegistryNotifications) {
    if (IsWrite && (N.API == "ZwOpenKey" || N.API == "ZwCreateKey") &&
        overlap(Address, Size, N.Arguments[2], 48))
      return registryError(
          "mutation of captured object attributes is not modeled");
    if (!overlap(Address, Size, N.Storage, StorageSize))
      continue;
    if (!N.Active || N.Thread != CurrentThreadKey || Address < N.Storage ||
        Size > StorageSize || Address - N.Storage > StorageSize - Size)
      return registryError(
          "notification storage is borrowed by its callback thread only");
    for (uint64_t I = Address - N.Storage; I < Address - N.Storage + Size;
         ++I) {
      bool Allowed = false;
      if (IsWrite)
        Allowed = !N.Post ? (I >= N.ContextOffset && I < N.ContextOffset + 8)
                          : (I >= PostOffset + 24 && I < PostOffset + 28);
      else if (N.PreSize == 136 && I < N.PreSize)
        Allowed = I < 16 || (I >= 24 && I < 28) || (I >= 32 && I < 60) ||
                  (I >= 64 && I <= 128);
      else if (N.PreSize != 136 && I < N.PreSize - 8) {
        Allowed = true;
        if (N.API == "ZwSetValueKey")
          Allowed = I < 36 || I >= 40;
        else if (N.API == "ZwQueryValueKey")
          Allowed = I < 20 || (I >= 24 && I < 36) || I >= 40;
      } else if (N.Post && I >= PostOffset && I < PostOffset + 48)
        Allowed = (I < PostOffset + 8 && !N.Status) ||
                  (I >= PostOffset + 8 && I < PostOffset + 12) ||
                  (I >= PostOffset + 16 && I < PostOffset + 28) ||
                  I >= PostOffset + 32;
      else if (N.PreSize == 136)
        Allowed = (N.Post && I >= 224 && I < 232) || (I >= 232 && I < 236) ||
                  (I >= 240 && I < 248);
      if (!Allowed)
        return registryError(
            "access to opaque or read-only notification fields");
    }
  }
  return llvm::Error::success();
}
llvm::Error
KernelModel::canReleaseRegistryCallbackStorage(uint64_t Address,
                                               uint64_t Size) const {
  if (RegistryCallback && (overlap(Address, Size, RegistryCallback->Routine) ||
                           (RegistryCallback->Context &&
                            overlap(Address, Size, RegistryCallback->Context))))
    return registryError(
        "cannot release registered routine or context storage");
  for (const auto &[ID, N] : RegistryNotifications)
    if (overlap(Address, Size, N.Storage, StorageSize) ||
        ((N.API == "ZwOpenKey" || N.API == "ZwCreateKey") &&
         overlap(Address, Size, N.Arguments[2], 48)))
      return registryError("cannot release an active notification");
  return llvm::Error::success();
}
llvm::Error KernelModel::validateRegistryCallbackUnload() const {
  if (RegistryCallback || !RegistryNotifications.empty() ||
      PendingRegistryCallback)
    return registryError(
        "unload returned with registered or executing registry callbacks");
  return llvm::Error::success();
}
} // namespace neverd::emulation
