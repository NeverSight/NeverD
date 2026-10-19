//===- KernelModel.cpp - Windows objects and APIs -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows object initialization and checked guest state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "../../../core/RAMInitializationWatch.h"
#include "../WindowsUnicode.h"
#include "../driver/DriverImage.h"
#include "../driver/DriverScenario.h"
#include "KernelException.h"
#include "KernelLoaderView.h"
#include "KernelModelRuntime.h"
#include "KernelPhysicalInventory.h"
#include "KernelReferenceImage.h"
#include "KernelReferenceThread.h"
#include "KernelSectionNames.h"
#include "KernelSnapshotMemory.h"
#include "KernelVaShadowQuery.h"
#include "KernelVersionQuery.h"
#include "KernelWaits.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverClock.h"
#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace neverd::emulation {
namespace {

using namespace windows;

#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

std::string foldedASCII(std::string Text) {
  for (char &C : Text)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  return Text;
}

bool singleComponent(llvm::StringRef Name, llvm::StringRef Prefix) {
  return Name.starts_with(Prefix) && Name.size() > Prefix.size() &&
         !Name.drop_front(Prefix.size()).contains('\\');
}

llvm::Error
validateObjectNameText(llvm::StringRef Name,
                       size_t MaxCharacters = profile::MaxDeviceNameSize) {
  if (Name.size() > MaxCharacters)
    return modelError("object name exceeds the bounded namespace size");
  for (unsigned char C : Name)
    if (C < ' ' || C > '~')
      return modelError("object-name model supports printable ASCII only; "
                        "Unicode namespace case folding is unsupported");
  return llvm::Error::success();
}

llvm::Error validateNativeObjectNameText(llvm::StringRef Name,
                                         bool Unicode = false) {
  auto Units = windows_unicode::utf16(Name);
  if (!Units)
    return Units.takeError();
  if (Units->size() > profile::MaxDeviceNameSize)
    return modelError("object name exceeds the bounded namespace size");
  for (uint16_t C : *Units) {
    if (C < ' ' || C == 127)
      return modelError(
          "object name contains an unsupported control character");
    if (!Unicode && C > '~')
      return modelError("object-name model supports printable ASCII only; "
                        "Unicode namespace case folding is unsupported");
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<std::string> KernelModel::linkKey(llvm::StringRef Name) const {
  if (auto E = validateObjectNameText(Name))
    return E;
  std::string Key = foldedASCII(Name.str());
  if (SectionNamespace)
    for (const auto &[Alias, Target] : SectionNamespace->Aliases)
      if (Name.equals_insensitive(Alias))
        return Key;
  if (singleComponent(Key, DosDevicesPrefix))
    return DosDeviceAliasPrefix.str() + Key.substr(DosDevicesPrefix.size());
  if (singleComponent(Key, "\\GLOBAL??\\"))
    return DosDeviceAliasPrefix.str() + Key.substr(10);
  if (singleComponent(Key, DosDeviceAliasPrefix))
    return Key;
  if (singleComponent(Key, "\\") && Key != "\\DEVICE" && Key != "\\REGISTRY" &&
      Key != "\\DRIVER" && Key != "\\DOSDEVICES" && Key != "\\CALLBACK" &&
      Key != "\\??")
    return Key;
  return modelError(
      "symbolic-link model supports a single root or DOS alias component");
}

llvm::Expected<std::string>
KernelModel::resolveObjectPath(llvm::StringRef Name, bool CaseInsensitive,
                               bool UnicodeTail) const {
  if (auto E = validateNativeObjectNameText(Name, UnicodeTail))
    return E;
  std::string Path = Name.str();
  std::set<std::string> Seen;
  for (unsigned Depth = 0; Depth < 32; ++Depth) {
    llvm::StringRef P(Path);
    const std::pair<const std::string, std::string> *Alias = nullptr;
    if (SectionNamespace)
      for (const auto &Entry : SectionNamespace->Aliases) {
        const bool Match =
            CaseInsensitive
                ? (P.equals_insensitive(Entry.first) ||
                   P.starts_with_insensitive(Entry.first + "\\"))
                : (P == Entry.first || P.starts_with(Entry.first + "\\"));
        if (Match && (!Alias || Entry.first.size() > Alias->first.size()))
          Alias = &Entry;
      }
    if (Alias) {
      if (!Seen.insert(foldedASCII(Alias->first)).second)
        return modelError("symbolic-link cycle in object path");
      Path = Alias->second + Path.substr(Alias->first.size());
      continue;
    }
    if (P.starts_with_insensitive("\\Device\\"))
      return Path;
    if (P.starts_with_insensitive("\\Callback\\"))
      return Path;
    size_t End = std::string::npos;
    if (P.starts_with_insensitive("\\DosDevices\\"))
      End = Path.find('\\', 12);
    else if (P.starts_with_insensitive("\\??\\"))
      End = Path.find('\\', 4);
    else if (P.starts_with("\\"))
      End = Path.find('\\', 1);
    else
      return modelError("object path must be absolute");
    if (End == std::string::npos)
      End = Path.size();
    auto Key = linkKey(P.take_front(End));
    if (!Key)
      return Key.takeError();
    const auto Link = findSymbolicLink(*Key);
    if (!Link)
      return Path;
    if (!CaseInsensitive &&
        P.take_front(End).take_back(End - P.take_front(End).find_last_of('\\') -
                                    1) !=
            llvm::StringRef(objectName(Link))
                .drop_front(objectName(Link).find_last_of('\\') + 1))
      return Path;
    if (!Seen.insert(*Key).second)
      return modelError("symbolic-link cycle in object path");
    std::string Suffix = Path.substr(End);
    if (llvm::StringRef(SymbolicLinks.at(Link).Target).ends_with("\\") &&
        llvm::StringRef(Suffix).starts_with("\\"))
      Suffix.erase(0, 1);
    Path = SymbolicLinks.at(Link).Target + Suffix;
    if (auto E = validateNativeObjectNameText(Path, UnicodeTail))
      return E;
  }
  return modelError("object path exceeds symbolic-link traversal limit");
}

llvm::Expected<uint64_t>
KernelModel::resolveDeviceName(llvm::StringRef Name,
                               bool CaseInsensitive) const {
  if (auto E = validateObjectNameText(Name))
    return E;
  for (const auto &[Address, Object] : Devices)
    if (!Object.DeletePending &&
        (Object.Name == Name ||
         (CaseInsensitive && Name.equals_insensitive(Object.Name))))
      return Address;
  const auto Folded = foldedASCII(Name.str());
  if (!singleComponent(Folded, DosDevicesPrefix) &&
      !singleComponent(Folded, DosDeviceAliasPrefix) &&
      !singleComponent(Folded, "\\GLOBAL??\\"))
    return 0;
  auto Key = linkKey(Name);
  if (!Key)
    return Key.takeError();
  const auto Link = findSymbolicLink(*Key);
  if (!Link)
    return 0;
  const auto Target = foldedASCII(SymbolicLinks.at(Link).Target);
  for (const auto &[Address, Object] : Devices)
    if (!Object.DeletePending && foldedASCII(Object.Name) == Target)
      return Address;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::allocate(uint64_t Size,
                                               uint64_t Alignment) {
  return allocateInArena(Size, Alignment, NextAllocation, AllocationEnd);
}

llvm::Expected<uint64_t> KernelModel::allocateInArena(uint64_t Size,
                                                      uint64_t Alignment,
                                                      uint64_t &Cursor,
                                                      uint64_t ArenaEnd) {
  auto Planned = planArenaAllocation(Size, Alignment, Cursor, ArenaEnd);
  if (!Planned)
    return Planned.takeError();
  const uint64_t Start = *Planned;
  if (auto E = runtime::writeBytes(Memory, Start, Size))
    return E;
  ArenaAllocations.emplace(Start, Size);
  Cursor = Start + Size;
  return Start;
}
llvm::Expected<uint64_t>
KernelModel::planArenaAllocation(uint64_t Size, uint64_t Alignment,
                                 uint64_t Cursor, uint64_t ArenaEnd) const {
  if (!Size || Size > KernelMemoryBytes)
    return modelError("invalid model allocation size");
  if (!Alignment || (Alignment & (Alignment - 1)) ||
      Cursor > UINT64_MAX - (Alignment - 1))
    return modelError("invalid model allocation alignment");
  const uint64_t Start = (Cursor + Alignment - 1) & ~(Alignment - 1);
  if (Start > ArenaEnd || Size > ArenaEnd - Start)
    return modelError("Windows initialization model arena exhausted");
  return Start;
}

llvm::Expected<uint64_t>
KernelModel::makeUnicodeString(const std::string &Text) {
  auto Bytes = windows_unicode::bytes(Text, true);
  if (!Bytes)
    return Bytes.takeError();
  if (Text.find('\0') != std::string::npos)
    return modelError("initialization strings must be non-null UTF-8");
  if (Bytes->size() > MaxUnicodeBytes)
    return modelError("initialization string exceeds UNICODE_STRING capacity");
  auto Record = allocate(16 + Bytes->size());
  if (!Record)
    return Record.takeError();
  if (auto E = Memory.write(*Record + 16, *Bytes))
    return E;
  if (auto E = Memory.writeInteger(*Record, Bytes->size() - 2, 2))
    return E;
  if (auto E = Memory.writeInteger(*Record + 2, Bytes->size(), 2))
    return E;
  if (auto E = Memory.writeInteger(*Record + 8, *Record + 16, 8))
    return E;
  return *Record;
}

llvm::Error KernelModel::initialize(const DriverImage &Image,
                                    const DriverOptions &Options) {
  if (DriverObject)
    return modelError("kernel model cannot be initialized twice");
  if (auto E = validateDriverIRPInitialization(Options))
    return E;
  if (auto E = validateDriverFileSystem(Options.FileSystem))
    return E;
  if (Options.LoaderView) {
    if (auto E = kernel_loader_view::validate(*Options.LoaderView))
      return E;
    if (!Image.Size || Image.Size > UINT32_MAX ||
        Image.Size > UINT64_MAX - Image.Base || Image.Entry < Image.Base ||
        Image.Entry - Image.Base >= Image.Size)
      return modelError("driver loader_view: invalid image metadata");
  }
  if (Options.VersionQuery) {
    if (auto E = kernel_version_query::validate(*Options.VersionQuery))
      return E;
    if (Options.Contract != ExecutionContract::CheckedX64)
      return modelError("driver version_query requires checked-x64 execution");
  }
  if (Options.PhysicalMemoryInventory) {
    if (auto E = kernel_physical_inventory::validate(
            *Options.PhysicalMemoryInventory))
      return E;
    if (Options.Contract != ExecutionContract::CheckedX64)
      return modelError(
          "driver physical_memory_inventory requires checked-x64 execution");
  }
  if (Options.KernelVaShadowQuery) {
    if (auto E = kernel_va_shadow_query::validate(*Options.KernelVaShadowQuery))
      return E;
    if (Options.Contract != ExecutionContract::CheckedX64)
      return modelError(
          "driver kernel_va_shadow_query requires checked-x64 execution");
  }
  std::map<uint64_t, uint64_t> ImageRanges;
  for (const auto &Region : Image.Regions) {
    const uint64_t Size = Region.Bytes.size();
    if (!Size || Region.Address < Image.Base ||
        Region.Address - Image.Base >= Image.Size ||
        Size > Image.Size - (Region.Address - Image.Base) ||
        Size > UINT64_MAX - Region.Address)
      return modelError("invalid driver image RAM ownership range");
    if (!ImageRanges.empty()) {
      auto &Last = *ImageRanges.rbegin();
      if (Region.Address < Last.first + Last.second)
        return modelError("overlapping driver image RAM ownership ranges");
      if (Region.Address == Last.first + Last.second) {
        Last.second += Size;
        continue;
      }
    }
    ImageRanges.emplace(Region.Address, Size);
  }
  ImageRAM = std::move(ImageRanges);
  Contract = Options.Contract;
  VersionQuery = Options.VersionQuery;
  PhysicalMemoryInventory = Options.PhysicalMemoryInventory;
  KernelVaShadowQuery = Options.KernelVaShadowQuery;
  if (VersionQuery || PhysicalMemoryInventory || KernelVaShadowQuery)
    UnpackOpaqueEffects = true;
  InstructionClock = Options.Scheduling.has_value();
  Scheduler = KernelScheduler(KernelScheduler::Limits{},
                              Options.Scheduling.has_value());
  ConfiguredPnpDevices = Options.PnpDevices;
  if (Options.FileSystem && Options.FiniteFileSystem)
    return modelError("filesystem and finite_filesystem cannot coexist");
  WindowsIRPInitialization = Options.IRPInitializationPolicy ==
                             DriverIRPInitializationPolicy::Windows19041;
  if (auto E = validateDriverKernelMemorySize(Options.KernelMemorySize,
                                              Options.MemoryLimit))
    return E;
  KernelMemoryBytes = driverKernelMemorySize(Options.KernelMemorySize);
  if (Options.Version || Options.KernelMemorySize)
    if (auto E = Physical.configureManagedCapacity(KernelMemoryBytes))
      return E;
  if (Options.Smbios)
    if (auto E = validateDriverSmbios(*Options.Smbios))
      return E;
  Smbios = Options.Smbios;
  if (auto E =
          validateDriverSystemClockIncrement(Options.SystemClockIncrement100ns))
    return E;
  if (auto E = validateDriverObjectNamespace(Options.DriverNamespace,
                                             Options.ServiceName))
    return E;
  InstructionClock = Options.Scheduling.has_value();
  if (auto E = validateDriverCallbackDrainPolicy(Options.CallbackDrainPolicy))
    return E;
  ForegroundReadyCallbacks =
      foregroundReadyCallbacks(Options.CallbackDrainPolicy);
  Version = Options.Version;
  if (Version)
    UnpackOpaqueEffects = true;
  if (Version && Options.LoaderView)
    return modelError("windows_version and loader_view select distinct module "
                      "record contracts");
  SecurePciEnabled = Options.SecurePciEnabled;
  DmaGuardPolicyEnabled = Options.DmaGuardPolicyEnabled;
  CodeIntegrityOptions = Options.CodeIntegrityOptions;
  auto Mcfg = resolveDriverFirmwareMcfg(Options.PciMcfg, Options.AcpiTables);
  if (!Mcfg)
    return Mcfg.takeError();
  AcpiTables = Options.AcpiTables;
  if (auto E = configurePciMcfg(*Mcfg, Options.PciMcfg.has_value()))
    return E;
  if (auto E = initializeProcessEvents(Options))
    return E;
  WindowsPoolTags = Options.PoolTagPolicy == DriverPoolTagPolicy::Windows19041;
  WindowsNonpagedMDLMappings =
      Options.MDLMappingPolicy == DriverMDLMappingPolicy::Windows19041;
  if (WindowsNonpagedMDLMappings &&
      (!Version || Version->Major != 10 || Version->Minor != 0 ||
       Version->Build != 19041))
    return modelError(
        "Windows nonpaged MDL remapping requires version 10.0.19041");
  if (WindowsPoolTags && (!Version || Version->Major != 10 ||
                          Version->Minor != 0 || Version->Build != 19041))
    return modelError("Windows ordinary-pool tags require version 10.0.19041");
  if (DebuggerReference && DebuggerReference->debugger() &&
      (!Version || Version->Major != 10 || Version->Minor != 0 ||
       Version->Build != 19041))
    return modelError("kernel debugger data requires the explicit Windows "
                      "10.0.19041 profile");
  KernelScheduler::Limits ClockLimits;
  ClockLimits.SystemClockIncrement100ns = Options.SystemClockIncrement100ns;
  Scheduler = KernelScheduler(ClockLimits, Options.Scheduling.has_value());
  if (Options.SystemClockIncrement100ns)
    if (auto E = initializeSharedClock())
      return E;
  ConfiguredPnpDevices = Options.PnpDevices;
  NlsOemMultibyte = Options.NlsOemMultibyte;
  if (auto E = validateDriverPciConfiguration(Options))
    return E;
  PciConfiguration = Options.PciConfiguration;
  IRPIOPriorityHint = Options.IRPIOPriorityHint;
  if (auto E = validateDriverFiniteFileSystem(Options.FiniteFileSystem))
    return E;
  FiniteFileSystem = Options.FiniteFileSystem;
  if (auto E = validateDriverFilterManager(Options))
    return E;
  FilterManagerFileNames = Options.FilterManagerFileNames;
  if (auto E = validateDriverNetworkInterfaces(Options))
    return E;
  NetworkInterfaces = Options.NetworkInterfaces;
  if (auto E = validateDriverNdisConfiguration(Options))
    return E;
  NdisConfiguration = Options.NdisConfiguration;
  if (auto E = validateDriverRegistryHives(Options))
    return E;
  if (auto E = validateDriverRegistryEvents(Options))
    return E;
  if (auto E =
          Registry.initialize(Options.Registry, Options.RegistrySystemTime100ns,
                              Options.RegistryHives))
    return E;
  if (Exports && !DebuggerReference) {
    auto Modules = makeKernelModuleImages(*Exports);
    if (!Modules)
      return Modules.takeError();
    for (auto &Module : *Modules) {
      const uint64_t Base = Module.Identity.Base, Size = Module.Identity.Size;
      if (auto E = Memory.map(Base, Size, Read | Write))
        return E;
      if (auto E = Memory.write(Base, Module.Bytes))
        return E;
      if (auto E = Memory.protect(Base, Size, Read))
        return E;
      if (auto E = Memory.protect(Base + profile::KernelModuleCodeRVA,
                                  profile::ThunkSize, Read | Execute))
        return E;
      LoadedModules.push_back(
          {Base,
           Size,
           "\\SystemRoot\\System32\\" + Module.Identity.Name,
           false,
           0,
           0,
           {},
           {},
           Module.Identity.Name});
    }
  }
  if (!Version)
    LoadedModules.push_back({Image.Base,
                             Image.Size,
                             "\\SystemRoot\\System32\\drivers\\" + Image.Name,
                             true,
                             Image.DLLCharacteristics,
                             0,
                             {},
                             {},
                             Image.Name});
  if (auto E =
          Memory.map(profile::KernelArenaBase, KernelMemoryBytes, Read | Write))
    return E;
  NextAllocation = profile::KernelArenaBase;
  if (Version)
    NextThreadClientID = profile::InitialThreadID;
  AllocationEnd = profile::KernelArenaBase + KernelMemoryBytes;
  if (auto E = initializeNdisConfiguration())
    return E;
  auto Object = allocate(DriverObjectSize);
  if (!Object)
    return Object.takeError();
  DriverObject = *Object;
  ReferencedObjects.emplace(DriverObject, ObjectReferences{ObjectKind::Driver});
  if (Image.Entry)
    ForegroundThreadStart = Image.Entry;
  auto Extension = allocate(DriverExtensionSize);
  if (!Extension)
    return Extension.takeError();
  DriverExtension = *Extension;
  if (Options.LoaderView) {
    auto Record = allocate(kernel_loader_view::RecordSize);
    if (!Record)
      return Record.takeError();
    DriverLoaderRecord = *Record;
    for (const auto &[Field, Value] :
         std::array<std::pair<kernel_loader_view::Field, uint64_t>, 4>{
             {{kernel_loader_view::DllBase, Image.Base},
              {kernel_loader_view::EntryPoint, Image.Entry},
              {kernel_loader_view::SizeOfImage, Image.Size},
              {kernel_loader_view::Flags, *Options.LoaderView->Flags}}})
      if (auto E = Memory.writeInteger(DriverLoaderRecord + Field.Offset, Value,
                                       Field.Size))
        return E;
    if (auto E = Memory.writeInteger(DriverObject + DriverSectionOffset,
                                     DriverLoaderRecord, profile::PointerSize))
      return E;
    // Even an unused declaration changes the concrete environment. It does
    // not carry a rebinding contract for an independently loaded driver.
    UnpackOpaqueEffects = true;
  }
  auto Path =
      makeUnicodeString(RegistryServicesPrefix.str() + Options.ServiceName);
  if (!Path)
    return Path.takeError();
  RegistryPath = *Path;
  ReferencedObjects.at(DriverObject).Name =
      DriverPrefix.str() + Options.ServiceName;
  auto Name = makeUnicodeString(ReferencedObjects.at(DriverObject).Name);
  if (!Name)
    return Name.takeError();
  auto Hardware = makeUnicodeString(RegistryHardwarePath.str());
  if (!Hardware)
    return Hardware.takeError();
  if (Version)
    if (auto E = initializeLoadedModule(Image, Options.ServiceName))
      return E;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F :
       std::array<Field, 7>{{{0, DriverType, 2},
                             {2, DriverObjectSize, 2},
                             {DriverStartOffset, Image.Base, 8},
                             {DriverImageSizeOffset, Image.Size, 4},
                             {DriverExtensionOffset, DriverExtension, 8},
                             {DriverHardwareOffset, *Hardware, 8},
                             {DriverInitOffset, Image.Entry, 8}}})
    if (auto E = Memory.writeInteger(DriverObject + F.Offset, F.Value, F.Size))
      return E;
  std::array<uint8_t, 16> StringRecord{};
  if (auto E = Memory.read(*Name, StringRecord))
    return E;
  if (auto E = Memory.write(DriverObject + DriverNameOffset, StringRecord))
    return E;
  if (auto E = initializeDriverNamespace(Options))
    return E;
  if (auto E = Memory.writeInteger(DriverExtension, DriverObject, 8))
    return E;
  Result.DriverObject = DriverObject;
  if (auto E =
          Dispatcher.configure(DriverObject, profile::WorkerThreadIdentity))
    return E;
  if (Exports) {
    Framework = std::make_unique<KernelFramework>(
        Memory, *Exports, [this](uint64_t Size) { return allocate(Size); },
        [this](uint64_t Address, uint32_t Size, bool IsWrite) {
          return validateGuestAccess(Address, Size, IsWrite);
        },
        [this](uint64_t Address, uint64_t Size) -> llvm::Error {
          auto Lock = FrameworkCallbackLocks.find(Address);
          if (Lock == FrameworkCallbackLocks.end())
            return prepareReleaseRange(Address, Size);
          const uint64_t Storage = Lock->second.Storage;
          if (auto E = prepareReleaseRanges(
                  {{Address, Size}, {Storage, dispatcher::EventSize}}))
            return E;
          FreedRanges.emplace(Storage, dispatcher::EventSize);
          FrameworkCallbackLocks.erase(Lock);
          return llvm::Error::success();
        });
    Framework->configure(DriverObject, RegistryPath, Options.ServiceName);
    configureFrameworkDeviceHost();
    configureFrameworkLockHost();
    configureFrameworkRequestHost();
    configureFrameworkInterruptHost();
  }
  auto Watch = watchInitialization(DriverObject + DriverDispatchOffset,
                                   DispatchBytesWritten.size());
  if (!Watch)
    return Watch.takeError();
  DispatchInitialization = std::move(*Watch);
  if (auto E = initializeObjectNamespace())
    return E;
  if (auto E = initializeDataExports(Options))
    return E;
  if (auto E = initializeFileSystem(Options))
    return E;
  if (Options.PageTables) {
    if (!Version || Version->Major != 10 || Version->Minor != 0 ||
        Version->Build != 19041)
      return modelError("guest page tables require Windows 10.0.19041");
    GuestPageTables = std::make_unique<KernelGuestPageTables>(Memory, Physical);
    if (auto E = GuestPageTables->initialize(Options.MemoryLimit))
      return E;
    if (auto E = Memory.map(systemva::PagedPoolBase, KernelMemoryBytes,
                            Read | Write))
      return E;
    NextPagedAllocation = systemva::PagedPoolBase;
    PagedAllocationEnd = systemva::PagedPoolBase + KernelMemoryBytes;
  }
  if (Version && !Options.PageTables)
    if (auto E = Physical.enableGuestFrames(Options.MemoryLimit &
                                            ~(profile::PageSize - 1)))
      return E;
  if (auto E = initializeObjectTypes())
    return E;
  if (auto E = initializePciMcfg())
    return E;
  if (auto E = initializeHalAcpiCache())
    return E;
  if (auto E = initializeSections(Options))
    return E;
  if (auto E = prepareFiniteFileSystem())
    return E;
  if (auto E = initializeServiceImages(Image, Options))
    return E;
  return initializeProcessMemory();
}

llvm::Error KernelModel::synchronizeGuestPageTables() {
  return GuestPageTables ? GuestPageTables->synchronize()
                         : llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::guestCR3() {
  if (!GuestPageTables)
    return modelError("CR3 read requires enabled guest page tables");
  return GuestPageTables->root();
}

llvm::Expected<std::shared_ptr<RAMInitializationWatch>>
KernelModel::watchInitialization(uint64_t Address, uint64_t Size) {
  auto View = Memory.pinBacking(Address, Size);
  if (!View)
    return View.takeError();
  return RAMInitializationWatch::create(*View);
}

bool KernelModel::initialized(
    llvm::ArrayRef<bool> Flags,
    const std::shared_ptr<RAMInitializationWatch> &Watch, uint64_t Offset,
    uint64_t Size) {
  if (Offset > Flags.size() || Size > Flags.size() - Offset)
    return false;
  for (uint64_t I = Offset; I < Offset + Size; ++I)
    if (!Flags[I] && (!Watch || !Watch->initialized(I)))
      return false;
  return true;
}
bool KernelModel::anyInitialized(
    llvm::ArrayRef<bool> Flags,
    const std::shared_ptr<RAMInitializationWatch> &Watch, uint64_t Offset,
    uint64_t Size) {
  if (Offset > Flags.size() || Size > Flags.size() - Offset)
    return false;
  for (uint64_t I = Offset; I < Offset + Size; ++I)
    if (initialized(Flags, Watch, I))
      return true;
  return false;
}

llvm::Error KernelModel::finishEntry() {
  if (!DriverObject || !RegistryPath || EntryFinished || !Requests.empty() ||
      Unloading)
    return modelError(
        "DriverEntry lifetime can end only once after initialization");
  const auto Allocation = ArenaAllocations.find(RegistryPath);
  if (Allocation == ArenaAllocations.end())
    return modelError("RegistryPath has no owned guest allocation");
  // The WDK requires the driver to copy RegistryPath's contents if it needs
  // them later: the I/O manager releases this input after DriverEntry returns.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-driver_initialize
  if (auto E = prepareReleaseRange(RegistryPath, Allocation->second))
    return E;
  FreedRanges.emplace(RegistryPath, Allocation->second);
  EntryFinished = true;
  return llvm::Error::success();
}

llvm::Expected<std::string> KernelModel::readObjectName(uint64_t Address,
                                                        size_t MaxCharacters) {
  if (auto E = runtime::checkBufferRange(Address, 16))
    return E;
  if (auto E = validateGuestAccess(Address, 16, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + 2, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || *Length > *Maximum || *Length > MaxCharacters * 2)
    return modelError("invalid or oversized object-name UNICODE_STRING");
  if (auto E = runtime::checkBufferRange(*Buffer, *Length))
    return E;
  if (auto E = validateGuestAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (!Bytes.empty())
    if (auto E = Memory.read(*Buffer, Bytes))
      return E;
  std::string Name;
  for (size_t I = 0; I < Bytes.size(); I += 2) {
    if (Bytes[I + 1])
      return modelError("object-name model supports printable ASCII only; "
                        "Unicode namespace case folding is unsupported");
    Name.push_back(static_cast<char>(Bytes[I]));
  }
  if (auto E = validateObjectNameText(Name, MaxCharacters))
    return E;
  return Name;
}

llvm::Expected<uint64_t> KernelModel::createDevice(llvm::ArrayRef<uint64_t> A) {
  if (!isGuestDriverObject(A[0]))
    return modelError("IoCreateDevice received an unknown DRIVER_OBJECT");
  if (auto E = runtime::checkBufferRange(A[6], 8))
    return E;
  if (auto E = validateGuestAccess(A[6], 8, true))
    return E;
  std::string Name;
  if (A[2]) {
    auto Parsed = readObjectName(A[2]);
    if (!Parsed)
      return Parsed.takeError();
    Name = *Parsed;
    // A null name requests an unnamed device; a supplied empty name is invalid.
    if (Name.empty())
      return modelError("device-name model supports \\Device\\Name only");
  }
  auto Created = createDeviceObjectForOwner(
      Name, static_cast<uint32_t>(A[1]), static_cast<uint32_t>(A[3]),
      static_cast<uint32_t>(A[4]), static_cast<uint8_t>(A[5]) != 0, A[0],
      DeviceOwnerKind::Guest);
  if (!Created)
    return Created.takeError();
  if (Created->Status != StatusSuccess)
    return Created->Status;
  if (auto E = Memory.writeInteger(A[6], Created->Address, 8)) {
    // A backend mapping/permission failure is discovered only by the write.
    // Preserve the old API's unpublished-device state and allocated bytes.
    auto Previous = Memory.readInteger(Created->Address + DeviceNext, 8);
    if (!Previous)
      return llvm::joinErrors(std::move(E), Previous.takeError());
    if (auto Restore =
            Memory.writeInteger(A[0] + DriverDeviceHead, *Previous, 8))
      return llvm::joinErrors(std::move(E), std::move(Restore));
    ReferencedObjects.erase(Created->Address);
    Devices.erase(Created->Address);
    return E;
  }
  return Created->Status;
}

llvm::Expected<KernelModel::DeviceCreation>
KernelModel::createDeviceObject(llvm::StringRef Name, uint32_t ExtensionSize,
                                uint32_t Type, uint32_t Characteristics,
                                bool Exclusive) {
  return createDeviceObjectForOwner(Name, ExtensionSize, Type, Characteristics,
                                    Exclusive, DriverObject,
                                    DeviceOwnerKind::Guest);
}

llvm::Expected<KernelModel::DeviceCreation>
KernelModel::createDeviceObjectForOwner(llvm::StringRef Name,
                                        uint32_t ExtensionSize, uint32_t Type,
                                        uint32_t Characteristics,
                                        bool Exclusive, uint64_t Owner,
                                        DeviceOwnerKind OwnerKind) {
  if (auto E = validateDeviceTopology())
    return E;
  if (!Owner || (OwnerKind == DeviceOwnerKind::Guest
                     ? !isGuestDriverObject(Owner)
                     : Owner != deviceOwnerDriver(OwnerKind)))
    return modelError("device creation requires a registered driver owner");
  if (Characteristics & ~uint32_t(SecureOpen))
    return modelError(
        "IoCreateDevice model supports FILE_DEVICE_SECURE_OPEN only");
  if (ExtensionSize > UINT16_MAX - DeviceObjectSize)
    return modelError(
        "device extension exceeds the bounded DEVICE_OBJECT size");
  if (auto E = validateObjectNameText(Name))
    return E;
  if (!Name.empty()) {
    if (Name.equals_insensitive(section::PhysicalName) ||
        NamedSections.contains(llvm::StringRef(Name).lower()))
      return DeviceCreation{StatusObjectNameCollision, 0};
    const std::string Key = foldedASCII(Name.str());
    if (!singleComponent(Key, DevicePrefix))
      return modelError("device-name model supports \\Device\\Name only");
    for (const auto &[Address, Existing] : Devices)
      if (!Existing.DeletePending && foldedASCII(Existing.Name) == Key)
        return DeviceCreation{StatusObjectNameCollision, 0};
  }
  auto Previous = Memory.readInteger(Owner + DriverDeviceHead, 8);
  if (!Previous)
    return Previous.takeError();
  if (*Previous &&
      (!Devices.count(*Previous) || Devices.at(*Previous).OwnerDriver != Owner))
    return modelError("DRIVER_OBJECT device list was corrupted");
  const uint64_t Size = DeviceObjectSize + ExtensionSize;
  const uint64_t Start = (NextAllocation + 15) & ~uint64_t(15);
  if (Start > AllocationEnd || Size > AllocationEnd - Start)
    return DeviceCreation{StatusInsufficientResources, 0};
  auto Object = allocate(Size);
  if (!Object)
    return Object.takeError();
  const uint64_t Extension = *Object + DeviceObjectSize;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F : std::array<Field, 10>{
           {{0, DeviceType, 2},
            {2, Size, 2},
            {DeviceDriverOffset, Owner, 8},
            {DeviceNext, *Previous, 8},
            {DeviceFlagsOffset,
             OwnerKind == DeviceOwnerKind::PnpProvider ? DeviceBusEnumerated
             : OwnerKind == DeviceOwnerKind::Guest
                 ? DeviceInitializing | (Exclusive ? DeviceExclusive : 0)
                 : 0,
             4},
            {DeviceCharacteristicsOffset, Characteristics, 4},
            {DeviceExtensionOffset, Extension, 8},
            {DeviceTypeOffset, Type, 4},
            {DeviceStackCountOffset, 1, 1},
            {DeviceAlignmentOffset, DeviceAlignmentMask, 4}}})
    if (auto E = Memory.writeInteger(*Object + F.Offset, F.Value, F.Size))
      return E;
  if (auto E = Memory.writeInteger(Owner + DriverDeviceHead, *Object, 8))
    return E;
  DeviceRecord Device;
  Device.Address = *Object;
  Device.Extension = Extension;
  Device.Type = Type;
  Device.Name = Name.str();
  Device.OwnerDriver = Owner;
  Device.OwnerKind = OwnerKind;
  Device.Size = Size;
  Devices.emplace(*Object, std::move(Device));
  ReferencedObjects.emplace(*Object, ObjectReferences{ObjectKind::Device});
  return DeviceCreation{StatusSuccess, *Object};
}

llvm::Expected<uint32_t>
KernelModel::createSymbolicLink(llvm::StringRef Name, llvm::StringRef Target) {
  if (NamedSections.contains(Name.lower()))
    return StatusObjectNameCollision;
  if (SectionNamespace)
    for (const auto &D : SectionNamespace->Directories)
      if (Name.equals_insensitive(D))
        return StatusObjectNameCollision;
  auto Key = linkKey(Name.str());
  if (!Key)
    return Key.takeError();
  if (auto E = (Version || FiniteFileSystem || SectionNamespace)
                   ? validateNativeObjectNameText(Target, true)
                   : validateObjectNameText(Target))
    return E;
  if (!(FiniteFileSystem || SectionNamespace) &&
      !singleComponent(foldedASCII(Target.str()), DevicePrefix))
    return modelError("symbolic-link targets must be \\Device\\Name");
  if (findSymbolicLink(*Key))
    return StatusObjectNameCollision;
  auto Object = allocate(profile::ProcessTokenSize);
  if (!Object)
    return Object.takeError();
  std::string PublishedName =
      "\\GLOBAL??\\" + Name.substr(Name.rfind('\\') + 1).str();
  if (Version || FiniteFileSystem || SectionNamespace) {
    const auto Folded = foldedASCII(Name.str());
    if (!singleComponent(Folded, DosDevicesPrefix) &&
        !singleComponent(Folded, DosDeviceAliasPrefix) &&
        !singleComponent(Folded, "\\GLOBAL??\\"))
      PublishedName = Name.str();
  }
  auto Encoded = windows_unicode::bytes(Target, true);
  if (!Encoded)
    return Encoded.takeError();
  ReferencedObjects.emplace(
      *Object, ObjectReferences{ObjectKind::SymbolicLink, 0, 0, PublishedName});
  auto Bytes = std::move(*Encoded);
  SymbolicLinks.emplace(*Object,
                        SymbolicLinkObject{Target.str(), std::move(Bytes)});
  return StatusSuccess;
}

llvm::Expected<uint32_t> KernelModel::deleteSymbolicLink(llvm::StringRef Name) {
  auto Key = linkKey(Name.str());
  if (!Key)
    return Key.takeError();
  if (FiniteFileSystemLinks.contains(*Key))
    return modelError("configured filesystem aliases are provider-owned");
  if (SectionNamespace)
    for (const auto &[Alias, Target] : SectionNamespace->Aliases)
      if (Name.equals_insensitive(Alias))
        return modelError(
            "configured section directory aliases are provider-owned");
  const auto Object = findSymbolicLink(*Key);
  if (!Object)
    return StatusObjectNameNotFound;
  SymbolicLinks.at(Object).Permanent = false;
  if (Version || FiniteFileSystem || SectionNamespace)
    SymbolicLinks.at(Object).Named = false;
  retireObjectIfUnreferenced(Object);
  return StatusSuccess;
}

llvm::Error KernelModel::deleteDevice(uint64_t Address) {
  auto Device = Devices.find(Address);
  if (Device == Devices.end() || Device->second.DeletePending)
    return modelError("IoDeleteDevice received an unknown or deleted device");
  if (Device->second.OwnerKind != DeviceOwnerKind::Guest)
    return modelError("IoDeleteDevice cannot delete a provider-owned device");
  if (auto E = validateDeviceTopology())
    return E;
  if (auto E = canReleaseRemoveLockStorage(Address, Device->second.Size))
    return E;
  if (auto E = Interrupts.canReleaseRange(Address, Device->second.Size))
    return E;
  for (const auto &[Handle, Owner] : PoFxDeviceObjects)
    if (Owner == Address)
      return modelError("IoDeleteDevice requires PoFxUnregisterDevice first");
  // The matching 19041 IoDeleteDevice cancels shutdown registrations before
  // marking the device for deletion (including duplicate queue entries).
  if (Device->second.ShutdownNotifications)
    if (auto E = clearShutdownNotifications(Address))
      return E;
  Device->second.DeletePending = true;
  return retireDeviceIfUnreferenced(Address);
}

llvm::Error KernelModel::retireDeviceIfUnreferenced(uint64_t Address) {
  auto It = Devices.find(Address);
  if (It == Devices.end())
    return modelError("IoDeleteDevice received an unknown or deleted device");
  auto &Device = It->second;
  const auto &References = ReferencedObjects.at(Address);
  if (!Device.DeletePending || Device.InternalReferences ||
      Device.ShutdownNotifications || Device.Lower || Device.Upper ||
      Scheduler.hasOutstanding(Address))
    return llvm::Error::success();
  for (const auto &[Id, File] : FileObjects)
    if (File.Address && File.Device == Address)
      return llvm::Error::success();
  for (const auto &[ID, File] : NativeOpenFiles)
    if (File.Device == Address)
      return llvm::Error::success();
  for (const auto &[IRP, Request] : Requests)
    if (Request.Device == Address)
      return llvm::Error::success();
  const bool ReleaseStorage = !References.Pointers && !References.Handles;
  if (ReleaseStorage)
    if (auto E = prepareReleaseRange(Address, Device.Size))
      return E;
  // I/O-manager deletion removes the driver-list entry independently of
  // object-manager pointer references. Those references retain the storage.
  if (!Device.UnlinkedFromDriver) {
    uint64_t Link = Device.OwnerDriver + DriverDeviceHead;
    std::set<uint64_t> Seen;
    while (true) {
      auto Current = Memory.readInteger(Link, 8);
      if (!Current)
        return Current.takeError();
      if (!*Current || !Devices.count(*Current) ||
          Devices.at(*Current).OwnerDriver != Device.OwnerDriver ||
          Devices.at(*Current).UnlinkedFromDriver ||
          !Seen.insert(*Current).second)
        return modelError(
            "device missing from, or cycle in, DRIVER_OBJECT list");
      auto Next = Memory.readInteger(*Current + DeviceNext, 8);
      if (!Next)
        return Next.takeError();
      if (*Current == Address) {
        if (auto E = Memory.writeInteger(Link, *Next, 8))
          return E;
        Device.UnlinkedFromDriver = true;
        break;
      }
      Link = *Current + DeviceNext;
    }
  }
  if (!ReleaseStorage)
    return llvm::Error::success();
  FreedRanges.emplace(Address, Device.Size);
  Devices.erase(It);
  ReferencedObjects.erase(Address);
  WorkReferences.erase(Address);
  return llvm::Error::success();
}

llvm::Error KernelModel::snapshot() {
  KernelSnapshotMemory Observation(Memory);
  snapshotBugCheckCallbacks();
  if (auto E = snapshotUserBuffers())
    return E;
  if (auto E = snapshotSections())
    return E;
  Result.Registry = Registry.snapshot();
  // Owner state remains observable even if a guest object projection is
  // corrupt.
  auto Volumes = FileSystem.snapshot();
  if (!Volumes)
    return Volumes.takeError();
  if (FileSystem.volumeCount() !=
      size_t(
          std::count_if(Devices.begin(), Devices.end(), [](const auto &Entry) {
            return Entry.second.OwnerKind ==
                   DeviceOwnerKind::FileSystemProvider;
          })))
    return modelError(
        "filesystem volume device inventory differs from its owner");
  Result.FileSystem = std::move(*Volumes);
  if (auto E = validateFileSystemOpenBindings())
    return E;
  Result.RegistryHives = Registry.hiveSnapshot();
  Result.PciConfiguration = PciConfiguration;
  if (auto E = snapshotFiniteFileSystem())
    return E;
  if (!DriverObject)
    return modelError("cannot snapshot an uninitialized kernel model");
  // A terminal CPU fault still permits pure reads of committed RAM. Keep the
  // same object validators, without authorizing further guest/device access.
  if (auto E = validateDeviceTopology(ObjectRead::Snapshot))
    return E;
  auto Read = [&](uint64_t Address) {
    return readObjectField(Address, 8, ObjectRead::Snapshot);
  };
  auto Unload = Read(DriverObject + DriverUnloadOffset);
  if (!Unload)
    return Unload.takeError();
  Result.DriverUnload = *Unload;
  auto Extension = Read(DriverObject + DriverExtensionOffset);
  if (!Extension)
    return Extension.takeError();
  if (*Extension != DriverExtension)
    return modelError("DRIVER_OBJECT.DriverExtension was corrupted");
  auto AddDevice = Read(DriverExtension + DriverAddDeviceOffset);
  if (!AddDevice)
    return AddDevice.takeError();
  Result.AddDevice = *AddDevice;
  for (size_t I = 0; I < Result.MajorFunctions.size(); ++I) {
    if (!anyInitialized(DispatchBytesWritten, DispatchInitialization, I * 8,
                        8)) {
      Result.MajorFunctions[I] = 0;
      continue;
    }
    if (!initialized(DispatchBytesWritten, DispatchInitialization, I * 8, 8))
      return modelError("driver left a partially initialized dispatch pointer");
    auto Function = Read(DriverObject + DriverDispatchOffset + I * 8);
    if (!Function)
      return Function.takeError();
    Result.MajorFunctions[I] = *Function;
  }
  auto Inventory = driverDeviceInventory(ObjectRead::Snapshot);
  if (!Inventory)
    return Inventory.takeError();
  Result.Devices.clear();
  for (uint64_t Address : *Inventory) {
    const auto &Device = Devices.at(Address);
    if (Device.OwnerKind == DeviceOwnerKind::Guest)
      Result.Devices.push_back({Device.Address, Device.Extension, Device.Type,
                                Device.Name, Device.ReportedDevicePower});
  }
  return snapshotPnpDevices();
}

llvm::Error KernelModel::validateGuestAccess(uint64_t Address, uint32_t Size,
                                             bool IsWrite) const {
  return validateGuestAccessImpl(Address, Size, IsWrite, true);
}
bool KernelModel::isModuleResourceCandidate(uint64_t Address,
                                            uint32_t Size) const {
  if (!Size || Size > UINT64_MAX - Address)
    return false;
  for (const auto &M : LoadedModules)
    if (Address <= M.Base ? M.Base - Address < Size : Address - M.Base < M.Size)
      return false;
  // Only probe identity/lifetime here. The read path cannot record attempted
  // writes to initialized object fields. Actual accesses keep their original
  // read/write policy after resource publication and exact-PC continuation.
  if (auto E = validateGuestAccess(Address, Size, false)) {
    llvm::consumeError(std::move(E));
    return false;
  }
  return true;
}
llvm::Error KernelModel::validateGuestCPUWrite(uint64_t Address,
                                               uint32_t Size) const {
  return validateGuestAccessImpl(Address, Size, true, true, false, false);
}

llvm::Error KernelModel::validateGuestAccessImpl(
    uint64_t Address, uint32_t Size, bool IsWrite, bool IncludeDispatcher,
    bool PhysicalAccess, bool PrepareImageWrites) const {
  if (!Size)
    return llvm::Error::success();
  if (Size > UINT64_MAX - Address)
    return modelError("overflowing guest access in Windows model");
  if (auto E = validateDataExportAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateBugCheckAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateLookasideAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateFastMutexAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateExecutiveResourceAccess(Address, Size))
    return E;
  const uint64_t End = Address + Size;
  if (auto E = validateNlsDataAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateHalDispatchAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateAvlAccess(Address, Size, IsWrite))
    return E;
  if (!PhysicalAccess)
    if (auto E = validateProcessMemoryAccess(Address, Size,
                                             IsWrite && PrepareImageWrites))
      return E;
  if (!PhysicalAccess) {
    auto Section =
        validateSectionViewAccess(Address, Size, IsWrite, IncludeDispatcher);
    if (!Section)
      return Section.takeError();
    if (*Section)
      return llvm::Error::success();
  }
  if (auto E = validateSharedClockAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validatePciMcfgAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateNamedDriverAccess(Address, Size, IsWrite))
    return E;
  for (const auto &[Object, View] : NativeInitialThreadViews)
    if ((Address < View.Auxiliary + View.Size && View.Auxiliary < End) ||
        (Address < Object && Object - 0x30 < End))
      return modelError("guest access to opaque thread-construction resources");
  for (const auto &[Kind, Object] : ObjectTypes)
    if (Address < Object + profile::ProcessTokenSize && Object < End)
      return modelError(
          "guest access to an opaque object type (object-type identity)");
  if (Address < profile::ObjectTypeDataBase + profile::PageSize &&
      profile::ObjectTypeDataBase < End &&
      (IsWrite ||
       (!isObjectTypeDataRead(Address, Size) &&
        !isProcessDataRead(Address, Size) &&
        !isProcessorDataRead(Address, Size) && !isNlsDataRead(Address, Size) &&
        !isHalDispatchRead(Address, Size))))
    return modelError(
        "guest access to opaque object-type or process data storage");
  if (Address < profile::ProcessorEnvironmentBase + profile::PageSize &&
      profile::ProcessorEnvironmentBase < End)
    return modelError("guest access to the private processor view");
  if (!PhysicalAccess && Address < profile::UserProbeLimit) {
    if (auto E = validateUserMdlViewAccess(Address, Size, IsWrite))
      return E;
    if (canCatchUserAccess(Address, Size))
      return llvm::Error::success();
    for (const auto &[Base, Allocation] : UserAllocations) {
      const uint64_t Mapped =
          (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
      if (Address < Base + Mapped && Base < End)
        return RevokedUserAllocations.contains(Base)
                   ? modelError("unmapped user virtual address")
                   : modelError("user address access requires the requesting "
                                "process at IRQL <= APC_LEVEL");
    }
    // Unmapped low addresses outside our synthetic process retain the ordinary
    // backend memory-fault observation used by other execution phases.
  }
  if (Address < profile::GuardThunkBase + profile::PageSize &&
      profile::GuardThunkBase < End)
    return modelError("guest access to an opaque CFG helper");
  if (Framework)
    if (auto E = Framework->validateGuestAccess(Address, Size, IsWrite))
      return E;
  if (CurrentIRQL > APCLevel)
    for (const auto &[Base, Allocation] : Allocations)
      if (!Allocation.NonPaged && Address < Base + Allocation.Size &&
          Base < End && !Physical.hasPinnedPages(Address, Size))
        return modelError("paged pool access requires IRQL <= APC_LEVEL or "
                          "live physical page locks");
  if (KernelExportRegistry::overlapsThunk(Address, Size))
    return modelError(
        "kernel import thunks have no modeled readable or writable data");
  auto CheckArena = [&](uint64_t Base, uint64_t Limit,
                        uint64_t Cursor) -> llvm::Error {
    if (!Limit)
      return llvm::Error::success();
    if (Address < Limit && Cursor < End)
      return modelError("guest access to unallocated Windows model arena");
    const uint64_t ArenaFirst = std::max(Address, Base);
    const uint64_t ArenaLast = std::min(End, Limit);
    if (ArenaFirst >= ArenaLast)
      return llvm::Error::success();
    auto Allocation = ArenaAllocations.upper_bound(ArenaFirst);
    if (Allocation != ArenaAllocations.begin())
      --Allocation;
    uint64_t Current = ArenaFirst;
    while (Current < ArenaLast) {
      if (Allocation == ArenaAllocations.end() || Allocation->first > Current ||
          Current >= Allocation->first + Allocation->second)
        return modelError(
            "guest access to unallocated arena alignment padding");
      Current = std::min(ArenaLast, Allocation->first + Allocation->second);
      ++Allocation;
    }
    return llvm::Error::success();
  };
  if (auto E =
          CheckArena(profile::KernelArenaBase, AllocationEnd, NextAllocation))
    return E;
  if (auto E = CheckArena(systemva::PagedPoolBase, PagedAllocationEnd,
                          NextPagedAllocation))
    return E;
  for (const auto &[Start, Length] : FreedRanges)
    if (Address < Start + Length && Start < End)
      return modelError("guest access to a freed model object or allocation");
  for (const auto &[Range, References] : WaitBlockReferences)
    if (References && Address < Range.first + Range.second && Range.first < End)
      return modelError(kernel_wait::OpaqueBlocks);
  if (IncludeDispatcher)
    if (auto E = Dispatcher.validateGuestAccess(Address, Size, IsWrite))
      return E;
  if (auto E = RemoveLocks.validateGuestAccess(Address, Size, IsWrite))
    return E;
  if (auto E = DMA.validateGuestAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateExecutiveCallbackAccess(Address, Size))
    return E;
  if (auto E = validateRegistryCallbackAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateProcessNotifyAccess(Address, Size, IsWrite))
    return E;
  if (auto E = Interrupts.validateGuestAccess(Address, Size, IsWrite))
    return E;
  for (const auto &[Item, Device] : WorkItems)
    if (Address < Item + profile::WorkItemTokenSize && Item < End)
      return modelError("guest access to an opaque IO_WORKITEM");
  for (const auto &[ProcessID, Object] : ProcessObjects)
    if (Address < Object + ProcessObjectSize && Object < End) {
      if (!IsWrite && PublishedProcessExitStatuses.contains(Object) &&
          Address >= Object + windows::ProcessExitStatus19041Offset &&
          Size <= 4 &&
          Address - Object - windows::ProcessExitStatus19041Offset <= 4 - Size)
        continue;
      if (!IsWrite && PublishedProcessSectionBases.contains(Object) &&
          Address >= Object + windows::ProcessSectionBase19041Offset &&
          Size <= profile::PointerSize &&
          Address - Object - windows::ProcessSectionBase19041Offset <=
              profile::PointerSize - Size)
        continue;
      if (!IsWrite && PublishedProcessPebs.contains(Object) &&
          Address >= Object + windows::ProcessPeb19041Offset &&
          Size <= profile::PointerSize &&
          Address - Object - windows::ProcessPeb19041Offset <=
              profile::PointerSize - Size)
        continue;
      if (!IsWrite && PublishedProcessImageNames.contains(Object) &&
          Address >= Object + windows::ProcessImageFileName19041Offset &&
          Size <= DriverProcessImageFileNameSize &&
          Address - Object - windows::ProcessImageFileName19041Offset <=
              DriverProcessImageFileNameSize - Size)
        continue;
      if (!IsWrite && DebuggerReference && !DebuggerData.empty()) {
        const auto &Facts = *DebuggerReference->debugger();
        auto Within = [&](uint32_t Offset, uint32_t Bytes) {
          return Offset && Address >= Object + Offset && Size <= Bytes &&
                 Address - Object - Offset <= Bytes - Size;
        };
        if (Within(Facts.ProcessIDOffset, 8) ||
            (Facts.ActiveProcessHead &&
             !ExitedUserProcesses.contains(ProcessID) &&
             Within(Facts.ActiveProcessLinksOffset, 16)))
          continue;
        if (ProcessID == windows::SystemProcessID &&
            Within(KernelReferenceThreadInput::ProcessThreadListHeadOffset,
                   16) &&
            std::any_of(NativeInitialThreadViews.begin(),
                        NativeInitialThreadViews.end(), [&](const auto &View) {
                          return isNativeThreadViewRead(View.first, View.first,
                                                        1);
                        }))
          continue;
      }
      return modelError("guest access to an opaque process object");
    }
  for (const auto &[Object, Thread] : ThreadObjects)
    if (Address < Object + (ThreadIdentities.contains(Object)
                                ? ThreadObjectSize
                                : profile::ProcessTokenSize) &&
        Object < End &&
        (IsWrite || !ThreadIdentities.contains(Object) ||
         !isThreadIdentityRead(Object, Address, Size)))
      return modelError("guest access to an opaque thread object");
  for (const auto &[Object, State] : ReferencedObjects)
    if ((State.Kind == ObjectKind::Directory ||
         State.Kind == ObjectKind::SymbolicLink ||
         State.Kind == ObjectKind::Key) &&
        Address < Object + profile::ProcessTokenSize && Object < End)
      return modelError("guest access to an opaque namespace object");
  for (const auto &[Kind, Object] : ObjectTypes)
    if (Address < Object + profile::ProcessTokenSize && Object < End)
      return modelError(
          "guest access to an opaque object type (object-type identity)");
  for (const auto &[Handle, Owner] : PoFxDeviceObjects)
    if (Address < Handle + profile::PointerSize && Handle < End)
      return modelError("guest access to an opaque PoFx handle");
  for (const auto &Attachment : ProcessAttachments)
    if (!Attachment.Private && Address < Attachment.ApcState + KAPCStateSize &&
        Attachment.ApcState < End)
      return modelError("guest access to an active opaque APC state");
  if (IsWrite)
    for (const auto &[Lock, State] : ExecutiveSpinLocks)
      if (Address < Lock + 8 && Lock < End)
        return modelError("guest write to a held executive spin lock");
  if (IsWrite)
    for (const auto &[Lock, State] : QueuedSpinLocks)
      if (Address < Lock + 8 && Lock < End)
        return modelError("guest write to a held executive spin lock");
  if (auto E = validateIOAccess(Address, Size, IsWrite, !IncludeDispatcher))
    return E;
  if (auto E = validateFilterNameAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateNdisConfigurationAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateMDLAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateLoadedModuleAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateBugCheckAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validatePersistentDumpAccess(Address, Size, IsWrite))
    return E;
  if (auto E = validateReferenceDataAccess(Address, Size, IsWrite))
    return E;
  for (const auto &[Object, File] : NativeFiles) {
    if (IsWrite && Address < Object + windows::FileSize && Object < End)
      return modelError("guest write to a provider-owned FILE_OBJECT");
    const uint64_t Start = File.NameStorage;
    const uint64_t Length = ArenaAllocations.at(Start);
    if (Address < Start + Length && Start < End &&
        (IsWrite || Address < Start + 16))
      return modelError(
          "guest access to opaque or read-only file name storage");
  }

  auto CheckObject = [&](uint64_t Object, uint64_t Length,
                         auto &&Allowed) -> llvm::Error {
    if (!Object || Address >= Object + Length || Object >= End)
      return llvm::Error::success();
    const uint64_t First = std::max(Address, Object) - Object;
    const uint64_t Last = std::min(End, Object + Length) - Object;
    for (uint64_t Offset = First; Offset < Last; ++Offset)
      if (!Allowed(Offset))
        return modelError(IsWrite ? "guest write to an opaque or read-only "
                                    "Windows object field"
                                  : "guest read of an unmodeled Windows "
                                    "object field");
    return llvm::Error::success();
  };

  if (auto E =
          CheckObject(DriverObject, DriverObjectSize, [&](uint64_t Offset) {
            if (IsWrite)
              return (Offset >= DriverFastIOOffset &&
                      Offset < DriverInitOffset) ||
                     Offset >= DriverStartIOOffset;
            if (Offset >= DriverFlagsOffset && Offset < DriverFlagsOffset + 4)
              return false;
            if (Offset >= DriverSectionOffset && Offset < DriverExtensionOffset)
              return DriverLoaderRecord != 0 || DriverModule;
            if (Offset >= DriverDispatchOffset)
              return initialized(DispatchBytesWritten, DispatchInitialization,
                                 Offset - DriverDispatchOffset);
            return true;
          }))
    return E;
  if (auto E = CheckObject(DriverLoaderRecord, kernel_loader_view::RecordSize,
                           [&](uint64_t Offset) {
                             return !IsWrite &&
                                    kernel_loader_view::readable(Offset);
                           }))
    return E;
  if (auto E = CheckObject(
          DriverExtension, DriverExtensionSize, [&](uint64_t Offset) {
            return Offset >= DriverAddDeviceOffset &&
                   Offset < DriverAddDeviceOffset + profile::PointerSize;
          }))
    return E;
  for (uint64_t ProviderDriver :
       {PnpProviderDriver, FileSystemProviderDriver, FiniteFileSystemDriver})
    if (auto E =
            CheckObject(ProviderDriver, DriverObjectSize, [&](uint64_t Offset) {
              return !IsWrite &&
                     (Offset < 4 ||
                      (Offset >= DriverDeviceHead &&
                       Offset < DriverDeviceHead + profile::PointerSize));
            }))
      return E;
  for (const auto &[Object, Device] : Devices) {
    if (auto E = CheckObject(Object, DeviceObjectSize, [&](uint64_t Offset) {
          if (IsWrite) {
            if (Device.OwnerKind != DeviceOwnerKind::Guest)
              return false;
            return (Offset >= DeviceNext && Offset < DeviceAttachedOffset) ||
                   (Offset >= DeviceCurrentIRP &&
                    Offset < DeviceCurrentIRP + profile::PointerSize) ||
                   (Offset >= DeviceFlagsOffset &&
                    Offset < DeviceCharacteristicsOffset) ||
                   Offset == DeviceStackCountOffset ||
                   (Offset >= DeviceAlignmentOffset &&
                    Offset < DeviceAlignmentOffset + 4);
          }
          return Offset < DeviceAttachedOffset ||
                 (Offset >= DeviceCurrentIRP && Offset < DeviceVPBOffset) ||
                 (Offset >= DeviceExtensionOffset &&
                  Offset < DeviceQueueOffset) ||
                 (Offset >= DeviceAlignmentOffset &&
                  Offset < DeviceQueueStateOffset) ||
                 (Offset >= DeviceSectorSizeOffset &&
                  Offset < DeviceSectorSizeOffset + 2) ||
                 Offset >= DeviceReservedOffset;
        }))
      return E;
  }
  if (auto E = validateBuiltControlBufferAccess(Address, Size, IsWrite))
    return E;
  return llvm::Error::success();
}

std::optional<KernelModel::ExecutionProcessContext>
KernelModel::executionProcessContext() const {
  auto CallbackMode = [&](ExecutionProcessContext Context) {
    // These notifications are inside the modeled driver Zw* call, whose
    // temporary PreviousMode is KernelMode. Process identity and user-buffer
    // authority still come exclusively from the captured caller context.
    if (CurrentGuestCall.Owner == GuestCallOwner::RegistryCallback) {
      auto It = RegistryNotifications.find(CurrentGuestCall.ID);
      if (It != RegistryNotifications.end() && It->second.Active &&
          It->second.Thread == CurrentThreadKey)
        Context.PreviousMode = KernelMode;
    }
    return Context;
  };
  const bool Attached = hasProcessAttachment(CurrentExecution);
  if (Version && Attached) {
    auto It = std::find_if(
        ProcessAttachments.rbegin(), ProcessAttachments.rend(),
        [&](const auto &A) { return A.Execution == CurrentExecution; });
    auto Context = It->PreviousContext;
    Context.ProcessID =
        UserRequestContext ? CurrentUserProcessID : uint32_t(SystemProcessID);
    Context.RestrictedAttachment = true;
    return CallbackMode(Context);
  }
  if (auto It = InheritedExecutionContexts.find(CurrentExecution);
      It != InheritedExecutionContexts.end() &&
      It->second.ThreadKey == CurrentThreadKey) {
    auto Context = It->second.Process;
    if (Context && Attached) {
      Context->ProcessID = CurrentUserProcessID;
      Context->RestrictedAttachment = true;
    }
    if (Context)
      *Context = CallbackMode(*Context);
    return Context;
  }
  if (ProcessNotificationState &&
      ProcessNotificationState->Thread == CurrentThreadKey) {
    const auto PID =
        Result.ProcessEvents.at(ProcessNotificationState->ReportIndex)
            .ActorProcessID;
    return ExecutionProcessContext{PID, PID, KernelMode, false};
  }
  if (CurrentExecution == profile::StackBase) {
    const uint32_t Process =
        UserRequestContext ? CurrentUserProcessID : SystemProcessID;
    return CallbackMode(ExecutionProcessContext{
        Process, Process, uint8_t(UserRequestContext ? UserMode : KernelMode),
        Attached});
  }
  if (Scheduler.active() &&
      (Scheduler.active()->Kind == KernelScheduler::CallbackKind::WorkItem ||
       Scheduler.active()->Kind ==
           KernelScheduler::CallbackKind::FrameworkInterruptWorkItem ||
       Scheduler.active()->Kind ==
           KernelScheduler::CallbackKind::FrameworkDeferred ||
       Scheduler.active()->Kind == KernelScheduler::CallbackKind::PoFx ||
       Scheduler.active()->Kind == KernelScheduler::CallbackKind::SystemThread))
    return ExecutionProcessContext{Attached ? CurrentUserProcessID
                                            : uint32_t(SystemProcessID),
                                   SystemProcessID, KernelMode, Attached};
  return std::nullopt;
}

llvm::Error
KernelModel::inheritExecutionContext(uint64_t Child, uint64_t Parent,
                                     std::optional<uint32_t> ProcessID) {
  if (!Child || Child == Parent || Parent != CurrentExecution ||
      ExecutionThreadKeys.contains(Child) ||
      InheritedExecutionContexts.contains(Child))
    return modelError("nested context inheritance requires a fresh "
                      "child of the active execution");
  auto Context = executionProcessContext();
  if (ProcessID && (!Context || !*ProcessID))
    return modelError("internal process attachment requires a known caller");
  const bool Switch = ProcessID && *ProcessID != Context->ProcessID;
  if (Switch) {
    Context->ProcessID = *ProcessID;
  }
  InheritedExecutionContexts.emplace(
      Child, InheritedExecutionContext{
                 CurrentThreadKey, Switch ? *ProcessID : CurrentUserProcessID,
                 Switch ? *ProcessID != SystemProcessID
                        : canCatchUserAccess(profile::UserArenaBase, 1),
                 Context});
  return llvm::Error::success();
}

llvm::Error KernelModel::activateInheritedProcess() {
  const auto Inherited = InheritedExecutionContexts.find(CurrentExecution);
  if (Inherited == InheritedExecutionContexts.end() ||
      Inherited->second.ThreadKey != CurrentThreadKey ||
      !Inherited->second.Process || hasProcessAttachment(CurrentExecution))
    return llvm::Error::success();
  const auto &Context = Inherited->second;
  return setUserRequestContext(Context.Process->ProcessID != SystemProcessID,
                               Context.Process->ProcessID);
}

bool KernelModel::canCatchUserAccess(uint64_t Address, uint64_t Size) const {
  if (Size && Address >= profile::SectionViewBase &&
      Address < profile::SectionViewBase + profile::SectionViewSize &&
      Size <= profile::SectionViewBase + profile::SectionViewSize - Address &&
      CurrentIRQL <= APCLevel) {
    const auto Context = executionProcessContext();
    const uint32_t PID =
        UserRequestContext ? CurrentUserProcessID : uint32_t(SystemProcessID);
    if (Context && Context->ProcessID == PID)
      for (const auto &[Key, V] : SectionViews)
        if (V.ProcessID == PID && Address >= V.Base &&
            Address - V.Base < V.Size && Size <= V.Size - (Address - V.Base))
          return true;
  }
  const auto Inherited = InheritedExecutionContexts.find(CurrentExecution);
  const bool InheritedPermission =
      Inherited != InheritedExecutionContexts.end() &&
      Inherited->second.ThreadKey == CurrentThreadKey &&
      Inherited->second.UserProcessID == CurrentUserProcessID &&
      Inherited->second.UserMemoryAuthority;
  return Size && Address < profile::UserProbeLimit &&
         Size <= profile::UserProbeLimit - Address && UserRequestContext &&
         (CurrentExecution == profile::StackBase || InheritedPermission ||
          (hasProcessAttachment(CurrentExecution))) &&
         CurrentIRQL <= APCLevel;
}

llvm::Expected<uint64_t> KernelModel::probeUserBuffer(uint64_t Address,
                                                      uint64_t Size,
                                                      uint32_t Alignment,
                                                      bool ForWrite) {
  // A zero-length probe does not inspect even an invalid pointer/alignment.
  if (!Size)
    return 0;
  if (!Alignment || (Alignment & (Alignment - 1)))
    return modelError("user buffer probe requires a power-of-two alignment");
  if (Address >= profile::UserProbeLimit ||
      Size > profile::UserProbeLimit - Address)
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  if (Address & (Alignment - 1))
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusDatatypeMisalignment);
  if (!ForWrite)
    return 0;
  if (!canCatchUserAccess(Address, Size))
    return modelError("ProbeForWrite requires the requesting process context");
  // This historical API actually writes on every covered page. Preflight a
  // single byte before each touch so an expected access violation never
  // latches the backend's terminal checked-memory fault.
  for (uint64_t Byte = Address;;) {
    if (auto E = prepareProcessImageWrite(Byte, 1))
      return std::move(E);
    auto Allowed = Memory.canAccess(Byte, 1, Read | Write);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return llvm::make_error<KernelGuestException>(
          exceptions::StatusAccessViolation);
    auto Value = Memory.readInteger(Byte, 1);
    if (!Value)
      return Value.takeError();
    if (auto E = Memory.writeInteger(Byte, *Value, 1))
      return std::move(E);
    const uint64_t Next = (Byte & ~(profile::PageSize - 1)) + profile::PageSize;
    if (Next >= Address + Size)
      return 0;
    Byte = Next;
  }
}

llvm::Expected<std::string> KernelModel::readNativeObjectName(uint64_t Address,
                                                              bool Unicode) {
  if (auto E = runtime::checkBufferRange(Address, 16))
    return E;
  if (auto E = validateGuestAccess(Address, 16, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + 2, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || *Length > *Maximum ||
      *Length > profile::MaxDeviceNameSize * 2)
    return modelError("invalid or oversized object-name UNICODE_STRING");
  if (auto E = runtime::checkBufferRange(*Buffer, *Length))
    return E;
  if (auto E = validateGuestAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (!Bytes.empty())
    if (auto E = Memory.read(*Buffer, Bytes))
      return E;
  std::vector<uint16_t> Units(Bytes.size() / 2);
  for (size_t I = 0; I < Units.size(); ++I)
    Units[I] = uint16_t(Bytes[I * 2]) | (uint16_t(Bytes[I * 2 + 1]) << 8);
  auto Name = windows_unicode::utf8(Units);
  if (!Name)
    return Name.takeError();
  if (auto E = validateNativeObjectNameText(*Name, Unicode))
    return E;
  return Name;
}
} // namespace neverd::emulation
