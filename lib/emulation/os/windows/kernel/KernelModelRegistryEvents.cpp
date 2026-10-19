//===- KernelModelRegistryEvents.cpp - Scenario registry operation driver -===//
// NeverD Decompiler
///
/// \file
/// Scenario registry operation driver.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "registry event: " + T);
}
} // namespace
llvm::Expected<uint64_t>
KernelModel::beginRegistryEvent(const DriverRegistryEvent &E, size_t Index) {
  using Kind = DriverRegistryEvent::Kind;
  if (ActiveRegistryEvent || !Registry.isConfigured() || CurrentIRQL ||
      handleProcessID() != E.ActorProcessID ||
      !isLiveProcess(E.ActorProcessID) || hasPendingModelGuestCall() ||
      PendingWait)
    return invalid("requires its live actor at PASSIVE_LEVEL");
  RegistryEventState State;
  State.Index = Result.RegistryEvents.size();
  State.Thread = CurrentThreadKey;
  auto Reserve = [&](size_t Size) -> llvm::Expected<uint64_t> {
    auto A = allocate(Size);
    if (A)
      State.Storage.emplace_back(*A, Size);
    return A;
  };
  auto String = [&](llvm::StringRef Text) -> llvm::Expected<uint64_t> {
    auto A = makeUnicodeString(Text.str());
    if (A)
      State.Storage.emplace_back(*A, 16 + (Text.size() + 1) * 2);
    return A;
  };
  auto Control = Reserve(64);
  if (!Control)
    return Control.takeError();
  State.Control = *Control;
  std::vector<uint64_t> Arguments;
  llvm::StringRef API;
  if (E.Operation == Kind::Open) {
    auto Path = String(E.Path);
    if (!Path)
      return Path.takeError();
    if (auto Err = Memory.writeInteger(*Control + 16, 48, 4))
      return Err;
    if (E.RootHandle) {
      auto Root = RegistryEventHandles.find(E.RootHandle);
      // A failed earlier open must remain an invalid nonnull root handle;
      // the canonical registry owner supplies STATUS_INVALID_HANDLE.
      const auto Handle =
          Root == RegistryEventHandles.end() ? UINT64_MAX : Root->second;
      if (auto Err = Memory.writeInteger(*Control + 24, Handle, 8))
        return Err;
    }
    if (auto Err = Memory.writeInteger(*Control + 32, *Path, 8))
      return Err;
    // OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE; no host or user access check.
    if (auto Err = Memory.writeInteger(*Control + 40, 0x240, 4))
      return Err;
    API = "ZwOpenKey";
    Arguments = {*Control, E.DesiredAccess, *Control + 16};
  } else {
    auto H = RegistryEventHandles.find(E.Handle);
    const uint64_t Handle = H == RegistryEventHandles.end() ? 0 : H->second;
    if (E.Operation == Kind::Close) {
      API = "ZwClose";
      Arguments = {Handle};
    } else {
      auto Name = String(E.ValueName);
      if (!Name)
        return Name.takeError();
      if (E.Operation == Kind::SetValue) {
        uint64_t Data = 0;
        if (!E.Data.empty()) {
          auto A = Reserve(E.Data.size());
          if (!A)
            return A.takeError();
          Data = *A;
          if (auto Err = Memory.write(Data, E.Data))
            return Err;
        }
        API = "ZwSetValueKey";
        Arguments = {Handle, *Name, 0, *E.ValueType, Data, E.Data.size()};
      } else if (E.Operation == Kind::QueryValue) {
        if (E.OutputSize) {
          auto A = Reserve(E.OutputSize);
          if (!A)
            return A.takeError();
          State.Output = *A;
        }
        API = "ZwQueryValueKey";
        Arguments = {Handle,       *Name,        2,
                     State.Output, E.OutputSize, *Control + 8};
      } else
        return invalid("unsupported operation");
    }
  }
  DriverRegistryEventResult Observation;
  Observation.Event = E;
  Observation.SourceIndex = Index;
  Result.RegistryEvents.push_back(std::move(Observation));
  ActiveRegistryEvent = std::move(State);
  // The same registry owner handles actual guest Zw calls and external events.
  // No direct snapshot mutation or fabricated callback invocation is involved.
  return registryOperation(API, Arguments);
}
llvm::Error KernelModel::finishRegistryEvent(uint64_t Status) {
  using Kind = DriverRegistryEvent::Kind;
  if (!ActiveRegistryEvent || ActiveRegistryEvent->Thread != CurrentThreadKey ||
      CurrentIRQL || PendingRegistryCallback || !RegistryNotifications.empty())
    return invalid("completion lost its actor or has an unfinished filter");
  const auto &S = *ActiveRegistryEvent;
  auto &R = Result.RegistryEvents.at(S.Index);
  if (R.Completed)
    return invalid("operation already completed");
  if (!uint32_t(Status)) {
    if (R.Event.Operation == Kind::Open) {
      auto H = Memory.readInteger(S.Control, 8);
      if (!H)
        return H.takeError();
      if (!Registry.ownsHandle(*H))
        return invalid("successful open lost its canonical key handle");
      RegistryEventHandles.emplace(R.Event.Handle, *H);
    } else if (R.Event.Operation == Kind::Close)
      RegistryEventHandles.erase(R.Event.Handle);
    else if (R.Event.Operation == Kind::QueryValue) {
      auto Size = Memory.readInteger(S.Control + 8, 4);
      if (!Size)
        return Size.takeError();
      if (*Size < 12 || *Size > R.Event.OutputSize)
        return invalid(
            "successful query returned an invalid partial-information size");
      R.Output.resize(*Size);
      if (auto Err = Memory.read(S.Output, R.Output))
        return Err;
      if (llvm::support::endian::read32le(R.Output.data() + 8) != *Size - 12)
        return invalid(
            "partial-information data length differs from result length");
    }
  }
  if (R.Event.Operation == Kind::QueryValue &&
      (!uint32_t(Status) || uint32_t(Status) == 0xc0000023 ||
       uint32_t(Status) == 0x80000005)) {
    auto Size = Memory.readInteger(S.Control + 8, 4);
    if (!Size)
      return Size.takeError();
    R.RequiredSize = uint32_t(*Size);
  }
  for (const auto &[Address, Size] : S.Storage) {
    if (auto Err = prepareReleaseRange(Address, Size))
      return Err;
    FreedRanges.emplace(Address, Size);
  }
  R.Status = uint32_t(Status);
  R.Completed = true;
  ActiveRegistryEvent.reset();
  return llvm::Error::success();
}
} // namespace neverd::emulation
