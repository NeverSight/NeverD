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

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <utility>

namespace neverd::emulation {
namespace {
enum class KernelRoutineAvailability { Export, Helper };

#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

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

std::optional<llvm::StringRef>
KernelExportRegistry::canonicalImportModule(llvm::StringRef Module) {
  if (Module.equals_insensitive(KernelProvider) ||
      Module.equals_insensitive(KernelAliasProvider))
    return KernelProvider;
  if (Module.equals_insensitive(FrameworkLoaderProvider))
    return FrameworkLoaderProvider;
  if (Module.equals_insensitive(HALProvider))
    return HALProvider;
  return std::nullopt;
}

std::optional<uint64_t>
KernelExportRegistry::moduleBase(llvm::StringRef Module) {
  const auto Canonical = canonicalImportModule(Module);
  if (!Canonical)
    return std::nullopt;
  unsigned Index = *Canonical == KernelProvider ? 0
                   : *Canonical == HALProvider  ? 1
                                                : 2;
  return profile::KernelModuleBase + Index * profile::KernelModuleStride;
}

bool KernelExportRegistry::overlapsThunk(uint64_t Address, uint64_t Size) {
  const auto Overlaps = [&](uint64_t Base) {
    return Size && Address < Base + profile::ThunkSize &&
           (Address >= Base || Size > Base - Address);
  };
  if (Overlaps(profile::ThunkBase))
    return true;
  for (unsigned I = 0; I < profile::KernelModuleCount; ++I)
    if (Overlaps(profile::KernelModuleBase + I * profile::KernelModuleStride +
                 profile::KernelModuleCodeRVA))
      return true;
  return false;
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
  if (Exports.size() >= profile::ThunkSize / profile::ThunkStride - 1)
    return invalid("kernel export table exhausted its thunk capacity");
  uint64_t Base = profile::ThunkBase;
  if (Kind == ExportKind::ModuleExport) {
    const auto Image = moduleBase(Module);
    if (!Image)
      return invalid("kernel export has no module image identity");
    Base = *Image + profile::KernelModuleCodeRVA;
  }
  const uint64_t Address = Base + Exports.size() * profile::ThunkStride;
  Names.emplace(std::move(Key), Address);
  Exports.emplace(Address,
                  Export{Name.str(), Module.str(), Address, Kind, Binding});
  return Address;
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
KernelExportRegistry::resolve(llvm::StringRef Name) const {
  if (!Initialized)
    return invalid("kernel export registry is not initialized");
  if (Name.empty())
    return 0;
  if (!validName(Name))
    return invalid("kernel export names must be bounded printable ASCII");
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
  auto I = Exports.find(Address);
  return I == Exports.end() ? nullptr : &I->second;
}

size_t KernelExportRegistry::availableThunkCount() const {
  return profile::ThunkSize / profile::ThunkStride - 1 - Exports.size();
}
} // namespace neverd::emulation
