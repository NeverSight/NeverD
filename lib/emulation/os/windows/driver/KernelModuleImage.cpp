//===- KernelModuleImage.cpp - Pure native guest mapping ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure native guest mapping.
///

#include "KernelModuleImage.h"

#include "../kernel/KernelExportRegistry.h"
#include "DriverImage.h"

#include "neverd/loader/COFF/PEProgramExports.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "kernel module: " + Text);
}
} // namespace
llvm::Expected<ModuleMappingPlan>
planKernelGuestModule(const ModuleBinding &Binding,
                      const modules::ModuleImage &Source,
                      uint64_t MemoryLimit) {
  if ((!Binding.Scope.empty() && Binding.Scope != "kernel") ||
      !KernelExportRegistry::canonicalImportModule(Binding.GuestName))
    return invalid("requires a kernel scope and explicit PE module basename");
  auto Image =
      loadKernelModuleImage(Source, MemoryLimit, Binding.Base.value_or(0));
  if (!Image)
    return Image.takeError();
  KernelModuleUnwindInput Unwind{
      std::make_shared<const ExceptionInfo>(std::move(Image->Exceptions)),
      Image->PreferredBase, Image->Regions.front().Bytes.size(),
      Image->SecurityCookieAddress};
  ModuleMappingPlan Plan{};
  Plan.Size = Image->Size;
  Plan.Image.Base = Image->Base;
  Plan.Image.Entry = Image->Entry;
  for (auto &Region : Image->Regions) {
    Plan.Image.MappedBytes += Region.Bytes.size();
    Plan.Image.Regions.push_back(
        {Region.Address, Region.Permissions, std::move(Region.Bytes)});
  }
  for (const auto &Export : Image->Exports.Entries) {
    if (Export.Kind == PEExportKind::Hole)
      continue;
    if (Export.Kind == PEExportKind::Forwarder)
      continue;
    const uint64_t Address = Plan.Image.Base + Export.RVA;
    const auto Region = std::find_if(
        Plan.Image.Regions.begin(), Plan.Image.Regions.end(),
        [&](const auto &R) {
          return Address >= R.Address && Address - R.Address < R.Bytes.size();
        });
    if (Region == Plan.Image.Regions.end() ||
        !(Region->Permissions & (Read | Execute)))
      return invalid("export target has no readable or executable mapping");
    const auto Kind = Region->Permissions & Execute
                          ? modules::SymbolKind::Function
                          : modules::SymbolKind::Data;
    if (Export.Names.empty())
      Plan.Exports.push_back({{}, Export.Ordinal, Address, Kind});
    else
      for (const auto &Name : Export.Names)
        Plan.Exports.push_back({Name, Export.Ordinal, Address, Kind});
  }
  auto Preparation = std::make_shared<KernelModulePreparation>();
  Preparation->Guard = std::move(Image->Guard);
  if (Preparation->Guard.Enabled) {
    auto Patch = [&](uint64_t Slot, uint64_t Target) -> llvm::Error {
      if (!Slot)
        return llvm::Error::success();
      for (auto &R : Plan.Image.Regions)
        if (Slot >= R.Address && R.Bytes.size() >= 8 &&
            Slot - R.Address <= R.Bytes.size() - 8) {
          llvm::support::endian::write64le(R.Bytes.data() + Slot - R.Address,
                                           Target);
          return llvm::Error::success();
        }
      return invalid("validated CFG slot disappeared from its image");
    };
    if (auto E = Patch(Preparation->Guard.CheckPointerAddress,
                       profile::GuardCheckThunk))
      return std::move(E);
    if (auto E = Patch(Preparation->Guard.DispatchPointerAddress,
                       profile::GuardDispatchThunk))
      return std::move(E);
  }
  Preparation->Imports = std::move(Image->Imports);
  Preparation->Exports = std::move(Image->Exports);
  Preparation->Unwind = std::move(Unwind);
  Plan.Preparation = std::move(Preparation);
  return Plan;
}
} // namespace neverd::emulation
