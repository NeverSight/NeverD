//===- DriverProcessImages.h - Validated image-section mapping plan
//--------===//
// NeverD Decompiler
///
/// \file
/// Validated image-section mapping plan.
///

#ifndef NEVERD_DRIVER_PROCESS_IMAGES_H
#define NEVERD_DRIVER_PROCESS_IMAGES_H
#include "neverd/emulation/DriverFiniteFileSystem.h"
#include "neverd/emulation/DriverProcessImages.h"

#include "llvm/Support/Error.h"

#include <optional>
namespace neverd::emulation {
struct DriverProcessImageFileIdentity {
  size_t Volume;
  std::string Key;
};
/// A declared matching volume makes its file identity authoritative. An
/// unrelated or omitted volume catalogue does not invent a source file.
llvm::Expected<std::optional<DriverProcessImageFileIdentity>>
driverProcessImageFileIdentity(
    const DriverProcessImageMapping &Source,
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes);
struct DriverProcessImagePlan {
  uint64_t Base = 0;
  std::string MappedFile;
  std::string DeviceName;
  std::vector<uint8_t> Bytes;
  // Native protection for every complete page in the image allocation.
  std::vector<uint8_t> PageProtection;
};
/// Bounded ordinary 19041 PE64 SEC_IMAGE mapping. This does not execute a
/// user loader, resolve imports, run TLS, or infer process metadata.
llvm::Expected<DriverProcessImagePlan>
planDriverProcessImage(const DriverProcessImageMapping &Source);
} // namespace neverd::emulation
#endif
