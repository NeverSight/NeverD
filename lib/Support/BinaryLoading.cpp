//===- BinaryLoading.cpp - Binary format auto-detection -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements binary format detection and loading via the Loader factory.
///
//===----------------------------------------------------------------------===//

#include "neverd/Support/BinaryLoading.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

using namespace llvm;

namespace neverd {

Expected<BinaryImage> loadBinary(const std::filesystem::path &Path) {
  auto TheLoader = Loader::create(Path);
  if (!TheLoader)
    return make_error<StringError>("unknown binary format: " + Path.string(),
                                   inconvertibleErrorCode());
  return TheLoader->load(Path);
}

} // namespace neverd
