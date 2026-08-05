//===- ELFInplace.h - ELF in-place patching ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF in-place binary rewriting without adding new sections.
/// Supports both ELF32 and ELF64 via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFINPLACE_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFINPLACE_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

class ELFInplaceRewriter : public InplaceRewriter {
protected:
  llvm::StringRef getTextSectionName() const override {
    return section_names::elf::Text;
  }
  BinaryFormat getBinaryFormat() const override { return BinaryFormat::ELF; }
  std::unique_ptr<RelocResolver> createRelocResolver() const override;
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_ELF_ELFINPLACE_H
