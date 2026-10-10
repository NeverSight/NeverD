//===- KernelModuleImages.h - Resident kernel provider identities --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNELMODULEIMAGES_H
#define NEVERD_EMULATION_KERNELMODULEIMAGES_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::emulation {
class KernelExportRegistry;
struct KernelLoadedModule {
  std::string Name;
  uint64_t Base = 0, Size = 0;
};
struct KernelModuleImage {
  KernelLoadedModule Identity;
  /// Raw offsets equal RVAs; code starts after the read-only header page and
  /// ends before export metadata. Every function address comes from Registry.
  std::vector<uint8_t> Bytes;
};
llvm::Expected<std::vector<KernelModuleImage>>
makeKernelModuleImages(const KernelExportRegistry &Registry);
} // namespace neverd::emulation
#endif
