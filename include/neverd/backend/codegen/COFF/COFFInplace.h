//===- COFFInplace.h - COFF/PE in-place patching ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE in-place binary rewriting without adding new sections.
/// Supports both no-shift (replace-in-place with NOP pad) and with-shift
/// (expand/shrink the .text section, fix up cross-references) modes.
/// Handles both PE32 and PE32+ via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFINPLACE_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFINPLACE_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

class COFFInplaceRewriter : public InplaceRewriter {
protected:
  llvm::StringRef getTextSectionName() const override {
    return section_names::coff::Text;
  }
  BinaryFormat getBinaryFormat() const override { return BinaryFormat::COFF; }
  std::unique_ptr<RelocResolver> createRelocResolver() const override;
  bool needsExecPermission() const override { return false; }
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFINPLACE_H
