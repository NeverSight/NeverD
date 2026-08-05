//===- MachOInplace.h - Mach-O in-place patching ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O in-place binary rewriting without adding new sections.
/// Handles both 32-bit and 64-bit Mach-O via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOINPLACE_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOINPLACE_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

class MachOInplaceRewriter : public InplaceRewriter {
protected:
  llvm::StringRef getTextSectionName() const override {
    return section_names::macho::Text;
  }
  BinaryFormat getBinaryFormat() const override { return BinaryFormat::MachO; }
  std::unique_ptr<RelocResolver> createRelocResolver() const override;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOINPLACE_H
