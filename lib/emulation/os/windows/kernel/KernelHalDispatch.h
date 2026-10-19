///
/// \file
/// KernelHalDispatch.
///

#ifndef NEVERD_KERNEL_HAL_DISPATCH_H
#define NEVERD_KERNEL_HAL_DISPATCH_H
#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringRef.h"
namespace neverd::emulation::hal_dispatch {
inline constexpr llvm::StringLiteral ExportName = "HalDispatchTable";
inline constexpr llvm::StringLiteral CacheFunction = "HalGetCachedAcpiTable";
inline constexpr llvm::StringLiteral Provider = "HAL_DISPATCH";
inline constexpr uint32_t Bytes = 0xb8, Alignment = 8, CacheOffset = 0xa8,
                          Version = 4;
inline constexpr uint64_t Slot = profile::ObjectTypeDataBase + 0x200;
static_assert(Slot + Bytes <= profile::ObjectTypeDataBase + profile::PageSize);
inline bool isData(llvm::StringRef Name) { return Name == ExportName; }
} // namespace neverd::emulation::hal_dispatch
#endif
