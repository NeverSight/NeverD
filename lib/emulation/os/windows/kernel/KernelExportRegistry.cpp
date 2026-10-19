//===- KernelExportRegistry.cpp - Guest kernel export identity ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable thunk assignment and explicit export availability for one session.
///
//===----------------------------------------------------------------------===//

#include "KernelExportRegistry.h"

#include "../driver/DriverImage.h"
#include "../exception/X64SEHMetadata.h"
#include "KernelDataExports.h"
#include "KernelHalDispatch.h"
#include "KernelNlsData.h"
#include "KernelObjectTypes.h"
#include "KernelProcessData.h"
#include "KernelProcessorData.h"
#include "KernelReferenceImage.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {
enum class KernelRoutineAvailability { Export, Helper };

#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

// Recognition establishes an import identity, not an implementation of the
// provider's routines. Preserve the existing module indices when extending it.
constexpr llvm::StringLiteral ImportModules[] = {
    KernelProvider,        HALProvider,           FrameworkLoaderProvider,
    FilterManagerProvider, NetworkDriverProvider, NetworkIOProvider};
static_assert(std::size(ImportModules) == profile::KernelModuleCount);

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

bool validName(llvm::StringRef Name) {
  return !Name.empty() && Name.size() <= profile::MaxKernelExportNameSize &&
         std::all_of(Name.begin(), Name.end(),
                     [](unsigned char C) { return C >= '!' && C <= '~'; });
}

llvm::StringRef defaultProvider(llvm::StringRef Name) {
#define NEVERD_KERNEL_HAL_API(Symbol, Arity, IRQL, Operation)                  \
  if (Name == #Symbol)                                                         \
    return HALProvider;
#include "KernelHALAPIs.def"
#undef NEVERD_KERNEL_HAL_API
  return KernelProvider;
}
} // namespace

std::optional<std::string>
KernelExportRegistry::canonicalImportModule(llvm::StringRef Module) {
  if (Module.empty() || Module.size() > profile::MaxKernelExportNameSize ||
      !std::all_of(Module.begin(), Module.end(),
                   [](unsigned char C) {
                     return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
                            (C >= '0' && C <= '9') || C == '_' || C == '-' ||
                            C == '.';
                   }) ||
      Module.front() == '.' || Module.contains(".."))
    return std::nullopt;
  const auto Extension = Module.rsplit('.');
  if (Extension.first.empty() || (!Extension.second.equals_insensitive("sys") &&
                                  !Extension.second.equals_insensitive("dll") &&
                                  !Extension.second.equals_insensitive("exe")))
    return std::nullopt;
  if (Module.equals_insensitive(KernelProvider) ||
      Module.equals_insensitive(KernelAliasProvider))
    return KernelProvider.str();
  return Module.lower();
}

bool KernelExportRegistry::matchesImportProvider(llvm::StringRef Requested,
                                                 llvm::StringRef Provided) {
  const auto Name = canonicalImportModule(Provided);
  if (!Name)
    return false;
  if (auto Exact = canonicalImportModule(Requested))
    return *Exact == *Name;
  if (llvm::StringRef(*Name).rsplit('.').first.equals_insensitive(Requested))
    return true;
  const auto Alias = canonicalImportModule(Requested.str() + ".exe");
  return Alias && *Alias == KernelProvider && *Name == KernelProvider;
}

std::optional<uint64_t>
KernelExportRegistry::moduleBase(llvm::StringRef Module) {
  const auto Canonical = canonicalImportModule(Module);
  if (!Canonical)
    return std::nullopt;
  for (unsigned Index = 0; Index < std::size(ImportModules); ++Index)
    if (*Canonical == ImportModules[Index])
      return profile::KernelModuleBase + Index * profile::KernelModuleStride;
  return std::nullopt;
}

bool KernelExportRegistry::overlapsThunk(uint64_t Address, uint64_t Size) {
  for (const auto &Range : executionWatches())
    if (Size && Address < Range.Address + Range.Size &&
        (Address >= Range.Address || Size > Range.Address - Address))
      return true;
  return false;
}

bool KernelExportRegistry::overlapsData(uint64_t Address, uint64_t Size) {
  const uint64_t First =
      profile::KernelModuleBase + profile::KernelModuleDataRVA;
  return Size && Address < First + profile::KernelModuleDataSize &&
         (Address >= First || Size > First - Address);
}

llvm::ArrayRef<ExecutionWatch> KernelExportRegistry::executionWatches() {
  static const auto Ranges = [] {
    std::array<ExecutionWatch, profile::KernelModuleCount + 1> Result{};
    Result[0] = {profile::ThunkBase, profile::ThunkSize};
    for (unsigned I = 0; I < profile::KernelModuleCount; ++I)
      Result[I + 1] = {profile::KernelModuleBase +
                           I * profile::KernelModuleStride +
                           profile::KernelModuleCodeRVA,
                       profile::ThunkSize};
    return Result;
  }();
  return Ranges;
}

llvm::Error KernelExportRegistry::initialize(const DriverOptions &Options) {
  if (Initialized)
    return invalid("kernel export registry is already initialized");
  if (Options.KernelExports.size() > profile::MaxImports)
    return invalid("kernel export inventory exceeds the profile capacity");
  // Publish the namespace only after every override and capacity check passes.
  // A malformed inventory must not leave a partially seeded export table.
  KernelExportRegistry Candidate;
  for (const auto &[Name, Present] : Options.KernelExports) {
    if (!validName(Name))
      return invalid("kernel export names must be bounded printable ASCII");
    if (!Present)
      Candidate.Names.emplace(Identity{defaultProvider(Name).str(), Name, 0},
                              0);
  }
#define NEVERD_KERNEL_API(Name, Arity, Availability)                           \
  if (KernelRoutineAvailability::Availability ==                               \
          KernelRoutineAvailability::Export &&                                 \
      !Candidate.Names.count(Identity{KernelProvider.str(), #Name, 0})) {      \
    auto Address = Candidate.insert(KernelProvider, #Name);                    \
    if (!Address)                                                              \
      return Address.takeError();                                              \
  }
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
#define NEVERD_KERNEL_HAL_API(Name, Arity, IRQL, Operation)                    \
  if (!Candidate.Names.count(Identity{HALProvider.str(), #Name, 0})) {         \
    auto Address = Candidate.insert(HALProvider, #Name);                       \
    if (!Address)                                                              \
      return Address.takeError();                                              \
  }
#include "KernelHALAPIs.def"
#undef NEVERD_KERNEL_HAL_API
  for (const auto &Data : kernel_data::Exports)
    if (!((Options.Version || !Options.KernelImage.empty()) &&
          (object_type::find(Data.Name) || process_data::find(Data.Name) ||
           processor::isData(Data.Name))) &&
        !Candidate.Names.count(
            Identity{KernelProvider.str(), Data.Name.str(), 0})) {
      auto Address = Candidate.insert(KernelProvider, Data.Name);
      if (!Address)
        return Address.takeError();
    }
  // Personality identity is needed for table-driven dispatch, not a callable
  // DDI contract. Direct execution still has no argument-count/model fallback.
  for (const auto Name : x64SEHPersonalityExports())
    if (!Candidate.Names.contains(
            Identity{KernelProvider.str(), Name.str(), 0})) {
      auto Address = Candidate.insert(KernelProvider, Name);
      if (!Address)
        return Address.takeError();
    }
  for (const auto &Entry : object_type::Entries) {
    Identity Key{KernelProvider.str(), Entry.Name.str(), 0};
    if (Candidate.Names.contains(Key))
      continue;
    const auto Address = object_type::slot(Entry.Type);
    Candidate.Names.emplace(std::move(Key), Address);
    Candidate.Exports.emplace(Address,
                              Export{Entry.Name.str(), KernelProvider.str(),
                                     Address, ExportKind::ObjectTypeData, 0});
  }
  const Identity ProcessKey{KernelProvider.str(),
                            process_data::InitialSystemProcess.str(), 0};
  if (!Candidate.Names.contains(ProcessKey)) {
    const auto Address = process_data::InitialSystemProcessSlot;
    Candidate.Names.emplace(ProcessKey, Address);
    Candidate.Exports.emplace(Address,
                              Export{process_data::InitialSystemProcess.str(),
                                     KernelProvider.str(), Address,
                                     ExportKind::ProcessData, 0});
  }
  const Identity ProcessorKey{KernelProvider.str(),
                              processor::CountExport.str(), 0};
  if (!Candidate.Names.contains(ProcessorKey)) {
    Candidate.Names.emplace(ProcessorKey, processor::CountSlot);
    Candidate.Exports.emplace(processor::CountSlot,
                              Export{processor::CountExport.str(),
                                     KernelProvider.str(), processor::CountSlot,
                                     ExportKind::ProcessorData, 0});
  }
  const Identity NlsKey{KernelProvider.str(), nls::OemMultibyteExport.str(), 0};
  if (!Candidate.Names.contains(NlsKey)) {
    Candidate.Names.emplace(NlsKey, nls::OemMultibyteSlot);
    Candidate.Exports.emplace(
        nls::OemMultibyteSlot,
        Export{nls::OemMultibyteExport.str(), KernelProvider.str(),
               nls::OemMultibyteSlot, ExportKind::NlsData, 0});
  }
  const Identity HalKey{KernelProvider.str(), hal_dispatch::ExportName.str(),
                        0};
  if (!Candidate.Names.contains(HalKey)) {
    Candidate.Names.emplace(HalKey, hal_dispatch::Slot);
    Candidate.Exports.emplace(hal_dispatch::Slot,
                              Export{hal_dispatch::ExportName.str(),
                                     KernelProvider.str(), hal_dispatch::Slot,
                                     ExportKind::HalDispatchData, 0});
  }
  for (const auto &[Name, Present] : Options.KernelExports)
    if (Present) {
      auto Address = Candidate.insert(defaultProvider(Name), Name);
      if (!Address)
        return Address.takeError();
    }
  Candidate.Initialized = true;
  *this = std::move(Candidate);
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelExportRegistry::insert(llvm::StringRef Module,
                                                      llvm::StringRef Name,
                                                      ExportKind Kind,
                                                      uint64_t Binding) {
  Identity Key{Module.str(), Name.str(), Binding};
  if (auto I = Names.find(Key); I != Names.end())
    return I->second;
  const auto *Data =
      Kind == ExportKind::ModuleExport && Module == KernelProvider
          ? kernel_data::lookup(Name)
          : nullptr;
  if (!Data && ThunkCount >= profile::ThunkSize / profile::ThunkStride - 1)
    return invalid("kernel export table exhausted its thunk capacity");
  uint64_t Base = profile::ThunkBase;
  if (Kind == ExportKind::ModuleExport) {
    const auto Image = moduleBase(Module);
    if (Image)
      Base = *Image + profile::KernelModuleCodeRVA;
  }
  uint64_t Address = Base + ThunkCount * profile::ThunkStride;
  if (Data) {
    Address = *moduleBase(Module) + profile::KernelModuleDataRVA + Data->Offset;
    Kind = ExportKind::ModuleData;
  } else {
    ++ThunkCount;
  }
  Names.emplace(std::move(Key), Address);
  Exports.emplace(Address,
                  Export{Name.str(), Module.str(), Address, Kind, Binding});
  return Address;
}

llvm::Error
KernelExportRegistry::attachReference(const KernelReferenceImage &Reference,
                                      const DriverOptions &Options) {
  KernelExportRegistry Candidate = *this;
  if (auto E = Candidate.attachReferenceImage(
          Reference.exports(), Options, Reference.objectTypeExports(),
          Reference.processDataExports(), Reference.processorDataExports(),
          Reference.nlsDataExports(), Reference.halDispatchExports()))
    return E;
  if (Reference.debugger())
    Candidate.ReferenceHalCacheFunction =
        Reference.debugger()->HalCacheDispatch;
  *this = std::move(Candidate);
  return llvm::Error::success();
}

llvm::Error KernelExportRegistry::attachReferenceImage(
    const std::map<std::string, uint64_t> &Inventory,
    const DriverOptions &Options, const std::map<std::string, uint64_t> &Data,
    const std::map<std::string, uint64_t> &ProcessData,
    const std::map<std::string, uint64_t> &ProcessorData,
    const std::map<std::string, uint64_t> &NlsData,
    const std::map<std::string, uint64_t> &HalData) {
  if (!Initialized || ReferenceNamespace ||
      Inventory.size() > profile::MaxImports)
    return invalid("invalid kernel reference export inventory");
  KernelExportRegistry Candidate = *this;
  for (const auto &[Name, Address] : Data)
    if (!object_type::find(Name) || !Address ||
        Address % profile::PointerSize || !Inventory.contains(Name) ||
        Inventory.at(Name))
      return invalid("invalid kernel object-type data export inventory");
  for (const auto &[Name, Address] : ProcessData)
    if (!process_data::find(Name) || !Address ||
        Address % profile::PointerSize || !Inventory.contains(Name) ||
        Inventory.at(Name))
      return invalid("invalid kernel process data export inventory");
  for (const auto &[Name, Address] : ProcessorData)
    if (!processor::isData(Name) || !Address ||
        Address % processor::CountBytes || !Inventory.contains(Name) ||
        Inventory.at(Name))
      return invalid("invalid kernel processor data export inventory");
  for (const auto &[Name, Address] : NlsData)
    if (!nls::isData(Name) || !Address || !Inventory.contains(Name) ||
        Inventory.at(Name))
      return invalid("invalid kernel NLS data export inventory");
  for (const auto &[Name, Address] : HalData)
    if (!hal_dispatch::isData(Name) || !Address ||
        Address % hal_dispatch::Alignment || !Inventory.contains(Name) ||
        Inventory.at(Name))
      return invalid("invalid kernel HAL dispatch data export inventory");
  // Typed byte exports must not alias the interior of another data slot.
  // Validate all ranges before publishing any reference identities.
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (const auto *Map :
       {&Data, &ProcessData, &ProcessorData, &NlsData, &HalData})
    for (const auto &[Name, Address] : *Map) {
      const uint64_t Size = hal_dispatch::isData(Name) ? hal_dispatch::Bytes
                            : processor::isData(Name)  ? processor::CountBytes
                            : nls::isData(Name)        ? nls::DataBytes
                                                       : 8;
      if (Address > UINT64_MAX - Size)
        return invalid("kernel typed data export extent overflows");
      Ranges.emplace_back(Address, Address + Size);
    }
  std::sort(Ranges.begin(), Ranges.end());
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return invalid("kernel typed data export extents overlap");
  for (auto &[Key, Address] : Candidate.Names) {
    const auto &[Module, Name, Binding] = Key;
    if (Module == KernelProvider && !Inventory.contains(Name) &&
        !Options.KernelExports.contains(Name)) {
      Candidate.Exports.erase(Address);
      Address = 0;
    }
  }
  for (const auto &[Name, Code] : Inventory) {
    if (!validName(Name))
      return invalid("invalid kernel reference export name");
    if (auto Override = Options.KernelExports.find(Name);
        Override != Options.KernelExports.end() && !Override->second)
      continue;
    auto Token = Candidate.insert(KernelProvider, Name);
    if (!Token)
      return Token.takeError();
    const auto Type = object_type::find(Name);
    const bool Process = process_data::find(Name);
    const bool Processor = processor::isData(Name);
    const bool Nls = nls::isData(Name);
    const bool Hal = hal_dispatch::isData(Name);
    if (Type && Code)
      return invalid("kernel object-type export is executable code");
    if (Process && Code)
      return invalid("kernel process data export is executable code");
    if (Processor && Code)
      return invalid("kernel processor data export is executable code");
    if (Nls && Code)
      return invalid("kernel NLS data export is executable code");
    if (Hal && Code)
      return invalid("kernel HAL dispatch export is executable code");
    if (Process && !ProcessData.contains(Name))
      return invalid(
          "kernel process data export requires an authenticated slot");
    if (Processor && !ProcessorData.contains(Name))
      return invalid(
          "kernel processor data export requires an authenticated slot");
    if (Nls && !NlsData.contains(Name))
      return invalid("kernel NLS data export requires an authenticated slot");
    if (Hal && !HalData.contains(Name))
      return invalid(
          "kernel HAL dispatch export requires an authenticated table");
    const uint64_t Target = Hal         ? HalData.at(Name)
                            : Nls       ? NlsData.at(Name)
                            : Processor ? ProcessorData.at(Name)
                            : Process   ? ProcessData.at(Name)
                            : Type && Data.contains(Name) ? Data.at(Name)
                                                          : Code;
    if (!Target || !*Token)
      continue;
    if (Candidate.Exports.contains(Target))
      return invalid("ambiguous kernel reference export address");
    auto Export = Candidate.Exports.at(*Token);
    Candidate.Exports.erase(*Token);
    Export.Address = Target;
    // An authenticated kernel can re-export a modeled HAL routine. Both
    // import spellings then name that observed address and share the HAL
    // contract; an arbitrary second import still cannot establish an alias.
    if (Code && defaultProvider(Name) == HALProvider) {
      const Identity HALKey{HALProvider.str(), Name, 0};
      if (auto I = Candidate.Names.find(HALKey); I != Candidate.Names.end()) {
        Candidate.Exports.erase(I->second);
        I->second = Target;
      }
      Export.Module = HALProvider.str();
    }
    if (Type)
      Export.Kind = ExportKind::ObjectTypeData;
    if (Process)
      Export.Kind = ExportKind::ProcessData;
    if (Processor)
      Export.Kind = ExportKind::ProcessorData;
    if (Nls)
      Export.Kind = ExportKind::NlsData;
    if (Hal)
      Export.Kind = ExportKind::HalDispatchData;
    Candidate.Exports.emplace(Target, std::move(Export));
    Candidate.Names[Identity{KernelProvider.str(), Name, 0}] = Target;
  }
  Candidate.ReferenceNamespace = true;
  *this = std::move(Candidate);
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelExportRegistry::bindImport(const DriverImport &Import) {
  if (!Initialized)
    return invalid("kernel export registry is not initialized");
  const auto Module = canonicalImportModule(Import.Module);
  if (!Module)
    return invalid("unsupported import provider: " + Import.Module);
  if (!validName(Import.Name))
    return invalid("import names must be bounded printable ASCII");
  auto Address = insert(*Module, Import.Name);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return invalid("driver imports an explicitly absent kernel export: " +
                   Import.Name);
  return *Address;
}

llvm::Expected<uint64_t>
KernelExportRegistry::existingImport(const DriverImport &Import) const {
  auto Found = findImport(Import);
  if (!Found)
    return Found.takeError();
  if (!*Found)
    return invalid("module import has no existing binding: " + Import.Module +
                   "!" + Import.Name);
  return **Found;
}
llvm::Expected<std::optional<uint64_t>>
KernelExportRegistry::findImport(const DriverImport &Import) const {
  const auto Module = canonicalImportModule(Import.Module);
  if (!Initialized || !Module || !validName(Import.Name))
    return invalid("invalid module import identity");
  if (Import.Ordinal)
    return std::optional<uint64_t>();
  const auto Found = Names.find(Identity{*Module, Import.Name, 0});
  if (Found == Names.end())
    return std::optional<uint64_t>();
  if (!Found->second)
    return invalid("module import is explicitly absent: " + Import.Module +
                   "!" + Import.Name);
  return std::optional(Found->second);
}
std::set<std::string> KernelExportRegistry::importProviders() const {
  std::set<std::string> Providers;
  for (const auto &[Key, Address] : Names)
    if (const auto Module = canonicalImportModule(std::get<0>(Key)))
      Providers.insert(*Module);
  return Providers;
}

llvm::Expected<uint64_t>
KernelExportRegistry::resolve(llvm::StringRef Name) const {
  if (!Initialized)
    return invalid("kernel export registry is not initialized");
  if (Name.empty())
    return 0;
  if (!validName(Name))
    return invalid("kernel export names must be bounded printable ASCII");
  if (const auto Default =
          Names.find(Identity{defaultProvider(Name).str(), Name.str(), 0});
      Default != Names.end() && !Default->second)
    return uint64_t(0);
  std::optional<uint64_t> Address;
  for (llvm::StringRef Module : {KernelProvider, HALProvider}) {
    auto I = Names.find(Identity{Module.str(), Name.str(), 0});
    if (I == Names.end())
      continue;
    if (Address && *Address && I->second && *Address != I->second)
      return invalid("ambiguous kernel/HAL export identity: " + Name);
    if (!Address || I->second)
      Address = I->second;
  }
  if (!Address && ReferenceNamespace)
    return 0;
  if (!Address)
    return invalid("kernel export availability is unspecified: " + Name +
                   "; declare it in kernel_exports");
  return *Address;
}

llvm::Expected<uint64_t>
KernelExportRegistry::insertFrameworkFunction(uint64_t BindingIdentity,
                                              llvm::StringRef Name) {
  if (!Initialized)
    return invalid("kernel export registry is not initialized");
  if (!BindingIdentity)
    return invalid("framework function requires a nonzero binding identity");
  if (!validName(Name))
    return invalid("framework function names must be bounded printable ASCII");
  return insert(FrameworkProvider, Name, ExportKind::FrameworkFunction,
                BindingIdentity);
}
llvm::Expected<uint64_t>
KernelExportRegistry::insertHalDispatchFunction(uint64_t Table) {
  auto Found = Names.find(
      Identity{KernelProvider.str(), hal_dispatch::ExportName.str(), 0});
  if (!Initialized || !Table || Found == Names.end() || Found->second != Table)
    return invalid("HAL function requires the live typed dispatch table");
  const Identity Key{hal_dispatch::Provider.str(),
                     hal_dispatch::CacheFunction.str(), Table};
  if (auto Existing = Names.find(Key); Existing != Names.end())
    return Existing->second;
  KernelExportRegistry Candidate = *this;
  auto Thunk =
      Candidate.insert(hal_dispatch::Provider, hal_dispatch::CacheFunction,
                       ExportKind::HalDispatchFunction, Table);
  if (!Thunk)
    return Thunk.takeError();
  const auto Target =
      ReferenceHalCacheFunction ? ReferenceHalCacheFunction : *Thunk;
  if (Target != *Thunk) {
    if (Candidate.Exports.contains(Target))
      return invalid(
          "HAL dispatch target aliases an existing callable identity");
    auto E = Candidate.Exports.at(*Thunk);
    Candidate.Exports.erase(*Thunk);
    E.Address = Target;
    Candidate.Exports.emplace(Target, std::move(E));
    Candidate.Names[Key] = Target;
  }
  *this = std::move(Candidate);
  return Target;
}

llvm::Expected<uint64_t>
KernelExportRegistry::insertDMAFunction(uint64_t Adapter,
                                        llvm::StringRef Name) {
  if (!Initialized || !Adapter || !validName(Name))
    return invalid(
        "DMA function requires an initialized namespace, adapter and name");
  return insert(DMAProvider, Name, ExportKind::DMAFunction, Adapter);
}

llvm::Expected<uint64_t>
KernelExportRegistry::insertProviderFunction(uint64_t PDO,
                                             llvm::StringRef Name) {
  if (!Initialized || !PDO || !validName(Name))
    return invalid(
        "provider callback requires an initialized namespace, PDO and name");
  return insert(BusProvider, Name, ExportKind::ProviderFunction, PDO);
}

const KernelExportRegistry::Export *
KernelExportRegistry::lookup(uint64_t Address) const {
  // Only registered identities are recognized, including authenticated native
  // code/data and typed model slots outside the synthetic service windows.
  auto I = Exports.find(Address);
  return I == Exports.end() ? nullptr : &I->second;
}

size_t KernelExportRegistry::availableThunkCount() const {
  return profile::ThunkSize / profile::ThunkStride - 1 - ThunkCount;
}
} // namespace neverd::emulation
