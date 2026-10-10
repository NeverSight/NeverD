//===- ArtifactView.h - Immutable occurrence selection -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable occurrence selection.
///
//===----------------------------------------------------------------------===//

#pragma once
#include "neverd/web/Blob.h"

#include "llvm/Support/JSON.h"

namespace neverd::web {
/// One authoritative immutable selection for direct-byte consumers. Encoded
/// Bun sources and embedded map text retain their dedicated decoding models.
struct ArtifactView {
  Blob Content;
  std::string BlobHash;
  uint64_t StorageOffset = 0;
  llvm::json::Object Origin;
};
} // namespace neverd::web
