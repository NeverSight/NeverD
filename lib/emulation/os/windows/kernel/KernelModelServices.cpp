//===- KernelModelServices.cpp - Shared-session legacy driver services ----===//
///
/// \file
/// Shared-session legacy driver services.
///

#include "../driver/DriverImage.h"
#include "KernelExportRegistry.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
using namespace windows;
constexpr llvm::StringLiteral ServicePrefix =
    "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "driver services: " + T);
}
bool overlaps(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
namespace module {
#define NEVERD_KERNEL_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelLoadedModule.def"
#undef NEVERD_KERNEL_MODULE_VALUE
} // namespace module
} // namespace
bool KernelModel::isGuestDriverObject(uint64_t Object) const {
  if (Object == DriverObject)
    return true;
  for (const auto &[Key, D] : NamedDrivers)
    if (D.Object == Object && D.Guest)
      return true;
  return false;
}
bool KernelModel::isServiceImagePC(uint64_t PC) const {
  for (const auto &[ID, S] : ServiceImages)
    if (S.Mapped && PC >= S.Image->Base && PC - S.Image->Base < S.Image->Size)
      return true;
  return false;
}
bool KernelModel::isServiceThreadEntry(uint64_t PC) const {
  const auto *E = Exports ? Exports->lookup(PC) : nullptr;
  const auto &Active = Scheduler.active();
  if (!Active || Active->Kind != KernelScheduler::CallbackKind::SystemThread ||
      Active->PC != PC)
    return false;
  const auto T = ThreadObjects.find(Active->Object);
  return E && E->Kind == KernelExportRegistry::ExportKind::ModuleExport &&
         E->Module == "ntoskrnl.exe" &&
         (E->Name == "ZwLoadDriver" || E->Name == "ZwUnloadDriver") &&
         T != ThreadObjects.end() && !T->second.Exited &&
         T->second.CallbackID == Active->ID;
}
llvm::Error KernelModel::initializeServiceImages(const DriverImage &Primary,
                                                 const DriverOptions &O) {
  if (auto E = validateDriverServices(O))
    return E;
  PrimaryServiceName = O.ServiceName;
  ServiceImagesKnown = O.ServiceImages.has_value();
  if (!O.ServiceImages)
    return llvm::Error::success();
  uint64_t ID = 1;
  for (const auto &R : *O.ServiceImages) {
    auto I = loadDriverImage(R.Source, O.MemoryLimit, R.LoadAddress);
    if (!I)
      return I.takeError();
    if (I->Guard.Enabled)
      return invalid("child CFG images require a multi-image guard owner");
    if (overlaps(I->Base, I->Size, Primary.Base, Primary.Size))
      return invalid("child image overlaps primary image");
    for (const auto &[Other, S] : ServiceImages)
      if (overlaps(I->Base, I->Size, S.Image->Base, S.Image->Size))
        return invalid("child images overlap");
    // All host file reads and image validation happen before guest execution.
    // Mapping and symbol binding are deferred to a real ZwLoadDriver call.
    ServiceImageState S;
    S.Resource = R;
    S.Image = std::make_shared<DriverImage>(std::move(*I));
    ServiceImages.emplace(ID++, std::move(S));
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::validateServiceAPI(uint64_t PC,
                                            llvm::StringRef Name) const {
  return validateServiceCallContext(Name, PC);
}
llvm::Error KernelModel::validateServiceCallContext(llvm::StringRef Name,
                                                    uint64_t PC) const {
  bool Child = isServiceImagePC(PC);
  for (const auto &[ID, S] : ServiceImages)
    Child |= S.Active && S.Thread == CurrentThreadKey;
  if (!Child)
    return llvm::Error::success();
  // A finite synchronous legacy-driver profile. Other APIs need per-module
  // asynchronous/exception ownership before they can be admitted here.
  if (Name.starts_with("Rtl") || Name == "DbgPrint" || Name == "DbgPrintEx" ||
      Name == "ExAllocatePoolWithTag" || Name == "ExAllocatePool" ||
      Name == "ExAllocatePool2" || Name == "ExFreePoolWithTag" ||
      Name == "ExFreePool" || Name == "IoCreateDevice" ||
      Name == "IoDeleteDevice" || Name == "IoCreateSymbolicLink" ||
      Name == "IoDeleteSymbolicLink" || Name == "ZwOpenKey" ||
      Name == "IofCompleteRequest" || Name == "IoCompleteRequest" ||
      Name == "IoGetCurrentIrpStackLocation" || Name == "ZwQueryValueKey" ||
      Name == "ZwSetValueKey" || Name == "ZwClose" ||
      Name == "KeGetCurrentIrql" || Name == "MmIsAddressValid")
    return llvm::Error::success();
  return invalid("child legacy callback does not admit " + Name +
                 "; asynchronous or nested service ownership is required");
}
llvm::Error KernelModel::mapServiceImage(ServiceImageState &S) {
  if (!Exports)
    return invalid("child imports require the shared export registry");
  std::vector<uint64_t> Imports;
  for (const auto &I : S.Image->Imports) {
    auto A = Exports->bindImport(I);
    if (!A)
      return A.takeError();
    Imports.push_back(*A);
  }
  for (const auto &R : S.Image->Regions) {
    if (!S.Mapped) {
      if (auto E = Memory.map(R.Address, R.Bytes.size(), Read | Write))
        return E;
    } else if (auto E = Memory.protect(R.Address, R.Bytes.size(), Read | Write))
      return E;
    if (auto E = Memory.write(R.Address, R.Bytes))
      return E;
  }
  for (size_t N = 0; N < Imports.size(); ++N)
    if (auto E = Memory.writeInteger(S.Image->Imports[N].Slot, Imports[N], 8))
      return E;
  for (const auto &R : S.Image->Regions)
    if (auto E = Memory.protect(R.Address, R.Bytes.size(), R.Permissions))
      return E;
  S.Mapped = true;
  return synchronizeGuestPageTables();
}
llvm::Error KernelModel::createServiceObject(ServiceImageState &S) {
  auto Object = allocate(DriverObjectSize);
  if (!Object)
    return Object.takeError();
  S.Object = *Object;
  auto Ext = allocate(DriverExtensionSize);
  if (!Ext)
    return Ext.takeError();
  S.Extension = *Ext;
  auto Mod = allocate(module::Size);
  if (!Mod)
    return Mod.takeError();
  S.Module = *Mod;
  S.Storage = {{S.Object, DriverObjectSize},
               {S.Extension, DriverExtensionSize},
               {S.Module, module::Size}};
  auto String = [&](const std::string &Text) -> llvm::Expected<uint64_t> {
    auto A = makeUnicodeString(Text);
    if (A)
      S.Storage.emplace_back(*A, 16 + (Text.size() + 1) * 2);
    return A;
  };
  auto Path = String(ServicePrefix.str() + S.Resource.ServiceName);
  if (!Path)
    return Path.takeError();
  S.Path = *Path;
  auto Name = String("\\Driver\\" + S.Resource.ServiceName);
  if (!Name)
    return Name.takeError();
  auto Full = String(S.Resource.ImagePath);
  if (!Full)
    return Full.takeError();
  const auto Pos = S.Resource.ImagePath.find_last_of('\\');
  auto Base = String(S.Resource.ImagePath.substr(Pos + 1));
  if (!Base)
    return Base.takeError();
  struct F {
    uint64_t A, V;
    unsigned W;
  };
  for (const auto &F : std::initializer_list<F>{
           {S.Object, DriverType, 2},
           {S.Object + 2, DriverObjectSize, 2},
           {S.Object + DriverStartOffset, S.Image->Base, 8},
           {S.Object + DriverImageSizeOffset, S.Image->Size, 4},
           {S.Object + DriverExtensionOffset, S.Extension, 8},
           {S.Object + DriverInitOffset, S.Image->Entry, 8},
           {S.Object + DriverSectionOffset, S.Module, 8},
           {S.Extension, S.Object, 8},
           {S.Module + module::ImageBase, S.Image->Base, 8},
           {S.Module + module::EntryPoint, S.Image->Entry, 8},
           {S.Module + module::ImageSize, S.Image->Size, 4},
           {S.Module + module::LoadCount, 1, 2}})
    if (auto E = Memory.writeInteger(F.A, F.V, F.W))
      return E;
  for (const auto &[To, From] :
       {std::pair{S.Object + DriverNameOffset, *Name},
        std::pair{S.Module + module::FullName, *Full},
        std::pair{S.Module + module::BaseName, *Base}}) {
    std::array<uint8_t, 16> B{};
    if (auto E = Memory.read(From, B))
      return E;
    if (auto E = Memory.write(To, B))
      return E;
  }
  // Publish into the same loader ring observed through DriverSection.
  auto Last = Memory.readInteger(DriverModule + 8, 8);
  if (!Last)
    return Last.takeError();
  for (const auto &[At, Value] :
       {std::pair{S.Module, DriverModule}, std::pair{S.Module + 8, *Last},
        std::pair{*Last, S.Module}, std::pair{DriverModule + 8, S.Module}})
    if (auto E = Memory.writeInteger(At, Value, 8))
      return E;
  const auto Key =
      llvm::StringRef("\\Driver\\" + S.Resource.ServiceName).lower();
  if (NamedDrivers.contains(Key))
    return invalid("driver object name already exists");
  ReferencedObjects.emplace(
      S.Object, ObjectReferences{ObjectKind::Driver, 0, 0,
                                 "\\Driver\\" + S.Resource.ServiceName});
  NamedDrivers.emplace(
      Key,
      NamedDriverObject{"\\Driver\\" + S.Resource.ServiceName, S.Object, true});
  auto Watch =
      watchInitialization(S.Object + DriverDispatchOffset,
                          profile::MajorFunctionCount * profile::PointerSize);
  if (!Watch)
    return Watch.takeError();
  NamedDrivers.at(Key).DispatchInitialization = std::move(*Watch);
  LoadedModules.push_back({S.Image->Base, S.Image->Size, S.Resource.ImagePath,
                           true, S.Image->DLLCharacteristics, S.Module});
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::callDriverService(llvm::StringRef Name,
                                                        uint64_t Path) {
  if (!Version || Version->Major != 10 || Version->Minor ||
      Version->Build != 19041)
    return invalid("requires windows_version 10.0.19041");
  if (!Path && Name == "ZwLoadDriver")
    return StatusInvalidParameter;
  auto Text = readObjectName(Path);
  if (!Text)
    return Text.takeError();
  APICallDetail = *Text;
  if (!llvm::StringRef(*Text).starts_with_insensitive(ServicePrefix))
    return invalid("requires an absolute CurrentControlSet Services key");
  const auto Service = llvm::StringRef(*Text).drop_front(ServicePrefix.size());
  if (Service.empty() || Service.contains('\\'))
    return invalid("requires a single service name below Services");
  if (Service.equals_insensitive(PrimaryServiceName))
    return invalid(
        "self load/unload requires foreground image lifetime ownership");
  if (RegistryCallback)
    return invalid(
        "registry filters require service-load registry continuations");
  auto Key = Registry.providerKey(*Text);
  if (!Key)
    return Key.takeError();
  if (!*Key)
    return StatusObjectNameNotFound;
  // Native IopGetDriverNameFromKey consults ObjectName before deriving the
  // default directory/name. This finite provider must not ignore an override.
  auto ObjectName = Registry.providerValue(**Key, "ObjectName");
  if (!ObjectName)
    return ObjectName.takeError();
  if (ObjectName->Value)
    return invalid(
        "registry ObjectName overrides require an explicit driver-name owner");
  if (ObjectName->Status && ObjectName->Status != StatusObjectNameNotFound)
    return ObjectName->Status;
  ServiceImageState *Found = nullptr;
  uint64_t ID = 0;
  for (auto &[N, S] : ServiceImages)
    if (Service.equals_insensitive(S.Resource.ServiceName)) {
      Found = &S;
      ID = N;
      break;
    }
  if (Found && (Found->Phase == ServiceImageState::State::Loading ||
                Found->Phase == ServiceImageState::State::Unloading))
    return invalid(
        "concurrent service transition requires loader-lock scheduling");
  for (const auto &[Other, S] : ServiceImages)
    if (S.Phase == ServiceImageState::State::Loading ||
        S.Phase == ServiceImageState::State::Unloading)
      return invalid(
          "another service transition requires loader-lock scheduling");
  const bool Load = Name == "ZwLoadDriver";
  if (!Load) {
    if (!Found || Found->Phase != ServiceImageState::State::Loaded) {
      if (!ServiceImagesKnown)
        return invalid("unload identity requires service_images");
      return StatusObjectNameNotFound;
    }
    auto U = Memory.readInteger(Found->Object + DriverUnloadOffset, 8);
    if (!U)
      return U.takeError();
    if (!*U)
      return InvalidDeviceRequest;
    auto Executable = Memory.canAccess(*U, 1, Execute);
    if (!Executable)
      return Executable.takeError();
    if (*U < Found->Image->Base ||
        *U - Found->Image->Base >= Found->Image->Size || !*Executable)
      return invalid(
          "unload routine requires executable code in its live child image");
    const auto N = NamedDrivers.find(
        llvm::StringRef("\\Driver\\" + Found->Resource.ServiceName).lower());
    if (N == NamedDrivers.end() ||
        ReferencedObjects.at(N->second.Object).Pointers)
      return invalid("unload requires no retained child driver references");
    Found->Phase = ServiceImageState::State::Unloading;
    PendingServiceCall = KernelGuestCall{
        {GuestCallOwner::DriverService, ID}, *U, {Found->Object}, 0};
  } else {
    if (Found && Found->Phase == ServiceImageState::State::Loaded)
      return uint64_t(0xc000010e);
    auto Type = Registry.providerValue(**Key, "Type");
    if (!Type)
      return Type.takeError();
    if (Type->Status)
      return Type->Status;
    if (!Type->Value || Type->Value->Type != 4 ||
        Type->Value->Data.size() != 4 ||
        llvm::support::endian::read32le(Type->Value->Data.data()) != 1)
      return invalid(
          "load requires an explicit REG_DWORD Type=SERVICE_KERNEL_DRIVER");
    auto Image = Registry.providerValue(**Key, "ImagePath");
    if (!Image)
      return Image.takeError();
    if (Image->Status)
      return Image->Status;
    if (!Image->Value || (Image->Value->Type != 1 && Image->Value->Type != 2) ||
        (Image->Value->Data.size() & 1))
      return invalid(
          "ImagePath requires a literal counted UTF-16 registry string");
    std::string P;
    const auto &B = Image->Value->Data;
    for (size_t I = 0; I < B.size(); I += 2) {
      auto C = llvm::support::endian::read16le(B.data() + I);
      if (!C) {
        if (I + 2 != B.size())
          return invalid("ImagePath contains embedded NUL");
        break;
      }
      if (C < 32 || C >= 127 || C == '%')
        return invalid(
            "ImagePath requires literal ASCII without host expansion");
      P.push_back(char(C));
    }
    if (!Found ||
        !llvm::StringRef(P).equals_insensitive(Found->Resource.ImagePath))
      return invalid(
          "ImagePath has no matching explicit immutable service image");
    if (!Exports)
      return invalid("load requires shared exports");
    if (hasPendingModelGuestCall() || PendingWait)
      return invalid("load cannot replace a prepared callback or wait");
    if (auto E = mapServiceImage(*Found))
      return E;
    if (auto E = createServiceObject(*Found))
      return E;
    Found->Phase = ServiceImageState::State::Loading;
    PendingServiceCall = KernelGuestCall{{GuestCallOwner::DriverService, ID},
                                         Found->Image->Entry,
                                         {Found->Object, Found->Path},
                                         0};
  }
  Found->Thread = CurrentThreadKey;
  Found->Active = false;
  return 0;
}
llvm::Error KernelModel::beginServiceCall(uint64_t ID) {
  auto I = ServiceImages.find(ID);
  if (I == ServiceImages.end() || I->second.Active ||
      I->second.Thread != CurrentThreadKey || CurrentIRQL)
    return invalid("service callback lost its caller or PASSIVE_LEVEL");
  I->second.Active = true;
  return llvm::Error::success();
}
llvm::Error KernelModel::retireServiceImage(ServiceImageState &S) {
  for (const auto &[A, D] : Devices)
    if (D.OwnerDriver == S.Object)
      return invalid("child returned with live devices");
  if (Scheduler.queuedCallbackCount() || Scheduler.suspendedCallbackCount())
    return invalid(
        "image retirement requires quiescent queued/suspended callbacks");
  for (const auto &[Object, Thread] : ThreadObjects)
    if (Thread.System && !Thread.Exited &&
        (!Scheduler.active() || Scheduler.active()->ID != CurrentThreadKey ||
         Scheduler.active()->Object != Object))
      return invalid(
          "image retirement requires quiescent other system threads");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges = S.Storage;
  Ranges.emplace_back(S.Image->Base, S.Image->Size);
  if (auto E = prepareReleaseRanges(Ranges))
    return E;
  auto Next = Memory.readInteger(S.Module, 8),
       Prev = Memory.readInteger(S.Module + 8, 8);
  if (!Next || !Prev)
    return llvm::joinErrors(Next.takeError(), Prev.takeError());
  if (auto E = Memory.writeInteger(*Prev, *Next, 8))
    return E;
  if (auto E = Memory.writeInteger(*Next + 8, *Prev, 8))
    return E;
  for (const auto &R : S.Image->Regions)
    if (auto E = Memory.protect(R.Address, R.Bytes.size(), 0))
      return E;
  for (const auto &[B, N] : S.Storage)
    FreedRanges.emplace(B, N);
  ReferencedObjects.erase(S.Object);
  NamedDrivers.erase(
      llvm::StringRef("\\Driver\\" + S.Resource.ServiceName).lower());
  std::erase_if(LoadedModules, [&](const auto &M) {
    return M.Guest && M.Base == S.Image->Base;
  });
  S.Phase = ServiceImageState::State::Unloaded;
  S.Active = false;
  return synchronizeGuestPageTables();
}
llvm::Expected<std::optional<uint64_t>>
KernelModel::finishServiceCall(uint64_t ID, uint64_t Value) {
  auto I = ServiceImages.find(ID);
  if (I == ServiceImages.end() || !I->second.Active ||
      I->second.Thread != CurrentThreadKey || CurrentIRQL)
    return invalid("service callback return lost its caller or PASSIVE_LEVEL");
  auto &S = I->second;
  if (S.Phase == ServiceImageState::State::Loading) {
    if (uint32_t(Value) && int32_t(Value) >= 0)
      return invalid(
          "child DriverEntry must return STATUS_SUCCESS for a successful load");
    if (auto E = prepareReleaseRange(
            S.Path,
            16 +
                (ServicePrefix.size() + S.Resource.ServiceName.size() + 1) * 2))
      return E;
    FreedRanges.emplace(
        S.Path,
        16 + (ServicePrefix.size() + S.Resource.ServiceName.size() + 1) * 2);
    if (int32_t(Value) < 0) {
      if (auto E = retireServiceImage(S))
        return E;
      return std::optional<uint64_t>{uint32_t(Value)};
    }
    S.Phase = ServiceImageState::State::Loaded;
    S.Active = false;
    return std::optional<uint64_t>{uint32_t(Value)};
  }
  if (S.Phase != ServiceImageState::State::Unloading)
    return invalid("return requires a live service transition");
  if (auto E = retireServiceImage(S))
    return E;
  return std::optional<uint64_t>{StatusSuccess};
}
llvm::Error KernelModel::validateServiceUnload() const {
  if (PendingServiceCall)
    return invalid("foreground unload left a service callback");
  for (const auto &[ID, S] : ServiceImages)
    if (S.Phase != ServiceImageState::State::Unloaded)
      return invalid("foreground unload returned with a loaded child service");
  return llvm::Error::success();
}
} // namespace neverd::emulation
