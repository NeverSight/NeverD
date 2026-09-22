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

#include "DriverImage.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

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
} // namespace

llvm::Error KernelExportRegistry::initialize(const DriverOptions &Options) {
  if (!Names.empty() || !Exports.empty())
    return invalid("kernel export registry is already initialized");
  if (Options.KernelExports.size() > profile::MaxImports)
    return invalid("kernel export inventory exceeds the profile capacity");
  for (const auto &[Name, Present] : Options.KernelExports) {
    if (Name.empty() || Name.size() > profile::MaxKernelExportNameSize ||
        !std::all_of(Name.begin(), Name.end(),
                     [](unsigned char C) { return C >= 0x21 && C <= 0x7e; }))
      return invalid("kernel export names must be bounded printable ASCII");
    if (!Present)
      Names.emplace(Name, 0);
  }
#define NEVERD_KERNEL_API(Name, Arity, Availability)                           \
  if (KernelRoutineAvailability::Availability ==                               \
          KernelRoutineAvailability::Export &&                                 \
      !Names.count(#Name)) {                                                   \
    auto Address = insert(#Name);                                              \
    if (!Address)                                                              \
      return Address.takeError();                                              \
  }
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
  for (const auto &[Name, Present] : Options.KernelExports)
    if (Present) {
      auto Address = insert(Name);
      if (!Address)
        return Address.takeError();
    }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelExportRegistry::insert(llvm::StringRef Name) {
  if (auto I = Names.find(Name.str()); I != Names.end())
    return I->second;
  if (Exports.size() >= profile::ThunkSize / profile::ThunkStride - 1)
    return invalid("kernel export table exhausted its thunk capacity");
  const uint64_t Address =
      profile::ThunkBase + Exports.size() * profile::ThunkStride;
  Names.emplace(Name.str(), Address);
  Exports.emplace(Address, Export{Name.str(), KernelProvider.str(), Address});
  return Address;
}

llvm::Expected<uint64_t>
KernelExportRegistry::bindImport(const DriverImport &Import) {
  auto Address = insert(Import.Name);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return invalid("driver imports an explicitly absent kernel export: " +
                   Import.Name);
  return *Address;
}

llvm::Expected<uint64_t>
KernelExportRegistry::resolve(llvm::StringRef Name) const {
  if (Name.empty())
    return 0;
  auto I = Names.find(Name.str());
  if (I == Names.end())
    return invalid("kernel export availability is unspecified: " + Name +
                   "; declare it in kernel_exports");
  return I->second;
}

const KernelExportRegistry::Export *
KernelExportRegistry::lookup(uint64_t Address) const {
  auto I = Exports.find(Address);
  return I == Exports.end() ? nullptr : &I->second;
}
} // namespace neverd::emulation
