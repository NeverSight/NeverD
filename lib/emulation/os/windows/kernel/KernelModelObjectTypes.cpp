//===- KernelModelObjectTypes.cpp - Typed public data export slots --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Typed public data export slots.
///

#include "KernelModel.h"
#include "KernelObjectTypes.h"

namespace neverd::emulation {
llvm::Error KernelModel::initializeObjectTypes() {
  if (auto E = Memory.map(profile::ObjectTypeDataBase, profile::PageSize,
                          Read | Write))
    return E;
  for (const auto &Entry : object_type::Entries) {
    const auto Kind =
        Entry.Type == object_type::Kind::Thread    ? ObjectKind::Thread
        : Entry.Type == object_type::Kind::Process ? ObjectKind::Process
        : Entry.Type == object_type::Kind::File    ? ObjectKind::File
                                                   : ObjectKind::Driver;
    auto Identity = objectType(Kind);
    if (!Identity)
      return Identity.takeError();
    uint64_t Slot = object_type::slot(Entry.Type);
    if (Exports) {
      auto Address = Exports->resolve(Entry.Name);
      if (!Address)
        return Address.takeError();
      if (!*Address)
        continue;
      const auto *Export = Exports->lookup(*Address);
      if (Export &&
          Export->Kind == KernelExportRegistry::ExportKind::ModuleData) {
        ObjectTypeDataSlots.push_back(*Address);
        continue;
      }
      if (!Export ||
          Export->Kind != KernelExportRegistry::ExportKind::ObjectTypeData)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "object-type export is not data");
      Slot = *Address;
    }
    if (auto E = Memory.writeInteger(Slot, *Identity, 8))
      return E;
    ObjectTypeDataSlots.push_back(Slot);
  }
  if (auto E = initializeProcessData())
    return E;
  if (auto E = initializeProcessorData())
    return E;
  if (auto E = initializeNlsData())
    return E;
  if (auto E = initializeHalDispatch())
    return E;
  return Memory.protect(profile::ObjectTypeDataBase, profile::PageSize, Read);
}

bool KernelModel::isObjectTypeDataRead(uint64_t Address, uint32_t Size) const {
  for (const uint64_t Slot : ObjectTypeDataSlots)
    if (Address >= Slot && Size <= 8 && Address - Slot <= 8 - Size)
      return true;
  return false;
}
} // namespace neverd::emulation
