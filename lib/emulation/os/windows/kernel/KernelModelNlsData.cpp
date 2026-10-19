//===- KernelModelNlsData.cpp - Session-owned read-only OEM observation
//----===//
///
/// \file
/// Session-owned read-only OEM observation.
///

#include "KernelModel.h"
#include "KernelNlsData.h"
namespace neverd::emulation {
llvm::Error KernelModel::initializeNlsData() {
  uint64_t Slot = nls::OemMultibyteSlot;
  if (Exports) {
    auto Address = Exports->resolve(nls::OemMultibyteExport);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return llvm::Error::success();
    const auto *Export = Exports->lookup(*Address);
    if (!Export || Export->Kind != KernelExportRegistry::ExportKind::NlsData)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "OEM multibyte export is not typed NLS data");
    Slot = *Address;
  }
  NlsOemDataSlot = Slot;
  if (NlsOemMultibyte)
    return Memory.writeInteger(Slot, *NlsOemMultibyte, nls::DataBytes);
  return llvm::Error::success();
}
bool KernelModel::isNlsDataRead(uint64_t Address, uint32_t Size) const {
  return NlsOemMultibyte.has_value() && NlsOemDataSlot &&
         Size == nls::DataBytes && Address == *NlsOemDataSlot;
}
llvm::Error KernelModel::validateNlsDataAccess(uint64_t Address, uint32_t Size,
                                               bool IsWrite) const {
  if (!NlsOemDataSlot || !Size || Address >= *NlsOemDataSlot + nls::DataBytes ||
      Address + Size <= *NlsOemDataSlot)
    return llvm::Error::success();
  if (!NlsOemMultibyte)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "NlsMbOemCodePageTag requires nls_oem_multibyte");
  if (IsWrite || !isNlsDataRead(Address, Size))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "access to opaque or read-only NLS data");
  return llvm::Error::success();
}
} // namespace neverd::emulation
