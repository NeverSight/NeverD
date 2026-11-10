//===- PatchStorage.h - Replacement storage layout ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_PATCHSTORAGE_H
#define NEVERD_BACKEND_CODEGEN_PATCHSTORAGE_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

/// Page alignment used by replacement storage groups. Format installers must
/// additionally check the loaded image's native alignment constraints.
uint64_t sourcePatchStorageAlignment(Arch Architecture, BinaryFormat Format);

/// Zero denotes no writable allocation. Otherwise all new writable sections
/// occupy a tail starting at WritableOffset; the leading bytes can be RX and
/// the tail must be RW without execute permission.
struct PatchStorageLayout {
  uint64_t WritableOffset = 0;
  uint64_t ReadOnlySize = 0;
  /// Page-rounded prefix of the writable allocation which dyld makes read-only
  /// after fixups. Zero denotes no such allocation.
  uint64_t RelroSize = 0;
};

/// Native Mach-O readonly data may occupy format-reserved original storage.
/// This checks the producer representation; the Mach-O owner checks backing.
bool isExternalMachORelro(const CompiledImage &Image,
                          const CompiledSection &Section);

/// Validate section extents and a complete permission split before writing
/// any format headers. No writable section may share a page with new code.
llvm::Expected<PatchStorageLayout>
validatePatchStorageLayout(const CompiledImage &Image, uint64_t PageAlignment);

} // namespace neverd

#endif
