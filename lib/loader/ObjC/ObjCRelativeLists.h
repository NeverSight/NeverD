//===- ObjCRelativeLists.h - Conditional cache list references -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve an ordinary list or a preoptimized relative list array. Returned
/// origins describe declarations, never which images a process has loaded.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_OBJC_OBJCRELATIVELISTS_H
#define NEVERD_LOADER_OBJC_OBJCRELATIVELISTS_H

#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace neverd::objc {

struct ListReference {
  va_t Address = 0;
  std::optional<ObjCCacheListOrigin> CacheOrigin;
};

llvm::Expected<std::vector<ListReference>>
readListReferences(const BinaryImage &Image, va_t Address, size_t &Remaining);

} // namespace neverd::objc

#endif // NEVERD_LOADER_OBJC_OBJCRELATIVELISTS_H
