//===- DyldCacheLoading.h - Cache slide ownership ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply cache-owned slide recipes to the selected image's analysis model.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_DYLDCACHELOADING_H
#define NEVERD_LIB_LOADER_MACHO_DYLDCACHELOADING_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/Support/Error.h"

namespace neverd::macho_loader {
llvm::Error applyCacheSlideFixups(BinaryImage &Image,
                                  const LoadProgress &Progress);
} // namespace neverd::macho_loader

#endif // NEVERD_LIB_LOADER_MACHO_DYLDCACHELOADING_H
