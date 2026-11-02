//===- NeverDCAPIDyldCache.cpp - Shared-cache inventory ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Expose the validated image catalogue used by the selected-image loader.
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/loader/MachO/DyldSharedCache.h"

using namespace neverd;
using namespace neverd::sdk;

const char *neverd_dyld_cache_images_json(const char *Path) {
  using namespace macho_loader::cache_load;
  llvm::json::Array Images;
  llvm::json::Object Root;
  auto Cache = macho_loader::DyldSharedCache::open(
      std::filesystem::u8path(Path ? Path : ""));
  if (!Cache) {
    Root[kErrorKey] = jsonSafeText(llvm::toString(Cache.takeError()));
  } else {
    for (const auto &Image : Cache->images())
      Images.push_back(llvm::json::Object{{kPathKey, jsonSafeText(Image.Path)},
                                          {kAddressKey, vaHex(Image.Address)}});
  }
  Root[kImagesKey] = std::move(Images);
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}
