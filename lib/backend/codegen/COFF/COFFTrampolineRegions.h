//===- COFFTrampolineRegions.h - Authenticated PE entry storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve source entries in any executable PE section to bounded raw bytes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFTRAMPOLINEREGIONS_H
#define NEVERD_COFFTRAMPOLINEREGIONS_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {
llvm::Expected<std::vector<TextLayout>> collectCOFFSourceTrampolineRegions(
    llvm::ArrayRef<uint8_t> Binary, const BinaryImage &Image,
    const std::map<std::string, uint64_t> &OriginalVAs);
} // namespace neverd

#endif
