//===- KernelNlsData.h - Explicit OEM multibyte data export
//----------------===//
///
/// \file
/// Explicit OEM multibyte data export.
///

#ifndef NEVERD_KERNEL_NLS_DATA_H
#define NEVERD_KERNEL_NLS_DATA_H
#include "KernelObjectTypes.h"
namespace neverd::emulation::nls {
inline constexpr llvm::StringLiteral OemMultibyteExport = "NlsMbOemCodePageTag";
inline constexpr uint32_t DataBytes = 1;
inline constexpr uint64_t OemMultibyteSlot = profile::ObjectTypeDataBase + 0x48;
static_assert(OemMultibyteSlot + DataBytes <=
              object_type::identity(object_type::Kind::Thread));
inline bool isData(llvm::StringRef Name) { return Name == OemMultibyteExport; }
} // namespace neverd::emulation::nls
#endif
