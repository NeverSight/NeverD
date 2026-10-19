//===- KernelModelLoadedModule.cpp - Owned driver loader metadata --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned driver loader metadata.
///

#include "../driver/DriverImage.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace module {
#define NEVERD_KERNEL_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelLoadedModule.def"
#undef NEVERD_KERNEL_MODULE_VALUE
} // namespace module

llvm::Error KernelModel::initializeLoadedModule(const DriverImage &Image,
                                                const std::string &Service) {
  if (DriverModule)
    return llvm::Error::success();
  if (DriverLoaderRecord)
    return llvm::createStringError(
        "runtime modules conflict with the declared loader_view");
  UnpackOpaqueEffects = true;
  auto Record = allocate(module::Size);
  if (!Record)
    return Record.takeError();
  DriverModule = *Record;
  // This bounded environment contains just the loaded guest driver. Its names
  // belong to the modeled service namespace, never the host filesystem.
  const std::string Name = Service + ".sys";
  const std::string FullName = "\\SystemRoot\\System32\\drivers\\" + Name;
  auto Existing = std::find_if(
      LoadedModules.begin(), LoadedModules.end(),
      [&](const auto &M) { return M.Guest && M.Base == Image.Base; });
  if (Existing == LoadedModules.end())
    LoadedModules.push_back({Image.Base, Image.Size, FullName, true,
                             Image.DLLCharacteristics, DriverModule});
  else {
    Existing->Record = DriverModule;
    Existing->Path = FullName;
  }
  auto Full = makeUnicodeString(FullName);
  if (!Full)
    return Full.takeError();
  auto Base = makeUnicodeString(Name);
  if (!Base)
    return Base.takeError();
  ModuleNameRanges = {{*Full, 16 + (FullName.size() + 1) * 2},
                      {*Base, 16 + (Name.size() + 1) * 2}};
  struct Field {
    uint64_t Offset, Value;
    unsigned Width;
  };
  for (const auto &F : {Field{module::Links, DriverModule, 8},
                        Field{module::Links + 8, DriverModule, 8},
                        Field{module::ImageBase, Image.Base, 8},
                        Field{module::EntryPoint, Image.Entry, 8},
                        Field{module::ImageSize, Image.Size, 4},
                        Field{module::LoadCount, 1, 2}})
    if (auto E = Memory.writeInteger(DriverModule + F.Offset, F.Value, F.Width))
      return E;
  for (const auto &[Offset, Source] : {std::pair{module::FullName, *Full},
                                       std::pair{module::BaseName, *Base}}) {
    std::array<uint8_t, 16> Bytes{};
    if (auto E = Memory.read(Source, Bytes))
      return E;
    if (auto E = Memory.write(DriverModule + Offset, Bytes))
      return E;
  }
  // Flags start at zero by model policy. Guest changes are ordinary observed
  // stores to the flags word; they cannot grant API/model capabilities.
  return Memory.writeInteger(DriverObject + windows::DriverSectionOffset,
                             DriverModule, 8);
}

llvm::Error KernelModel::validateLoadedModuleAccess(uint64_t Address,
                                                    uint32_t Size,
                                                    bool IsWrite) const {
  const uint64_t End = Address + Size; // Caller has already checked overflow.
  if (IsWrite)
    for (const auto &[Base, Length] : ModuleNameRanges)
      if (Address < Base + Length && Base < End)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "guest write to read-only module name");
  std::vector<uint64_t> Records{DriverModule};
  for (const auto &M : LoadedModules)
    if (!M.InstanceID.empty() && M.Record && M.Record != DriverModule)
      Records.push_back(M.Record);
  for (const auto &[ID, S] : ServiceImages)
    if (S.Phase != ServiceImageState::State::Unloaded)
      Records.push_back(S.Module);
  for (const auto Record : Records) {
    if (!Record || Address >= Record + module::Size || Record >= End)
      continue;
    for (uint64_t Offset = std::max(Address, Record) - Record;
         Offset < std::min(End, Record + module::Size) - Record; ++Offset) {
      const bool Allowed =
          IsWrite ? Offset >= module::Flags && Offset < module::Flags + 4
                  : Offset < module::Links + 16 ||
                        (Offset >= module::ImageBase &&
                         Offset < module::ImageSize + 4) ||
                        (Offset >= module::FullName &&
                         Offset < module::LoadCount + 2);
      if (!Allowed)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "guest access to an opaque or read-only "
                                       "kernel loaded-module field");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
