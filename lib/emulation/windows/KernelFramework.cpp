//===- KernelFramework.cpp - KMDF binding and object lifetimes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original guest model of the public KMDF 1.33 ABI. Bind layouts and function
/// indices follow Microsoft's fxldr.h, wdfglobals.h and wdffuncenum.h. Object
/// lifetime follows the documented cleanup/reference/destroy contract:
/// https://learn.microsoft.com/windows-hardware/drivers/wdf/framework-object-life-cycle
/// The model never overlays host WDF records or executes host callback
/// pointers.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include "KernelResources.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace framework;
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

template <typename... Errors> llvm::Error joinedErrors(Errors... Values) {
  llvm::Error Result = llvm::Error::success();
  ((Result = llvm::joinErrors(std::move(Result), std::move(Values))), ...);
  return Result;
}

bool overlaps(uint64_t A, uint64_t Size, uint64_t B, uint64_t OtherSize) {
  return A < B + OtherSize && B < A + Size;
}
} // namespace

void KernelFramework::configure(uint64_t NewDriver, uint64_t NewRegistryPath,
                                std::string NewServiceName) {
  Driver = NewDriver;
  RegistryPath = NewRegistryPath;
  ServiceName = std::move(NewServiceName);
}

bool KernelFramework::hasPnpDriver() const {
  return std::any_of(Bindings.begin(), Bindings.end(), [](const auto &Entry) {
    const auto &B = Entry.second;
    return !B.Unbound && !B.Unloaded && B.DriverHandle && B.AddDeviceCallback;
  });
}

llvm::Expected<KernelFramework::PnpAddDevice>
KernelFramework::beginPnpAddDevice(uint64_t PDO) {
  if (!PDO || PnpDeviceHandles.count(PDO) ||
      std::any_of(DeviceInits.begin(), DeviceInits.end(),
                  [&](const auto &Entry) {
                    return Entry.second.Kind == DeviceInitKind::Pnp &&
                           Entry.second.PDO == PDO;
                  }))
    return invalid("PnP AddDevice requires a new physical device");
  auto B =
      std::find_if(Bindings.begin(), Bindings.end(), [](const auto &Entry) {
        const auto &Binding = Entry.second;
        return !Binding.Unbound && !Binding.Unloaded && Binding.DriverHandle &&
               Binding.AddDeviceCallback;
      });
  if (B == Bindings.end())
    return invalid("PnP AddDevice requires a live framework driver");
  auto Init = allocate(HandleSize, false, true);
  if (!Init)
    return Init.takeError();
  DeviceInits.emplace(*Init,
                      DeviceInit{B->second.Globals, DeviceInitKind::Pnp, PDO});
  return PnpAddDevice{B->second.AddDeviceCallback, B->second.DriverHandle,
                      *Init};
}

llvm::Error KernelFramework::finishPnpAddDevice(uint64_t PDO, uint64_t Init,
                                                uint32_t Status) {
  auto I = DeviceInits.find(Init);
  if (I != DeviceInits.end()) {
    if (I->second.Kind != DeviceInitKind::Pnp || I->second.PDO != PDO)
      return invalid("PnP AddDevice lost its framework initializer");
    if (auto E = retire(Init))
      return E;
    DeviceInits.erase(I);
  }
  auto Device = PnpDeviceHandles.find(PDO);
  if (!Status) {
    if (Device == PnpDeviceHandles.end())
      return invalid("successful PnP AddDevice did not create an FDO");
    auto &FDO = Devices.at(Device->second);
    if (!DevicesHost.FinishInitializing)
      return invalid("PnP device initialization host is unavailable");
    if (auto E = DevicesHost.FinishInitializing(FDO.Wdm))
      return E;
    FDO.Initialized = true;
    return llvm::Error::success();
  }
  if (Device == PnpDeviceHandles.end())
    return llvm::Error::success();
  std::vector<Step> Steps;
  if (auto E = planDelete(Device->second, Steps))
    return E;
  auto Started = start(std::move(Steps));
  if (!Started)
    return Started.takeError();
  return llvm::Error::success();
}

llvm::Error KernelFramework::removePnpDevice(uint64_t PDO) {
  auto Device = PnpDeviceHandles.find(PDO);
  if (Device == PnpDeviceHandles.end())
    return invalid("PnP removal has no live framework device");
  std::vector<Step> Steps;
  if (auto E = planDelete(Device->second, Steps))
    return E;
  auto Started = start(std::move(Steps));
  if (!Started)
    return Started.takeError();
  return llvm::Error::success();
}

llvm::Expected<bool> KernelFramework::beginPnpPowerTransition(
    uint64_t PDO, uint64_t IRP, DevicePnpRequest Minor, uint64_t RawResources,
    uint64_t TranslatedResources, uint64_t ResourceListSize) {
  auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return invalid("PnP power transition lost its framework device");
  auto Object = Objects.find(Handle->second);
  auto Device = Devices.find(Handle->second);
  if (Object == Objects.end() || Device == Devices.end() ||
      Object->second.Deleting || !Device->second.Initialized)
    return invalid("PnP power transition requires a live initialized device");
  if (PendingCall || !PnpTransitions.empty() || CompletedPnp)
    return invalid("another framework callback is still pending");

  const bool Entering = Minor == DevicePnpRequest::Start;
  const bool Leaving = Minor == DevicePnpRequest::Stop ||
                       Minor == DevicePnpRequest::Remove ||
                       Minor == DevicePnpRequest::SurpriseRemoval;
  if (!Entering && !Leaving)
    return false;
  auto &D = Device->second;
  if (Entering && (D.InD0 || D.HardwarePrepared))
    return invalid("PnP START reached an already prepared device");
  if (Leaving && !D.InD0 && !D.HardwarePrepared)
    return false;
  PnpTransition Transition{IRP, Handle->second, Entering,
                           Minor == DevicePnpRequest::Remove ||
                               Minor == DevicePnpRequest::SurpriseRemoval};
  std::vector<PnpStep> Resumes;
  for (const auto &[QueueHandle, Queue] : Queues) {
    if (Queue.Device != Handle->second || !Queue.PowerManaged)
      continue;
    for (const auto &[RequestHandle, Request] : Requests) {
      if (Request.Queue != QueueHandle || Request.Completed)
        continue;
      if (Entering && Request.PowerSuspended) {
        if (!Queue.IoResume)
          return invalid("suspended request has no EvtIoResume callback");
        Resumes.push_back({PnpPhase::IoResume, QueueHandle, RequestHandle});
      }
      if (!Leaving || Request.Queued)
        continue;
      if (Request.PowerSuspended)
        return invalid("power transition found an already suspended request");
      if (Request.InCallerContext)
        return invalid("power transition found a caller-context request "
                       "outside queue ownership");
      if (!Queue.IoStop) {
        Transition.WaitingRequests.insert(RequestHandle);
        continue;
      }
      Transition.Remaining.push_back(
          {PnpPhase::IoStop, QueueHandle, RequestHandle});
    }
  }
  if (Leaving)
    D.PowerQueuesHeld = true;

  if (Entering) {
    if (D.PrepareHardware)
      Transition.Remaining.push_back({PnpPhase::PrepareHardware});
    if (D.D0Entry)
      Transition.Remaining.push_back({PnpPhase::D0Entry});
    Transition.Remaining.insert(Transition.Remaining.end(), Resumes.begin(),
                                Resumes.end());
  } else {
    if (D.InD0 && D.D0Exit)
      Transition.Remaining.push_back({PnpPhase::D0Exit});
    if (D.HardwarePrepared && D.ReleaseHardware)
      Transition.Remaining.push_back({PnpPhase::ReleaseHardware});
  }
  if ((!Transition.Remaining.empty() || !Transition.WaitingRequests.empty()) &&
      NextContinuation == UINT64_MAX)
    return invalid("framework callback identity exhausted");

  if (Entering && (D.PrepareHardware || D.ReleaseHardware)) {
    if (D.RawResources.Handle || D.TranslatedResources.Handle)
      return invalid("previous hardware resource lists remain live");
    auto Raw = createResourceList(RawResources, ResourceListSize);
    if (!Raw)
      return Raw.takeError();
    auto Translated = createResourceList(TranslatedResources, ResourceListSize);
    if (!Translated) {
      auto E = Translated.takeError();
      return llvm::joinErrors(std::move(E), retireResourceList(*Raw));
    }
    if (Raw->Count != Translated->Count) {
      auto E = invalid("raw and translated resource counts differ");
      return joinedErrors(std::move(E), retireResourceList(*Raw),
                          retireResourceList(*Translated));
    }
    D.RawResources = *Raw;
    D.TranslatedResources = *Translated;
    D.ResourcesActive = true;
  }
  if (Transition.Remaining.empty() && Transition.WaitingRequests.empty()) {
    D.HardwarePrepared = Entering;
    D.InD0 = Entering;
    D.PowerQueuesHeld = !Entering;
    if (!Entering) {
      D.ResourcesActive = false;
      if (auto E = retireResourceLists(D))
        return std::move(E);
    }
    if (Entering) {
      std::vector<Step> Steps;
      appendPowerQueuePresentations(Handle->second, Steps);
      if (!Steps.empty()) {
        if (NextContinuation == UINT64_MAX)
          return invalid("framework callback identity exhausted");
        const uint64_t Token = NextContinuation++;
        Transition.CallbacksComplete = true;
        Continuations.emplace(Token, Continuation{std::move(Steps)});
        PnpTransitions.emplace(Token, std::move(Transition));
        auto Next = advance(Token);
        if (!Next)
          return Next.takeError();
        if (!Next->has_value())
          return true;
        PnpTransitions.erase(Token);
      }
    }
    return false;
  }
  const uint64_t Token = NextContinuation++;
  Continuations.emplace(Token, Continuation{});
  PnpTransitions.emplace(Token, std::move(Transition));
  auto &Active = PnpTransitions.at(Token);
  if (!Active.WaitingRequests.empty() &&
      (Active.Remaining.empty() ||
       Active.Remaining.front().Phase != PnpPhase::IoStop)) {
    Active.WaitingForRequests = true;
    return true;
  }
  if (auto E = schedulePnpCallback(Token))
    return E;
  return true;
}

void KernelFramework::appendPowerQueuePresentations(
    uint64_t Device, std::vector<Step> &Steps) const {
  for (const auto &[Handle, Queue] : Queues)
    if (Queue.Device == Device && Queue.PowerManaged &&
        Queue.Dispatch != QueueDispatchManual)
      for (size_t I = 0; I < Queue.Pending.size(); ++I)
        Steps.push_back({StepKind::PresentQueue, Handle});
}

llvm::Error KernelFramework::finalizePnpCallbacks(uint64_t Token) {
  auto Transition = PnpTransitions.find(Token);
  if (Transition == PnpTransitions.end() ||
      Transition->second.CallbacksComplete)
    return invalid("PnP transition lost its callback state");
  auto Device = Devices.find(Transition->second.Device);
  if (Device == Devices.end())
    return invalid("PnP transition lost its device");
  const bool Ready =
      Transition->second.Entering &&
      !(Transition->second.Status & profile::NTStatusFailureMask);
  Device->second.HardwarePrepared = Ready;
  Device->second.InD0 = Ready;
  Device->second.PowerQueuesHeld = !Ready;
  if (Ready)
    appendPowerQueuePresentations(Transition->second.Device,
                                  Continuations.at(Token).Steps);
  else {
    Device->second.ResourcesActive = false;
    if (auto E = retireResourceLists(Device->second))
      return E;
  }
  Transition->second.CallbacksComplete = true;
  return llvm::Error::success();
}

llvm::Error KernelFramework::resumePausedPnp() {
  if (PendingCall)
    return llvm::Error::success();
  for (auto Transition = PnpTransitions.begin();
       Transition != PnpTransitions.end(); ++Transition) {
    auto &State = Transition->second;
    if (!State.WaitingForRequests || !State.WaitingRequests.empty())
      continue;
    State.WaitingForRequests = false;
    const uint64_t Token = Transition->first;
    if (!State.Remaining.empty())
      return schedulePnpCallback(Token);
    if (auto E = finalizePnpCallbacks(Token))
      return E;
    auto Next = advance(Token);
    if (!Next)
      return Next.takeError();
    if (Next->has_value()) {
      CompletedPnp = PnpCompletion{State.IRP, State.Status};
      PnpTransitions.erase(Transition);
    }
    return llvm::Error::success();
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::schedulePnpCallback(uint64_t Token) {
  auto &Transition = PnpTransitions.at(Token);
  if (Transition.Remaining.empty())
    return invalid("PnP transition has no remaining callback");
  auto &D = Devices.at(Transition.Device);
  Transition.Current = Transition.Remaining.front();
  Transition.Remaining.pop_front();
  uint64_t Callback = 0;
  std::vector<uint64_t> Arguments{Transition.Device};
  switch (Transition.Current.Phase) {
  case PnpPhase::IoStop: {
    auto Queue = Queues.find(Transition.Current.Queue);
    auto Request = Requests.find(Transition.Current.Request);
    if (Queue == Queues.end() || Request == Requests.end() ||
        Request->second.Completed || Request->second.Queued)
      return invalid("I/O stop callback lost its driver-owned request");
    Callback = Queue->second.IoStop;
    uint64_t Flags =
        Transition.Removing ? RequestStopActionPurge : RequestStopActionSuspend;
    if (Request->second.Cancellation == CancelState::Marked)
      Flags |= RequestStopRequestCancelable;
    Arguments = {Transition.Current.Queue, Transition.Current.Request, Flags};
    break;
  }
  case PnpPhase::IoResume: {
    auto Queue = Queues.find(Transition.Current.Queue);
    auto Request = Requests.find(Transition.Current.Request);
    if (Queue == Queues.end() || Request == Requests.end() ||
        !Request->second.PowerSuspended || Request->second.Completed)
      return invalid("I/O resume callback lost its suspended request");
    Callback = Queue->second.IoResume;
    Arguments = {Transition.Current.Queue, Transition.Current.Request};
    break;
  }
  case PnpPhase::PrepareHardware:
    Callback = D.PrepareHardware;
    Arguments.push_back(D.RawResources.Handle);
    Arguments.push_back(D.TranslatedResources.Handle);
    break;
  case PnpPhase::D0Entry:
    Callback = D.D0Entry;
    Arguments.push_back(PowerDeviceD3Final);
    break;
  case PnpPhase::D0Exit:
    Callback = D.D0Exit;
    Arguments.push_back(PowerDeviceD3Final);
    break;
  case PnpPhase::ReleaseHardware:
    Callback = D.ReleaseHardware;
    Arguments.push_back(D.TranslatedResources.Handle);
    break;
  }
  if (!Callback)
    return invalid("PnP transition lost its registered callback");
  PendingCall = GuestCall{Token, Callback, std::move(Arguments)};
  return llvm::Error::success();
}

std::optional<KernelFramework::PnpCompletion>
KernelFramework::takePnpCompletion() {
  return std::exchange(CompletedPnp, std::nullopt);
}

std::optional<unsigned>
KernelFramework::argumentCount(const KernelExportRegistry::Export &Export) {
  if (Export.Kind == KernelExportRegistry::ExportKind::FrameworkFunction) {
    if (Export.Name == FrameworkUnloadRoutine)
      return 1;
#define NEVERD_FRAMEWORK_API(Symbol, Count)                                    \
  if (Export.Name == #Symbol)                                                  \
    return Count;
#include "KernelFrameworkAPIs.def"
#undef NEVERD_FRAMEWORK_API
    return std::nullopt;
  }
  if (Export.Module == FrameworkLoaderProvider) {
#define NEVERD_FRAMEWORK_LOADER_API(Symbol, Count)                             \
  if (Export.Name == #Symbol)                                                  \
    return Count;
#include "KernelFrameworkLoaderAPIs.def"
#undef NEVERD_FRAMEWORK_LOADER_API
  }
  return std::nullopt;
}

llvm::Expected<uint64_t> KernelFramework::read(uint64_t Address,
                                               unsigned Width) {
  if (auto E = ValidateAccess(Address, Width, false))
    return E;
  return Memory.readInteger(Address, Width);
}

llvm::Error KernelFramework::writable(uint64_t Address, uint32_t Size) {
  if (!Address)
    return invalid("null output pointer");
  return ValidateAccess(Address, Size, true);
}

llvm::Expected<std::vector<uint8_t>>
KernelFramework::readRegistryPath(uint64_t Address) {
  auto Length = read(Address, sizeof(char16_t));
  auto Maximum =
      read(Address + windows::UnicodeMaximumOffset, sizeof(char16_t));
  auto Buffer = read(Address + windows::UnicodeBufferOffset);
  if (!Length || !Maximum || !Buffer)
    return joinedErrors(Length.takeError(), Maximum.takeError(),
                        Buffer.takeError());
  if (!*Length || *Length % sizeof(char16_t) || *Maximum < *Length ||
      *Length > MaxRegistryPathBytes)
    return invalid("invalid counted driver registry path");
  if (auto E = ValidateAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  return Bytes;
}

llvm::Expected<uint64_t> KernelFramework::allocate(uint64_t Size, bool Writable,
                                                   bool Opaque) {
  auto Address = AllocateStorage(Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return invalid("framework storage budget exhausted");
  if (auto E = Memory.write(*Address, std::vector<uint8_t>(Size)))
    return E;
  Regions.emplace(*Address, Region{Size, Writable, Opaque});
  return *Address;
}

llvm::Error KernelFramework::retire(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Regions.find(Address);
  if (I == Regions.end() || I->second.Freed)
    return invalid("lost framework allocation ownership");
  if (auto E = ReleaseStorage(Address, I->second.Size))
    return E;
  I->second.Freed = true;
  return llvm::Error::success();
}

llvm::Expected<KernelFramework::ResourceList>
KernelFramework::createResourceList(uint64_t Source, uint64_t Size) {
  if (bool(Source) != bool(Size))
    return invalid("hardware resource list address and size disagree");
  uint32_t Count = 0;
  std::vector<uint8_t> Descriptors;
  if (Source) {
    if (Size < resources::ResourceHeaderSize ||
        (Size - resources::ResourceHeaderSize) %
            resources::ResourceDescriptorSize)
      return invalid("invalid hardware resource list size");
    auto FullCount = read(Source + resources::ResourceCountOffset,
                          resources::ResourceCountFieldSize);
    auto PartialCount = read(Source + resources::ResourcePartialCountOffset,
                             resources::ResourceCountFieldSize);
    if (!FullCount || !PartialCount)
      return joinedErrors(FullCount.takeError(), PartialCount.takeError());
    const uint64_t Expected = (Size - resources::ResourceHeaderSize) /
                              resources::ResourceDescriptorSize;
    if (*FullCount != resources::SupportedFullDescriptorCount ||
        *PartialCount != Expected ||
        Expected > std::numeric_limits<uint32_t>::max())
      return invalid("unsupported hardware resource list layout");
    Count = uint32_t(Expected);
    Descriptors.resize(Count * resources::ResourceDescriptorSize);
    if (auto E =
            Memory.read(Source + resources::ResourceHeaderSize, Descriptors))
      return std::move(E);
  }
  auto Handle = allocate(HandleSize, false, true);
  if (!Handle)
    return Handle.takeError();
  uint64_t Address = 0;
  if (!Descriptors.empty()) {
    auto Region = allocate(Descriptors.size(), false, false);
    if (!Region)
      return llvm::joinErrors(Region.takeError(), retire(*Handle));
    Address = *Region;
    if (auto E = Memory.write(Address, Descriptors))
      return joinedErrors(std::move(E), retire(Address), retire(*Handle));
  }
  return ResourceList{*Handle, Address, Count};
}

llvm::Error KernelFramework::retireResourceList(ResourceList &List) {
  auto E = llvm::joinErrors(retire(List.Descriptors), retire(List.Handle));
  List = {};
  return E;
}

llvm::Error KernelFramework::retireResourceLists(Device &D) {
  return llvm::joinErrors(retireResourceList(D.RawResources),
                          retireResourceList(D.TranslatedResources));
}

llvm::Error KernelFramework::validateGuestAccess(uint64_t Address,
                                                 uint32_t Size,
                                                 bool IsWrite) const {
  if (Size > UINT64_MAX - Address)
    return invalid("overflowing framework memory access");
  for (const auto &[Base, Region] : Regions) {
    if (!Size || !overlaps(Address, Size, Base, Region.Size))
      continue;
    if (Region.Freed)
      return invalid("guest access to freed framework storage");
    if (Region.Opaque || (IsWrite && !Region.Writable))
      return invalid("guest access to opaque or read-only framework storage");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelFramework::bind(llvm::ArrayRef<uint64_t> A) {
  if (A[0] != Driver)
    return invalid("binding requires this DriverEntry's driver object");
  auto Path = readRegistryPath(A[1]);
  auto EntryPath = readRegistryPath(RegistryPath);
  if (!Path || !EntryPath)
    return joinedErrors(Path.takeError(), EntryPath.takeError());
  if (*Path != *EntryPath)
    return invalid("binding registry path does not identify this driver");
  for (const auto &[Address, B] : Bindings)
    if (!B.Unbound)
      return invalid("driver already has a live framework binding");
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != BindV1Size && *Size != BindV2Size)
    return invalid("unsupported WDF_BIND_INFO size");
  if (auto E = ValidateAccess(A[2], *Size, false))
    return E;
  auto Component = read(A[2] + BindComponent);
  auto Major = read(A[2] + BindMajor, 4);
  auto Minor = read(A[2] + BindMinor, 4);
  auto Build = read(A[2] + BindBuild, 4);
  auto Count = read(A[2] + BindCount, 4);
  auto TableSlot = read(A[2] + BindTable);
  auto Module = read(A[2] + BindModule);
  if (!Component || !Major || !Minor || !Build || !Count || !TableSlot ||
      !Module)
    return joinedErrors(Component.takeError(), Major.takeError(),
                        Minor.takeError(), Build.takeError(), Count.takeError(),
                        TableSlot.takeError(), Module.takeError());
  if (*Major != MajorVersion || *Minor != MinorVersion ||
      *Build != BuildVersion || *Count != FunctionCount)
    return invalid(llvm::formatv("unsupported framework version or function "
                                 "count; expected {0}.{1}.{2}/{3}",
                                 MajorVersion, MinorVersion, BuildVersion,
                                 FunctionCount)
                       .str());
  if (*Module)
    return invalid("binding record already contains a module identity");
  for (size_t I = 0; I <= FrameworkComponent.size(); ++I) {
    auto Ch = read(*Component + sizeof(char16_t) * I, sizeof(char16_t));
    if (!Ch)
      return Ch.takeError();
    const uint64_t Expected =
        I == FrameworkComponent.size() ? 0 : uint8_t(FrameworkComponent[I]);
    if (*Ch != Expected)
      return invalid("unsupported framework component name");
  }
  uint64_t Higher = 0;
  if (*Size == BindV2Size) {
    auto MinimumSlot = read(A[2] + BindMinimum);
    auto HigherSlot = read(A[2] + BindHigher);
    if (!MinimumSlot || !HigherSlot)
      return joinedErrors(MinimumSlot.takeError(), HigherSlot.takeError());
    auto Minimum = read(*MinimumSlot, 4);
    if (!Minimum)
      return Minimum.takeError();
    if (*Minimum < 25 || *Minimum > MinorVersion)
      return invalid("unsupported minimum framework version");
    Higher = *HigherSlot;
    if (auto E = writable(Higher, 1))
      return E;
    // Count/structure globals are consulted only when the client is newer
    // than its provider. This profile provides the exact target version;
    // keep the client's already initialized unused availability data intact.
  }
  std::vector<std::pair<uint64_t, uint32_t>> Outputs{
      {A[3], 8}, {*TableSlot, 8}, {A[2] + BindModule, 8}};
  if (Higher)
    Outputs.emplace_back(Higher, 1);
  for (size_t I = 0; I < Outputs.size(); ++I) {
    const auto [Slot, Width] = Outputs[I];
    if (auto E = writable(Slot, Width))
      return E;
    if (Slot != A[2] + BindModule && overlaps(Slot, Width, A[2], *Size))
      return invalid("binding output overlaps its input record");
    for (size_t J = 0; J < I; ++J)
      if (overlaps(Slot, Width, Outputs[J].first, Outputs[J].second))
        return invalid("binding output slots overlap");
  }
  // Imports have already been bound. Reserve enough identities for the exact
  // function table and its eventual unload bridge before allocating anything.
  if (Exports.availableThunkCount() < FunctionCount + 1)
    return invalid("binding exceeds the remaining framework thunk capacity");
  auto Globals = allocate(GlobalsSize, false, false);
  if (!Globals)
    return Globals.takeError();
  auto Table = allocate(FunctionCount * 8, false, false);
  if (!Table)
    return joinedErrors(Table.takeError(), retire(*Globals));
  auto ModuleToken = allocate(HandleSize, false, true);
  if (!ModuleToken)
    return joinedErrors(ModuleToken.takeError(), retire(*Globals),
                        retire(*Table));
  struct Function {
    const char *Name;
    unsigned Index;
  };
  static constexpr Function Functions[] = {
#define NEVERD_FRAMEWORK_FUNCTION(Name, Index) {#Name, Index},
#include "KernelFrameworkFunctions.def"
#undef NEVERD_FRAMEWORK_FUNCTION
  };
  static_assert(std::size(Functions) == FunctionCount);
  static_assert(
      [] {
        for (size_t I = 0; I < std::size(Functions); ++I)
          if (Functions[I].Index != I)
            return false;
        return true;
      }(),
      "framework function inventory must cover every slot exactly once");
  for (const auto &Function : Functions) {
    auto Thunk = Exports.insertFrameworkFunction(*Globals, Function.Name);
    if (!Thunk)
      return Thunk.takeError();
    if (auto E = Memory.writeInteger(*Table + Function.Index * 8, *Thunk, 8))
      return E;
  }
  if (auto E = Memory.writeInteger(*TableSlot, *Table, 8))
    return E;
  if (auto E = Memory.writeInteger(A[3], *Globals, 8))
    return E;
  if (auto E = Memory.writeInteger(A[2] + BindModule, *ModuleToken, 8))
    return E;
  if (Higher)
    if (auto E = Memory.writeInteger(Higher, 0, 1))
      return E;
  Binding B;
  B.Info = A[2];
  B.Globals = *Globals;
  B.Table = *Table;
  B.Module = *ModuleToken;
  B.RegistryBytes = std::move(*Path);
  Bindings.emplace(*Globals, std::move(B));
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::unbind(llvm::ArrayRef<uint64_t> A) {
  auto I = Bindings.find(A[2]);
  if (I == Bindings.end() || I->second.Unbound || I->second.Info != A[1])
    return invalid("unbind requires the exact live binding and globals");
  auto &B = I->second;
  auto Path = readRegistryPath(A[0]);
  if (!Path)
    return Path.takeError();
  if (*Path != B.RegistryBytes)
    return invalid("unbind registry path does not identify this driver");
  if (B.Unbinding)
    return invalid("framework binding unregistration is already in progress");
  if (B.DriverHandle && !B.Unloaded) {
    // FxLibraryCommonUnregisterClient deletes a surviving driver directly.
    // This is the DriverEntry failure path: EvtDriverUnload is never invoked.
    std::vector<Step> Steps;
    if (auto E = planDelete(B.DriverHandle, Steps))
      return E;
    B.Unbinding = true;
    Steps.push_back({StepKind::FinishBindingUnbind, B.Globals});
    return start(std::move(Steps));
  }
  if (auto E = finishUnbind(B))
    return E;
  return 0;
}

llvm::Error KernelFramework::finishUnbind(Binding &B) {
  for (const auto &[Address, Init] : DeviceInits)
    if (Init.Binding == B.Globals)
      return invalid("unbind still owns an unconsumed device initializer");
  if (std::any_of(Objects.begin(), Objects.end(), [&](const auto &Entry) {
        return Entry.second.Binding == B.Globals;
      }))
    return invalid("unbind still owns live framework objects or references");
  for (uint64_t Address : {B.Globals, B.Table, B.Module, B.RegistryCopy})
    if (auto E = retire(Address))
      return E;
  B.Unbound = true;
  B.Unbinding = false;
  return llvm::Error::success();
}

llvm::Expected<KernelFramework::AttributeResult>
KernelFramework::attributes(uint64_t Address, AttributesUse Use) {
  // Match FxValidateObjectAttributes and wdfstatus.h. A documented guest
  // NTSTATUS is separate from an unsupported profile or invalid memory access.
  Attributes A;
  if (!Address) {
    if (Use == AttributesUse::AdditionalContext)
      return uint32_t(ParentNotSpecified);
    return A;
  }
  auto Size = read(Address, 4);
  if (!Size)
    return Size.takeError();
  if (*Size != AttributesSize)
    return uint32_t(InfoLengthMismatch);
  if (auto E = ValidateAccess(Address, AttributesSize, false))
    return E;
  auto Cleanup = read(Address + AttributesCleanup);
  auto Destroy = read(Address + AttributesDestroy);
  auto Execution = read(Address + AttributesExecution, 4);
  auto Synchronization = read(Address + AttributesSynchronization, 4);
  auto Parent = read(Address + AttributesParent);
  auto Override = read(Address + AttributesContextSize);
  auto Type = read(Address + AttributesContextType);
  if (!Cleanup || !Destroy || !Execution || !Synchronization || !Parent ||
      !Override || !Type)
    return joinedErrors(Cleanup.takeError(), Destroy.takeError(),
                        Execution.takeError(), Synchronization.takeError(),
                        Parent.takeError(), Override.takeError(),
                        Type.takeError());
  A.Cleanup = *Cleanup;
  A.Destroy = *Destroy;
  A.Parent = *Parent;
  if (*Type) {
    auto TypeSize = read(*Type, 4);
    if (!TypeSize)
      return TypeSize.takeError();
    if (*TypeSize != ContextTypeSize && *TypeSize != ContextTypeLegacySize)
      return uint32_t(InfoLengthMismatch);
    if (auto E = ValidateAccess(*Type, *TypeSize, false))
      return E;
    auto Name = read(*Type + ContextName);
    auto Bytes = read(*Type + ContextSize);
    if (!Name || !Bytes)
      return joinedErrors(Name.takeError(), Bytes.takeError());
    if (*Bytes && !*Name)
      return uint32_t(ObjectAttributesInvalid);
    A.Type = *Type;
    A.ContextSize = *Bytes;
  }
  if (*Override) {
    if (!*Type || *Override < A.ContextSize)
      return uint32_t(ObjectAttributesInvalid);
    A.ContextSize = *Override;
  }
  if (Use != AttributesUse::Object && *Parent)
    return uint32_t(ParentAssignmentNotAllowed);
  if (!*Execution || *Execution > ExecutionDispatch || !*Synchronization ||
      *Synchronization > SynchronizationNone)
    return uint32_t(ObjectAttributesInvalid);
  if (*Execution != ExecutionInherit && *Execution != ExecutionPassive)
    return invalid("unsupported framework object execution level");
  if (*Synchronization != SynchronizationInherit &&
      *Synchronization != SynchronizationNone)
    return invalid("unsupported framework object synchronization scope");
  if (A.ContextSize > MaxContextSize)
    return invalid("framework context exceeds the storage limit");
  return A;
}

llvm::Expected<uint64_t> KernelFramework::addContext(Object &O,
                                                     const Attributes &A) {
  if (auto I = O.Contexts.find(A.Type); I != O.Contexts.end())
    return I->second.Address;
  if (O.Contexts.size() >= MaxContextsPerObject)
    return invalid("framework context count exceeds the limit");
  uint64_t Storage = 0;
  if (A.ContextSize) {
    auto Allocation = allocate(A.ContextSize, true, false);
    if (!Allocation)
      return Allocation.takeError();
    Storage = *Allocation;
  } else if (A.Type) {
    // WDF permits typed zero-byte contexts and still returns a context address.
    // Preserve that identity without exposing any readable or writable byte.
    auto Identity = allocate(1, false, true);
    if (!Identity)
      return Identity.takeError();
    Storage = *Identity;
  }
  O.Contexts.emplace(A.Type,
                     Context{Storage, A.ContextSize, A.Cleanup, A.Destroy});
  O.ContextOrder.push_back(A.Type);
  return Storage;
}

llvm::Expected<uint64_t> KernelFramework::createObject(uint64_t Globals,
                                                       const Attributes &A,
                                                       bool IsDriver) {
  if (Objects.size() >= MaxObjects)
    return invalid("framework object limit exhausted");
  const uint64_t Parent = IsDriver   ? 0
                          : A.Parent ? A.Parent
                                     : Bindings.at(Globals).DriverHandle;
  if (!IsDriver) {
    auto I = Objects.find(Parent);
    if (I == Objects.end() || I->second.Binding != Globals ||
        I->second.Deleting)
      return invalid("object parent is not a live object in this binding");
  }
  auto Handle = allocate(HandleSize, false, true);
  if (!Handle)
    return Handle.takeError();
  Object O;
  O.Binding = Globals;
  O.Parent = Parent;
  O.Kind = IsDriver ? ObjectKind::Driver : ObjectKind::Generic;
  auto Context = addContext(O, A);
  if (!Context)
    return llvm::joinErrors(Context.takeError(), retire(*Handle));
  Objects.emplace(*Handle, std::move(O));
  if (Parent)
    Objects.at(Parent).Children.push_back(*Handle);
  return *Handle;
}

llvm::Expected<uint64_t>
KernelFramework::createDriver(Binding &B, llvm::ArrayRef<uint64_t> A) {
  if (A[1] != Driver)
    return invalid("WdfDriverCreate does not match the bound driver");
  auto SuppliedPath = readRegistryPath(A[2]);
  if (!SuppliedPath)
    return SuppliedPath.takeError();
  if (*SuppliedPath != B.RegistryBytes)
    return invalid("WdfDriverCreate registry path does not match its binding");
  auto Size = read(A[4], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != DriverConfigSize)
    return InfoLengthMismatch;
  auto AddDevice = read(A[4] + DriverConfigAddDevice);
  auto Unload = read(A[4] + DriverConfigUnload);
  auto Flags = read(A[4] + DriverConfigFlags, 4);
  auto Tag = read(A[4] + DriverConfigTag, 4);
  if (!AddDevice || !Unload || !Flags || !Tag)
    return joinedErrors(AddDevice.takeError(), Unload.takeError(),
                        Flags.takeError(), Tag.takeError());
  if (*Flags == DriverNonPnp) {
    if (*AddDevice)
      return InvalidParameter;
    if (!*Unload)
      return invalid("non-PnP framework drivers require EvtDriverUnload");
  } else if (*Flags == 0) {
    if (!*AddDevice)
      return invalid("PnP framework drivers require EvtDriverDeviceAdd");
  } else {
    return invalid("unsupported framework driver configuration flags");
  }
  if (B.DriverHandle)
    return DriverInternalError;
  auto Validation = attributes(A[3], AttributesUse::Driver);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  const auto &Attrs = std::get<Attributes>(*Validation);
  if (A[5])
    if (auto E = writable(A[5], 8))
      return E;
  auto Handle = createObject(B.Globals, Attrs, true);
  if (!Handle)
    return Handle.takeError();
  auto Thunk =
      Exports.insertFrameworkFunction(B.Globals, FrameworkUnloadRoutine);
  if (!Thunk)
    return Thunk.takeError();
  std::vector<uint8_t> Path = std::move(*SuppliedPath);
  Path.resize(Path.size() + sizeof(char16_t), 0);
  auto Copy = allocate(Path.size(), false, false);
  if (!Copy)
    return Copy.takeError();
  if (auto E = Memory.write(*Copy, Path))
    return E;
  std::vector<uint8_t> Name(GlobalsNameSize);
  std::copy_n(ServiceName.begin(),
              std::min(ServiceName.size(), Name.size() - 1), Name.begin());
  uint32_t PoolTag = *Tag;
  if (!PoolTag || PoolTag == ForbiddenPoolTag) {
    auto TagName = llvm::StringRef(ServiceName);
    if (TagName.take_front(3).equals_insensitive("WDF"))
      TagName = TagName.drop_front(3);
    PoolTag = DefaultPoolTag;
    if (TagName.size() > 2) {
      PoolTag = 0;
      for (size_t I = 0; I < std::min<size_t>(4, TagName.size()); ++I)
        PoolTag |= uint32_t(uint8_t(TagName[I])) << (8 * I);
    }
  }
  for (auto [Offset, Value, Width] :
       {std::tuple<uint64_t, uint64_t, unsigned>{GlobalsDriver, *Handle, 8},
        {GlobalsFlags, *Flags, 4},
        {GlobalsTag, PoolTag, 4},
        {GlobalsDisplaceUnload, 1, 1}})
    if (auto E = Memory.writeInteger(B.Globals + Offset, Value, Width))
      return E;
  if (auto E = Memory.write(B.Globals + GlobalsName, Name))
    return E;
  if (auto E =
          Memory.writeInteger(Driver + windows::DriverUnloadOffset, *Thunk, 8))
    return E;
  if (A[5])
    if (auto E = Memory.writeInteger(A[5], *Handle, 8))
      return E;
  B.DriverHandle = *Handle;
  B.AddDeviceCallback = *AddDevice;
  B.UnloadCallback = *Unload;
  B.RegistryCopy = *Copy;
  return 0;
}

llvm::Error KernelFramework::planDelete(uint64_t Handle,
                                        std::vector<Step> &Steps) {
  // Cancellation/draining of live queue requests needs an explicit schedule.
  // Detect that boundary before changing any ancestor or invoking cleanup.
  auto Preflight = [&](auto &&Self, uint64_t Current) -> llvm::Error {
    auto I = Objects.find(Current);
    if (I == Objects.end())
      return invalid("delete requires a live framework object");
    if (I->second.Kind == ObjectKind::Request) {
      const auto &R = Requests.at(Current);
      if (!R.Completed && !(Current == Handle && R.Completing))
        return invalid(
            "deletion with a live request requires queue cancellation "
            "or draining, which is not modeled");
      if (Current != Handle && I->second.InternalReferences)
        return invalid(
            "deletion with an outstanding cancellation callback requires "
            "asynchronous draining, which is not modeled");
    }
    if (I->second.Kind == ObjectKind::File) {
      auto File = FileObjects.find(Current);
      if (File == FileObjects.end())
        return invalid("framework file object lost its WDM identity");
      if (FileHandles.contains(File->second.Wdm))
        return invalid("deletion with an open file requires CLOSE");
    }
    if (I->second.Kind == ObjectKind::IoTarget && I->second.References)
      return invalid("device deletion with a referenced local target "
                     "requires the driver to release it first");
    if (I->second.Kind == ObjectKind::Queue &&
        (std::any_of(
             ReadyQueueCallbacks.begin(), ReadyQueueCallbacks.end(),
             [&](const auto &Entry) { return Entry.second == Current; }) ||
         std::any_of(
             CanceledQueueCallbacks.begin(), CanceledQueueCallbacks.end(),
             [&](const auto &Entry) { return Entry.second == Current; })))
      return invalid("deletion with an active queue callback requires "
                     "asynchronous draining, which is not modeled");
    for (uint64_t Child : I->second.Children)
      if (Objects.count(Child))
        if (auto E = Self(Self, Child))
          return E;
    return llvm::Error::success();
  };
  if (auto E = Preflight(Preflight, Handle))
    return E;
  std::vector<Step> Destruction;
  auto Visit = [&](auto &&Self, uint64_t Handle) -> llvm::Error {
    auto I = Objects.find(Handle);
    if (I == Objects.end())
      return invalid("delete requires a live framework object");
    auto &O = I->second;
    if (O.Deleting)
      return invalid("object deletion is already in progress");
    O.Deleting = true;
    for (uint64_t Child : O.Children)
      if (Objects.count(Child) && !Objects.at(Child).Deleting)
        if (auto E = Self(Self, Child))
          return E;
    for (uint64_t Type : O.ContextOrder)
      if (O.Contexts.at(Type).Cleanup)
        Steps.push_back(
            {StepKind::Callback, Handle, O.Contexts.at(Type).Cleanup});
    Steps.push_back({StepKind::Cleaned, Handle});
    Destruction.push_back({StepKind::TryDestroy, Handle});
    return llvm::Error::success();
  };
  if (auto E = Visit(Visit, Handle))
    return E;
  Steps.insert(Steps.end(), Destruction.begin(), Destruction.end());
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::advance(uint64_t Token) {
  auto I = Continuations.find(Token);
  if (I == Continuations.end() || PendingCall)
    return invalid("invalid framework callback continuation");
  auto &C = I->second;
  auto NotifyQueueState = [&]() {
    for (auto &[Handle, Q] : Queues) {
      if (!Q.StopComplete && !Q.DrainComplete)
        continue;
      const bool DriverOwned =
          std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
            return Entry.second.Queue == Handle && !Entry.second.Queued &&
                   !Entry.second.Completed;
          });
      if (DriverOwned)
        continue;
      if (std::any_of(CancelCallbacks.begin(), CancelCallbacks.end(),
                      [&](const auto &Entry) {
                        auto O = Objects.find(Entry.second);
                        return O != Objects.end() && O->second.Parent == Handle;
                      }) ||
          std::any_of(
              CanceledQueueCallbacks.begin(), CanceledQueueCallbacks.end(),
              [&](const auto &Entry) { return Entry.second == Handle; }) ||
          std::any_of(
              ReadyQueueCallbacks.begin(), ReadyQueueCallbacks.end(),
              [&](const auto &Entry) { return Entry.second == Handle; }))
        continue;
      if (Q.StopComplete) {
        PendingCall = GuestCall{Token, Q.StopComplete, {Handle, Q.StopContext}};
        Q.StopComplete = 0;
        Q.StopContext = 0;
      } else {
        if (!Q.Pending.empty())
          continue;
        PendingCall =
            GuestCall{Token, Q.DrainComplete, {Handle, Q.DrainContext}};
        Q.DrainComplete = 0;
        Q.DrainContext = 0;
      }
      return true;
    }
    return false;
  };
  if (NotifyQueueState())
    return std::optional<uint64_t>{};
  while (C.Index < C.Steps.size()) {
    const Step S = C.Steps[C.Index++];
    if (S.Kind == StepKind::Callback) {
      PendingCall = GuestCall{Token, S.PC, {S.Object}};
      return std::optional<uint64_t>{};
    }
    if (S.Kind == StepKind::PresentQueue) {
      auto Presented = presentQueued(S.Object, Token);
      if (!Presented)
        return Presented.takeError();
      if (*Presented)
        return std::optional<uint64_t>{};
      continue;
    }
    if (S.Kind == StepKind::DriverUnloaded) {
      Bindings.at(S.Object).Unloaded = true;
      continue;
    }
    if (S.Kind == StepKind::FinishBindingUnbind) {
      if (auto E = finishUnbind(Bindings.at(S.Object)))
        return E;
      continue;
    }
    if (S.Kind == StepKind::BeginDriverDelete) {
      std::vector<Step> Delete;
      if (auto E = planDelete(Bindings.at(S.Object).DriverHandle, Delete))
        return E;
      C.Steps.insert(C.Steps.begin() + C.Index, Delete.begin(), Delete.end());
      continue;
    }
    if (S.Kind == StepKind::PurgeCancelRequest) {
      auto R = Requests.find(S.Object);
      auto O = Objects.find(S.Object);
      if (R == Requests.end() || O == Objects.end())
        return invalid("purge cancellation lost its request object");
      if (R->second.Completed || R->second.Completing ||
          R->second.Cancellation == CancelState::Unmarked)
        continue;
      if (R->second.Cancellation != CancelState::Marked ||
          !R->second.CancelRoutine || !O->second.InternalReferences ||
          CancelCallbacks.contains(Token) || !RequestsHost.RecordCancel)
        return invalid("purge cancellation lost driver request ownership");
      if (auto E = RequestsHost.RecordCancel(R->second.IRP))
        return E;
      const uint64_t Routine = R->second.CancelRoutine;
      R->second.Cancellation = CancelState::Queued;
      R->second.CancelRoutine = 0;
      CancelCallbacks.emplace(Token, S.Object);
      C.Steps.insert(C.Steps.begin() + C.Index,
                     {StepKind::CancelReturned, S.Object});
      if (auto E = beginCancelCallback(Token))
        return E;
      PendingCall = GuestCall{Token, Routine, {S.Object}};
      return std::optional<uint64_t>{};
    }
    if (S.Kind == StepKind::CanceledOnQueue) {
      auto R = Requests.find(S.Object);
      if (R == Requests.end() || !R->second.CanceledOnQueue ||
          R->second.Queued || R->second.Completed || R->second.Completing)
        return invalid("queued cancellation lost its driver-owned request");
      auto Q = Queues.find(R->second.Queue);
      if (Q == Queues.end() || !Q->second.CanceledOnQueue)
        return invalid("queued cancellation lost its callback");
      if (!CanceledQueueCallbacks.emplace(Token, R->second.Queue).second)
        return invalid("queued cancellation callback already active");
      C.Steps.insert(C.Steps.begin() + C.Index,
                     {StepKind::CanceledOnQueueReturned, R->second.Queue});
      PendingCall = GuestCall{
          Token, Q->second.CanceledOnQueue, {R->second.Queue, S.Object}};
      return std::optional<uint64_t>{};
    }
    if (S.Kind == StepKind::CanceledOnQueueReturned) {
      auto Callback = CanceledQueueCallbacks.find(Token);
      if (Callback == CanceledQueueCallbacks.end() ||
          Callback->second != S.Object)
        return invalid("queued cancellation callback return lost its queue");
      CanceledQueueCallbacks.erase(Callback);
      if (NotifyQueueState())
        return std::optional<uint64_t>{};
      continue;
    }
    if (S.Kind == StepKind::ReadyNotify) {
      auto Q = Queues.find(S.Object);
      if (Q == Queues.end())
        return invalid("ready notification lost its manual queue");
      if (!Q->second.ReadyNotify || !Q->second.Dispatching ||
          queuePnpHeld(Q->second) || Q->second.Pending.empty()) {
        if (Q->second.Pending.empty() || !Q->second.ReadyNotify)
          Q->second.ReadyPending = false;
        continue;
      }
      Q->second.ReadyPending = false;
      if (!ReadyQueueCallbacks.emplace(Token, S.Object).second)
        return invalid("ready notification callback already active");
      C.Steps.insert(C.Steps.begin() + C.Index,
                     {StepKind::ReadyNotifyReturned, S.Object});
      PendingCall = GuestCall{
          Token, Q->second.ReadyNotify, {S.Object, Q->second.ReadyContext}};
      return std::optional<uint64_t>{};
    }
    if (S.Kind == StepKind::ReadyNotifyReturned) {
      auto Callback = ReadyQueueCallbacks.find(Token);
      if (Callback == ReadyQueueCallbacks.end() || Callback->second != S.Object)
        return invalid("ready notification callback return lost its queue");
      ReadyQueueCallbacks.erase(Callback);
      if (NotifyQueueState())
        return std::optional<uint64_t>{};
      continue;
    }
    if (S.Kind == StepKind::CompleteRequest) {
      auto R = Requests.find(S.Object);
      if (R == Requests.end() || !R->second.Completing || R->second.Completed)
        return invalid("completion continuation lost its live request");
      auto Information = RequestsHost.Information(R->second.IRP);
      if (!Information)
        return Information.takeError();
      if (auto E = RequestsHost.Complete(
              R->second.IRP, R->second.CompletionStatus, *Information))
        return E;
      for (auto &[Handle, M] : RequestMemories)
        if (M.Request == S.Object && M.Active) {
          if (M.LockedMDL) {
            if (!RequestsHost.ReleaseUserBuffer)
              return invalid(
                  "framework user-memory release host is unavailable");
            if (auto E = RequestsHost.ReleaseUserBuffer(*M.LockedMDL))
              return E;
          }
          M.Active = false;
        }
      const uint64_t QueueHandle = R->second.Queue;
      R->second.Completed = true;
      R->second.Completing = false;
      R->second.Queue = 0;
      if (R->second.FileCreate &&
          (R->second.CompletionStatus & profile::NTStatusFailureMask) &&
          R->second.File) {
        const uint64_t File = R->second.File;
        if (auto E = unlinkFileObject(File))
          return E;
        std::vector<Step> Delete;
        if (auto E = planDelete(File, Delete))
          return E;
        C.Steps.insert(C.Steps.begin() + C.Index, Delete.begin(), Delete.end());
      }
      for (auto &Entry : PnpTransitions)
        Entry.second.WaitingRequests.erase(S.Object);
      if (NotifyQueueState()) {
        C.Steps.insert(C.Steps.begin() + C.Index,
                       {StepKind::PresentQueue, QueueHandle});
        return std::optional<uint64_t>{};
      }
      auto Presented = presentQueued(QueueHandle, Token);
      if (!Presented)
        return Presented.takeError();
      if (*Presented)
        return std::optional<uint64_t>{};
      continue;
    }
    if (S.Kind == StepKind::DeleteFileObject) {
      if (auto E = unlinkFileObject(S.Object))
        return E;
      std::vector<Step> Delete;
      if (auto E = planDelete(S.Object, Delete))
        return E;
      C.Steps.insert(C.Steps.begin() + C.Index, Delete.begin(), Delete.end());
      continue;
    }
    if (S.Kind == StepKind::CompleteFileIRP) {
      if (!RequestsHost.Complete)
        return invalid("file request completion host is unavailable");
      if (auto E = RequestsHost.Complete(S.Object, windows::StatusSuccess, 0))
        return E;
      continue;
    }
    if (S.Kind == StepKind::ForwardFileIRP) {
      if (!RequestsHost.ForwardFile || !RequestsHost.View)
        return invalid("lower file-request host is unavailable");
      auto View = RequestsHost.View(S.Object);
      if (!View)
        return View.takeError();
      auto Status = RequestsHost.ForwardFile(S.Object);
      if (!Status)
        return Status.takeError();
      if (*Status == windows::StatusPending)
        return invalid("asynchronous lower file completion is unsupported");
      if (View->Major == RequestMajorCreate &&
          (*Status & profile::NTStatusFailureMask) && S.File) {
        if (auto E = unlinkFileObject(S.File))
          return E;
        std::vector<Step> Delete;
        if (auto E = planDelete(S.File, Delete))
          return E;
        C.Steps.insert(C.Steps.begin() + C.Index, Delete.begin(), Delete.end());
      }
      continue;
    }
    if (S.Kind == StepKind::CancelReturned) {
      auto Callback = CancelCallbacks.find(Token);
      auto R = Requests.find(S.Object);
      auto O = Objects.find(S.Object);
      if (Callback == CancelCallbacks.end() || Callback->second != S.Object ||
          R == Requests.end() ||
          R->second.Cancellation != CancelState::Delivered ||
          O == Objects.end() || !O->second.InternalReferences)
        return invalid("cancellation return lost its request reference");
      --O->second.InternalReferences;
      CancelCallbacks.erase(Callback);
      // Completion may already have run cleanup and retired the WDM IRP while
      // this callback was suspended. Reuse the normal guest destroy sequence
      // only after both framework and driver holds are gone.
      if (O->second.Cleaned && O->second.DestroyEligible &&
          !O->second.References && !O->second.InternalReferences)
        C.Steps.insert(C.Steps.begin() + C.Index,
                       {StepKind::TryDestroy, S.Object});
      if (NotifyQueueState())
        return std::optional<uint64_t>{};
      continue;
    }
    auto OI = Objects.find(S.Object);
    if (OI == Objects.end())
      return invalid("callback sequence lost its object");
    auto &O = OI->second;
    if (S.Kind == StepKind::Cleaned) {
      O.Cleaned = true;
      continue;
    }
    if (S.Kind == StepKind::TryDestroy) {
      O.DestroyEligible = true;
      if (O.References || O.InternalReferences)
        continue;
      std::vector<Step> Destroy;
      for (uint64_t Type : O.ContextOrder)
        if (O.Contexts.at(Type).Destroy)
          Destroy.push_back(
              {StepKind::Callback, S.Object, O.Contexts.at(Type).Destroy});
      Destroy.push_back({StepKind::Destroy, S.Object});
      C.Steps.insert(C.Steps.begin() + C.Index, Destroy.begin(), Destroy.end());
      continue;
    }
    if (O.References || O.InternalReferences)
      return invalid("destroy callback retained an object reference");
    if (O.Kind == ObjectKind::Device) {
      for (const auto &[Handle, Queue] : Queues)
        if (Queue.Device == S.Object)
          return invalid("device deletion still owns a referenced queue");
      if (!DevicesHost.Delete)
        return invalid("framework device host is not configured");
      if (auto E = DevicesHost.Delete(Devices.at(S.Object).Wdm))
        return E;
      if (auto E = retireResourceLists(Devices.at(S.Object)))
        return E;
      if (Devices.at(S.Object).PDO)
        PnpDeviceHandles.erase(Devices.at(S.Object).PDO);
      Devices.erase(S.Object);
    }
    if (O.Kind == ObjectKind::Queue)
      Queues.erase(S.Object);
    if (O.Kind == ObjectKind::File)
      FileObjects.erase(S.Object);
    if (O.Kind == ObjectKind::Request)
      Requests.erase(S.Object);
    if (O.Kind == ObjectKind::Memory) {
      auto M = RequestMemories.find(S.Object);
      if (M == RequestMemories.end())
        return invalid("framework memory lost its object state");
      if (M->second.Active && M->second.LockedMDL) {
        if (!RequestsHost.ReleaseUserBuffer)
          return invalid("framework user-memory release host is unavailable");
        if (auto E = RequestsHost.ReleaseUserBuffer(*M->second.LockedMDL))
          return E;
      }
      RequestMemories.erase(M);
    }
    for (const auto &[Type, Context] : O.Contexts)
      if (auto E = retire(Context.Address))
        return E;
    if (auto E = retire(S.Object))
      return E;
    Objects.erase(OI);
  }
  Continuations.erase(I);
  if (auto E = resumePausedPnp())
    return E;
  return PendingCall ? std::optional<uint64_t>{} : std::optional<uint64_t>{0};
}

llvm::Expected<uint64_t> KernelFramework::start(std::vector<Step> Steps) {
  const uint64_t Token = NextContinuation++;
  Continuations.emplace(Token, Continuation{std::move(Steps)});
  auto Result = advance(Token);
  if (!Result)
    return Result.takeError();
  return 0; // Deferred callback completion is latched by DriverSession.
}

std::optional<KernelFramework::GuestCall> KernelFramework::takeGuestCall() {
  return std::exchange(PendingCall, std::nullopt);
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::finishGuestCall(uint64_t Token, uint64_t Result) {
  auto Transition = PnpTransitions.find(Token);
  if (Transition != PnpTransitions.end()) {
    auto Device = Devices.find(Transition->second.Device);
    if (Device == Devices.end() || CompletedPnp)
      return invalid("PnP power callback lost its device or completion");
    auto &State = Transition->second;
    if (!State.CallbacksComplete) {
      const bool StoppingRequest = State.Current.Phase == PnpPhase::IoStop;
      const bool ResumingRequest = State.Current.Phase == PnpPhase::IoResume;
      if (StoppingRequest) {
        auto Request = Requests.find(State.Current.Request);
        if (Request != Requests.end() && !Request->second.Completed) {
          if (Request->second.StopAcknowledged)
            Request->second.StopAcknowledged = false;
          else
            State.WaitingRequests.insert(State.Current.Request);
        }
      }
      if (ResumingRequest)
        if (auto Request = Requests.find(State.Current.Request);
            Request != Requests.end())
          Request->second.PowerSuspended = false;
      const uint32_t Status = StoppingRequest || ResumingRequest
                                  ? windows::StatusSuccess
                                  : uint32_t(Result);
      if (!StoppingRequest && !ResumingRequest &&
          Status == windows::StatusPending)
        return invalid("PnP power callback returned STATUS_PENDING");
      const bool Failed = Status & profile::NTStatusFailureMask;
      if (Failed && !(State.Status & profile::NTStatusFailureMask))
        State.Status = Status;
      if (State.Entering && Failed &&
          (State.Current.Phase == PnpPhase::PrepareHardware ||
           State.Current.Phase == PnpPhase::D0Entry)) {
        State.Remaining.clear();
        if (Device->second.ReleaseHardware)
          State.Remaining.push_back({PnpPhase::ReleaseHardware});
      }
      if (!State.Remaining.empty() &&
          State.Remaining.front().Phase == PnpPhase::IoStop) {
        if (auto E = schedulePnpCallback(Token))
          return E;
        return std::optional<uint64_t>{};
      }
      if (!State.WaitingRequests.empty()) {
        State.WaitingForRequests = true;
        return std::optional<uint64_t>{0};
      }
      if (!State.Remaining.empty()) {
        if (auto E = schedulePnpCallback(Token))
          return E;
        return std::optional<uint64_t>{};
      }
      if (auto E = finalizePnpCallbacks(Token))
        return E;
    }
  }
  auto Cancel = CancelCallbacks.find(Token);
  if (Cancel != CancelCallbacks.end() &&
      Requests.at(Cancel->second).Cancellation != CancelState::Delivered)
    return invalid("cancellation callback has not entered guest execution");
  auto Next = advance(Token);
  if (!Next)
    return Next.takeError();
  if (Next->has_value()) {
    auto Complete = PnpTransitions.find(Token);
    if (Complete != PnpTransitions.end() &&
        Complete->second.CallbacksComplete) {
      CompletedPnp =
          PnpCompletion{Complete->second.IRP, Complete->second.Status};
      PnpTransitions.erase(Complete);
    }
  }
  if (*Next) {
    if (auto E = flushReadyNotifications())
      return std::move(E);
    if (PendingCall)
      return std::optional<uint64_t>{};
  }
  return Next;
}

llvm::Error KernelFramework::flushReadyNotifications() {
  if (PendingCall)
    return llvm::Error::success();
  std::vector<Step> Steps;
  for (const auto &[Handle, Queue] : Queues) {
    if (!Queue.ReadyPending || !Queue.ReadyNotify || !Queue.Dispatching ||
        queuePnpHeld(Queue) || Queue.Pending.empty())
      continue;
    const bool Active =
        std::any_of(ReadyQueueCallbacks.begin(), ReadyQueueCallbacks.end(),
                    [&](const auto &Entry) { return Entry.second == Handle; });
    if (!Active)
      Steps.push_back({StepKind::ReadyNotify, Handle});
  }
  if (Steps.empty())
    return llvm::Error::success();
  if (auto E = preflightCancellationToken(0))
    return E;
  auto Started = start(std::move(Steps));
  if (!Started)
    return Started.takeError();
  return llvm::Error::success();
}

bool KernelFramework::hasLiveBinding() const {
  return std::any_of(Bindings.begin(), Bindings.end(),
                     [](const auto &Entry) { return !Entry.second.Unbound; });
}

llvm::Expected<uint64_t>
KernelFramework::call(const KernelExportRegistry::Export &Export,
                      llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  auto Count = argumentCount(Export);
  if (!Count || *Count != A.size())
    return invalid("unsupported framework routine or argument count");
  if (IRQL)
    return invalid("current framework object callbacks require PASSIVE_LEVEL");
  if (Export.Kind != KernelExportRegistry::ExportKind::FrameworkFunction)
    return Export.Name == api::WdfVersionBind ? bind(A) : unbind(A);
  auto BI = Bindings.find(Export.Binding);
  if (BI == Bindings.end() || BI->second.Unbound)
    return invalid("table entry belongs to an unbound framework instance");
  auto &B = BI->second;
  if (Export.Name == FrameworkUnloadRoutine) {
    if (A[0] != Driver || !B.DriverHandle || B.Unloaded)
      return invalid("invalid or repeated framework driver unload");
    std::vector<Step> Steps;
    if (B.UnloadCallback)
      Steps.push_back({StepKind::Callback, B.DriverHandle, B.UnloadCallback});
    Steps.push_back({StepKind::BeginDriverDelete, B.Globals});
    Steps.push_back({StepKind::DriverUnloaded, B.Globals});
    return start(std::move(Steps));
  }
  if (A[0] != B.Globals)
    return invalid("framework function received another binding's globals");
  auto Control = callControl(Export.Name, B, A);
  if (!Control)
    return Control.takeError();
  if (*Control)
    return **Control;
  auto File = callFile(Export.Name, B, A);
  if (!File)
    return File.takeError();
  if (*File)
    return **File;
  auto Queue = callQueue(Export.Name, B, A);
  if (!Queue)
    return Queue.takeError();
  if (*Queue)
    return **Queue;
  auto Accessor = callRequestAccessors(Export.Name, B, A);
  if (!Accessor)
    return Accessor.takeError();
  if (*Accessor)
    return **Accessor;
  auto Request = callRequest(Export.Name, B, A);
  if (!Request)
    return Request.takeError();
  if (*Request)
    return **Request;
  if (Export.Name == api::WdfDriverCreate)
    return createDriver(B, A);
  if (Export.Name == api::WdfWdmDriverGetWdfDriverHandle) {
    if (A[1] != Driver || !Objects.count(B.DriverHandle))
      return invalid("WDM driver does not own a live framework driver");
    return B.DriverHandle;
  }
  if (Export.Name == api::WdfObjectCreate) {
    auto Validation = attributes(A[1], AttributesUse::Object);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return *Status;
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (auto E = writable(A[2], 8))
      return E;
    auto Handle = createObject(B.Globals, Attrs, false);
    if (!Handle)
      return Handle.takeError();
    if (auto E = Memory.writeInteger(A[2], *Handle, 8))
      return E;
    return 0;
  }
  if (Export.Name == api::WdfObjectContextGetObject) {
    for (const auto &[Handle, O] : Objects)
      if (O.Binding == B.Globals)
        for (const auto &[Type, Context] : O.Contexts)
          if (Context.Address && Context.Address == A[1])
            return Handle;
    return invalid("context pointer has no live framework owner");
  }
  auto OI = Objects.find(A[1]);
  if (OI == Objects.end() || OI->second.Binding != B.Globals)
    return invalid("invalid, foreign or deleted framework handle");
  auto &O = OI->second;
  if (Export.Name == api::WdfDriverGetRegistryPath ||
      Export.Name == api::WdfDriverWdmGetDriverObject) {
    if (O.Kind != ObjectKind::Driver)
      return invalid("framework handle has the wrong object type");
    return Export.Name == api::WdfDriverGetRegistryPath ? B.RegistryCopy
                                                        : Driver;
  }
  if (Export.Name == api::WdfObjectGetTypedContextWorker) {
    auto TypeSize = read(A[2], 4);
    if (!TypeSize)
      return TypeSize.takeError();
    if (*TypeSize != ContextTypeSize && *TypeSize != ContextTypeLegacySize)
      return invalid("unsupported typed context record size");
    auto CI = O.Contexts.find(A[2]);
    return CI == O.Contexts.end() ? 0 : CI->second.Address;
  }
  if (Export.Name == api::WdfObjectAllocateContext) {
    if (O.Deleting)
      return DeletePending;
    auto Validation = attributes(A[2], AttributesUse::AdditionalContext);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return *Status;
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (!Attrs.Type)
      return ObjectNameInvalid;
    if (A[3])
      if (auto E = writable(A[3], 8))
        return E;
    const bool Exists = O.Contexts.count(Attrs.Type);
    auto Context = addContext(O, Attrs);
    if (!Context)
      return Context.takeError();
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], *Context, 8))
        return E;
    return Exists ? ObjectNameExists : 0;
  }
  if (Export.Name == api::WdfObjectReferenceActual) {
    if (O.Cleaned)
      return invalid(
          "references after cleanup are outside this framework profile");
    if (O.References == UINT64_MAX)
      return invalid("framework reference count overflow");
    ++O.References;
    return 0;
  }
  if (Export.Name == api::WdfObjectDereferenceActual) {
    if (!O.References)
      return invalid("framework reference count underflow");
    --O.References;
    if (O.Cleaned && O.DestroyEligible && !O.References &&
        !O.InternalReferences)
      return start({{StepKind::TryDestroy, A[1]}});
    return 0;
  }
  if (Export.Name == api::WdfObjectDelete) {
    if (O.Kind == ObjectKind::Driver)
      return invalid("WDFDRIVER cannot be deleted by the driver");
    if (O.Kind == ObjectKind::Queue && Queues.at(A[1]).IsDefault)
      return invalid("the default queue cannot be deleted by the driver");
    if (O.Kind == ObjectKind::Request)
      return invalid("an incoming framework request is released by completion");
    if (O.Kind == ObjectKind::IoTarget)
      return invalid("the local I/O target is owned by its device");
    if (O.Kind == ObjectKind::File)
      return invalid("a framework file object is released by CLOSE");
    if (O.Kind == ObjectKind::Device && Devices.at(A[1]).PDO)
      return invalid("a PnP framework device is deleted by removal");
    std::vector<Step> Steps;
    if (auto E = planDelete(A[1], Steps))
      return E;
    return start(std::move(Steps));
  }
  return invalid("unhandled framework routine");
}
} // namespace neverd::emulation
